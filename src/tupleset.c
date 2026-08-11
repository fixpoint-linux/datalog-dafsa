/*
 * tupleset.c — In-memory sorted tuple set: hash + binary-search prefix
 *
 * Hash set uses open addressing with FNV-1a hashing (same basis as the
 * vendored sig_compute).  Hash slots store 1-based indices into data[]
 * (0 = empty).  Hash load factor target is 0.7; we grow at 0.75.
 *
 * The sorted array is the source of truth for ts_prefix (binary search)
 * and for bulk iteration.  ts_add consults the hash set FIRST, so the
 * array NEVER holds duplicates.
 */
#include "tupleset.h"
#include <stdlib.h>
#include <string.h>

/* FNV-1a constants (matching vendor/dafsa_internal.h) */
#define FNV_OFFSET 14695981039346656037ULL
#define FNV_PRIME  1099511628211ULL

/* ─── Internal helpers ───────────────────────────────────────────────── */

/* FNV-1a hash over arity u32 columns, byte at a time.
 * Matches the style of overlay_hash_bytes in vendor/dafsa_view.c. */
static uint64_t ts_hash_cols(const uint32_t *cols, uint8_t arity)
{
    uint64_t h = FNV_OFFSET;
    uint8_t i;
    for (i = 0; i < arity; i++) {
        uint32_t v = cols[i];
        h ^= (v & 0xFF);        h *= FNV_PRIME;
        h ^= ((v >> 8) & 0xFF); h *= FNV_PRIME;
        h ^= ((v >> 16) & 0xFF); h *= FNV_PRIME;
        h ^= ((v >> 24) & 0xFF); h *= FNV_PRIME;
    }
    return h;
}

/* Compare two arity-strided tuples for equality. */
static int ts_tuple_eq(const uint32_t *a, const uint32_t *b, uint8_t arity)
{
    return memcmp(a, b, (size_t)arity * sizeof(uint32_t)) == 0;
}

/* Grow the hash table to at least `need` slots (power of two). */
static int ts_hash_grow(tuple_set *ts, size_t need)
{
    size_t new_cap = 16;
    while (new_cap < need) new_cap *= 2;
    if (new_cap > (size_t)1 << 30) return -1;  /* too large */

    uint32_t *new_htab = calloc(new_cap, sizeof(uint32_t));
    if (!new_htab) return -1;

    /* Rehash all existing entries */
    if (ts->htab) {
        size_t i;
        for (i = 0; i < ts->hcap; i++) {
            uint32_t slot = ts->htab[i];
            if (slot == 0) continue;
            const uint32_t *cols = ts->data + (size_t)(slot - 1) * ts->arity;
            uint64_t h = ts_hash_cols(cols, ts->arity);
            size_t idx = (size_t)(h & (new_cap - 1));
            while (new_htab[idx] != 0)
                idx = (idx + 1) & (new_cap - 1);
            new_htab[idx] = slot;
        }
        free(ts->htab);
    }
    ts->htab = new_htab;
    ts->hcap = new_cap;
    return 0;
}

/* Hash-table lookup: returns 1-based index into data[] if found, 0 if absent. */
static uint32_t ts_hash_find(const tuple_set *ts, const uint32_t *cols)
{
    uint64_t h;
    size_t idx;
    if (!ts->htab || ts->hcap == 0) return 0;

    h   = ts_hash_cols(cols, ts->arity);
    idx = (size_t)(h & (ts->hcap - 1));
    for (;;) {
        uint32_t slot = ts->htab[idx];
        if (slot == 0) return 0;  /* empty → not found */
        if (ts_tuple_eq(cols, ts->data + (size_t)(slot - 1) * ts->arity, ts->arity))
            return slot;
        idx = (idx + 1) & (ts->hcap - 1);
    }
}

/* Hash-table insert: store 1-based index.  Caller must ensure it's not
 * already present and that there is room. */
static void ts_hash_insert(tuple_set *ts, const uint32_t *cols, uint32_t idx1)
{
    uint64_t h = ts_hash_cols(cols, ts->arity);
    size_t i = (size_t)(h & (ts->hcap - 1));
    while (ts->htab[i] != 0)
        i = (i + 1) & (ts->hcap - 1);
    ts->htab[i] = idx1;
}

/* ─── Sorting ────────────────────────────────────────────────────────── */

static uint8_t g_sort_ar;
static int cmp_ts(const void *a, const void *b)
{
    const uint32_t *x = a, *y = b;
    uint8_t i;
    for (i = 0; i < g_sort_ar; i++) {
        if (x[i] < y[i]) return -1;
        if (x[i] > y[i]) return 1;
    }
    return 0;
}

/* ─── Public API ─────────────────────────────────────────────────────── */

int ts_init(tuple_set *ts, uint8_t arity)
{
    if (!ts || arity == 0 || arity > 8) return -1;
    memset(ts, 0, sizeof(*ts));
    ts->arity = arity;
    return 0;
}

void ts_free(tuple_set *ts)
{
    if (!ts) return;
    free(ts->data);
    free(ts->htab);
    memset(ts, 0, sizeof(*ts));
}

int ts_contains(const tuple_set *ts, const uint32_t *cols)
{
    if (!ts || !cols || ts->count == 0) return 0;
    return ts_hash_find(ts, cols) != 0;
}

