/*
 * dl.h — Public C API for the DAFSA-backed Datalog VM (M0: fact store + interner)
 *
 * M0 scope:
 *   dl_open / dl_close
 *   dl_declare_relation
 *   dl_load_facts
 *   dl_lookup
 *   dl_prefix
 */
#ifndef DL_H
#define DL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Opaque handle ──────────────────────────────────────────────────── */

typedef struct dl_db dl_db;

/* ─── Lifecycle ───────────────────────────────────────────────────────── */

/* Open or create a database directory.  Returns NULL on error. */
dl_db  *dl_open(const char *dir);
void    dl_close(dl_db *db);

/* ─── Schema ──────────────────────────────────────────────────────────── */

/* Declare a relation with a fixed arity (1-8).  Creates an empty DAFSA
 * for the relation.  Idempotent: if the relation already exists with the
 * same arity it is a no-op; if arity mismatches or arity > 8, returns -1. */
int dl_declare_relation(dl_db *db, const char *name, uint8_t arity);

/* ─── Fact loading ────────────────────────────────────────────────────── */

/* Load ground facts from a headerless CSV file into a declared relation.
 * Values: quoted strings (interned) or bare integers (stored raw as u32).
 * Returns number of facts loaded, or -1 on error. */
int dl_load_facts(dl_db *db, const char *rel, const char *csv_path);

/* ─── Query primitives ────────────────────────────────────────────────── */

/* Exact lookup: returns 1 if the fact (cols[0..arity-1]) exists, else 0.
 * cols are u32 values (symbol ids for interned strings, raw for ints). */
int dl_lookup(dl_db *db, const char *rel,
              const uint32_t *cols, uint8_t arity);

/* Tuple enumeration callback.  Return non-zero to stop early.
 * cols has arity entries; user is the opaque pointer passed to dl_prefix. */
typedef int (*dl_tuple_cb)(const uint32_t *cols, uint8_t arity, void *user);

/* Prefix enumeration: bind the first k columns to `leading` values and
 * enumerate all matching complete tuples via cb.  Returns the count of
 * tuples visited, or -1 on error. */
long dl_prefix(dl_db *db, const char *rel,
               const uint32_t *leading, uint8_t k,
               dl_tuple_cb cb, void *user);

/* ─── Rule loading & compilation (M1) ──────────────────────────────────── */

/* Parse and compile Datalog rules from a source string.
 * Rules may reference existing relations (must match arity) or declare
 * new derived relations.  Returns 0 on success, -1 on error (message to
 * stderr).  The compiled program is stored internally; subsequent calls
 * to dl_compile append to it. */
int dl_load_rules(dl_db *db, const char *dl_source);

/* Compile all previously loaded rules into bytecode and run them against
 * the database, materializing derived relations in-place.
 * Returns 0 on success, -1 on error. */
int dl_compile(dl_db *db);

/* Evaluate all compiled rules and stream the goal relation's tuples via cb.
 * Returns the number of tuples emitted, or -1 on error. */
long dl_query(dl_db *db, const char *goal_rel, dl_tuple_cb cb, void *user);

#ifdef __cplusplus
}
#endif

#endif /* DL_H */
