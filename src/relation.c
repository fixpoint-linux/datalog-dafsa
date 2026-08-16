/*
 * relation.c — Per-relation DAFSA with fixed-width u32BE column encoding
 *
 * Key encoding (the linchpin, architecture §2.3):
 *   | col0 u32BE | col1 u32BE | ... | col_{a-1} u32BE | \0 |
 *   Total: 4*a + 1 bytes, NO inter-column separators.
 *
 * Exact lookup:  dafsa_lookup_n(key, 4*a+1)
 * Prefix enum:   custom DFS walker (walks k*4 prefix bytes via trans_find,
 *                then DFS from the resulting state — NO W\0 requirement).
 *                This is the linchpin: "bind k leading columns, enumerate
 *                rest" = walk k*4 bytes, DFS from there.
 *
 * Uses dafsa_internal.h for the DFS internals (enum_dfs, trans_find, State).
 */

#include "relation.h"
#include "dafsa.h"
#include "dafsa_internal.h"
#include "regexwalk.h"
#include "tupleset.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

#define MAX_ARITY 8
#define MAX_KEY_LEN (MAX_ARITY * 4 + 1)  /* 33 */

struct relation {
    dafsa     *d;         /* VIEW = base ∪ derived; all reads enumerate this */
    dafsa     *base;      /* BASE = durable EDB facts; base == d for EDB-only */
    uint8_t    arity;
    dafsa_wal *wal;       /* M7: per-relation WAL handle, or NULL */
    char      *wal_path;  /* M7: path to WAL file (owned) */
};

/* ─── Key encoding helpers ────────────────────────────────────────────── */

/* Write arity u32 columns as big-endian into buf (4*arity bytes).
 * Does NOT write the trailing \0 — caller must add it. */
static void write_cols_be(unsigned char *buf, const uint32_t *cols,
                          uint8_t arity)
{
    uint8_t i;
    for (i = 0; i < arity; i++) {
        uint32_t v = cols[i];
        buf[4*i]     = (unsigned char)((v >> 24) & 0xFF);
        buf[4*i + 1] = (unsigned char)((v >> 16) & 0xFF);
        buf[4*i + 2] = (unsigned char)((v >> 8)  & 0xFF);
        buf[4*i + 3] = (unsigned char)(v & 0xFF);
    }
}

/* Read arity u32 columns from big-endian buf. */
static void read_cols_be(uint32_t *cols, const unsigned char *buf,
                         uint8_t arity)
{
    uint8_t i;
    for (i = 0; i < arity; i++) {
        cols[i] = ((uint32_t)buf[4*i]     << 24) |
                  ((uint32_t)buf[4*i + 1] << 16) |
                  ((uint32_t)buf[4*i + 2] << 8)  |
                  ((uint32_t)buf[4*i + 3]);
    }
}

/* Build the full key (4*arity + 1 bytes) in buf.  buf must be >= MAX_KEY_LEN. */
static size_t encode_key(unsigned char *buf, const uint32_t *cols,
                         uint8_t arity)
{
    write_cols_be(buf, cols, arity);
    buf[4 * arity] = 0x00;  /* trailing guard */
    return (size_t)(4 * arity + 1);
}

/* ─── Lifecycle ───────────────────────────────────────────────────────── */

relation *rel_create(uint8_t arity)
{
    relation *rel;

    if (arity == 0 || arity > MAX_ARITY) return NULL;

    rel = calloc(1, sizeof(*rel));
    if (!rel) return NULL;

    rel->d = dafsa_create();
    if (!rel->d) { free(rel); return NULL; }

    rel->base = rel->d;   /* EDB-only: base aliases view */
    rel->arity = arity;
    return rel;
}

