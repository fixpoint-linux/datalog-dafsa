/*
 * permindex.c — Permutation index builder for M6
 *
 * Builds perm-π DAFSA indices from base relation facts.
 * Each perm index re-encodes tuples: c_{π(0)} c_{π(1)} ... c_{π(a-1)} \0
 * so a join on original columns {π(0)..π(k-1)} becomes a leading-k prefix lookup.
 */

#include "permindex.h"
#include "dl.h"
#include "relation.h"
#include "tupleset.h"
#include "snapshot.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ─── Internal dl_db access (must match dl.c) ────────────────────────── */

struct dl_db_internal {
    char *dir; void *ir;
    struct { char *name; void *rel; } rels[64];
    size_t nrels;
    int _m7_lock_fd;            /* M7: fcntl lock fd (must match dl_db layout) */
    void *crules; int n_crules;
    int fixpoint_dirty; uint32_t snap_version;
    view_cache_slot vcache[DL_VIEW_CACHE_SZ];
    void *fault_hook; void *fault_user;
    perm_index_entry perms[MAX_PERMS];
    int n_perms;
};

static void *db_rel_raw(struct dl_db *db, int idx)
{
    struct dl_db_internal *d = (struct dl_db_internal *)db;
    if (idx < 0 || (size_t)idx >= d->nrels) return NULL;
    return d->rels[idx].rel;
}

/* ─── Build a single permutation index ────────────────────────────────── */

int permindex_build(struct dl_db *db, int rel_id, int perm_id)
{
    struct dl_db_internal *d = (struct dl_db_internal *)db;
    perm_index_entry *pe;
    relation *base_rel;
    tuple_set ts;
    uint8_t ar;

    if (!db || perm_id < 0 || perm_id >= d->n_perms) return -1;
    pe = &d->perms[perm_id];
    if (pe->rel_id != rel_id) return -1;

    base_rel = (relation *)db_rel_raw(db, rel_id);
    if (!base_rel) return -1;

    ar = pe->arity;

    /* Collect base facts */
    if (ts_init(&ts, ar) != 0) return -1;

    if (rel_prefix(base_rel, NULL, 0, ts_sink_cb, &ts) < 0) {
        ts_free(&ts);
        return -1;
    }

    if (ts.count == 0) {
        /* Empty relation: create empty perm index */
        if (pe->pidx_rel) rel_free(pe->pidx_rel);
        pe->pidx_rel = rel_create(ar);
        pe->dirty = 0;
        ts_free(&ts);
        return 0;
    }

    /* Re-encode each tuple via permutation */
    {
        tuple_set pts;
        long ci;

        if (ts_init(&pts, ar) != 0) {
            ts_free(&ts);
            return -1;
        }

        for (ci = 0; ci < ts.count; ci++) {
            const uint32_t *row = ts.data + (size_t)ci * (size_t)ar;
            uint32_t prow[8];
            int j;
            for (j = 0; j < (int)ar; j++)
                prow[j] = row[pe->perm[j]];
            ts_add(&pts, prow);
        }

        /* Sort and bulk-build minimal DAFSA */
        ts_sort(&pts);

        if (pe->pidx_rel)
            rel_free(pe->pidx_rel);

        pe->pidx_rel = rel_create(ar);
        if (!pe->pidx_rel) {
            ts_free(&pts); ts_free(&ts);
            return -1;
        }

        if (rel_build_from_tupleset(pe->pidx_rel, &pts) != 0) {
            rel_free(pe->pidx_rel);
            pe->pidx_rel = NULL;
            ts_free(&pts); ts_free(&ts);
            return -1;
        }

        ts_free(&pts);
    }

    ts_free(&ts);
    pe->dirty = 0;
    return 0;
}

/* ─── Build all dirty permutation indices ─────────────────────────────── */

int permindex_build_dirty(struct dl_db *db)
{
    struct dl_db_internal *d = (struct dl_db_internal *)db;
    int i;

    if (!db) return 0;

    for (i = 0; i < d->n_perms; i++) {
        if (d->perms[i].dirty) {
            if (permindex_build(db, d->perms[i].rel_id, i) != 0)
                return -1;
        }
    }
    return 0;
}

/* ─── Mark permutation indices dirty for a relation ─────────────────── */

void permindex_mark_dirty(struct dl_db *db, int rel_id)
{
    struct dl_db_internal *d = (struct dl_db_internal *)db;
    int pi;

    if (!db) return;

    for (pi = 0; pi < d->n_perms; pi++) {
        if (d->perms[pi].rel_id == rel_id)
            d->perms[pi].dirty = 1;
    }
}

/* ─── Free all permutation index relations ────────────────────────────── */

void permindex_free_all(struct dl_db *db)
{
    struct dl_db_internal *d = (struct dl_db_internal *)db;
    int i;

    if (!db) return;

    for (i = 0; i < d->n_perms; i++) {
        if (d->perms[i].pidx_rel) {
            rel_free(d->perms[i].pidx_rel);
            d->perms[i].pidx_rel = NULL;
        }
    }
}
