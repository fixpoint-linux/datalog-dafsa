/*
 * snapshot.c — Snapshot mmap query path (M4)
 *
 * Implements:
 *   - view_prefix: zero-copy prefix enumeration via dafsa_view
 *   - snapshot_query_scan: resolve relation, open view, prefix-enumerate
 *   - view cache: small 8-slot LRU
 *   - snapshot_read_current
 *
 * dl_publish_snapshot lives in dl.c (needs full dl_db struct access).
 *
 * Uses dafsa_internal.h for view_trans_find + view_enum_dfs
 * (already exported non-static; precedent: relation.c line 19).
 */

#include "snapshot.h"
#include "dafsa.h"
#include "dafsa_internal.h"
#include "regexwalk.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>

/* ─── Constants ────────────────────────────────────────────────────────── */

#define MAX_ARITY     8
#define MAX_KEY_LEN   (MAX_ARITY * 4 + 1)  /* 33 */

/* ─── Byte-swap helpers (mirror relation.c:36-60) ─────────────────────── */

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

/* ─── view_prefix — the crux ───────────────────────────────────────────── */

struct vp_ctx {
    const uint32_t *leading;
    uint8_t         k;
    uint8_t         arity;
    dl_tuple_cb     cb;
    void           *user;
    long            count;
};

/* view_enum_dfs callback: reassemble full tuple and forward to user cb. */
static int vp_dfs_cb(const unsigned char *payload, size_t payload_len,
                     void *user)
{
    struct vp_ctx *ctx = (struct vp_ctx *)user;
    uint8_t n_rem = ctx->arity - ctx->k;
    uint32_t full_tuple[MAX_ARITY];
    uint8_t i;

    (void)payload_len;

    /* leading columns */
    for (i = 0; i < ctx->k; i++)
        full_tuple[i] = ctx->leading[i];

    /* remaining columns from payload (skip trailing \0) */
    if (n_rem > 0)
        read_cols_be(&full_tuple[ctx->k], payload, n_rem);

    if (ctx->cb(full_tuple, ctx->arity, ctx->user) != 0)
        return 1;
    return 0;
}

/* Walk k*4 prefix bytes via view_trans_find, then view_enum_dfs from the
 * resulting state.  Mirrors rel_prefix (relation.c:206-251).
 *
 * CRITICAL: does NOT call dafsa_view_prefix_enum — that function has the
 * W\0 trap (requires a 0x00 edge before DFS).  We call view_enum_dfs
 * directly from the prefix-walk endpoint, exactly as relation.c's
 * prefix_dfs does. */
long view_prefix(void *view_handle, uint8_t arity,
                 const uint32_t *leading, uint8_t k,
                 dl_tuple_cb cb, void *user)
{
    dafsa_view *v = (dafsa_view *)view_handle;
    uint32_t current;
    uint8_t i;
    unsigned char buf[MAX_KEY_LEN];
    struct vp_ctx ctx;

    if (!v || !cb) return -1;
    if (k > arity) return -1;
    if (k > 0 && !leading) return -1;
    if (arity > MAX_ARITY) return -1;

    current = v->initial;

    /* Walk the k*4 prefix bytes */
    for (i = 0; i < k; i++) {
        unsigned char col_be[4];
        int b;
        uint32_t vv = leading[i];
        col_be[0] = (unsigned char)((vv >> 24) & 0xFF);
        col_be[1] = (unsigned char)((vv >> 16) & 0xFF);
        col_be[2] = (unsigned char)((vv >> 8)  & 0xFF);
        col_be[3] = (unsigned char)(vv & 0xFF);

        for (b = 0; b < 4; b++) {
            uint32_t target;
            if (view_trans_find(v, current, col_be[b], &target) != 0)
                return 0;
            current = target;
        }
    }

    /* DFS from current state (NO W\0 — call view_enum_dfs directly) */
    ctx.leading = leading;
    ctx.k       = k;
    ctx.arity   = arity;
    ctx.cb      = cb;
    ctx.user    = user;
    ctx.count   = 0;

    view_enum_dfs(v, current, buf, 0, vp_dfs_cb, &ctx, &ctx.count);
    return ctx.count;
}

