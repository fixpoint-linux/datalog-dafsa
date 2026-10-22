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

/* Typed schema for the rule typechecker (full definition in schema.h,
 * self-contained).  Forward-declared here so dl_attach_schema below can take
 * a pointer without dragging schema.h into this public header. */
typedef struct dl_schema dl_schema;

/* ─── Lifecycle ───────────────────────────────────────────────────────── */

/* Open or create a database directory.  Returns NULL on error. */
dl_db  *dl_open(const char *dir);
void    dl_close(dl_db *db);

/* Error codes for dl_open2 */
#define DL_E_LOCKED   1   /* database is locked by another writer */
#define DL_E_CONFLICT 2   /* CAS: expected revision != current revision */

/* Distinct code the rule-evaluation family surfaces when a rule body needs
 * more simultaneous join frames than the interpreter provides (8: one frame
 * per enumerating body atom — SCAN/LOOKUP/LOOKUP_PERM/HASH_JOIN/WALK/
 * MAT_JOIN/LIST_MEMBER/RANGE).  Wherever a contract below says "-1 on
 * error", the frame-limit overflow returns DL_E_FRAMES INSTEAD, with a
 * diagnostic naming the rule and the atom on stderr — so a caller can tell
 * it apart from every other failure.  Data-dependent by nature (a frame is
 * only live while its atom's loop sits on the backtracking stack), so the
 * compile step cannot reject it; the same program may still answer
 * correctly on data too small to reach the limit.  The incremental
 * maintenance ladder (dl_consolidate's aggregate/delete/delta branches)
 * masks it as -1 on that call: those branches retry as a full re-eval, and
 * the retry surfaces DL_E_FRAMES. */
#define DL_E_FRAMES   (-2)

/* Open with explicit error reporting.  On success, *err_out=0 and handle
 * is returned.  On failure, *err_out is set and NULL is returned.
 * err_out may be NULL (behaviour identical to dl_open). */
dl_db  *dl_open2(const char *dir, int *err_out);

/* Open an EXISTING database READ-ONLY.  A read-only handle COEXISTS with a
 * live writer in another process (see "CONCURRENCY MODEL" below).
 *
 * The directory must already be a database (it is NEVER created): *err_out
 * is -1 unless <dir> exists and contains rels.txt or symbols.dafsa AND the
 * <dir>/LOCK file exists (dl_open_ro never creates files, LOCK included;
 * every database directory created by dl_open/dl_open2 has one).
 *
 * A read-only handle never writes: no mkdir, no LOCK creation, per-relation
 * and txn WALs are replayed IN MEMORY only (no compaction, no truncation,
 * torn tails tolerated), and dl_close performs no saves — every db file is
 * byte-identical before/after (dirty flags are never applied).  All write
 * APIs return -1 (dl_term_cons/dl_term_append and dl_intern_str return 0);
 * every query/lookup/traversal/intern-lookup API works and reads the live
 * state (committed WAL records included).
 *
 * CAVEAT (pre-existing routing, unchanged): when a snapshot has been
 * published (dl_publish_snapshot, or a budget-triggered dl_consolidate),
 * the dl_query/dl_rank/dl_select family prefers that snapshot, while
 * dl_prefix reads the live WAL-replayed state.
 */
dl_db  *dl_open_ro(const char *dir, int *err_out);

