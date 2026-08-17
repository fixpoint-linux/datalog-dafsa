/*
 * vm.h — Bytecode VM for the M2 Datalog evaluator
 *
 * Flat binding-table bytecode interpreter. Instructions:
 *   OP_SCAN, OP_LOOKUP, OP_EQ, OP_EQ_CONST, OP_PROJECT, OP_HALT,
 *   OP_NEG_CHECK, OP_AGG_ACC, OP_AGG_EMIT, OP_WALK, OP_LOOKUP_PERM,
 *   OP_HASH_JOIN, OP_CMP (M9 comparison), OP_ARITH (M9 arithmetic),
 *   OP_STR_FILTER / OP_STR_LEN / OP_STR_BIND (M9-strings),
 *   OP_MAT_BEGIN / OP_MAT_JOIN (BUSHY intermediate results),
 *   OP_LIST_CONS / OP_LIST_CAR / OP_LIST_CDR / OP_LIST_APPEND /
 *   OP_LIST_MEMBER (LISTS), OP_RANGE (RANGE leading-column predicate).
 *
 * M2: vm_execute performs stratified semi-naive fixpoint evaluation.
 * Rules are grouped by stratum; recursive strata iterate to fixpoint.
 *
 * Execution model: stack-based with one frame per SCAN/LOOKUP loop.
 * Tuples are collected into arrays at frame init (in-memory for M2).
 */
#ifndef VM_H
#define VM_H

#include "compiler.h"
#include "dl.h"

/* Evaluate all compiled rules against the database, materializing derived
 * relations in-place.  Returns 0 on success, -1 on error. */
int vm_execute(dl_db *db, compiled_rule **rules, int n_rules);

/* Evaluate rules and stream tuples of the goal relation via a callback.
 * Returns number of tuples emitted, or -1 on error. */
long vm_query(dl_db *db, compiled_rule **rules, int n_rules,
              const char *goal_rel, dl_tuple_cb cb, void *user);

/* ─── top-down QSQ support (expose the body-atom override + rule entry) ── */

struct tuple_set;  /* full def in tupleset.h */

typedef struct {
    int              body_idx;   /* which body atom to override */
    const struct tuple_set *ts;  /* tuple_set to enumerate (NULL = DAFSA) */
    int              perm_id;    /* perm_id for perm shadow, -1 if none */
} vm_override;

/* Run ONE compiled rule body with overrides, collecting projected head
 * tuples via cb when dry==1 (no DAFSA commit).  This is the body-evaluation
 * primitive the top-down driver (src/topdown.c) reuses; it is a thin wrapper
 * over the existing static exec_rule. */
long vm_exec_rule(dl_db *db, const compiled_rule *cr,
                  const vm_override *ov, int n_ov,
                  int dry, dl_tuple_cb cb, void *user);

/* ─── IVM Slice 1/2: insert-only incremental maintenance ──────────────── */

/* 1 if EVERY compiled rule is IVM-insert-eligible: negation-free,
 * aggregate-free, and every positive body atom compiles to an
 * override-compatible opcode (OP_SCAN/OP_LOOKUP).  Recursive rules are now
 * ALLOWED — they are maintained by the delta-seeded semi-naive fixpoint
 * (vm_execute_ivm), not the non-recursive worklist.  Returns 0 if any rule
 * needs the full-fixpoint path (the correctness floor): negation, aggregates,
 * OP_WALK, OP_LOOKUP_PERM, OP_HASH_JOIN, or a recursive rule whose base body
 * atom reads a rule-head relation (a non-recursive head or a chained recursive
 * SCC feeding this SCC — its re-derived tuples have no per-insert delta). */
int vm_ivm_eligible(dl_db *db);

/* 1 if ANY compiled rule is recursive (routes the publish path to the
 * delta-seeded semi-naive fixpoint instead of the non-recursive worklist). */
int vm_has_recursive(dl_db *db);

/* Propagate db->delta_pending insert deltas through the dependent rules,
 * maintaining derived views incrementally (semi-naive single-step via the
 * existing exec_rule join seeded with the delta as the changed body atom).
 * Consumes (frees) db->delta_pending.  Returns 0 on success, -1 on error.
 *
 * PRECONDITION: vm_ivm_eligible(db) == 1 (the caller enforces this; if a
 * rule outside the class sneaks in, its delta override would be ignored and
 * we would silently mis-evaluate — never call this on an ineligible program).
 */
int vm_propagate_deltas(dl_db *db);

/* Free all pending delta tuple_sets (used after a full re-eval consumes the
 * pending changes, and on dl_close). */
void vm_clear_deltas(dl_db *db);

/* Incremental maintenance for a program that CONTAINS recursive rules.  Runs
 * the stratified evaluator WITHOUT resetting rule-head views — the prior
 * derived state is preserved and ADDED to — seeding each recursive stratum's
 * semi-naive fixpoint with the current view (idb) + the pending insert deltas
 * (delta) so only the changed facts re-derive.  READS (does not free)
 * db->delta_pending; the caller clears them after.  Returns 0 on success,
 * -1 on error.
 *
 * PRECONDITION: vm_ivm_eligible(db) == 1 and vm_has_recursive(db) == 1. */
int vm_execute_ivm(dl_db *db);