/* ─── snapshot_read_current ────────────────────────────────────────────── */

uint32_t snapshot_read_current(const char *db_dir)
{
    char path[4096];
    FILE *f;
    unsigned long v;

    snprintf(path, sizeof(path), "%s/snapshots/CURRENT", db_dir);
    f = fopen(path, "r");
    if (!f) return 0;

    if (fscanf(f, "%lu", &v) != 1 || v > 0xFFFFFFFFUL) {
        fclose(f);
        return 0;
    }
    fclose(f);
    return (uint32_t)v;
}

/* ─── View cache ───────────────────────────────────────────────────────── */

void vcache_invalidate(view_cache_slot *vcache)
{
    int i;
    for (i = 0; i < DL_VIEW_CACHE_SZ; i++) {
        if (vcache[i].view) {
            dafsa_view_close((dafsa_view *)vcache[i].view);
            vcache[i].view = NULL;
        }
        vcache[i].rel_name[0] = '\0';
        vcache[i].used = 0;
    }
}

/* Open or find a cached view. */
void *view_open_cached(view_cache_slot *vcache,
                                    const char *rel_name,
                                    const char *sdir)
{
    int i, lru_idx = 0, empty_idx = -1;
    int lru_used_val = 0x7FFFFFFF;

    for (i = 0; i < DL_VIEW_CACHE_SZ; i++) {
        if (vcache[i].view && strcmp(vcache[i].rel_name, rel_name) == 0) {
            vcache[i].used++;
            return (dafsa_view *)vcache[i].view;
        }
        if (!vcache[i].view && empty_idx < 0)
            empty_idx = i;
        if (vcache[i].used < lru_used_val) {
            lru_used_val = vcache[i].used;
            lru_idx = i;
        }
    }

    /* Not in cache — open from snapshot dir */
    {
        char path[8192];
        dafsa_view *v;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
        snprintf(path, sizeof(path), "%s/%s.dafsa", sdir, rel_name);
#pragma GCC diagnostic pop
        v = dafsa_view_open(path);
        if (!v) return NULL;

        if (empty_idx >= 0) {
            i = empty_idx;
        } else {
            i = lru_idx;
            if (vcache[i].view)
                dafsa_view_close((dafsa_view *)vcache[i].view);
        }

        vcache[i].view  = v;
        vcache[i].used  = 1;
        strncpy(vcache[i].rel_name, rel_name, sizeof(vcache[i].rel_name) - 1);
        vcache[i].rel_name[sizeof(vcache[i].rel_name) - 1] = '\0';
        return v;
    }
}

/* ─── manifest_find_rel ────────────────────────────────────────────────── */

int manifest_find_rel_ex(const char *sdir, const char *rel_name,
                         uint8_t *arity_out, int *variadic_out)
{
    char path[8192];
    FILE *f;
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    int found = 0;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
    snprintf(path, sizeof(path), "%s/manifest.txt", sdir);
#pragma GCC diagnostic pop
    f = fopen(path, "r");
    if (!f) return -1;

    while ((len = getline(&line, &cap, f)) > 0) {
        char *colon;
        if (len > 0 && line[len-1] == '\n') line[--len] = '\0';
        if (len > 0 && line[len-1] == '\r') line[--len] = '\0';

        if (line[0] == '#' || line[0] == 'D') continue;

        colon = strchr(line, ':');
        if (!colon) continue;
        *colon = '\0';

        if (strcmp(line, rel_name) == 0) {
            char *rest = colon + 1;
            if (rest[0] == '*') {
                /* v2 variadic marker: 'name:*:edb|idb'. */
                if (variadic_out) {
                    *variadic_out = 1;
                    *arity_out = 0;
                    found = 1;
                    break;
                }
                /* Legacy caller semantics: not found as a fixed relation. */
                continue;
            }
            {
                int a = atoi(rest);
                if (a >= 1 && a <= 8) {
                    *arity_out = (uint8_t)a;
                    if (variadic_out) *variadic_out = 0;
                    found = 1;
                    break;
                }
            }
        }
    }

    /* Not the plain name: a variadic caller detects the '*' marker above;
     * a per-variant 'name.<a>' line never matches the plain goal name
     * because strcmp compares the whole left side. */
    free(line);
    fclose(f);
    return found;
}