/* ─── CONCURRENCY MODEL (MEASURED) ─────────────────────────────────────── */
/*
 * MANY READERS AND ONE WRITER, LIVE AT THE SAME TIME.  <dir>/LOCK carries
 * two independent fcntl BYTE-RANGE locks, because the two roles are
 * different and one whole-file lock cannot express both:
 *
 *   byte 0 — WRITER EXCLUSIVITY.  A read-write handle (dl_open/dl_open2)
 *       holds F_WRLCK here for its whole lifetime.  Read-only handles never
 *       touch this byte, so a live writer does NOT exclude readers.  A
 *       second writer in another process QUEUES for it: the acquisition is
 *       F_SETLKW, so the waiter sleeps on the kernel's per-inode lock queue
 *       and is woken when the holder releases — still exactly one writer per
 *       database directory, but writers queue instead of racing (MEASURED
 *       Phase-0 M1b: with a fail-fast acquire, a fresh-process-per-call
 *       workload at K=4/8/16 succeeded first try only 31%/14%/8% of the
 *       time, with p95 open waits of 1.08/3.35/7.78 s and bimodal retries;
 *       the lock was costing LATENCY through unfairness, not capacity).
 *       An unbounded wait is safe here because fcntl locks are released
 *       automatically when the holding process dies (so a crashed writer
 *       cannot leave a stale lock) and because the hold is short (a publish
 *       is ~90 ms worst case, a new-string fact 0.05 ms).
 *
 *       DL_LOCK_FAIL_FAST=1 selects F_SETLK instead and returns NULL with
 *       *err_out = DL_E_LOCKED at once.  That is the mode for a caller that
 *       wants "busy" rather than "queued" — the `dl` CLI's read commands use
 *       it so their read-only fallback still fires instead of blocking
 *       behind a live writer.  THE DEFAULT IS TO QUEUE.
 *
 *   byte 1 — MUTATION WINDOW.  The writer holds F_WRLCK here around every
 *       on-disk state REPLACEMENT (WAL truncation during compaction, the
 *       snapshot materialization/publish sweep, rels.txt, the dl_close save
 *       sweep).  A reader holds F_RDLCK here for the whole of its open — the
 *       metadata read plus the eager per-relation WAL replay — and RELEASES
 *       it before the handle is returned.  The writer's acquisition BLOCKS
 *       (F_SETLKW); a reader's is BOUNDED: F_SETLK plus a short jittered
 *       sleep retried until a deadline (DL_LOCK_READ_WAIT_MS, default 2000),
 *       after which the reader fails with DL_E_LOCKED.  A reader therefore
 *       waits out at most one mutation window but can never hang forever
 *       behind a writer that is alive-but-slow.
 *
 * What a reader sees: the writer's committed state as of the reader's own
 * OPEN — the durability contract's cycle boundary (below), not a live view.
 * Cross-process read-your-own-writes therefore needs a re-open.  A reader
 * never observes a torn record: WAL appends are self-framing
 * (op | key_len | key | CRC32), a torn tail fails its CRC and replay stops
 * there, and every file that is REPLACED (DAFSA, rels.txt, symbols, terms)
 * commits as write-tmp + fsync + rename, so a lazy loader sees a complete
 * old or a complete new inode.  That is why the writer does not hold the
 * window per fact-add: appends and atomic renames are already safe against
 * a concurrent reader; only the WAL truncation is not.
 *
 * One handle per directory per PROCESS remains the supported rule: the
 * engine has no internal mutex, and two handles hold independent in-memory
 * views.  (The byte-range split did remove the old hazard in which a
 * same-process dl_open_ro's F_RDLCK silently REPLACED that process's own
 * F_WRLCK — fcntl locks are per process — and so admitted a second writer in
 * another process while the RW handle was live; a same-process RO open now
 * touches byte 1 only and leaves byte 0 exclusive.)
 */

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
 * maintenance (IVM/DRed/aggregates) — they always evaluate via the full
 * fixpoint (never silently mis-evaluated). */
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
 * Durability is cycle-granularity: the fact is WAL-appended and visible
 * in memory on return, but no fsync happens here — every WAL is fsync'd
 * at the next cycle boundary (dl_publish_snapshot, dl_close, or a
 * consolidation-time WAL compaction).  An unclean stop before that
 * boundary loses the current cycle's appends; everything before the last
 * boundary replays at open. */
int dl_add_fact(dl_db *db, const char *rel,
                const uint32_t *cols, uint8_t arity);

/* Delete a single fact from a relation.  cols has `arity` u32 values.
 * Returns 1 if deleted, 0 if absent, -1 on error.
 * Durability is cycle-granularity: the delete is WAL-appended and takes
 * effect in memory on return, but no fsync happens here — every WAL is
 * fsync'd at the next cycle boundary (dl_publish_snapshot, dl_close, or a
 * consolidation-time WAL compaction).  An unclean stop before that
 * boundary loses the current cycle's appends; everything before the last
 * boundary replays at open. */
int dl_delete_fact(dl_db *db, const char *rel,
                   const uint32_t *cols, uint8_t arity);

/* ─── CAS revision API (per-entity compare-and-swap revision counter) ──── */

/* Compare-and-swap the revision counter for `entity`.  If the entity's
 * CURRENT stored revision equals `expected`, replace it with `new_value`
 * (DELETE + ADD of the rev row, each WAL-appended only — durability is
 * cycle-granularity as for dl_add_fact — with IVM delta capture) and
 * return 0.  If the current revision differs, returns
 * DL_E_CONFLICT and makes NO change.  If expected == new_value it is an
 * idempotent no-op returning 0.  Returns -1 on error (NULL db/entity, or the
 * internal "rev" relation could not be ensured).  The first successful CAS on
 * a fresh database (or a first write for a given entity) implicitly starts the
 * entity at current revision 0.
 *
 * A caller that reads a revision with dl_rev_get, performs work, then CASes
 * with the read value as `expected` gets optimistic-concurrency semantics. */
int dl_cas_revision(dl_db *db, const char *entity,
                    uint32_t expected, uint32_t new_value);

/* Read the current revision counter for `entity` into *out.  Returns 0 on
 * success (an entity with no rev row reads as 0), -1 on error (NULL db/entity/
 * out, or the internal "rev" relation could not be ensured). */
int dl_rev_get(dl_db *db, const char *entity, uint32_t *out);

/* ─── Transactions (CAS optimistic-concurrency, Slice 2) ───────────────── */

/* Begin a transaction: buffer subsequent dl_txn_* operations and commit them
 * atomically (one WAL + one fsync) via dl_txn_commit.  Rejects nesting (a
 * transaction is already open) with -1.  Returns 0 on success. */
int dl_txn_begin(dl_db *db);

/* Buffer a compare-and-swap of `entity`'s revision (current must equal
 * `expected`; replaced with `new_value`).  Validation happens at commit, so a
 * conflict there aborts the WHOLE transaction with DL_E_CONFLICT.  If
 * expected == new_value it is an idempotent no-op (nothing buffered).  The
 * entity string is interned eagerly.  Returns 0 on success, -1 on error. */
int dl_txn_cas(dl_db *db, const char *entity,
               uint32_t expected, uint32_t new_value);

/* Buffer a fact add inside the transaction.  cols has `arity` u32 values
 * (already interned, as for dl_add_fact).  The "rev" relation and variadic
 * relations are rejected (-1).  Returns 0 on success, -1 on error. */
int dl_txn_add_fact(dl_db *db, const char *rel,
                    const uint32_t *cols, uint8_t arity);