relation *rel_open(const char *path, uint8_t arity)
{
    relation *rel;

    if (arity == 0 || arity > MAX_ARITY) return NULL;

    rel = calloc(1, sizeof(*rel));
    if (!rel) return NULL;

    rel->d = dafsa_load(path);
    if (!rel->d) {
        /* File doesn't exist or is corrupt — start fresh */
        rel->d = dafsa_create();
        if (!rel->d) { free(rel); return NULL; }
    }

    rel->base = rel->d;   /* EDB-only: base aliases view */
    rel->arity = arity;
    return rel;
}

int rel_save(const relation *rel, const char *path)
{
    if (!rel || !path) return -1;
    return dafsa_save(rel->d, path);
}

int rel_save_base(const relation *rel, const char *path)
{
    if (!rel || !path) return -1;
    return dafsa_save(rel->base, path);
}

void rel_free(relation *rel)
{
    if (!rel) return;
    if (rel->base != rel->d) dafsa_free(rel->base);
    dafsa_free(rel->d);
    if (rel->wal) dafsa_wal_close(rel->wal);
    free(rel->wal_path);
    free(rel);
}

uint8_t rel_arity(const relation *rel)
{
    return rel ? rel->arity : 0;
}

uint64_t rel_count(const relation *rel)
{
    dafsa_stats_out st;
    if (!rel || !rel->d) return 0;
    dafsa_stats(rel->d, &st);
    return (uint64_t)st.n_final;
}

/* ─── Order statistics (Tier-2) ────────────────────────────────────────── */

/* #distinct tuples strictly lexicographically < cols (O(1) amortized: the
 * subtree counts build lazily once).  Returns UINT64_MAX on error. */
uint64_t rel_rank(const relation *rel, const uint32_t *cols)
{
    unsigned char key[MAX_KEY_LEN];
    size_t key_len;

    if (!rel || !rel->d || !cols) return UINT64_MAX;
    key_len = encode_key(key, cols, rel->arity);
    return dafsa_rank_n(rel->d, key, key_len);
}

/* k-th tuple (0-indexed, lex order) into cols_out (arity columns).
 * Returns 0 on success, -1 if k >= count or on error. */
int rel_select(const relation *rel, uint64_t k, uint32_t *cols_out)
{
    unsigned char key[MAX_KEY_LEN];
    int key_len;

    if (!rel || !rel->d || !cols_out) return -1;
    key_len = dafsa_select_n(rel->d, k, key, MAX_KEY_LEN);
    if (key_len < 0) return -1;
    read_cols_be(cols_out, key, rel->arity);
    return 0;
}

/* #distinct tuples in the half-open range [lo, hi).  Returns UINT64_MAX on
 * error. */
uint64_t rel_range_count(const relation *rel, const uint32_t *lo,
                         const uint32_t *hi)
{
    unsigned char lo_key[MAX_KEY_LEN], hi_key[MAX_KEY_LEN];
    size_t lo_len, hi_len;

    if (!rel || !rel->d || !lo || !hi) return UINT64_MAX;
    lo_len = encode_key(lo_key, lo, rel->arity);
    hi_len = encode_key(hi_key, hi, rel->arity);
    return dafsa_range_count_n(rel->d, lo_key, lo_len, hi_key, hi_len);
}

/* O(1) distinct-tuple count via the memoized subtree array (builds lazily).
 * Returns 0 on error/OOM. */
uint64_t rel_count_subtree(const relation *rel)
{
    uint64_t n;

    if (!rel || !rel->d) return 0;
    n = dafsa_ensure_subtree(rel->d);
    if (!rel->d->subtree_valid) return 0;   /* OOM during build: degrade to 0 */
    return n;
}

/* ─── Prefix-bound order statistics (Tier-2 follow-up #1) ──────────────── */

/* Walk the k*4-byte prefix of `leading` (u32BE each) from the root via
 * trans_find to the DAFSA state representing that bound.  Returns the state
 * index, or -1 if the prefix is not a prefix of any key.  k must be <= arity
 * and (when k > 0) leading non-NULL — validated by the caller. */
