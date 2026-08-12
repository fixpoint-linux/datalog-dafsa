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

int manifest_find_rel(const char *sdir, const char *rel_name,
                             uint8_t *arity_out)
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
            int a = atoi(colon + 1);
            if (a >= 1 && a <= 8) {
                *arity_out = (uint8_t)a;
                found = 1;
                break;
            }
        }
    }

    free(line);
    fclose(f);
    return found;
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
    dafsa_view *v;

    /* Build snapshot dir path */
    snprintf(sdir, sizeof(sdir), "%s/snapshots/%u", db_dir, snap_version);

    /* Resolve arity from manifest */
    if (!manifest_find_rel(sdir, goal_rel, &arity))
        return -1;

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