/* Buffer a fact delete inside the transaction.  Same contract as
 * dl_txn_add_fact.  Returns 0 on success, -1 on error. */
int dl_txn_delete_fact(dl_db *db, const char *rel,
                       const uint32_t *cols, uint8_t arity);

/* Atomically commit the buffered transaction: validate all CAS ops (a
 * conflict returns DL_E_CONFLICT and applies NOTHING — earlier buffered ops
 * are discarded, the WAL is untouched), save the interner/term-store if
 * dirty, append one record per op + a COMMIT marker to the txn WAL, fsync,
 * then apply all ops in-memory (with IVM delta capture).  An empty
 * transaction commits trivially (0).  Returns 0 on success, DL_E_CONFLICT on
 * a CAS conflict, -1 on error. */
int dl_txn_commit(dl_db *db);

/* Abort the transaction, discarding all buffered ops.  The WAL is never
 * written, so nothing is durable.  Returns 0 on success, -1 if no transaction
 * is open. */
int dl_txn_rollback(dl_db *db);

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

/* ─── Pull-based sorted iterator (range-index follow-up #5) ────────────── */

/* Opaque RESUMABLE cursor over the tuples of a relation in ascending key
 * order (u32BE key encoding => numeric order == lex order).  Reads from the
 * mmap snapshot view when a snapshot is current (db->snap_version > 0), else
 * the in-memory relation.  VARIADIC relations are rejected (NULL / -1).
 *
 * A DAFSA is a minimal acyclic DFA with per-state transitions sorted by
 * symbol, so pre-order DFS == byte/lex == u32BE numeric order; from a bound
 * state every final state sits at the same fixed depth with no outgoing
 * edges, so emit-then-backtrack yields exactly one tuple per dl_iter_next. */
typedef struct dl_iter dl_iter;

/* Open a cursor bound to the first k leading columns == `leading`
 * (k == 0: no bound — enumerate all tuples).  Returns NULL on any error:
 * NULL db/rel, unknown relation, VARIADIC relation, k > arity, k > 0 &&
 * !leading, or an open/view-open/OOM failure.  An ABSENT leading prefix
 * yields a VALID EMPTY iterator (first dl_iter_next returns 0), not NULL.
 *
 * LIFETIME: in LIVE mode the cursor borrows the relation's in-memory view
 * (rel->d) from db, so the iterator MUST NOT outlive db (same contract as
 * dl_prefix).  In VIEW (snapshot) mode the cursor OWNS an mmap dafsa_view
 * and is independent of db; it is closed by dl_iter_close. */
dl_iter *dl_iter_open(dl_db *db, const char *rel,
                      const uint32_t *leading, uint8_t k);

/* Re-bind the cursor to a new leading prefix (k columns == `leading`).
 * Returns 0 on success, -1 on error (NULL it, k > arity, k > 0 && !leading).
 * An absent prefix yields an empty iterator (dl_iter_next returns 0). */
int dl_iter_seek(dl_iter *it, const uint32_t *leading, uint8_t k);

/* Fetch the next tuple in ascending order into cols_out (arity u32 values).
 * Returns 1 on success, 0 at the end of the enumeration, -1 on error (NULL
 * it/cols_out, or a DAFSA depth overflow).  End (0) and error (-1) are
 * never conflated. */
int dl_iter_next(dl_iter *it, uint32_t *cols_out);

/* The relation's arity, or 0 for a NULL/error cursor. */
uint8_t dl_iter_arity(const dl_iter *it);

/* Close the cursor (NULL-safe).  Closes an OWNED mmap snapshot view. */
void dl_iter_close(dl_iter *it);

/* Merge-join callback: l has la columns, r has ra columns; user is the
 * opaque pointer passed to dl_merge_join.  Return non-zero to stop early. */
typedef int (*dl_join_cb)(const uint32_t *l, uint8_t la,
                          const uint32_t *r, uint8_t ra, void *user);

/* Equi-join two sorted iterators on their first jcols columns (cross-product
 * semantics, preserving duplicates), streaming the result in sorted order via
 * cb.  Returns the number of pairs emitted, or -1 on error (NULL it/cb,
 * jcols == 0, or jcols > either arity).  Both iterators are left EXHAUSTED
 * on return.  If cb returns non-zero the join stops early and returns the
 * count emitted so far. */
long dl_merge_join(dl_iter *l, dl_iter *r, uint8_t jcols,
                   dl_join_cb cb, void *user);

/* ─── Order statistics (Tier-2) ───────────────────────────────────────── */

/* Number of distinct tuples strictly lexicographically smaller than `cols`.
 * `cols` has `arity` u32 values; for a fixed-arity relation `arity` must equal
 * the declared arity.  u32BE key encoding => numeric order == lex order.
 * Returns the rank, or UINT64_MAX on error (NULL db/rel/cols, unknown rel,
 * arity mismatch, or a VARIADIC relation — variadic is rejected in this slice).
 * When a snapshot has been published (db->snap_version > 0) this reads the
 * mmap snapshot view, so the result reflects the PUBLISHED state even if the
 * live in-memory relation has since been mutated; otherwise it reads the
 * in-memory relation (rel->d). */
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

/* ─── Permuted order statistics (Tier-2 follow-up #2) ──────────────────── */