static int rel_prefix_state(const relation *rel, const uint32_t *leading,
                            uint8_t k)
{
    unsigned int current;
    uint8_t i;

    if (rel->arity == 0) return -1;
    current = rel->d->initial;
    for (i = 0; i < k; i++) {
        unsigned char col_be[4];
        uint32_t v = leading[i];
        int b;
        col_be[0] = (unsigned char)((v >> 24) & 0xFF);
        col_be[1] = (unsigned char)((v >> 16) & 0xFF);
        col_be[2] = (unsigned char)((v >> 8)  & 0xFF);
        col_be[3] = (unsigned char)(v & 0xFF);
        for (b = 0; b < 4; b++) {
            int tr = trans_find(&rel->d->states[current], col_be[b]);
            if (tr < 0) return -1;   /* bound prefix not present in any key */
            current = trans_arr_c(&rel->d->states[current])[tr].target;
        }
    }
    return (int)current;
}

/* #distinct tuples with leading==`leading` (first k columns) AND full tuple
 * strictly lexicographically < cols (arity columns; first k must equal
 * leading).  If the leading prefix is absent (no tuple has it), returns 0.
 * UINT64_MAX on error. */
uint64_t rel_rank_bound(const relation *rel, const uint32_t *leading,
                        uint8_t k, const uint32_t *cols)
{
    unsigned char key[MAX_KEY_LEN];
    size_t key_len;
    int s;

    if (!rel || !rel->d || !cols) return UINT64_MAX;
    if (k > rel->arity) return UINT64_MAX;
    if (k > 0 && !leading) return UINT64_MAX;
    s = rel_prefix_state(rel, leading, k);
    if (s < 0) return 0;   /* bound absent: rank 0 within the (empty) bound */
    key_len = encode_key(key, &cols[k], (uint8_t)(rel->arity - k));
    return dafsa_rank_from(rel->d, (unsigned int)s, key, key_len);
}

/* idx-th tuple (0-indexed) among those with leading==`leading`, writing the
 * FULL tuple (arity columns; leading cols are copied through).  0 on success,
 * -1 if the bound is absent, idx >= the bound's count, or on error. */
int rel_select_bound(const relation *rel, const uint32_t *leading, uint8_t k,
                     uint64_t idx, uint32_t *cols_out)
{
    unsigned char key[MAX_KEY_LEN];
    int key_len;
    int s;

    if (!rel || !rel->d || !cols_out) return -1;
    if (k > rel->arity) return -1;
    if (k > 0 && !leading) return -1;
    s = rel_prefix_state(rel, leading, k);
    if (s < 0) return -1;   /* bound absent */
    key_len = dafsa_select_from(rel->d, (unsigned int)s, idx, key, MAX_KEY_LEN);
    if (key_len < 0) return -1;
    if (k > 0) memcpy(cols_out, leading, (size_t)k * sizeof(uint32_t));
    read_cols_be(&cols_out[k], key, (uint8_t)(rel->arity - k));
    return 0;
}

/* #distinct tuples with leading==`leading` AND full tuple in the half-open
 * range [lo, hi) (arity columns each; first k of lo/hi must equal leading).
 * If the leading prefix is absent, returns 0.  UINT64_MAX on error. */
uint64_t rel_range_count_bound(const relation *rel, const uint32_t *leading,
                               uint8_t k, const uint32_t *lo,
                               const uint32_t *hi)
{
    unsigned char lo_key[MAX_KEY_LEN], hi_key[MAX_KEY_LEN];
    size_t lo_len, hi_len;
    int s;

    if (!rel || !rel->d || !lo || !hi) return UINT64_MAX;
    if (k > rel->arity) return UINT64_MAX;
    if (k > 0 && !leading) return UINT64_MAX;
    s = rel_prefix_state(rel, leading, k);
    if (s < 0) return 0;   /* bound absent */
    lo_len = encode_key(lo_key, &lo[k], (uint8_t)(rel->arity - k));
    hi_len = encode_key(hi_key, &hi[k], (uint8_t)(rel->arity - k));
    return dafsa_range_count_from(rel->d, (unsigned int)s,
                                  lo_key, lo_len, hi_key, hi_len);
}

