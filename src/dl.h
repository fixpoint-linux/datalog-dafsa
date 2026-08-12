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

/* Error codes for dl_open2 */
#define DL_E_LOCKED 1   /* database is locked by another writer */

/* Open with explicit error reporting.  On success, *err_out=0 and handle
 * is returned.  On failure, *err_out is set and NULL is returned.
 * err_out may be NULL (behaviour identical to dl_open). */
dl_db  *dl_open2(const char *dir, int *err_out);

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

/* ─── Incremental fact API (M7) ────────────────────────────────────────── */

/* Add a single fact to a relation.  cols has `arity` u32 values.
 * Returns 1 if added, 0 if duplicate, -1 on error.
 * Durable: WAL-appended + fsync'd before in-memory commit. */
int dl_add_fact(dl_db *db, const char *rel,
                const uint32_t *cols, uint8_t arity);

/* Delete a single fact from a relation.  cols has `arity` u32 values.
 * Returns 1 if deleted, 0 if absent, -1 on error.
 * Durable: WAL-appended + fsync'd before in-memory commit. */
int dl_delete_fact(dl_db *db, const char *rel,
                   const uint32_t *cols, uint8_t arity);

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
 * Returns the number of tuples emitted, or -1 on error.
 * If a snapshot has been published, reads from mmap'd snapshot (bypasses VM). */
long dl_query(dl_db *db, const char *goal_rel, dl_tuple_cb cb, void *user);

/* Prefix-bind k leading columns and enumerate via cb.
 * Reads from snapshot if published, else falls back to in-memory path. */
long dl_query_bound(dl_db *db, const char *goal_rel,
                    const uint32_t *leading, uint8_t k,
                    dl_tuple_cb cb, void *user);

/* ─── Regex pattern query ─────────────────────────────────────────────── */

/* Forward declaration (full struct in regexwalk.h) */
struct regex_dfa;

/* Enumerate all tuples in a relation whose full key matches the compiled
 * regex DFA.  Reads from snapshot if published, else falls back to
 * in-memory path.  Returns tuple count or -1 on error. */
long dl_pattern(dl_db *db, const char *rel, const struct regex_dfa *dfa,
                dl_tuple_cb cb, void *user);

/* ─── Snapshot publish (M4) ────────────────────────────────────────────── */

/* Atomic publish: saves interner + all relations to a versioned snapshot
 * directory, flips the CURRENT pointer.  Returns 0 on success, -1 on error.
 * After publish, dl_query reads from mmap instead of the VM. */
int dl_publish_snapshot(dl_db *db);

/* ─── Fault-injection hooks (test-only, NULL in production) ────────────── */

typedef enum {
    DL_FPOINT_AFTER_REL_SAVE,
    DL_FPOINT_AFTER_RENAME
} dl_fpoint;

/* Set a fault-injection hook.  At each fpoint during publish, if hook is
 * non-NULL it is called; non-zero return aborts the publish. */
void dl_set_fault_hook(dl_db *db,
                       int (*hook)(dl_fpoint fp, void *user),
                       void *user);

/* ─── Interner access (M7) ─────────────────────────────────────────────── */

/* Intern a string, return its sym_id (1-based).  Returns 0 on OOM. */
uint32_t    dl_intern_str(dl_db *db, const char *str);

/* Look up a sym_id → string.  Returns NULL if id is out of range. */
const char *dl_intern_str_of(dl_db *db, uint32_t sym_id);

#ifdef __cplusplus
}
#endif

#endif /* DL_H */