/* The *_perm family evaluates order statistics over a PERMUTED view of a
 * relation (order-by on a non-leading column).  perm_id is a permutation
 * index previously returned by dl_db_declare_perm(db, rel_id, arity, perm),
 * where perm[j] is the ORIGINAL column that appears at permuted position j
 * (so perm = {1,0} over an arity-2 relation orders tuples by col1, then col0
 * as the tiebreaker — the permuted relation's DAFSA sorts the re-encoded
 * keys lexicographically).
 *
 * `cols`/`lo`/`hi` are FULL arity-length tuples in the ORIGINAL column order
 * (as read by dl_select/dl_lookup).  Rank/range_count FORWARD-map the input
 * to permuted order (pcols[j] = cols[perm[j]]) before ranking; select
 * INVERSE-maps its permuted-order result back to original order
 * (cols_out[perm[j]] = pcols_out[j]).  A select→rank round-trip over the
 * same permuted relation is therefore the identity.
 *
 * The permuted relation is built on demand (permindex_build) if it is not
 * yet materialized or is dirty — a stale/NULL index is never silently used.
 *
 * Error handling mirrors dl_rank/dl_select/dl_range_count (NULL db/rel/input,
 * unknown rel, VARIADIC relation, arity mismatch) plus bad perm_id or a perm
 * that does not belong to rel/arity: rank/range_count return UINT64_MAX and
 * select returns -1. */
uint64_t dl_rank_perm(dl_db *db, const char *rel, int perm_id,
                      const uint32_t *cols, uint8_t arity);
int dl_select_perm(dl_db *db, const char *rel, int perm_id, uint64_t k,
                   uint32_t *cols_out, uint8_t arity);
uint64_t dl_range_count_perm(dl_db *db, const char *rel, int perm_id,
                             const uint32_t *lo, const uint32_t *hi,
                             uint8_t arity);

/* Number of permutation indices currently declared on the database (equals
 * db->n_perms).  Useful for tests of automatic perm-index selection. */
int dl_db_perm_count(const dl_db *db);

/* ─── Schema attach (rule typechecker) ─────────────────────────────────── */

/* Attach (or detach) a typed schema for rule typechecking.  A non-NULL
 * schema is consulted by dl_load_rules BEFORE compilation; if the rules fail
 * to typecheck against it, the load aborts with -1.  Passing NULL detaches.
 * The caller retains ownership of the schema; the database only stores the
 * pointer.  Returns 0 on success, -1 on a NULL db. */
int dl_attach_schema(dl_db *db, const dl_schema *schema);

/* ─── Rule loading & compilation (M1) ──────────────────────────────────── */

/* Parse and compile Datalog rules from a source string.
 * Rules may reference existing relations (must match arity) or declare
 * new derived relations.  Returns 0 on success, -1 on error (message to
 * stderr).  The compiled program is stored internally; subsequent calls
 * to dl_compile append to it.
 *
 * ACCUMULATION CONTRACT — a program may be loaded in several batches, and the
 * batches are ONE program, not several:
 *   - Which rules participate: every retained rule.  dl_compile / dl_query /
 *     the fact-propagation path evaluate the accumulated rule set, and a query
 *     after the second dl_load_rules sees both batches' rules.
 *   - Stratification and recursion are computed over the ACCUMULATED rule set
 *     (strata are a property of the program, not of a batch), so the ANSWER
 *     does not depend on how the rules were grouped or ordered: loading {A,B}
 *     as one call, as two calls in either order, or as one call per rule gives
 *     the same relation contents.  A batch that closes an unstratifiable
 *     cycle, or that makes an existing rule a recursive aggregate / a
 *     recursive rule over a variadic head / a range over a recursive relation,
 *     is REFUSED (returns -1) with the single-batch program's diagnostic.
 *   - The COMPILED PROGRAM is the one a single-batch load of the same rules
 *     produces, not just the answers.  Whether a non-leading, partially-bound
 *     body atom becomes OP_LOOKUP_PERM or OP_HASH_JOIN — and whether it forces
 *     a permutation index, which is how a large program is refused with "too
 *     many permutation indices" — is a COMPILE-TIME decision taken from the
 *     recursive set (compiler.zig emit_nonleading_join).  Republishing
 *     is_recursive onto a retained rule cannot reach it, so when a load makes
 *     ANY relation newly recursive — the union's recursion vector differs from
 *     the one the retained rules were compiled under — every retained rule and
 *     the incoming batch are RECOMPILED against the union's strata and
 *     recursion.  A split load therefore plans (and is refused) as its
 *     single-batch twin does.  A load that changes no relation's recursion
 *     status recompiles nothing: the per-batch compile already saw the same
 *     vector (compute_strata carries recursion across loads), so the only
 *     decision that could move is a card-dependent one, and those a
 *     single-batch compile freezes at its own load too.
 *     Boundary, measured: when recursion does change, that recompile also
 *     re-plans card-dependent atoms from the card estimates AT THAT LOAD, so a
 *     program split around fact insertion plans as a single-batch load
 *     performed at the last load's moment would — not as one performed before
 *     the facts existed.  And a rule promoted to recursive makes the program
 *     IVM-eligible where the stale op made it IVM-ineligible: that is the
 *     single-batch maintenance class, because the fixpoint shadows perm indices
 *     on recursive heads (vm.zig vm_ivm_eligible) and the incremental path is
 *     gated on it.  The answers are unaffected either way: the recursive driver
 *     feeds every BODY-ATOM op the compiler can emit (SCAN, LOOKUP,
 *     LOOKUP_PERM, HASH_JOIN and the pattern WALK — vm.zig driverFeeds).
 *   - Previously derived relations are NOT kept: each evaluation resets the
 *     derived views and re-derives them from the EDB, so no tuple derived under
 *     an earlier rule set survives.
 * Trailing asymmetry worth knowing: a body predicate that is neither declared
 * nor a head of the SAME batch is "unknown predicate", so a batch that reads a
 * relation a later batch will define must have that relation declared first
 * (dl_declare_relation), which is also how an EDB is introduced. */
