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
    OP_CMP         = 13, /* M9: comparison filter — a=lhs slot, b=rhs slot,
                             imm=cmp code 0=LT 1=LE 2=GT 3=GE 4=NE; both bound,
                             false -> backtrack */
    OP_ARITH       = 14, /* M9: arithmetic bind — a=lhs slot, b=rhs slot,
                             c=result temp slot, imm=arith code
                             0=ADD 1=SUB 2=MUL 3=DIV 4=MOD; result written via
                             b_try (never overwrite); DIV/MOD rhs==0 -> backtrack;
                             u32 wrap-around */
    OP_STR_FILTER  = 15, /* M9-strings: string filter — a=lhs slot, b=rhs slot,
                             imm=0 PREFIX 1 SUFFIX 2 CONTAINS; both operands are
                             interned symbols (intern_str_of NULL -> backtrack);
                             false -> backtrack */
    OP_STR_LEN     = 16, /* M9-strings: string length bind — a=operand slot,
                             c=result int temp slot; operand NULL via
                             intern_str_of -> backtrack; writes BYTE length
                             (strlen) via b_try */
    OP_STR_BIND    = 17, /* M9-strings: string-producing bind — a=lhs slot,
                             b=rhs slot (unused for unary ops), c=result temp
                             slot, imm=0 CONCAT (1 LOWER / 2 UPPER RESERVED);
                             builds the result into a heap buffer and calls
                             intern_str(db->ir, buf); intern==0 (OOM or result
                             > 4096 bytes) -> backtrack; else b_try(c, sym_id).
                             This is the ONLY opcode that interns at runtime. */
    OP_MAT_BEGIN   = 18, /* BUSHY (v2): materialize a subtree into a per-rule
                             intermediate buffer.  a=buf_idx, imm=end_ip (index
                             of the instruction AFTER the subtree), b=interface
                             arity, slots[0..b-1]=interface var slots in
                             [shared ++ private] canonical ascending-slot order.
                             Recursively exec_range(ip+1..imm) with FRESH
                             bindings, capturing the interface projection into
                             the pool tuple_set via ts_add, then ip=imm. */
    OP_MAT_JOIN    = 19, /* BUSHY (v2): hash-join two materialized buffers on
                             their leading shared columns.  a=L buf_idx,
                             b=R buf_idx, c=n_shared; slots[0..out_ar-1]=outer
                             var slot per output column, output layout
                             [shared(0..c-1) ++ L.private ++ R.private].  Pushes
                             a frame iterating the result (like OP_HASH_JOIN)
                             binding output cols into the current bindings. */
    OP_LIST_CONS   = 20, /* LISTS (v2): intern cons(b, c) into the term store
                             and bind result a.  a=result slot, b=head slot,
                             c=tail slot.  term_cons==0 (OOM or non-list tail)
                             -> backtrack. */
    OP_LIST_CAR    = 21, /* LISTS (v2): a=operand slot, c=result slot.  If
                             !is_list(a) or a==NIL -> backtrack; else bind
                             c = car(a) via b_try. */
    OP_LIST_CDR    = 22, /* LISTS (v2): a=operand slot, c=result slot
                             (symmetric to OP_LIST_CAR). */
    OP_LIST_APPEND = 23, /* LISTS (v2): a,b=operand slots, c=result slot.
                             !is_list(a) or !is_list(b) -> backtrack; else
                             bind c = append(a,b) via b_try. */
    OP_LIST_MEMBER = 24, /* LISTS (v2): a=list operand slot, b=member var slot.
                             L (a) must be bound; if b is bound this is a
                             linear-scan FILTER (b in L?), else a GENERATOR
                             pushing a frame that binds b to each element of
                             L in turn (mirrors OP_MAT_JOIN's frame).  L not a
                             list -> backtrack. */
    OP_RANGE       = 25, /* RANGE (v2): a=lo slot, b=hi slot, c=X slot,
                             imm=rel_id.  Rel must be EDB or NON-recursive IDB
                             (rejected at compile time if in a recursive SCC —
                             OP_RANGE reads db->rels[rel].rel directly, which
                             the recursive fixpoint never updates).  If X (c)
                             is bound: FILTER — Lo<=X<Hi AND rel_has_col0(rel,X);
                             else: GENERATOR — push a frame binding X to each
                             DISTINCT col0 value of Rel in [Lo,Hi), lex order
                             (mirrors OP_LIST_MEMBER's frame). */
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

/* ─── BUSHY (v2) compile-time toggles ────────────────────────────────────
 *
 * g_bushy (default 1): emit binary-tree (bushy) join plans for eligible
 * high-arity rules.  Set to 0 to FORCE LEFT-DEEP (the always-on reorder
 * still applies) — the correctness backstop used by the equivalence test.
 *
 * g_reorder (default 1): always-on greedy left-deep reorder of positive
 * atoms by ascending rel_count().  Set to 0 to restore the v1 body-order
 * left-deep emission.
 *
 * Both are plain globals so tests can flip them directly.  They must be set
 * BEFORE dl_compile; they are read at compile time and are NOT thread-safe
 * (single-threaded library).  No env-var initialization is provided. */
extern int g_bushy;
extern int g_reorder;

/*
 * Automatic perm-index selection (M6-permsel).
 *
 * g_perm_select (default 1): auto-declare and use OP_LOOKUP_PERM indices for
 * non-leading-column joins according to the cost model in emit_nonleading_join.
 * Set to 0 to force the OP_HASH_JOIN fallback (the oracle-equivalence
 * backstop — never builds a new perm index).  A recursive IDB body atom ALWAYS
 * uses OP_LOOKUP_PERM regardless of this flag (hash-joining it would read a
 * stale DAFSA and silently mis-evaluate).
 *
 * g_perm_card_threshold (default 4): minimum relation cardinality before a
 * perm index is deemed worth building.  Below it, non-leading joins use
 * OP_HASH_JOIN.  Only a performance hint — output is identical either way.
 *
 * Both are plain globals so tests can flip them directly, mirroring
 * g_bushy/g_reorder.  They must be set BEFORE dl_compile; they are read at
 * compile time and are NOT thread-safe (single-threaded library). */
extern int g_perm_select;
extern int g_perm_card_threshold;

#endif