/* ─── IVM Slice 3: DRed deletion (over-delete + re-derive) ─────────────── */

/* 1 if EVERY compiled rule is DRed-deletion-eligible: non-recursive,
 * aggregate-free, and free of the override-incompatible opcodes
 * (OP_WALK / OP_LOOKUP_PERM / OP_HASH_JOIN).  Stratified NEGATION IS allowed:
 * the bounded over-delete cascade handles retraction through positive body
 * atoms, and the heads of negation-containing rules take a conservative
 * full view reset (drop derived + re-derive) because negation-driven
 * retractions cannot be enumerated by the positive cascade.  Returns 0 if
 * any deletion must fall back to the full fixpoint (the correctness floor):
 * recursion, aggregates, or an opcode whose body atom cannot be overridden
 * by the over-delete cascade. */
int vm_dred_eligible(dl_db *db);

/* DRed deletion maintenance: consume db->del_pending (and any co-pending
 * db->delta_pending inserts) by
 *   (1) over-deleting every tuple transitively derived from a deleted base
 *       fact through POSITIVE body atoms (bounded cascade, stratum-ordered),
 *   (2) resetting (view = base) every head of a rule containing negation
 *       when any change is pending (negation retractions are not enumerable
 *       by the positive cascade — conservative full reset, propagated to
 *       dependents),
 *   (3) re-deriving the affected cone stratum-by-stratum (ADD-only fixpoint,
 *       so survivors and newly-derivable tuples — including tuples unlocked
 *       by a negated body atom becoming true — are re-added).
 * Base facts are never over-deleted (mixed EDB+IDB semantics: delete from
 * base; if the tuple is still derivable it survives in the view, else it
 * vanishes).  Frees both pending sets.  Returns 0 on success, -1 on error
 * (the publish path then forces a full re-eval — never mis-evaluate).
 *
 * PRECONDITION: vm_dred_eligible(db) == 1 (the publish dispatch enforces). */
int vm_dred_delete(dl_db *db);

/* Free all pending delete tuple_sets (after a full re-eval consumes the
 * pending changes, and on dl_close). */
void vm_clear_deletes(dl_db *db);

/* Test observable: number of times vm_dred_delete has run in this process.
 * Lets the equivalence-oracle tests PROVE the DRed path was taken (a test
 * that silently fell back to the full re-eval would also produce correct
 * views — this counter distinguishes them). */
extern int vm_dred_runs;

/* ─── IVM Slice 4: aggregates under change ──────────────────────────────── */

/* 1 if the program's AGGREGATE rules are all incrementally maintainable and
 * the surrounding program permits it:
 *   - non-recursive (aggregate rules are never recursive by construction);
 *   - every aggregate rule is TRACTABLE: its body is exactly ONE positive
 *     OP_SCAN anchor atom (pure-EDB — not a rule head), the group-by columns
 *     are a leading prefix of that atom IN GROUP-KEY ORDER, the result var is
 *     the LAST head column (group = leading prefix of the head tuple), and the
 *     source var (sum/min/max) is a column of the anchor.  count/sum/min/max
 *     are all handled by the SAME affected-group re-scan (uniform, so min/max
 *     delete-extremum and empty-group edges fall out for free);
 *   - every aggregate HEAD is TERMINAL (no rule body reads it) and has NO
 *     base facts (mixed EDB+IDB aggregate heads are out of scope);
 *   - non-aggregate rules are free of OP_WALK / OP_LOOKUP_PERM / OP_HASH_JOIN
 *     (they are maintained by the existing DRed / insert-propagator, which
 *     skip aggregate rules).  Stratified negation in non-aggregate rules is
 *     allowed (DRed handles it).
 * Returns 0 for anything that must fall back to the full fixpoint. */
int vm_agg_eligible(dl_db *db);

/* Incremental aggregate maintenance: for every affected aggregate rule,
 * re-scan ONLY the groups touched by the pending +/- deltas (O(group) per
 * affected group) and update the aggregate head view in place (delete the
 * stale head tuple, add the recomputed one; an emptied group drops its head
 * tuple).  Then delegates the NON-aggregate part of the program to the
 * existing DRed / insert-propagator (which skip aggregate rules).  Consumes
 * (frees) the pending +/- delta sets.  Returns 0 on success, -1 on error
 * (the publish path then forces a full re-eval — never mis-evaluate).
 *
 * PRECONDITION: vm_agg_eligible(db) == 1 (the publish dispatch enforces). */
int vm_agg_maintain(dl_db *db);

/* Test observable: number of times vm_agg_maintain has run in this process.
 * Proves the aggregate-IVM path was taken (not the full re-eval fallback). */
extern int vm_agg_runs;

/* IVM Slice 5: test observable — number of times vm_propagate_deltas has run
 * in this process.  Proves a BULK-LOAD publish routed through the insert
 * propagator (the batched-delta path) rather than a full re-eval fallback
 * (which would also produce correct views — the counter distinguishes them). */
extern int vm_propagate_runs;

/* Test observable: number of DISTINCT col0 values yielded by OP_RANGE lazy
 * generators (incremented once per successful range_resume).  Proves the lazy
 * path short-circuits under an early-stopping consumer. */
extern long vm_range_yields;

#endif