int manifest_find_rel(const char *sdir, const char *rel_name,
                      uint8_t *arity_out)
{
    return manifest_find_rel_ex(sdir, rel_name, arity_out, NULL);
}

/* v2: scan the manifest for per-variant lines 'name.<a>:<a>:...' of the
 * variadic relation `rel_name` and mark present[a]=1. */
void manifest_find_variants(const char *sdir, const char *rel_name,
                            uint8_t present[9])
{
    char path[8192];
    FILE *f;
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    size_t nlen = strlen(rel_name);

    memset(present, 0, 9);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
    snprintf(path, sizeof(path), "%s/manifest.txt", sdir);
#pragma GCC diagnostic pop
    f = fopen(path, "r");
    if (!f) return;

    while ((len = getline(&line, &cap, f)) > 0) {
        char *colon;
        if (len > 0 && line[len-1] == '\n') line[--len] = '\0';
        if (len > 0 && line[len-1] == '\r') line[--len] = '\0';

        if (line[0] == '#' || line[0] == 'D') continue;

        colon = strchr(line, ':');
        if (!colon) continue;
        *colon = '\0';

        /* Match '<rel_name>.<d>' with d a single digit 1..8. */
        if ((size_t)(colon - line) == nlen + 2 &&
            strncmp(line, rel_name, nlen) == 0 &&
            line[nlen] == '.' &&
            line[nlen + 1] >= '1' && line[nlen + 1] <= '8') {
            int a = line[nlen + 1] - '0';
            int parsed = atoi(colon + 1);
            if (parsed == a)   /* sanity: variant line carries its arity */
                present[a] = 1;
        }
    }

    free(line);
    fclose(f);
}

/* ─── snapshot_query_scan ──────────────────────────────────────────────── */

long snapshot_query_scan(const char *db_dir, uint32_t snap_version,
                         view_cache_slot *vcache,
                         const char *goal_rel,
                         const uint32_t *leading, uint8_t k,
                         dl_tuple_cb cb, void *user)
{
    char sdir[8192];
    uint8_t arity = 0;
    int variadic = 0;
    dafsa_view *v;

    /* Build snapshot dir path */
    snprintf(sdir, sizeof(sdir), "%s/snapshots/%u", db_dir, snap_version);

    /* Resolve arity (and variadic-ness) from the manifest */
    if (!manifest_find_rel_ex(sdir, goal_rel, &arity, &variadic))
        return -1;

    if (variadic) {
        /* v2: prefix enumeration fanned out over every PRESENT variant
         * a >= max(k,1) — each variant view walk is the existing mmap'd
         * fixed-width prefix walk (view_prefix); the cb's arity parameter
         * disambiguates tuples across arities. */
        uint8_t present[MAX_ARITY + 1];
        long total = 0;
        uint8_t a;

        if (k > MAX_ARITY) return -1;
        if (k > 0 && !leading) return -1;

        manifest_find_variants(sdir, goal_rel, present);
        for (a = (k > 0 ? k : 1); a <= MAX_ARITY; a++) {
            char vname[384];
            long n;
            if (!present[a]) continue;
            if (snprintf(vname, sizeof(vname), "%s.%d",
                         goal_rel, (int)a) >= (int)sizeof(vname))
                return -1;
            v = view_open_cached(vcache, vname, sdir);
            if (!v) return -1;
            n = view_prefix(v, a, leading, k, cb, user);
            if (n < 0) return -1;
            total += n;
        }
        return total;
    }

    if (k > arity) return -1;
    if (k > 0 && !leading) return -1;

    /* Open view (from cache if warm) */
    v = view_open_cached(vcache, goal_rel, sdir);
    if (!v) return -1;

    return view_prefix(v, arity, leading, k, cb, user);
}