int dl_load_rules(dl_db *db, const char *dl_source);

/* Compile all previously loaded rules into bytecode and run them against
 * the database, materializing derived relations in-place.
 * Returns 0 on success, -1 on error (or DL_E_FRAMES — see the block above). */
int dl_compile(dl_db *db);

/* Evaluate all compiled rules and stream the goal relation's tuples via cb.
 * Returns the number of tuples emitted, or -1 on error (or DL_E_FRAMES —
 * see the block above).
 * If a snapshot has been published, reads from mmap'd snapshot (bypasses VM). */
long dl_query(dl_db *db, const char *goal_rel, dl_tuple_cb cb, void *user);

/* Read-only arbitrary-rule query: parse + compile + evaluate `dl_source` as a
 * SELF-CONTAINED rule program against a SHALLOW clone of `db`, stream every
 * tuple of the goal relation `goal_rel` via `cb`, then discard the clone.
 *
 * Leaves `db` 100% untouched: no relation is added to db->rels, no existing
 * relation's contents are mutated, and nothing is written to disk (the clone
 * has dir==NULL, so the VM/compile path can never touch the filesystem).
 *
 * Semantics mirror dl_query on a hypothetical throwaway copy of the store:
 *   - body atoms that name an existing relation read its CURRENT (view)
 *     contents — exactly as dl_query would;
 *   - rule heads are evaluated on the clone only; a head whose name matches
 *     an existing relation is deep-copied in the clone so the VM writes into
 *     the copy, never the borrowed live relation;
 *   - `goal_rel` may be either a head of `dl_source` (derived) or an existing
 *     relation.
 *
 * Supports the full evaluation class: variadic relations, list/range builtins,
 * negation, aggregates, recursion.  No adornment or bound arguments required.
 *
 * Returns the tuple count streamed, or -1 on error (NULL args, parse/compile
 * failure, goal_rel not found in the clone after evaluation, OOM).  Errors are
 * printed to stderr in the engine's fx style; a rule body that exceeds the
 * interpreter's frame limit returns DL_E_FRAMES — see the block above. */
long dl_query_rules_ro(dl_db *db, const char *dl_source,
                       const char *goal_rel, dl_tuple_cb cb, void *user);

/* ─── Well-founded semantics (negation over recursion) ─────────────────── */

/* Truth slice of the well-founded partial model streamed by
 * dl_query_wfs_ro's `truth` parameter.  Mode 0 is implemented in slice 1;
 * modes 1-3 return DL_WFS_ERR_NOT_IMPLEMENTED (never a wrong answer). */
#define DL_WFS_TRUE_ONLY   0   /* stream atoms TRUE in the WFS model   */
#define DL_WFS_FALSE_ONLY  1   /* stream atoms FALSE in the WFS model  */
#define DL_WFS_UNDEF_ONLY  2   /* stream atoms UNDEFINED               */
#define DL_WFS_ALL_TAGGED  3   /* all atoms, truth tag appended (later)*/

/* Negative results of dl_query_wfs_ro (distinct from the tuple count). */
#define DL_WFS_ERR_REJECTED        (-2) /* program outside the S1 subset          */
#define DL_WFS_ERR_OOM             (-3) /* allocation failure                     */
#define DL_WFS_ERR_NOT_IMPLEMENTED (-4) /* truth mode 1/2/3 in this slice         */
#define DL_WFS_ERR_NO_CONVERGE     (-5) /* alternating fixpoint round cap exceeded */
#define DL_WFS_ERR_INTERNAL        (-6) /* internal invariant violated            */

/* Read-only arbitrary-rule query evaluated under WELL-FOUNDED semantics
 * (van Gelder alternating fixpoint): the program may negate a predicate of
 * its own recursion cycle — the class every other entry rejects as
 * "unstratifiable".  Canonical case:  win(X) :- move(X,Y), !win(Y).
 *
 * Same shape and db-isolation contract as dl_query_rules_ro (parse ->
 * clone -> declare -> AST REWRITE -> compile -> driver -> goal scan ->
 * teardown; db is 100% untouched, nothing is written to disk).  The
 * negated atom is rewritten into a POSITIVE atom over a driver-owned
 * complement relation, so the existing compiler and VM are reused as-is.
 *
 * Slice-1 subset (violations return DL_WFS_ERR_REJECTED with a stderr
 * diagnostic): pure relational rules over EDB + IDB, negation of IDB
 * predicates only, safe negation (a negated atom's variables must be bound
 * by earlier positive body atoms); no aggregates, builtins, lists, regex
 * patterns, variadic relations, or arithmetic.  Stratified programs are
 * answered identically to dl_query_rules_ro (the well-founded model of a
 * stratified program is its stratified least fixpoint).
 *
 * Returns the tuple count streamed, or a negative DL_WFS_ERR_* above. */
long dl_query_wfs_ro(dl_db *db, const char *dl_source,
                     const char *goal_rel, int truth,
                     dl_tuple_cb cb, void *user);

