/*
 * dl.h — Public C API for the DAFSA-backed Datalog VM (M7 complete)
 *
 * M7 scope:
 *   fact store + interner
 *   rule parser / compiler / VM
 *   aggregates, equality, disjunction
 *   snapshot publish + mmap query path
 *   regex / pattern walker
 *   permutation indices + hash-join
 *   durability (lock, WAL, incremental add/delete)
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
 * same arity it is a no-op; if arity mismatches or arity > 8, returns -1.
 *
 * v2 VARIABLE ARITY: passing arity == 0 declares a VARIADIC relation
 * (equivalent to dl_declare_relation_variadic below) — facts of any arity
 * 1..8 may be added, each arity stored as its own fixed-width DAFSA
 * variant (<name>.<a>.dafsa).  Arity 0 was an error before v2, so no
 * existing program changes meaning. */
int dl_declare_relation(dl_db *db, const char *name, uint8_t arity);

/* Declare a VARIADIC relation: facts of any arity 1..8 are accepted
 * (dl_add_fact / dl_load_facts route by each fact's own arity; rule atoms
 * resolve to the variant matching their syntactic argument count).
 * Storage is per-arity fixed-width — the fixed-width key encoding is
 * unchanged, and this is a strict on-disk superset (rels.txt gains a
 * 'name:*:edb|idb' marker line that older binaries simply skip).
 * Idempotent on an existing variadic relation; -1 if the name exists as a
 * fixed-arity relation (or vice versa).  Mixed with rules: variadic heads
 * must be declared BEFORE dl_load_rules; aggregates over a variadic
 * relation and recursive variadic heads are compile errors; programs
 * containing a variadic relation are excluded from incremental
 * maintenance (IVM/DRed/aggregates) and magic-sets queries — they always
 * evaluate via the full fixpoint (never silently mis-evaluated). */
int dl_declare_relation_variadic(dl_db *db, const char *name);

/* ─── Fact loading ────────────────────────────────────────────────────── */

/* Load ground facts from a headerless CSV file into a declared relation.
 * Values: quoted strings (interned) or bare integers (stored raw as u32).
 * Returns number of facts loaded, or -1 on error.
 *
 * v2 VARIABLE ARITY: for a VARIADIC relation the CSV rows may have
 * VARIABLE WIDTH — each row's field count (1..8) IS that fact's arity, and
 * it is routed to the matching variant.  Rows with 0 or >8 fields are
 * skipped.  Returns the total facts loaded across all arities. */
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

/* ─── Order statistics (Tier-2) ───────────────────────────────────────── */

/* Number of distinct tuples strictly lexicographically smaller than `cols`.
 * `cols` has `arity` u32 values; for a fixed-arity relation `arity` must equal
 * the declared arity.  u32BE key encoding => numeric order == lex order.
 * Returns the rank, or UINT64_MAX on error (NULL db/rel/cols, unknown rel,
 * arity mismatch, or a VARIADIC relation — variadic is rejected in this slice).
 * Operates on the in-memory relation (rel->d), NOT a published snapshot; when a
 * snapshot has been published the result reflects the current in-memory view
 * (the snapshot mmap path is not covered here). */
uint64_t dl_rank(dl_db *db, const char *rel, const uint32_t *cols, uint8_t arity);

/* The k-th tuple (0-indexed, lex order) written into cols_out (`arity` u32
 * values).  Returns 0 on success, -1 on error (NULL/unknown rel, arity
 * mismatch, VARIADIC relation) or if k >= the number of tuples. */
int dl_select(dl_db *db, const char *rel, uint64_t k,
              uint32_t *cols_out, uint8_t arity);

/* Number of distinct tuples in the half-open range [lo, hi), i.e.
 * rank(hi) - rank(lo).  lo and hi each have `arity` u32 values.  Returns the
 * count, or UINT64_MAX on error (as dl_rank). */
uint64_t dl_range_count(dl_db *db, const char *rel, const uint32_t *lo,
                        const uint32_t *hi, uint8_t arity);

/* O(1) distinct-tuple count for a fixed-arity relation (memoized subtree
 * array).  Returns the count, or UINT64_MAX on error (NULL/unknown rel,
 * VARIADIC relation), or 0 for an empty relation. */
uint64_t dl_count(dl_db *db, const char *rel);

/* ─── Prefix-bound order statistics (Tier-2 follow-up #1) ──────────────── */

/* The *_bound family restricts to tuples whose first k columns equal the
 * `leading` bound, then applies rank/select/range_count to the suffix (the
 * remaining arity-k columns).  `cols`/`lo`/`hi` are FULL arity-length tuples
 * (their first k entries must equal `leading`, which is what the bound is
 * applied to).  k==0 means no bound.
 * Semantics otherwise mirror dl_rank/dl_select/dl_range_count; error handling
 * (NULL/unknown rel, arity mismatch, VARIADIC) is identical. */

/* #distinct tuples with leading==`leading` AND full tuple strictly < cols.
 * UINT64_MAX on error; 0 if the leading prefix is absent from the relation. */
uint64_t dl_rank_bound(dl_db *db, const char *rel, const uint32_t *leading,
                       uint8_t k, const uint32_t *cols, uint8_t arity);

/* idx-th tuple (0-indexed) among those with leading==`leading`, writing the
 * FULL arity-length tuple into cols_out.  0 on success; -1 on error, absent
 * leading prefix, or idx >= the bound's tuple count. */
