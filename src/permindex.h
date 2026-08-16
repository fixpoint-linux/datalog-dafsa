/*
 * permindex.h — Internal permutation-index structures for M6
 *
 * A perm-π index of relation R (arity a) is a separate DAFSA whose keys
 * are R's facts re-encoded in column order c_{π(0)} c_{π(1)} ... c_{π(a-1)} \0.
 * A join on original columns {π(0)..π(k-1)} becomes a leading-k prefix lookup.
 *
 * Stored on disk as <db>/<rel>__PI<hex_id>__.dafsa.
 * Snapshot: <snap>/<rel>__PI<hex_id>__.dafsa + manifest line.
 */

#ifndef PERMINDEX_H
#define PERMINDEX_H

#include <stdint.h>

/* Forward declarations */
struct dl_db;
struct relation;

/* Maximum permutation indices per relation (cap ≤8) */
#define MAX_PERMS_PER_REL 8
#define MAX_PERMS        64

typedef struct {
    int             rel_id;
    uint8_t         arity;
    uint8_t         perm[8];     /* permutation: prow[j] = row[perm[j]] — i.e.
                                    perm[j] is the ORIGINAL column that appears
                                    at permuted position j */
    struct relation *pidx_rel;   /* the permuted DAFSA */
    int             dirty;       /* 1 if needs rebuild */
} perm_index_entry;

/* Declare a permutation index for relation rel_id.
 * Returns perm_id (existing if dup), or -1 on table-full or arity overflow. */
int dl_db_declare_perm(struct dl_db *db, int rel_id, uint8_t arity,
                       const uint8_t perm[8]);

/* Return the perm array for a given rel_id + perm_id, or NULL. */
const uint8_t *dl_db_get_perm(struct dl_db *db, int rel_id, int perm_id);

/* Return the permuted relation DAFSA for a given rel_id + perm_id, or NULL. */
struct relation *dl_db_get_perm_rel(struct dl_db *db, int rel_id, int perm_id);

/* Build a single permutation index from the base relation. Returns 0 on success. */
int permindex_build(struct dl_db *db, int rel_id, int perm_id);

/* Build all dirty permutation indices. Returns 0 on success (or if no dirty). */
int permindex_build_dirty(struct dl_db *db);

/* Mark all permutation indices for a relation as dirty. */
void permindex_mark_dirty(struct dl_db *db, int rel_id);

/* Free all permutation index relations (for dl_close). */
void permindex_free_all(struct dl_db *db);

#endif /* PERMINDEX_H */