/* Prefix-bind k leading columns and enumerate via cb.
 * Reads from snapshot if published, else falls back to in-memory path. */
long dl_query_bound(dl_db *db, const char *goal_rel,
                    const uint32_t *leading, uint8_t k,
                    dl_tuple_cb cb, void *user);

/* ─── Regex pattern query ─────────────────────────────────────────────── */

/* Forward declaration (full struct in regexwalk.h) */
struct regex_dfa;


/* Enumerate all tuples in a relation whose column string content matches the
 * compiled regex DFA.  col is 0-based column index (default 0).
 * Reads from snapshot if published, else falls back to in-memory path. */
long dl_pattern(dl_db *db, const char *rel_name, uint8_t col, const struct regex_dfa *dfa,
               dl_tuple_cb cb, void *user);

/* ─── Fuzzy (Levenshtein) symbol lookup ────────────────────────────────── */

/* Candidate callback: the matched sym_id, its string, its length, and the
 * BYTE edit distance to the needle (always <= k).  Return non-zero to stop. */
typedef int (*dl_fuzzy_cb)(uint32_t sym_id, const char *name, size_t name_len,
                           uint32_t dist, void *user);

/* Enumerate every interned symbol whose name is within edit distance k of
 * `needle`, in symbol-id order.  Zero index storage and no index BUILD: the
 * candidates come from a banded linear scan of the live symbol table (a name
 * outside [len-k, len+k] cannot be within k edits), each one checked with
 * lev_distance_cap.  (A walk of the symbol DAFSA was measured 500-1000x slower
 * at 166k symbols, because the lazily-built forward DAFSA costs 10-19 s to
 * materialize and the edit-distance automaton needs no index at all — see
 * docs/datalog-dafsa-fuzzy-lookup.md.)
 *
 * k is 0..2; k >= 3 is REFUSED with -1 (the DP-row determinisation explodes).
 * An empty needle and a needle longer than 512 bytes are REFUSED with -1 —
 * the same two refusals lev_compile applies (see regexwalk.h).
 *
 * BYTE DISTANCE, not codepoint distance: the comparison is over bytes, so a
 * non-ASCII codepoint costs one edit per UTF-8 byte ('é' = 2).  Correct for
 * ASCII; over-counts non-ASCII.
 *
 * Returns the number of matches, or -1 on error (NULL args, k > 2, bad needle). */
long dl_fuzzy_lookup(dl_db *db, const char *needle, uint32_t k,
                     dl_fuzzy_cb cb, void *user);

/* ─── Snapshot publish (M4) ────────────────────────────────────────────── */

/* Atomic publish: saves interner + all relations to a versioned snapshot
 * directory, flips the CURRENT pointer.  Returns 0 on success, -1 on error
 * (or DL_E_FRAMES when the fixpoint hits the frame limit — see the block
 * above; the incremental ladder may mask it as -1, see there).
 * After publish, dl_query reads from mmap instead of the VM. */
int dl_publish_snapshot(dl_db *db);

/* ─── Periodic consolidation (selfreg periodic snapshot) ───────────────── */

/* Consolidate without necessarily snapshotting: fsync every per-relation WAL
 * (the cycle-boundary durability barrier), fold each relation's hot-write
 * overlay into its DAFSA, advance the access epoch, apply decay, and
 * materialize derived facts (the IVM cascade).  This is the cheap O(delta)
 * per-turn maintenance point.
 *
 * It materializes a NEW versioned snapshot when the total WAL-append bytes
 * since the last materialization meet or exceed the snapshot budget (the
 * default 64 MiB, override DL_SNAPSHOT_BUDGET_MB, or dl_set_snapshot_budget).
 * With a budget of 0 it never snapshots (the pure "in-memory with sync"
 * mode).  So the snapshot is PERIODIC (budget-triggered at consolidation-
 * time) instead of per-publish; dl_publish_snapshot remains the explicit
 * every-call checkpoint.
 *
 * BETWEEN materializations, dl_query/dl_rank/dl_select/dl_pattern serve
 * the LAST materialized snapshot (snap_version does not advance), not the
 * just-consolidated live store; use dl_lookup/dl_prefix for current state,
 * or dl_publish_snapshot to force a fresh snapshot now.
 *
 * Returns 0 on success, -1 on error (NULL db, read-only handle, open txn,
 * or a consolidation/materialization failure; the fixpoint's frame-limit
 * overflow surfaces as DL_E_FRAMES — see the block above). */
int dl_consolidate(dl_db *db);

/* Set the WAL-append byte budget that triggers a snapshot materialization
 * inside dl_consolidate.  bytes == 0 disables auto-snapshot: dl_consolidate
 * then only consolidates and never materializes.  The default is 64 MiB
 * (DL_SNAPSHOT_BUDGET_MB).  Returns 0 on success, -1 on a NULL db. */
int dl_set_snapshot_budget(dl_db *db, uint64_t bytes);

/* ─── Time-travel / as-of queries ──────────────────────────────────────── */

/* Enumerate every published snapshot version in ascending order.  Returns the
 * TOTAL number of versions (even when `cap` is smaller), filling at most
 * `cap` entries of `out` (out may be NULL to just size).  Returns 0 when no
 * snapshot has been published, and -1 on a NULL db. */
long dl_snapshot_versions(const dl_db *db, uint32_t *out, size_t cap);