int ts_add(tuple_set *ts, const uint32_t *cols)
{
    if (!ts || !cols) return -1;

    /* Check hash first (authority on uniqueness) */
    if (ts_hash_find(ts, cols) != 0)
        return 0;  /* duplicate */

    /* Grow data array if needed */
    if (ts->count >= ts->cap) {
        long nc = ts->cap ? ts->cap * 2 : 1024;
        uint32_t *nd = realloc(ts->data,
            (size_t)nc * (size_t)ts->arity * sizeof(uint32_t));
        if (!nd) return -1;
        ts->data = nd;
        ts->cap  = nc;
    }

    /* Append to sorted array */
    memcpy(ts->data + (size_t)ts->count * ts->arity, cols,
           (size_t)ts->arity * sizeof(uint32_t));
    ts->count++;

    /* Grow hash table if needed (load factor 0.75) */
    {
        size_t need = (size_t)((double)ts->count / 0.70);
        if (need < 16) need = 16;
        if (ts->hcap == 0 || (size_t)ts->count > ts->hcap - (ts->hcap >> 2)) {
            if (ts_hash_grow(ts, need) != 0) {
                ts->count--;  /* roll back */
                return -1;
            }
        }
    }

    /* Insert into hash table */
    ts_hash_insert(ts, cols, (uint32_t)ts->count);  /* 1-based */
    ts->hused++;
    return 1;
}

long ts_prefix(const tuple_set *ts, const uint32_t *p, uint8_t k,
               long first_idx_out[1])
{
    long lo, hi;
    uint8_t ar;

    if (first_idx_out) first_idx_out[0] = 0;
    if (!ts || !p || ts->count == 0 || k > ts->arity) return 0;
    ar = ts->arity;

    /* Binary search for lower bound */
    lo = 0;
    hi = ts->count;
    while (lo < hi) {
        long mid = lo + (hi - lo) / 2;
        const uint32_t *r = ts->data + mid * ar;
        int c = 0;
        uint8_t i;
        for (i = 0; i < k; i++) {
            if (r[i] < p[i]) { c = -1; break; }
            if (r[i] > p[i]) { c = 1; break; }
        }
        if (c < 0) lo = mid + 1;
        else       hi = mid;
    }

    if (lo >= ts->count) return 0;

    /* Verify match */
    {
        const uint32_t *r = ts->data + lo * ar;
        uint8_t i;
        for (i = 0; i < k; i++)
            if (r[i] != p[i]) return 0;
    }

    /* Walk backward to find first match */
    {
        long first = lo;
        while (first > 0) {
            const uint32_t *r = ts->data + (first - 1) * ar;
            int match = 1;
            uint8_t i;
            for (i = 0; i < k; i++)
                if (r[i] != p[i]) { match = 0; break; }
            if (!match) break;
            first--;
        }
        if (first_idx_out) first_idx_out[0] = first;

        /* Walk forward to count matches */
        long last = lo;
        while (last < ts->count) {
            const uint32_t *r = ts->data + last * ar;
            int match = 1;
            uint8_t i;
            for (i = 0; i < k; i++)
                if (r[i] != p[i]) { match = 0; break; }
            if (!match) break;
            last++;
        }
        return last - first;
    }
}

void ts_sort(tuple_set *ts)
{
    if (!ts || ts->count <= 1 || ts->arity == 0) return;

    g_sort_ar = ts->arity;
    qsort(ts->data, (size_t)ts->count, (size_t)ts->arity * sizeof(uint32_t), cmp_ts);

    /* Dedup: compact in-place (safety net; hash should prevent dups) */
    {
        long i, w = 1;
        for (i = 1; i < ts->count; i++) {
            const uint32_t *prev = ts->data + (size_t)(w - 1) * ts->arity;
            const uint32_t *cur  = ts->data + (size_t)i * ts->arity;
            if (memcmp(prev, cur, (size_t)ts->arity * sizeof(uint32_t)) != 0) {
                if (w != i)
                    memcpy(ts->data + (size_t)w * ts->arity, cur,
                           (size_t)ts->arity * sizeof(uint32_t));
                w++;
            }
        }
        ts->count = w;
    }

    /* Rebuild hash table: positions changed after sort+dedup */
    if (ts->htab) {
        memset(ts->htab, 0, ts->hcap * sizeof(uint32_t));
        ts->hused = 0;
    }
    {
        long i;
        for (i = 0; i < ts->count; i++) {
            const uint32_t *cols = ts->data + i * ts->arity;
            /* Ensure hash table is large enough */
            size_t need = (size_t)((double)(i + 1) / 0.70);
            if (need < 16) need = 16;
            if (ts->hcap == 0 || (size_t)(i + 1) > ts->hcap - (ts->hcap >> 2)) {
                if (ts_hash_grow(ts, need) != 0) return;
            }
            ts_hash_insert(ts, cols, (uint32_t)(i + 1));
            ts->hused++;
        }
    }
}

void ts_reset(tuple_set *ts)
{
    if (!ts) return;
    ts->count = 0;
    if (ts->htab) {
        memset(ts->htab, 0, ts->hcap * sizeof(uint32_t));
        ts->hused = 0;
    }
}