/* ─── Fact operations ─────────────────────────────────────────────────── */

static int rel_add_d(relation *rel, dafsa *d, const uint32_t *cols)
{
    unsigned char key[MAX_KEY_LEN];
    size_t key_len;

    if (!rel || !d || !cols) return -1;

    key_len = encode_key(key, cols, rel->arity);
    return dafsa_add_n(d, key, key_len);
}

int rel_add(relation *rel, const uint32_t *cols)
{
    return rel_add_d(rel, rel ? rel->d : NULL, cols);
}

int rel_add_base(relation *rel, const uint32_t *cols)
{
    return rel_add_d(rel, rel ? rel->base : NULL, cols);
}

static int rel_exact_d(const relation *rel, const dafsa *d, const uint32_t *cols)
{
    unsigned char key[MAX_KEY_LEN];
    size_t key_len;

    if (!rel || !d || !cols) return 0;

    key_len = encode_key(key, cols, rel->arity);
    return dafsa_lookup_n(d, key, key_len);
}

int rel_exact(const relation *rel, const uint32_t *cols)
{
    return rel_exact_d(rel, rel ? rel->d : NULL, cols);
}

int rel_exact_base(const relation *rel, const uint32_t *cols)
{
    return rel_exact_d(rel, rel ? rel->base : NULL, cols);
}

static int rel_delete_d(relation *rel, dafsa *d, const uint32_t *cols)
{
    unsigned char key[MAX_KEY_LEN];
    size_t key_len;

    if (!rel || !d || !cols) return -1;

    key_len = encode_key(key, cols, rel->arity);
    return dafsa_delete_n(d, key, key_len);
}

int rel_delete(relation *rel, const uint32_t *cols)
{
    return rel_delete_d(rel, rel ? rel->d : NULL, cols);
}

int rel_delete_base(relation *rel, const uint32_t *cols)
{
    return rel_delete_d(rel, rel ? rel->base : NULL, cols);
}

/* ─── Prefix enumeration ──────────────────────────────────────────────── */

/* Context passed through the DFS recursion */
struct prefix_ctx {
    const uint32_t *leading;   /* bound leading column values */
    uint8_t         k;         /* how many leading columns are bound */
    uint8_t         arity;     /* total arity of the relation */
    rel_enum_cb     cb;        /* user callback */
    void           *user;
    long            count;     /* running count */
};

/* Recursive DFS from `state`, accumulating bytes into `buf`.
 * At each final state, reconstruct the full tuple and call the user cb.
 * Returns non-zero to stop early (propagated from cb). */
static int prefix_dfs(const dafsa *d, unsigned int state,
                      unsigned char *buf, size_t depth,
                      struct prefix_ctx *ctx)
{
    const State *s = &d->states[state];
    uint32_t j;

    if (s->is_final) {
        /* Payload is `depth` bytes: the remaining columns + trailing \0.
         * Number of remaining columns = arity - k.
         * Expected payload length = (arity - k) * 4 + 1. */
        uint8_t n_rem = ctx->arity - ctx->k;
        uint32_t full_tuple[MAX_ARITY];
        uint8_t i;

        /* Copy leading columns */
        for (i = 0; i < ctx->k; i++)
            full_tuple[i] = ctx->leading[i];

        /* Parse remaining columns from buf (skip trailing \0) */
        if (n_rem > 0 && depth >= (size_t)n_rem * 4) {
            read_cols_be(&full_tuple[ctx->k], buf, n_rem);
        }

        ctx->count++;
        if (ctx->cb(full_tuple, ctx->arity, ctx->user) != 0)
            return 1;
    }

    if (depth >= MAX_KEY_LEN) return 0;

    for (j = 0; j < s->ntrans; j++) {
        const Edge *e = &trans_arr_c(s)[j];
        buf[depth] = e->sym;
        if (prefix_dfs(d, e->target, buf, depth + 1, ctx) != 0)
            return 1;
    }
    return 0;
}