/* As-of query: stream the tuples of `goal_rel` as of the published snapshot
 * `version`, bypassing the live/CURRENT routing entirely (db->snap_version is
 * never mutated).  Reads the version's manifest + mmap view from disk.
 * Returns tuple count, or -1 on error: NULL db/goal_rel/cb, version == 0, a
 * nonexistent version, or a goal_rel absent from that version — never a
 * silently-empty result. */
long dl_query_version(dl_db *db, uint32_t version, const char *goal_rel,
                      dl_tuple_cb cb, void *user);

/* As-of prefix query: bind the first k columns of `goal_rel` to `leading`
 * and stream matching tuples from snapshot `version`.  Same error contract
 * as dl_query_version.
 *
 * `leading` holds SYM IDS, and they must be that snapshot's ids: on a store
 * whose db-root symbols.array diverges from the snapshot's, a live-interned id
 * names another string (see dl_intern_str_version). */
long dl_query_bound_version(dl_db *db, uint32_t version, const char *goal_rel,
                            const uint32_t *leading, uint8_t k,
                            dl_tuple_cb cb, void *user);

/* Manifest-relation callback for dl_snapshot_relations.  `name` is
 * NUL-terminated and valid only for the duration of the call; `idb` is 1
 * when the manifest records the relation as derived.  Return non-zero to
 * stop the enumeration early. */
typedef int (*dl_relation_cb)(const char *name, uint8_t arity, int idb,
                              void *user);

/* Enumerate the FIXED-ARITY relations recorded in snapshot `version`'s
 * manifest.txt ('name:arity:edb|idb' lines), streaming each via `cb`.
 * Skips comment/directive lines, variadic '*' star markers and their
 * per-variant 'name.<d>' lines (a variadic relation has no single fixed
 * arity), and the reserved '<rel>__PI<hex>__' perm-index names.  Malformed
 * lines are skipped, not fatal.
 * Returns the number of relations emitted, or -1 on error: NULL db/cb,
 * version == 0, a nonexistent/unreadable snapshot version, or a manifest
 * entry this walker cannot represent. */
long dl_snapshot_relations(dl_db *db, uint32_t version,
                           dl_relation_cb cb, void *user);

/* Opt-in snapshot retention: keep at most `n` most-recent versions, pruning
 * older ones at the end of each successful publish.  n == 0 (the default)
 * keeps every version.  Returns 0 on success, -1 on a NULL db. */
int dl_set_snapshot_retain(dl_db *db, unsigned n);

/* ─── Fault-injection hooks (test-only, NULL in production) ────────────── */

typedef enum {
    DL_FPOINT_AFTER_REL_SAVE,
    DL_FPOINT_AFTER_RENAME,
    DL_FPOINT_TXN_BEFORE_MARKER,  /* txn commit: after data records, before COMMIT marker */
    DL_FPOINT_DERIVED_SAVE        /* opt-in derived symbols.dafsa (DL_PUB_DERIVED=1):
                                   * fired before the file is written; a non-zero
                                   * return simulates the write failing (ENOSPC,
                                   * EACCES, ...) and aborts the publish */
} dl_fpoint;

/* Set a fault-injection hook.  At each fpoint during publish, if hook is
 * non-NULL it is called; non-zero return aborts the publish. */
void dl_set_fault_hook(dl_db *db,
                       int (*hook)(dl_fpoint fp, void *user),
                       void *user);

/* ─── Interner access (M7) ─────────────────────────────────────────────── */

/* Intern a string, return its sym_id (1-based).  Returns 0 on OOM. */
uint32_t    dl_intern_str(dl_db *db, const char *str);

/* NON-MUTATING lookup: sym_id for str if already interned, else 0.
 * Never grows the interner / never marks it dirty (unlike dl_intern_str). */
uint32_t    dl_intern_str_find(dl_db *db, const char *str);

/* VERSION-side interning: resolve `str` against the vocabulary of published
 * snapshot `version` — that snapshot's OWN symbols.array — WITHOUT creating a
 * symbol.  This is the entry point every VERSION-aware query must use to turn
 * a term into an id; LIVE queries keep using dl_intern_str.
 *
 * WHY IT IS REQUIRED (LIVE-vs-VERSION read discipline).  A sym_id is NOT
 * stable across a publish: the live interner numbers strings from the db-ROOT
 * <db>/symbols.array, which can diverge from a snapshot's — a legacy store
 * written before publish persisted the root array, or a restored / copied /
 * edited root array.  On such a store the same id names a DIFFERENT string in
 * each map, so a term interned live and then matched by a version read (which
 * matches ids against that snapshot's __postings__) selects ANOTHER term's
 * rows.  MEASURED on a diverged store: `search "blue" --version 1` returned
 * the "red" observation and `search "red" --version 1` returned the same row,
 * where the intact store returns the "blue" row for "blue".  Interning against
 * the snapshot's array makes the id mean what that snapshot means by it.
 *
 * LOOKUP-ONLY, BY CONTRACT: never creates a symbol, never grows the live
 * interner, never marks it dirty (so it is valid on a read-only handle).  A
 * term that snapshot's vocabulary does NOT contain yields *sym_out == 0 — the
 * not-found sentinel.  That term has no postings as-of that version, so a
 * query over it matches NOTHING; it is never a freshly minted id, which could
 * be another string's id in that snapshot.
 *
 * Returns 0 on success (*sym_out set), or -1 on error: NULL db / str / sym_out,
 * version == 0 (that is the LIVE path — use dl_intern_str), a nonexistent
 * version, or a snapshot whose symbols.array is missing or unreadable.  A
 * damaged snapshot is an explicit ERROR, never an empty vocabulary and never a
 * fallback to the live interner.
 *
 * The snapshot's interner is read once and cached per version (the same cache
 * dl_pattern's version path uses); dl_close frees it.
 *
 * Example — search as-of a version:
 *     uint32_t sym = 0;
 *     if (dl_intern_str_version(db, v, "blue", &sym) != 0) return rc_error;
 *     if (sym != 0)  // 0 = absent from that snapshot: no matches, not an id
 *         dl_search_top_version(db, v, &sym, 1, ids, scores, n); */
