/*
 * compiler.h — Datalog rule compiler: AST → bytecode
 *
 * Resolves relation/column refs against the dl_db, assigns binding-table
 * slots (var_id u8), and emits bytecode for the M2 VM.
 *
 * M2 adds: stratification, negation safety check, NEG_CHECK instruction,
 * fixpoint support via semi-naive evaluation in vm_execute.
 */
#ifndef COMPILER_H
#define COMPILER_H

#include "parser.h"
#include "dl.h"
#include "regexwalk.h"
#include <stdint.h>

#define MAX_VARS   64
#define MAX_ARITY  8

typedef enum {
    OP_HALT      = 0,
    OP_SCAN      = 1,
    OP_LOOKUP    = 2,
    OP_EQ        = 3,
    OP_EQ_CONST  = 4,
    OP_PROJECT   = 5,
    OP_OPEN_REL  = 6,
    OP_NEG_CHECK = 7,   /* M2: negated atom membership check */
    OP_AGG_ACC   = 8,   /* M3: accumulate a binding into a group bucket */
    OP_AGG_EMIT  = 9,   /* M3: emit one tuple per aggregate group */
    OP_WALK      = 10,  /* M5: SCAN with regex pattern filter */
    OP_LOOKUP_PERM = 11, /* M6: permuted prefix lookup */
    OP_HASH_JOIN   = 12, /* M6: real in-frame hash join */
} vm_opcode;

typedef struct {
    uint8_t  op;
    uint8_t  a, b, c;
    uint32_t imm;
    uint8_t  slots[8];
    uint8_t  body_idx;   /* M2: which body atom this instruction belongs to */
} vm_instr;

typedef struct {
    char    *name;
    uint8_t  slot;
} var_info;

typedef struct {
    char      *head_pred;
    uint8_t    head_rel_id;
    uint8_t    n_vars;
    var_info  *vars;
    int        n_instrs;
    vm_instr  *instrs;

    /* M2 fields */
    int        stratum;       /* assigned stratum (0-based) */
    int        is_recursive;  /* 1 if this rule's head appears in any body */

    /* M3 field */
    int        has_aggregate; /* 1 if this rule uses a grouped aggregate */

    /* M5 fields */
    int        n_patterns;    /* number of compiled regex DFAs */
    regex_dfa **patterns;     /* indexed by OP_WALK imm field */
} compiled_rule;

int  compile_rules(dl_db *db, rule **rules, int n_rules,
                   compiled_rule ***out_rules, int *out_n);
void compiled_rule_free(compiled_rule *cr);

#endif