static long rel_prefix_d(const relation *rel, const dafsa *d,
                         const uint32_t *leading, uint8_t k,
                         rel_enum_cb cb, void *user)
{
    unsigned int current;
    unsigned char buf[MAX_KEY_LEN];
    struct prefix_ctx ctx;
    uint8_t i;

    if (!rel || !d || !cb) return -1;
    if (k > rel->arity) return -1;
    if (k > 0 && !leading) return -1;

    /* Walk the k*4 prefix bytes */
    current = d->initial;
    for (i = 0; i < k; i++) {
        unsigned char col_be[4];
        int tr;

        /* Write single column as u32 BE */
        {
            uint32_t v = leading[i];
            col_be[0] = (unsigned char)((v >> 24) & 0xFF);
            col_be[1] = (unsigned char)((v >> 16) & 0xFF);
            col_be[2] = (unsigned char)((v >> 8)  & 0xFF);
            col_be[3] = (unsigned char)(v & 0xFF);
        }

        for (int b = 0; b < 4; b++) {
            tr = trans_find(&d->states[current], col_be[b]);
            if (tr < 0) return 0;  /* prefix not found */
            current = trans_arr_c(&d->states[current])[tr].target;
        }
    }

    /* Set up context and DFS from current state */
    ctx.leading = leading;
    ctx.k       = k;
    ctx.arity   = rel->arity;
    ctx.cb      = cb;
    ctx.user    = user;
    ctx.count   = 0;

    prefix_dfs(d, current, buf, 0, &ctx);
    return ctx.count;
}

long rel_prefix(const relation *rel,
                const uint32_t *leading, uint8_t k,
                rel_enum_cb cb, void *user)
{
    return rel_prefix_d(rel, rel ? rel->d : NULL, leading, k, cb, user);
}

long rel_prefix_base(const relation *rel,
                     const uint32_t *leading, uint8_t k,
                     rel_enum_cb cb, void *user)
{
    return rel_prefix_d(rel, rel ? rel->base : NULL, leading, k, cb, user);
}

/* ─── Bulk build from tuple set ──────────────────────────────────────── */

/* ts_sink_cb: rel_enum_cb-compatible callback that adds tuples to a tuple_set.
 * This is used to union existing DAFSA facts into a tuple_set before bulk
 * rebuilding from combined (existing + new) data. */
int ts_sink_cb(const uint32_t *cols, uint8_t arity, void *user)
{
    tuple_set *ts = (tuple_set *)user;
    (void)arity;
    return (ts_add(ts, cols) < 0) ? -1 : 0;
}

/* Build a fresh minimal DAFSA from a SORTED, DEDUPLICATED tuple_set.
 * Returns NULL on error/OOM. */
static dafsa *dafsa_build_from_ts(const struct tuple_set *ts, uint8_t arity)
{
    unsigned char **keys = NULL;
    size_t        *lens = NULL;
    dafsa         *new_d = NULL;
    long           n;
    long           i;

    n = ts->count;
    if (n == 0)
        return dafsa_create();

    keys = calloc((size_t)n, sizeof(unsigned char *));
    lens = calloc((size_t)n, sizeof(size_t));
    if (!keys || !lens) goto out;

    for (i = 0; i < n; i++) {
        const uint32_t *cols = ts->data + (size_t)i * ts->arity;
        unsigned char *key = malloc(MAX_KEY_LEN);
        if (!key) goto out;
        keys[i] = key;
        lens[i] = encode_key(key, cols, arity);
    }

    new_d = dafsa_build_sorted((const unsigned char *const *)keys,
                               (const size_t *)lens, (size_t)n);

out:
    if (keys) {
        for (i = 0; i < n; i++)
            free(keys[i]);
        free(keys);
    }
    free(lens);
    return new_d;
}