int         dl_intern_str_version(dl_db *db, uint32_t version, const char *str,
                                  uint32_t *sym_out);

/* Look up a sym_id → string.  Returns NULL if id is out of range. */
const char *dl_intern_str_of(dl_db *db, uint32_t sym_id);

/* The id → string half of a VERSION read: resolve sym_id in snapshot
 * `version`'s OWN symbols.array, never in the live interner.  Every version
 * read (dl_pattern, dl_query, dl_query_bound, dl_intern_str_version) returns
 * ids as-of THAT snapshot; resolving them with dl_intern_str_of names another
 * string on a store whose db-root array has diverged from the snapshot's (a
 * restored/copied/edited store — the same store dl_intern_str_version exists
 * for, in the mirror direction).  MEASURED before this entry point existed: on
 * a store whose root array had `doc1`/`doc2` swapped against its snapshot's,
 * `dl pattern observation '^doc1$'` printed `doc2 ...` — right row, wrong name,
 * exit code 0.
 *
 * Returns NULL for version == 0 (the LIVE path), sym_id == 0, an id this
 * snapshot never assigned, and a snapshot whose array is
 * missing/unreadable/damaged (its recorded size/count disagree).  A caller
 * that prints ids then renders the raw number.  Lookup-only: mints no symbol,
 * grows no interner, and therefore works on a read-only handle. */
const char *dl_intern_str_of_version(dl_db *db, uint32_t version, uint32_t sym_id);

/* Return the interner's forward DAFSA as a MUTABLE handle, lazily materializing
 * it if absent.  After this, dl_intern_str GROWS the DAFSA in place on new
 * symbols (instead of freeing it), so interning stays off the linear rev[]
 * scan.  Returns void* because dl.h does not expose the dafsa type.  NULL on
 * NULL db / OOM (callers treat NULL as "degrade to the safe linear path"). */
void *dl_intern_fwd_mutable(dl_db *db);

/* ─── Graph traversal (Tier-2) ───────────────────────────────────────────── */

/* Callback for dl_traverse: receives (node_sym, depth).
 * Return non-zero to stop early. */
typedef int (*dl_traverse_cb)(uint32_t node_sym, uint8_t depth, void *user);

/* BFS traversal over edge(from,to,type) graph from start node.
 * Both forward and reverse edges are traversed (bidirectional).
 * depth clamped to 1..3; max_nodes bounds total visited nodes.
 * cb receives each visited node with its depth from start.
 * Returns the number of nodes visited, or -1 on error (missing relations,
 * wrong arity, or intern failure).
 * Operates on LIVE relations (not snapshot). */
long dl_traverse(dl_db *db, const char *start,
                 int depth, int max_nodes,
                 dl_traverse_cb cb, void *user);

/* Callback for dl_node_observations: receives each observation string.
 * Return non-zero to stop early. */
typedef int (*dl_str_cb)(const char *s, void *user);

/* Fetch observations for a node from the observation(node,content) relation.
 * Resolves content sym_ids via dl_intern_str_of.
 * Returns the number of observations emitted, or -1 on error.
 * Operates on LIVE relations (not snapshot). */
long dl_node_observations(dl_db *db, const char *node,
                          int max_obs, dl_str_cb cb, void *user);

/* ─── List term-store access (v2 lists, public wrappers) ─────────────────── */
/* A List column VALUE is a hash-consed list handle in the reserved high range
 * [TERM_BASE, ...) (TERM_BASE = the empty list NIL).  These thin wrappers
 * forward to the db's term store so callers can build and walk list values
 * through the PUBLIC API (matching dl_intern_str).  Each returns
 * 0 (or 0/NULL) on a NULL db / absent term store / invalid operand. */

/* Intern the cons cell (head, tail); tail MUST be a list.  Returns the
 * canonical handle, or 0 on error/OOM. */
uint32_t dl_term_cons(dl_db *db, uint32_t head, uint32_t tail);

/* append(a, b): list a followed by list b.  Returns the canonical handle, or
 * 0 on error/OOM or a non-list operand.  append(NIL, b) == b. */
uint32_t dl_term_append(dl_db *db, uint32_t a, uint32_t b);

/* 1 if v is a list handle in this store (EXACT index-range test). */
int      dl_term_is_list(const dl_db *db, uint32_t v);

/* head / tail of a cons cell.  Precondition: h is a list handle and h != NIL;
 * returns 0 otherwise. */
uint32_t dl_term_car(const dl_db *db, uint32_t h);
uint32_t dl_term_cdr(const dl_db *db, uint32_t h);

#ifdef __cplusplus
}
#endif

/* Full-text tier (lexical half) query path: tokenize, aux_index_*, dl_search*,
 * dl_search_ranked* (the partial-match / OR entry point).  Included here so
 * the whole public API surface is reachable via dl.h alone. */
#include "index.h"

#endif /* DL_H */
