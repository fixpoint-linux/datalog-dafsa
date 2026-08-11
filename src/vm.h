/*
 * vm.h — Bytecode VM for the M2 Datalog evaluator
 *
 * Flat binding-table bytecode interpreter. Instructions:
 *   OP_SCAN, OP_LOOKUP, OP_EQ, OP_EQ_CONST, OP_PROJECT, OP_HALT,
 *   OP_NEG_CHECK.
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

#endif