int rel_build_from_tupleset(relation *rel, const struct tuple_set *ts)
{
    dafsa *new_d;
    int    aliased;

    if (!rel || !ts) return -1;
    if (ts->arity != rel->arity) return -1;

    new_d = dafsa_build_from_ts(ts, rel->arity);
    if (!new_d) return -1;

    aliased = (rel->base == rel->d);
    dafsa_free(rel->d);
    rel->d = new_d;
    if (aliased) rel->base = rel->d;   /* keep base aliased for EDB rels */
    return 0;
}

int rel_build_base_from_tupleset(relation *rel, const struct tuple_set *ts)
{
    dafsa *new_b;
    int    aliased;

    if (!rel || !ts) return -1;
    if (ts->arity != rel->arity) return -1;

    new_b = dafsa_build_from_ts(ts, rel->arity);
    if (!new_b) return -1;

    aliased = (rel->base == rel->d);
    dafsa_free(rel->base);
    rel->base = new_b;
    if (aliased) rel->d = rel->base;   /* keep view aliased for EDB rels */
    return 0;
}

/* Copy the facts of `src` into a fresh minimal DAFSA (via enumeration +
 * bulk build). */
static dafsa *dafsa_copy_from(const relation *rel, const dafsa *src)
{
    tuple_set ts;
    dafsa *out = NULL;

    if (!rel || !src) return NULL;
    if (ts_init(&ts, rel->arity) != 0) return NULL;
    if (rel_prefix_d(rel, src, NULL, 0, ts_sink_cb, &ts) < 0) {
        ts_free(&ts);
        return NULL;
    }
    ts_sort(&ts);
    out = dafsa_build_from_ts(&ts, rel->arity);
    ts_free(&ts);
    return out;
}

/* ─── Base/view partition (IVM Slice 0) ────────────────────────────────── */

int rel_is_idb(const relation *rel)
{
    return rel ? (rel->base != rel->d) : 0;
}

int rel_reset_view(relation *rel)
{
    if (!rel) return -1;

    if (rel->base == rel->d) {
        /* First evaluation as a derived relation: SPLIT base from view.
         * At this point the view holds only base facts (no derivation has
         * run yet), so base = copy of view, and view already == base. */
        dafsa *nb = dafsa_copy_from(rel, rel->d);
        if (!nb) return -1;
        rel->base = nb;
        return 0;
    }

    /* Already split: drop stale derived facts — view = copy of base. */
    {
        dafsa *nv = dafsa_copy_from(rel, rel->base);
        if (!nv) return -1;
        dafsa_free(rel->d);
        rel->d = nv;
    }
    return 0;
}

/* ─── WAL operations (M7) ──────────────────────────────────────────────── */

/* WAL replay callback: apply ADD or DEL to the in-memory DAFSA.
 * Idempotent: ADD of existing = dup (returns 0, dafsa_add_n handles it),
 * DEL of absent = no-op (returns 0, dafsa_delete_n handles it). */
struct wal_replay_ctx {
    relation *rel;
    int       ok;  /* becomes -1 on first error */
};

static int wal_replay_cb(uint8_t op, const unsigned char *key,
                         uint32_t key_len, void *user)
{
    struct wal_replay_ctx *ctx = (struct wal_replay_ctx *)user;
    int rc;

    if (ctx->ok != 0) return 0;  /* already failed, skip */

    if (op == DAFSA_WAL_OP_ADD) {
        rc = dafsa_add_n(ctx->rel->base, key, key_len);
        if (rc < 0) ctx->ok = -1;
    } else if (op == DAFSA_WAL_OP_DEL) {
        rc = dafsa_delete_n(ctx->rel->base, key, key_len);
        if (rc < 0) ctx->ok = -1;
    }
    return 0;
}

int rel_wal_replay_into(relation *rel)
{
    struct wal_replay_ctx ctx;

    if (!rel || !rel->wal) return -1;

    ctx.rel = rel;
    ctx.ok  = 0;

    if (dafsa_wal_replay(rel->wal, wal_replay_cb, &ctx) != 0)
        return -1;

    return ctx.ok;
}

