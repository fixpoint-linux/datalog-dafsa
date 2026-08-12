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

/* ─── Manifest helpers ────────────────────────────────────────────────── */

/* Find a relation in a snapshot's manifest.txt, return arity in *arity_out.
 * Returns 1 on success, 0 if not found, -1 on error. */
int manifest_find_rel(const char *sdir, const char *rel_name,
                      uint8_t *arity_out);

#endif /* SNAPSHOT_H */
