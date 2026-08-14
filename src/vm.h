/*
 * vm.h — Bytecode VM for the M2 Datalog evaluator
 *
 * Flat binding-table bytecode interpreter. Instructions:
 *   OP_SCAN, OP_LOOKUP, OP_EQ, OP_EQ_CONST, OP_PROJECT, OP_HALT,
 *   OP_NEG_CHECK, OP_AGG_ACC, OP_AGG_EMIT, OP_WALK, OP_LOOKUP_PERM,
 *   OP_HASH_JOIN, OP_CMP (M9 comparison), OP_ARITH (M9 arithmetic),
 *   OP_STR_FILTER / OP_STR_LEN / OP_STR_BIND (M9-strings).
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

/* ─── IVM Slice 1: insert-only incremental maintenance ────────────────── */

/* 1 if EVERY compiled rule is IVM-insert-eligible (non-recursive,
 * negation-free, aggregate-free, and every positive body atom compiles to an
 * override-compatible opcode — OP_SCAN/OP_LOOKUP).  Returns 0 if any rule
 * needs the full-fixpoint path (the correctness floor). */
int vm_ivm_eligible(dl_db *db);

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

#endif