int rel_wal_append_add(relation *rel,
                       const unsigned char *key, uint32_t key_len)
{
    if (!rel || !rel->wal || !key) return -1;
    if (dafsa_wal_append_add(rel->wal, key, key_len) != 0) return -1;
    if (dafsa_wal_sync(rel->wal) != 0) return -1;
    return 0;
}

int rel_wal_append_del(relation *rel,
                       const unsigned char *key, uint32_t key_len)
{
    if (!rel || !rel->wal || !key) return -1;
    if (dafsa_wal_append_del(rel->wal, key, key_len) != 0) return -1;
    if (dafsa_wal_sync(rel->wal) != 0) return -1;
    return 0;
}

/* fsync helper: fsync the directory containing a path */
static int fsync_parent_dir(const char *path)
{
    const char *slash = strrchr(path, '/');
    char *dir;
    int fd, ret = -1;

    if (!slash || slash == path) {
        fd = open("/", O_RDONLY | O_DIRECTORY);
        if (fd >= 0) { ret = fsync(fd); close(fd); }
        return ret;
    }
    dir = strndup(path, (size_t)(slash - path));
    if (!dir) return -1;
    fd = open(dir, O_RDONLY | O_DIRECTORY);
    if (fd >= 0) { ret = fsync(fd); close(fd); }
    free(dir);
    return ret;
}

int rel_compact(relation *rel, const char *dafsa_path)
{
    if (!rel || !rel->wal || !dafsa_path) return -1;

    /* 1. Save the BASE DAFSA atomically (WAL records are base-only ops, so
     * the base DAFSA is their durable home; dafsa_save already does
     * tmp+fsync+rename+dir-fsync). */
    if (dafsa_save(rel->base, dafsa_path) != 0) return -1;

    /* 2. ftruncate WAL to 16 bytes (header-only) and fsync */
    if (ftruncate(rel->wal->fd, 16) != 0) return -1;
    if (fsync(rel->wal->fd) != 0) return -1;
    rel->wal->size = 16;

    /* 3. fsync the directory containing the WAL */
    if (fsync_parent_dir(rel->wal_path) != 0) return -1;

    return 0;
}

uint64_t rel_wal_size(const relation *rel)
{
    if (!rel || !rel->wal) return 0;
    return dafsa_wal_size(rel->wal);
}

uint64_t rel_dafsa_size(const relation *rel)
{
    dafsa_stats_out st;
    if (!rel || !rel->base) return 0;
    dafsa_stats(rel->base, &st);
    /* Return a rough byte estimate based on state/transition counts.
     * This is used for the 25% compaction threshold. */
    uint64_t est = (uint64_t)st.n_states_reachable * 64ULL  /* ~64B per State */
                 + (uint64_t)st.n_trans * 8ULL;             /* ~8B per transition */
    return est;
}

/* Open a relation with WAL: load base + replay + compact */
relation *rel_open_writable(const char *dafsa_path, const char *wal_path,
                            uint8_t arity)
{
    relation *rel;
    int wal_exists = 0;

    if (arity == 0 || arity > MAX_ARITY) return NULL;

    rel = calloc(1, sizeof(*rel));
    if (!rel) return NULL;

    /* Load base DAFSA */
    rel->d = dafsa_load(dafsa_path);
    if (!rel->d) {
        rel->d = dafsa_create();
        if (!rel->d) { free(rel); return NULL; }
    }
    rel->base = rel->d;   /* EDB-only: base aliases view */
    rel->arity = arity;

    /* Open WAL (rw) — auto-repairs torn tail */
    {
        struct stat st;
        wal_exists = (stat(wal_path, &st) == 0 && st.st_size > 16);
    }

    rel->wal = dafsa_wal_open_rw(wal_path);
    if (!rel->wal) { dafsa_free(rel->d); free(rel); return NULL; }

    rel->wal_path = strdup(wal_path);
    if (!rel->wal_path) { dafsa_wal_close(rel->wal); dafsa_free(rel->d); free(rel); return NULL; }

    /* If WAL had records > header, replay them into in-memory DAFSA */
    if (wal_exists) {
        if (rel_wal_replay_into(rel) != 0) {
            rel_free(rel);
            return NULL;
        }
        /* Compact immediately: save DAFSA + truncate WAL */
        if (rel_compact(rel, dafsa_path) != 0) {
            rel_free(rel);
            return NULL;
        }
    }

    return rel;
}