/* ─── Regex pattern walk (mmap view) ──────────────────────────────────── */

struct vpat_ctx {
    uint8_t     arity;
    dl_tuple_cb cb;
    void       *user;
    long        count;
};

static int vpat_cb(const unsigned char *key_bytes, size_t key_len,
                   void *user)
{
    struct vpat_ctx *ctx = (struct vpat_ctx *)user;
    uint32_t cols[MAX_ARITY];
    uint8_t i;

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

long view_pattern(void *view_handle, uint8_t arity,
                  const struct regex_dfa *dfa,
                  dl_tuple_cb cb, void *user)
{
    dafsa_view *v = (dafsa_view *)view_handle;
    struct vpat_ctx ctx;

    if (!v || !dfa || !cb) return -1;

    ctx.arity = arity;
    ctx.cb    = cb;
    ctx.user  = user;
    ctx.count = 0;

    long n = regex_dfa_walk_view(v, dfa, vpat_cb, &ctx);
    if (n < 0) return -1;
    return ctx.count;
}

/* ─── View order-statistics (rank/select/range_count/count) ───────────── */

/* Encode arity u32 cols as u32BE + trailing 0x00 (4*arity+1 bytes),
 * identical to relation.c encode_key. */
static size_t view_encode_key(unsigned char *buf, const uint32_t *cols,
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
    buf[4 * arity] = 0x00;
    return (size_t)(4 * arity + 1);
}

/* Rank of a tuple in the view's total order: the number of keys strictly
 * lexicographically less than `cols`. */
uint64_t view_rank(void *view_handle, uint8_t arity, const uint32_t *cols)
{
    dafsa_view *v = (dafsa_view *)view_handle;
    unsigned char key[MAX_KEY_LEN];
    size_t len;

    if (!v || !cols) return 0;
    if (arity > MAX_ARITY) return 0;
    len = view_encode_key(key, cols, arity);
    return dafsa_view_rank_n(v, key, len);
}

/* Select the k-th (0-indexed) key in the view's total order, decoding its
 * u32 columns into cols_out.  Returns 0 on success, or -1 if k is out of
 * range / on OOM (mirrors dl_select's 0/-1 contract). */
int view_select(void *view_handle, uint8_t arity, uint64_t k,
                uint32_t *cols_out)
{
    dafsa_view *v = (dafsa_view *)view_handle;
    unsigned char key[MAX_KEY_LEN];
    int n;

    if (!v || !cols_out) return -1;
    if (arity > MAX_ARITY) return -1;
    n = dafsa_view_select_n(v, k, key, sizeof(key));
    if (n < 0) return -1;
    read_cols_be(cols_out, key, arity);
    return 0;
}

/* Number of keys in the half-open range [lo, hi). */
uint64_t view_range_count(void *view_handle, uint8_t arity,
                          const uint32_t *lo, const uint32_t *hi)
{
    dafsa_view *v = (dafsa_view *)view_handle;
    unsigned char lo_key[MAX_KEY_LEN], hi_key[MAX_KEY_LEN];
    size_t lo_len, hi_len;

    if (!v || !lo || !hi) return 0;
    if (arity > MAX_ARITY) return 0;
    lo_len = view_encode_key(lo_key, lo, arity);
    hi_len = view_encode_key(hi_key, hi, arity);
    return dafsa_view_range_count_n(v, lo_key, lo_len, hi_key, hi_len);
}

/* Total number of keys in the view (subtree count of the root). */
uint64_t view_count(void *view_handle)
{
    dafsa_view *v = (dafsa_view *)view_handle;
    uint64_t *counts = NULL;
    uint64_t n;

    if (!v) return 0;
    n = dafsa_view_subtree_counts(v, &counts);
    free(counts);
    return n;
}
