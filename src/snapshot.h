/*
 * snapshot.h — Snapshot publish + mmap query path (M4)
 */

#ifndef SNAPSHOT_H
#define SNAPSHOT_H

#include "dl.h"
#include <stdint.h>

/* ─── View cache (lives inside dl_db) ─────────────────────────────────── */

#define DL_VIEW_CACHE_SZ 8

typedef struct {
    char         rel_name[64];
    void        *view;      /* dafsa_view* */
    int           used;     /* access marker for LRU eviction */
} view_cache_slot;

/* ─── Fault-injection hooks (test-only, NULL in production) ──────────── */

/* dl_fpoint enum is declared in dl.h */

/* ─── Internal helpers ────────────────────────────────────────────────── */

/* Read CURRENT version file; returns 0 if absent or unreadable. */
uint32_t snapshot_read_current(const char *db_dir);

/* Invalidate the view cache (close all views, zero array). */
void vcache_invalidate(view_cache_slot *vcache);

/* Query a published snapshot via mmap.
 * Returns tuple count or -1 on error. */
long snapshot_query_scan(const char *db_dir, uint32_t snap_version,
                         view_cache_slot *vcache,
                         const char *goal_rel,
                         const uint32_t *leading, uint8_t k,
                         dl_tuple_cb cb, void *user);

/* Low-level: prefix-enumerate via a dafsa_view handle.
 * Mirrors rel_prefix semantics exactly. */
long view_prefix(void *view_handle, uint8_t arity,
                 const uint32_t *leading, uint8_t k,
                 dl_tuple_cb cb, void *user);

/* Open or find a cached view.  Returns dafsa_view* or NULL. */
void *view_open_cached(view_cache_slot *vcache,
                       const char *rel_name, const char *sdir);

/* ─── Regex pattern walk ──────────────────────────────────────────────── */

/* Forward declaration (full struct in regexwalk.h) */
struct regex_dfa;

/* Walk a dafsa_view with a regex pattern, enumerating matching tuples.
 * Mirrors rel_pattern semantics (full-key match, decode u32BE columns). */
long view_pattern(void *view_handle, uint8_t arity,
                  const struct regex_dfa *dfa,
                  dl_tuple_cb cb, void *user);

/* ─── View order-statistics (rank/select/range_count/count) ───────────── */

/* Rank of a tuple (number of keys strictly lexicographically less than it)
 * in the view's total order.  Returns 0 on error. */
uint64_t view_rank(void *view_handle, uint8_t arity, const uint32_t *cols);

/* Select the k-th (0-indexed) key; decode into cols_out.  Returns 0 on
 * success, -1 on error (mirrors dl_select's 0/-1 contract). */
int view_select(void *view_handle, uint8_t arity, uint64_t k,
                uint32_t *cols_out);

/* Number of keys in the half-open range [lo, hi).  Returns 0 on error. */
uint64_t view_range_count(void *view_handle, uint8_t arity,
                          const uint32_t *lo, const uint32_t *hi);

/* Total number of keys in the view.  Returns 0 on error. */
uint64_t view_count(void *view_handle);

/* ─── Manifest helpers ────────────────────────────────────────────────── */

/* Find a relation in a snapshot's manifest.txt, return arity in *arity_out.
 * Returns 1 on success, 0 if not found, -1 on error. */
int manifest_find_rel(const char *sdir, const char *rel_name,
                      uint8_t *arity_out);

/* v2 extended lookup: also reports whether the entry is the VARIADIC
 * marker line ('name:*:...'), in which case *arity_out is 0 and the
 * relation is enumerated via manifest_find_variants.  *variadic_out may
 * be NULL (then behaves like manifest_find_rel and returns 0 for a
 * variadic entry). */
int manifest_find_rel_ex(const char *sdir, const char *rel_name,
                         uint8_t *arity_out, int *variadic_out);

/* v2: collect which arity variants of a variadic relation are present in
 * the manifest (per-variant lines 'name.<a>:<a>:edb|idb').
 * present[a] = 1 for each present variant a in 1..8; zeroed on entry. */
void manifest_find_variants(const char *sdir, const char *rel_name,
                            uint8_t present[9]);

#endif /* SNAPSHOT_H */