int dl_select_bound(dl_db *db, const char *rel, const uint32_t *leading,
                    uint8_t k, uint64_t idx, uint32_t *cols_out,
                    uint8_t arity);

/* #distinct tuples with leading==`leading` AND full tuple in the half-open
 * range [lo, hi).  UINT64_MAX on error; 0 if the leading prefix is absent. */
uint64_t dl_range_count_bound(dl_db *db, const char *rel,
                              const uint32_t *leading, uint8_t k,
                              const uint32_t *lo, const uint32_t *hi,
                              uint8_t arity);

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

/* Magic-sets bound query (M8, opt-in, first slice).
 *
 * Re-evaluates a SCOPED fixpoint seeded by the bound leading args
 * leading[0..k-1], materializing only the reachable IDB slice, streaming
 * the result, and leaving `db` completely unmutated.
 *
 * Invariant: the result is byte-for-byte identical to
 * dl_query_bound(db, goal_rel, leading, k, cb, user) over the fully
 * materialized goal relation.  Returns tuple count, or -1 on error.
 *
 * Semantics of edge cases:
 *   - k == 0: routes to dl_query (full materialization) — no bound args.
 *   - goal_rel is an EDB relation (not a rule head): degenerates to a
 *     plain prefix lookup (equivalent to dl_query_bound).
 *   - programs using negation, aggregates, or cross-predicate mutual
 *     recursion are REJECTED with a diagnostic and return -1 (never silently
 *     mis-evaluated).
 *
 * NOTE: reads db->rels[].rel without a lock — safe only when no concurrent
 * publish is in flight (documented single-writer / multi-reader model). */
long dl_query_magic(dl_db *db, const char *goal_rel,
                    const uint32_t *leading, uint8_t k,
                    dl_tuple_cb cb, void *user);

/* Magic-sets bound query with an ARBITRARY adornment (M8, non-leading slice).
 *
 * Binds the goal relation on an arbitrary subset of positions described by
 * `adorn`: a NUL-terminated string of exactly goal_arity chars, each 'b'
 * (bound) or 'f' (free) — e.g. "fb" binds position 1 of an arity-2 goal,
 * "bfb" binds position 1 of an arity-3 goal.  `vals` holds the bound values
 * packed in left-to-right bound-position order (exactly matching the 'b'
 * positions of `adorn`); `nvals` is their count.
 *
 * Invariant: the result is byte-for-byte identical to filtering the full
 * materialization of the goal (dl_query) on the bound positions.  Returns
 * tuple count, or -1 on error (never silently mis-evaluated).
 *
 * Semantics / rejection list (all return -1):
 *   - NULL db / goal_rel / adorn / cb.
 *   - goal_rel not found in db.
 *   - strlen(adorn) != goal arity.
 *   - adorn contains chars other than 'b' / 'f'.
 *   - nvals != count of 'b' in adorn.
 *   - nvals == 0 (all-free adorn): ROUTES to dl_query — no bound args.
 *   - goal_rel is an EDB relation (not a rule head): ROUTES to a direct
 *     full-scan + per-position filter (no transform).
 *   - the same conservative magic-transform rejects as dl_query_magic
 *     (negation on an adorned-closure predicate, aggregates over one,
 *     cross-predicate mutual recursion, adornment-closure blow-up,
 *     MAX_RELS overflow).  Multiple distinct adornments of one predicate
 *     (e.g. tc^bf and tc^bb) are now SUPPORTED via the adornment-closure
 *     fixpoint; each is a distinct relation.
 *
 * dl_query_magic (leading/k) is a shorthand shim that synthesizes
 * adorn = "b"*k ++ "f"*(arity-k) and routes here. */
long dl_query_magic_adorn(dl_db *db, const char *goal_rel,
                          const char *adorn, const uint32_t *vals, uint8_t nvals,
                          dl_tuple_cb cb, void *user);

/* Top-down / QSQ bound query (opt-in 4th per-query path).
 *
 * Evaluates the SAME adorned + magic program as dl_query_magic, but scheduled
 * DEMAND-DRIVEN (SLG worklist over subqueries) instead of as a forward
 * semi-naive fixpoint.  Invariant: the result is byte-for-byte identical to
 * dl_query_magic / dl_query_magic_adorn (and to filtering the full
 * materialization of the goal on the bound positions).  Returns tuple count,
 * or -1 on error (same rejection list as the magic path — variadic, list
 * builtins, negation on a closure predicate, aggregates over one,
 * cross-predicate mutual recursion, adornment-closure blow-up).  Leaves `db`
 * unmutated.  Not a speedup over forward magic for dense closures (both are
 * scoped evaluations of the same program); it is the architecture-of-record
 * for future selective-query / early-termination workloads. */
long dl_query_topdown(dl_db *db, const char *goal_rel,
                      const uint32_t *leading, uint8_t k,
                      dl_tuple_cb cb, void *user);

/* Top-down/QSQ with an ARBITRARY adornment (mirrors dl_query_magic_adorn's
 * signature and rejection semantics; `vals` are packed in left-to-right
 * bound-position order).  Routes all-free adornments to dl_query and EDB
 * goals to a direct full-scan + per-position filter, exactly like the magic
 * path. */
long dl_query_topdown_adorn(dl_db *db, const char *goal_rel,
                            const char *adorn, const uint32_t *vals,
                            uint8_t nvals, dl_tuple_cb cb, void *user);

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