/* Open a rule-head (IDB) relation: base and view are SEPARATE DAFSAs.
 * WAL replays into base only; the view is left as-is (re-derived later by
 * the VM). */
relation *rel_open_writable_idb(const char *base_path, const char *dafsa_path,
                                const char *wal_path, uint8_t arity)
{
    relation *rel;
    int wal_exists = 0;

    if (arity == 0 || arity > MAX_ARITY) return NULL;

    rel = calloc(1, sizeof(*rel));
    if (!rel) return NULL;

    /* Load base DAFSA */
    rel->base = dafsa_load(base_path);
    if (!rel->base) {
        rel->base = dafsa_create();
        if (!rel->base) { free(rel); return NULL; }
    }

    /* Load view DAFSA (base ∪ derived, as last materialized) */
    rel->d = dafsa_load(dafsa_path);
    if (!rel->d) {
        rel->d = dafsa_create();
        if (!rel->d) { dafsa_free(rel->base); free(rel); return NULL; }
    }
    rel->arity = arity;

    /* Open WAL (rw) — auto-repairs torn tail */
    {
        struct stat st;
        wal_exists = (stat(wal_path, &st) == 0 && st.st_size > 16);
    }

    rel->wal = dafsa_wal_open_rw(wal_path);
    if (!rel->wal) {
        dafsa_free(rel->d); dafsa_free(rel->base); free(rel);
        return NULL;
    }

    rel->wal_path = strdup(wal_path);
    if (!rel->wal_path) {
        dafsa_wal_close(rel->wal);
        dafsa_free(rel->d); dafsa_free(rel->base); free(rel);
        return NULL;
    }

    /* Replay WAL into BASE and compact immediately. */
    if (wal_exists) {
        if (rel_wal_replay_into(rel) != 0) {
            rel_free(rel);
            return NULL;
        }
        if (rel_compact(rel, base_path) != 0) {
            rel_free(rel);
            return NULL;
        }
    }

    return rel;
}

/* ─── Regex pattern walk ──────────────────────────────────────────────── */

struct pat_ctx {
    uint8_t     arity;
    rel_enum_cb cb;
    void       *user;
    long        count;
};

static int pat_cb(const unsigned char *key_bytes, size_t key_len,
                  void *user)
{
    struct pat_ctx *ctx = (struct pat_ctx *)user;
    uint32_t cols[MAX_ARITY];
    uint8_t i;

    /* Key is 4*arity+1 bytes. Decode columns from buf. */
    if (key_len != (size_t)ctx->arity * 4 + 1) return 0;

    for (i = 0; i < ctx->arity; i++) {
        cols[i] = ((uint32_t)key_bytes[4*i]     << 24) |
                  ((uint32_t)key_bytes[4*i + 1] << 16) |
                  ((uint32_t)key_bytes[4*i + 2] << 8)  |
                  ((uint32_t)key_bytes[4*i + 3]);
    }

    ctx->count++;
    return ctx->cb(cols, ctx->arity, ctx->user);
}

long rel_pattern(const relation *rel, const struct regex_dfa *dfa,
                 rel_enum_cb cb, void *user)
{
    struct pat_ctx ctx;

    if (!rel || !dfa || !cb) return -1;

    ctx.arity  = rel->arity;
    ctx.cb     = cb;
    ctx.user   = user;
    ctx.count  = 0;

    long n = regex_dfa_walk(rel->d, dfa, pat_cb, &ctx);
    if (n < 0) return -1;
    return ctx.count;
}
