/*
 * compiler.c — Datalog rule compiler: AST → bytecode
 *
 * M2 adds: stratification pass, negation safety check, NEG_CHECK for
 * negated body atoms, stratum + is_recursive on compiled_rule.
 *
 * KNOWN LIMITATION (B6): int/symbol id collision.  A raw integer column
 * value and an interned sym_id share the u32 namespace.  If a relation
 * column mixes raw ints (e.g., 1) and interned symbols (e.g., 'a' →
 * some sym_id), they collide.  In M1, relations should keep each column
 * type-homogeneous (all ints or all symbols) to avoid this.
 *
 * M9 (arithmetic/comparison builtins) operates on the raw u32 interpretation
 * (option (a), no type tag), exactly like OP_EQ and min/max/sum already do.
 * Consequence of B6, documented here:
 *   - an ordering comparison on a SYMBOL column compares sym_ids (a
 *     well-defined total order, NOT lexicographic);
 *   - arithmetic on a symbol column is MEANINGLESS.  Arithmetic expression
 *     operands are restricted to TOK_VAR/TOK_INT at PARSE time (a
 *     TOK_IDENT/TOK_STRING constant is rejected as a bad expression factor),
 *     so no var-type inference is needed in M9 — there is no path that
 *     feeds a symbol constant into OP_ARITH.
 *   - `X != ident` is the one builtin form that interns a symbol constant and
 *     compares sym_ids; it is supported and consistent with the above.
 *
 * M9-strings (concat/length/lower/upper/prefix/suffix/contains) operates on
 * SYMBOLS.
 *   - operands are TOK_VAR or TOK_IDENT (a "double-quoted" string constant
 *     interned at compile time); a TOK_INT operand is REJECTED (a raw int is
 *     never a string, and a raw int like 1 collides with sym_id 1).
 *   - a VAR operand that is actually an int-typed column value colliding with
 *     a valid sym_id is the DOCUMENTED B6 limitation; the runtime backstop is
 *     intern_str_of -> NULL -> backtrack (never crash, never mis-evaluate).
 *   - length is the BYTE length (strlen over UTF-8 bytes), not codepoints.
 *   - concat/lower/upper intern their result at RUNTIME (OP_STR_BIND); a result
 *     longer than 4096 bytes makes intern_str return 0 -> backtrack.  New syms
 *     mark the interner dirty and persist on dl_close/dl_publish (identical to
 *     fact-load interning).
 */

#include "compiler.h"
#include "dl_internal.h"
#include "intern.h"
#include "relation.h"
#include "regexwalk.h"
#include "permindex.h"
#include "snapshot.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ─── dl_db internals access (authoritative layout in dl_internal.h) ─── */

static int db_find_rel(dl_db *db, const char *name)
{
    size_t i;
    for (i = 0; i < db->nrels; i++)
        if (strcmp(db->rels[i].name, name) == 0) return (int)i;
    return -1;
}

static uint8_t db_rel_arity(dl_db *db, int idx)
{
    if (idx < 0 || (size_t)idx >= db->nrels) return 0;
    return rel_arity(db->rels[idx].rel);
}

static const char *db_rel_name(dl_db *db, int idx)
{
    if (idx < 0 || (size_t)idx >= db->nrels) return NULL;
    return db->rels[idx].name;
}

static size_t db_rel_count(dl_db *db)
{
    return db->nrels;
}

static interner *db_get_interner(dl_db *db)
{ return db->ir; }

/* ─── Variable slot tracking ────────────────────────────────────────── */

typedef struct {
    char    *name;
    uint8_t  slot;
} v_entry;

typedef struct {
    v_entry *e;
    int      n, cap, err;
} v_tab;

static void v_init(v_tab *t) { memset(t, 0, sizeof(*t)); }
static void v_free(v_tab *t) {
    int i; for (i = 0; i < t->n; i++) free(t->e[i].name);
    free(t->e);
}
static int v_find(v_tab *t, const char *n) {
    int i; for (i = 0; i < t->n; i++) if (!strcmp(t->e[i].name, n)) return i;
    return -1;
}
static int v_add(v_tab *t, const char *n) {
    int i = v_find(t, n);
    if (i >= 0) return i;
    if (t->n >= MAX_VARS) { t->err = 1; return -1; }
    if (t->n >= t->cap) {
        int nc = t->cap ? t->cap * 2 : 8;
        v_entry *ne = realloc(t->e, (size_t)nc * sizeof(v_entry));
        if (!ne) { t->err = 1; return -1; }
        t->e = ne; t->cap = nc;
    }
    t->e[t->n].name = strdup(n);
    t->e[t->n].slot  = (uint8_t)t->n;
    if (!t->e[t->n].name) { t->err = 1; return -1; }
    return t->n++;
}

/* Generate a fresh reserved slot name "__X%d" (X = 'k' constant or 't' temp)
 * that does NOT collide with any existing variable in the table.  User
 * variables are all added to the table before emission, so any name already
 * present here is a user variable — skip past it (and any other aliases) so a
 * user variable named e.g. `__k0` or `__t0` can never be silently overwritten
 * by a compiler-generated constant/temp slot.  Writes into `buf` (size `cap`)
 * and returns it; the caller must pass the value of the name to v_add. */
static const char *v_fresh_name(v_tab *t, char *buf, size_t cap,
                                int *counter, char kind)
{
    for (;;) {
        snprintf(buf, cap, "__%c%d", kind, (*counter)++);
        if (v_find(t, buf) < 0) return buf;
    }
}

/* ─── Instruction buffer ────────────────────────────────────────────── */

typedef struct {
    vm_instr *b;
    int n, cap, err;
} i_buf;

static void i_init(i_buf *b) { memset(b, 0, sizeof(*b)); }
static void i_free(i_buf *b) { free(b->b); }
static vm_instr *i_emit(i_buf *b) {
    if (b->n >= b->cap) {
        int nc = b->cap ? b->cap * 2 : 16;
        vm_instr *nb = realloc(b->b, (size_t)nc * sizeof(vm_instr));
        if (!nb) { b->err = 1; return NULL; }
        b->b = nb; b->cap = nc;
    }
    memset(&b->b[b->n], 0, sizeof(vm_instr));
    return &b->b[b->n++];
}

/* ─── Parse a constant from a token ─────────────────────────────────── */

static int token_const(dl_db *db, const token *t, uint32_t *out)
{
    if (t->kind == TOK_INT) { *out = t->ival; return 0; }
    if (t->kind == TOK_IDENT) {
        *out = intern_str(db_get_interner(db), t->text);
        if (*out == 0) {
            /* Truncate the echoed constant so a >4096-byte string doesn't
             * flood stderr with the whole literal.  brief is built with plain
             * char arithmetic (no %zu) to stay clear of -Wformat-truncation. */
            char brief[48];
            size_t tl = strlen(t->text);
            size_t pos = 0, x, dd, i;
            char rev[16];

            if (tl >= sizeof(brief)) {
                size_t keep = sizeof(brief) - 14;   /* room for "... (N bytes)" */
                memcpy(brief, t->text, keep);
                pos = keep;
                memcpy(brief + pos, "... (", 5);
                pos += 5;
            } else {
                memcpy(brief, t->text, tl);
                pos = tl;
            }
            /* decimal byte count into rev */
            x = tl; dd = 0;
            if (x == 0) rev[dd++] = '0';
            while (x) { rev[dd++] = (char)('0' + x % 10); x /= 10; }
            for (i = 0; i < dd; i++) brief[pos + i] = rev[dd - 1 - i];
            pos += dd;
            memcpy(brief + pos, " bytes)", 8);
            pos += 8;
            brief[pos] = '\0';

            fprintf(stderr, "compile error: failed to intern string constant "
                    "'%s' (out of memory, or string exceeds the %d-byte "
                    "interner key limit)\n", brief, 4096);
            return -1;
        }
        return 0;
    }
    fprintf(stderr, "compile error: internal: unexpected token kind %d in a "
            "constant position\n", (int)t->kind);
    return -1;
}

/* ─── M9: builtin atom classification + arithmetic lowering ──────────── */

/* comparison atom: pred in {<,<=,>,>=,!=}, nargs==2 */
static int is_comparison(const atom *a)
{
    if (!a || !a->pred) return 0;
    return strcmp(a->pred, "<")  == 0 || strcmp(a->pred, "<=") == 0 ||
           strcmp(a->pred, ">")  == 0 || strcmp(a->pred, ">=") == 0 ||
           strcmp(a->pred, "!=") == 0;
}

/* arithmetic atom: `X = E` — pred "=" with an attached expr tree (nargs==1) */
static int is_arith(const atom *a)
{
    return a && a->pred && strcmp(a->pred, "=") == 0 && a->arith != NULL;
}

/* equality atom: `X = Y` — pred "=" with nargs==2 and no expr tree */
static int is_equality(const atom *a)
{
    return a && a->pred && strcmp(a->pred, "=") == 0 &&
           a->nargs == 2 && a->arith == NULL;
}

/* M9-strings: string builtin classification.  The lowercase builtin names
 * are RESERVED — a body atom whose pred matches one is ALWAYS treated as a
 * builtin, never as a relation reference.  In scope: concat + length
 * (producers) and prefix/suffix/contains (filters).  lower/upper are
 * DEFERRED (OP_STR_BIND imm 1/2 reserved; not classified, so a rule using
 * them fails loudly as an unknown predicate / bad arithmetic factor). */

/* producing string builtin: `VAR = concat(A,B)` / `VAR = length(S)` /
 * `VAR = lower(S)` / `VAR = upper(S)` — args[0] is the result var,
 * args[1..] are the operand tokens */
static int is_str_producing(const atom *a)
{
    if (!a || !a->pred) return 0;
    return strcmp(a->pred, "concat") == 0 ||
           strcmp(a->pred, "length") == 0 ||
           strcmp(a->pred, "lower")  == 0 ||
           strcmp(a->pred, "upper")  == 0;
}

/* string filter builtin: bare function-call atom `prefix(S,P)` etc. —
 * args[0..1] are the operand tokens */
static int is_str_filter(const atom *a)
{
    if (!a || !a->pred) return 0;
    return strcmp(a->pred, "prefix")   == 0 ||
           strcmp(a->pred, "suffix")   == 0 ||
           strcmp(a->pred, "contains") == 0;
}

/* any string builtin (producer or filter) */
static int is_str_builtin(const atom *a)
{
    return is_str_producing(a) || is_str_filter(a);
}

/* any non-relational builtin (equality / comparison / arithmetic / string) */
static int is_builtin_pred(const atom *a)
{
    return is_equality(a) || is_comparison(a) || is_arith(a) ||
           is_str_builtin(a);
}

/* A string-builtin operand token must be TOK_VAR or TOK_IDENT (a string
 * constant).  TOK_INT is rejected — a raw int is never a string, and a raw
 * int like 1 collides with sym_id 1 (the documented B6 int/symbol namespace
 * collision), so feeding it to a string builtin would silently
 * mis-evaluate. */
static int str_operand_ok(const token *t)
{
    return t && (t->kind == TOK_VAR || t->kind == TOK_IDENT);
}

/* Validate the arity + operand/result kinds of a string builtin.  Returns
 * 1 if well-formed, 0 otherwise (caller rejects loudly). */
static int str_builtin_valid(const atom *a)
{
    int i, need;
    if (!a || !a->pred) return 0;
    if (is_str_producing(a)) {
        need = strcmp(a->pred, "concat") == 0 ? 3 : 2;
        if (a->nargs != need) return 0;
        if (a->args[0]->kind != TOK_VAR) return 0;
        for (i = 1; i < need; i++)
            if (!str_operand_ok(a->args[i])) return 0;
        return 1;
    }
    if (is_str_filter(a)) {
        if (a->nargs != 2) return 0;
        for (i = 0; i < 2; i++)
            if (!str_operand_ok(a->args[i])) return 0;
        return 1;
    }
    return 0;
}

/* string filter pred -> OP_STR_FILTER imm code */
static int str_filter_code(const char *pred)
{
    if (!strcmp(pred, "prefix")) return 0;
    if (!strcmp(pred, "suffix")) return 1;
    return 2;  /* contains */
}

/* string-producing builtin imm/opcode encoding.  Returns OP_STR_BIND imm for
 * concat/lower/upper (0/1/2), or -1 for length (which is OP_STR_LEN). */
static int str_bind_imm(const char *pred)
{
    if (!strcmp(pred, "concat")) return 0;
    if (!strcmp(pred, "lower"))  return 1;
    if (!strcmp(pred, "upper"))  return 2;
    return -1;  /* length */
}

/* comparison operator string -> OP_CMP imm code */
static int cmp_op_code(const char *pred)
{
    if (!strcmp(pred, "<"))  return 0;
    if (!strcmp(pred, "<=")) return 1;
    if (!strcmp(pred, ">"))  return 2;
    if (!strcmp(pred, ">=")) return 3;
    return 4;  /* != */
}

/* arithmetic operator char -> OP_ARITH imm code */
static int arith_op_code(char op)
{
    switch (op) {
        case '+': return 0;
        case '-': return 1;
        case '*': return 2;
        case '/': return 3;
        default:  return 4;  /* % */
    }
}

/* collect every variable in an expr tree into the var table (so v_find can
 * resolve operand vars referenced only inside arithmetic) */
static void collect_expr_vars(const expr *e, v_tab *vt)
{
    if (!e) return;
    if (e->kind == EX_VAR) {
        v_add(vt, e->var);
    } else if (e->kind == EX_BINOP) {
        collect_expr_vars(e->l, vt);
        collect_expr_vars(e->r, vt);
    }
}

/* does an expr tree contain a division/modulo by a literal 0? */
static int expr_has_div0(const expr *e)
{
    if (!e) return 0;
    if (e->kind == EX_BINOP) {
        if ((e->op == '/' || e->op == '%') && e->r &&
            e->r->kind == EX_INT && e->r->ival == 0)
            return 1;
        return expr_has_div0(e->l) || expr_has_div0(e->r);
    }
    return 0;
}

/* require every variable operand of an expr tree to be bound; returns 0 on
 * the first unbound operand (with a diagnostic), 1 if all bound */
static int expr_vars_bound(const expr *e, v_tab *vt, const int *bound_vars,
                           const char *head_pred)
{
    if (!e) return 1;
    if (e->kind == EX_VAR) {
        int vi = v_find(vt, e->var);
        if (vi < 0 || !bound_vars[vi]) {
            fprintf(stderr,
                "compile error: ungrounded arithmetic operand — variable "
                "'%s' is not bound by a positive body atom (rule '%s')\n",
                e->var, head_pred);
            return 0;
        }
        return 1;
    }
    if (e->kind == EX_BINOP)
        return expr_vars_bound(e->l, vt, bound_vars, head_pred) &&
               expr_vars_bound(e->r, vt, bound_vars, head_pred);
    return 1;
}

/* Lower an expr tree postorder into bytecode: EX_INT -> OP_EQ_CONST(__kN),
 * EX_VAR -> its var slot, EX_BINOP -> OP_ARITH(lhs, rhs, fresh __tN temp).
 * Returns the slot index holding the tree's value, or -1 on error. */
static int lower_expr(dl_db *db, const expr *e, v_tab *vt, i_buf *ib,
                      int *cc, int *tc, int body_idx)
{
    char cname[16];
    int vi;
    vm_instr *in;

    if (!e) return -1;
    switch (e->kind) {
    case EX_INT:
        v_fresh_name(vt, cname, sizeof(cname), cc, 'k');
        vi = v_add(vt, cname);
        if (vi < 0) return -1;
        in = i_emit(ib);
        if (!in) return -1;
        in->op = OP_EQ_CONST;
        in->a = vt->e[vi].slot;
        in->imm = e->ival;
        return (int)vt->e[vi].slot;
    case EX_VAR: {
        int v = v_find(vt, e->var);
        return (v >= 0) ? (int)vt->e[v].slot : -1;
    }
    case EX_BINOP: {
        int ls = lower_expr(db, e->l, vt, ib, cc, tc, body_idx);
        int rs = lower_expr(db, e->r, vt, ib, cc, tc, body_idx);
        if (ls < 0 || rs < 0) return -1;
        v_fresh_name(vt, cname, sizeof(cname), tc, 't');
        vi = v_add(vt, cname);
        if (vi < 0) return -1;
        in = i_emit(ib);
        if (!in) return -1;
        in->op = OP_ARITH;
        in->a = (uint8_t)ls;
        in->b = (uint8_t)rs;
        in->c = vt->e[vi].slot;
        in->imm = (uint32_t)arith_op_code(e->op);
        in->body_idx = (uint8_t)body_idx;
        return (int)vt->e[vi].slot;
    }
    default:
        return -1;
    }
}

/* Resolve a comparison operand to a slot: variables use their var slot,
 * constants (TOK_INT, or TOK_IDENT for `!=`) are materialized into a fresh
 * __kN slot via OP_EQ_CONST.  Returns the slot, or -1 on error. */
static int cmp_operand_slot(dl_db *db, const token *t, v_tab *vt, i_buf *ib,
                            int *cc)
{
    if (t->kind == TOK_VAR) {
        int vi = v_find(vt, t->text);
        return (vi >= 0) ? (int)vt->e[vi].slot : -1;
    }
    {
        char cname[16];
        int vi;
        uint32_t cv;
        vm_instr *eq;
        v_fresh_name(vt, cname, sizeof(cname), cc, 'k');
        vi = v_add(vt, cname);
        if (vi < 0) return -1;
        if (token_const(db, t, &cv)) return -1;
        eq = i_emit(ib);
        if (!eq) return -1;
        eq->op = OP_EQ_CONST;
        eq->a = vt->e[vi].slot;
        eq->imm = cv;
        return (int)vt->e[vi].slot;
    }
}

/* require every VARIABLE operand of a producing string builtin (args[1..])
 * to be bound; returns 0 on the first unbound operand (with a diagnostic),
 * 1 if all bound.  Constants are always "bound". */
static int str_producing_operands_bound(const atom *a, v_tab *vt,
                                        const int *bound_vars,
                                        const char *head_pred)
{
    int j;
    for (j = 1; j < a->nargs; j++) {
        token *t = a->args[j];
        if (t->kind != TOK_VAR) continue;
        {
            int vi = v_find(vt, t->text);
            if (vi < 0 || !bound_vars[vi]) {
                fprintf(stderr,
                    "compile error: ungrounded string operand — variable "
                    "'%s' in '%s' is not bound by a positive body atom "
                    "(rule '%s')\n", t->text, a->pred, head_pred);
                return 0;
            }
        }
    }
    return 1;
}

/* require every VARIABLE operand of a string filter (args[0..1]) to be
 * bound; returns 0 on the first unbound operand (with a diagnostic),
 * 1 if all bound. */
static int str_filter_operands_bound(const atom *a, v_tab *vt,
                                     const int *bound_vars,
                                     const char *head_pred)
{
    int j;
    for (j = 0; j < a->nargs; j++) {
        token *t = a->args[j];
        if (t->kind != TOK_VAR) continue;
        {
            int vi = v_find(vt, t->text);
            if (vi < 0 || !bound_vars[vi]) {
                fprintf(stderr,
                    "compile error: ungrounded string filter — variable "
                    "'%s' in '%s' is not bound by a positive body atom "
                    "(rule '%s')\n", t->text, a->pred, head_pred);
                return 0;
            }
        }
    }
    return 1;
}

/* ─── Stratification ────────────────────────────────────────────────── */

/*
 * Build dependency graph and assign strata.
 *
 * For each rule head :- body1, ..., bodyN:
 *   Add positive edge: body_pred -> head_pred
 *   For negated body atoms: add negative edge: body_pred -> head_pred
 *
 * Assign strata via iterative fixpoint:
 *   stratum(r) initialized to 0
 *   For positive edge p->q: stratum(q) >= stratum(p)
 *   For negative edge p->q: stratum(q) >  stratum(p)
 *
 * If after fixpoint any negative edge p->q has stratum(q) <= stratum(p),
 * the program is un-stratifiable (negation through recursion).
 *
 * Also computes SCCs on the positive subgraph (Kosaraju) and sets
 * out_recursive[ri]=1 for predicates in SCCs with >1 node or with
 * self-loops.  Fixes B3 (is_recursive over-marking).
 *
 * Returns 0 on success, -1 on unstratifiable error.
 */
static int compute_strata(dl_db *db, rule **rules, int n_rules,
                          int *out_strata, int *out_recursive, int out_max)
{
    size_t nrels = db_rel_count(db);
    int *stratum;
    int *comp = NULL;   /* M8: SCC id per relation (Kosaraju root), kept for
                           the strict-stratification pass */
    int i, changed, iteration;
    size_t ri;

    if (nrels == 0 || out_max < 1) return 0;

    stratum = calloc(nrels, sizeof(int));
    if (!stratum) return -1;

    /* Collect edges from rules */
    typedef struct { int from; int to; int is_neg; } edge_t;
    edge_t *edges = NULL;
    int n_edges = 0, edges_cap = 0;
#define ADD_EDGE(f, t, neg) do { \
    if (n_edges >= edges_cap) { \
        int nc = edges_cap ? edges_cap * 2 : 32; \
        edge_t *ne = realloc(edges, (size_t)nc * sizeof(edge_t)); \
        if (!ne) { free(edges); free(stratum); return -1; } \
        edges = ne; edges_cap = nc; \
    } \
    edges[n_edges].from = (f); \
    edges[n_edges].to   = (t); \
    edges[n_edges].is_neg = (neg); \
    n_edges++; \
} while(0)

    /* Also track self-loops: a rule whose head also appears in its body */
    int *self_loop = calloc(nrels, sizeof(int));
    if (!self_loop) { free(stratum); return -1; }

    for (i = 0; i < n_rules; i++) {
        rule *r = rules[i];
        int head_ri = db_find_rel(db, r->head->pred);
        if (head_ri < 0) continue;
        int bi;
        for (bi = 0; bi < r->nbody; bi++) {
            atom *ba = r->body[bi];
            int body_ri = db_find_rel(db, ba->pred);
            if (body_ri < 0) continue;
            ADD_EDGE(body_ri, head_ri, ba->negated);
            if (body_ri == head_ri) self_loop[head_ri] = 1;
        }
    }

    /* Iterative fixpoint stratum assignment */
    iteration = 0;
    do {
        changed = 0;
        for (i = 0; i < n_edges; i++) {
            int from_s = stratum[edges[i].from];
            int to_s   = stratum[edges[i].to];
            int needed;

            if (edges[i].is_neg) {
                needed = from_s + 1;
                if (needed > 1000000) needed = 1000000;
            } else {
                needed = from_s;
            }

            if (to_s < needed) {
                stratum[edges[i].to] = needed;
                changed = 1;
            }
        }
        iteration++;
    } while (changed && iteration < 10000);

    /* ── Kosaraju SCC on positive subgraph ────────────────────────── */
    /* Build adjacency list for positive edges only */
    {
        /* Count out-degrees */
        int *out_deg = calloc(nrels, sizeof(int));
        int *rev_deg = calloc(nrels, sizeof(int));
        int **adj = NULL, **rev_adj = NULL;
        int *adj_cap = NULL, *rev_cap = NULL;
        int *visited = NULL, *order = NULL;
        int order_n = 0;

        if (!out_deg) { free(self_loop); free(edges); free(stratum); return -1; }
        if (!rev_deg) { free(out_deg); free(self_loop); free(edges); free(stratum); return -1; }

        for (i = 0; i < n_edges; i++) {
            if (!edges[i].is_neg) {
                out_deg[edges[i].from]++;
                rev_deg[edges[i].to]++;
            }
        }

        adj = calloc(nrels, sizeof(int *));
        rev_adj = calloc(nrels, sizeof(int *));
        adj_cap = calloc(nrels, sizeof(int));
        rev_cap = calloc(nrels, sizeof(int));
        if (!adj || !rev_adj || !adj_cap || !rev_cap) goto scc_cleanup;

        for (ri = 0; ri < nrels; ri++) {
            if (out_deg[ri] > 0) {
                adj[ri] = malloc((size_t)out_deg[ri] * sizeof(int));
                if (!adj[ri]) goto scc_cleanup;
                adj_cap[ri] = out_deg[ri];
                out_deg[ri] = 0; /* reuse as write cursor */
            }
            if (rev_deg[ri] > 0) {
                rev_adj[ri] = malloc((size_t)rev_deg[ri] * sizeof(int));
                if (!rev_adj[ri]) goto scc_cleanup;
                rev_cap[ri] = rev_deg[ri];
                rev_deg[ri] = 0;
            }
        }

        for (i = 0; i < n_edges; i++) {
            if (!edges[i].is_neg) {
                int f = edges[i].from, t = edges[i].to;
                adj[f][out_deg[f]++] = t;
                rev_adj[t][rev_deg[t]++] = f;
            }
        }

        /* First pass: DFS on forward graph for postorder */
        visited = calloc(nrels, sizeof(int));
        order = malloc(nrels * sizeof(int));
        comp = malloc(nrels * sizeof(int));
        if (!visited || !order || !comp) goto scc_cleanup;

        /* Iterative DFS for postorder (avoid stack overflow on large graphs) */
        {
            /* Use explicit stack: (node, neighbor_index) pairs */
            int *stack_node = malloc((nrels + 1) * sizeof(int));
            int *stack_idx  = malloc((nrels + 1) * sizeof(int));
            int sp = 0;
            if (!stack_node || !stack_idx) {
                free(stack_node); free(stack_idx);
                goto scc_cleanup;
            }
            for (ri = 0; ri < nrels; ri++) {
                if (visited[ri]) continue;
                visited[ri] = 1;
                stack_node[sp] = (int)ri;
                stack_idx[sp] = 0;
                sp++;
                while (sp > 0) {
                    int node = stack_node[sp - 1];
                    int idx  = stack_idx[sp - 1];
                    int found = 0;
                    while (idx < adj_cap[node]) {
                        int nb = adj[node][idx];
                        stack_idx[sp - 1] = idx + 1;
                        if (!visited[nb]) {
                            visited[nb] = 1;
                            stack_node[sp] = nb;
                            stack_idx[sp] = 0;
                            sp++;
                            found = 1;
                            break;
                        }
                        idx = stack_idx[sp - 1];
                    }
                    if (!found) {
                        order[order_n++] = node;
                        sp--;
                    }
                }
            }
            free(stack_node);
            free(stack_idx);
        }

        /* Second pass: DFS on reverse graph in reverse postorder */
        memset(visited, 0, nrels * sizeof(int));
        {
            int *stack_node = malloc((nrels + 1) * sizeof(int));
            int *stack_idx  = malloc((nrels + 1) * sizeof(int));
            int sp = 0;
            if (!stack_node || !stack_idx) {
                free(stack_node); free(stack_idx);
                goto scc_cleanup;
            }
            for (i = order_n - 1; i >= 0; i--) {
                int root = order[i];
                if (visited[root]) continue;
                int scc_size = 0;
                visited[root] = 1;
                stack_node[sp] = root;
                stack_idx[sp] = 0;
                sp++;
                while (sp > 0) {
                    int node = stack_node[sp - 1];
                    int idx  = stack_idx[sp - 1];
                    int found = 0;
                    while (idx < rev_cap[node]) {
                        int nb = rev_adj[node][idx];
                        stack_idx[sp - 1] = idx + 1;
                        if (!visited[nb]) {
                            visited[nb] = 1;
                            stack_node[sp] = nb;
                            stack_idx[sp] = 0;
                            sp++;
                            found = 1;
                            break;
                        }
                        idx = stack_idx[sp - 1];
                    }
                    if (!found) {
                        comp[node] = root; /* assign SCC id = root */
                        scc_size++;
                        sp--;
                    }
                }
                /* Mark SCC as recursive if size > 1 or has self-loop */
                if (scc_size > 1) {
                    for (ri = 0; ri < nrels; ri++)
                        if (comp[ri] == root) out_recursive[ri] = 1;
                }
            }
            free(stack_node);
            free(stack_idx);
        }

        /* Self-loops also make a predicate recursive */
        for (ri = 0; ri < nrels; ri++)
            if (self_loop[ri]) out_recursive[ri] = 1;

    scc_cleanup:
        for (ri = 0; ri < nrels; ri++) {
            free(adj[ri]);
            free(rev_adj[ri]);
        }
        free(adj); free(rev_adj);
        free(adj_cap); free(rev_cap);
        free(out_deg); free(rev_deg);
        free(visited); free(order);
    }

    /* ── Strict stratification for non-recursive SCC dependents ──── */
    /* M2.1 fix: a non-recursive predicate Q that depends on a recursive
     * SCC P must be assigned a STRICTLY higher stratum than P.
     * Otherwise the semi-naive delta loop (which only commits candidates
     * for recursive heads) silently discards Q's output during the
     * fixpoint, and Q is only ever evaluated during the seed phase —
     * before the recursive fixpoint completes. */

    /* Seed recursion from ALREADY-COMPILED rules.  Rules may be loaded in
     * several dl_load_rules batches; a later batch's non-recursive rule that
     * depends on an EARLIER batch's recursive predicate sees no self-loop in
     * its own edge set, so the SCC pass above would not mark that predicate
     * recursive and the strict-stratification bump below would never fire.
     * With the IVM base/view reset, evaluating such a dependent in the same
     * stratum would read the recursive head BEFORE it is (re-)materialized —
     * a silent wrong answer.  Carry recursion across batches. */
    for (i = 0; i < db->n_crules; i++) {
        compiled_rule *pcr = db->crules[i];
        if (pcr->is_recursive && pcr->head_rel_id < (uint8_t)nrels)
            out_recursive[pcr->head_rel_id] = 1;
    }

    {
        int changed, iter2 = 0;
        do {
            changed = 0;
            for (i = 0; i < n_edges; i++) {
                int from_s = stratum[edges[i].from];
                int to_s   = stratum[edges[i].to];
                int needed;

                if (edges[i].is_neg) {
                    /* Negative edge: strict inequality always */
                    needed = from_s + 1;
                    if (needed > 1000000) needed = 1000000;
                } else if (out_recursive[edges[i].from]
                           && comp
                           && comp[edges[i].from] != comp[edges[i].to]) {
                    /* Positive edge from a recursive SCC to a predicate in
                     * a DIFFERENT SCC.  This covers two cases:
                     *   1. recursive -> NON-recursive dependent (original
                     *      M2.1 fix): the dependent needs the source's full
                     *      fixpoint, so it must be in a later stratum.
                     *   2. recursive -> recursive CHAINED dependent (M8
                     *      magic-sets: tc^bf depends on magic_tc^bf, both
                     *      recursive, distinct SCCs).  Same requirement.
                     *      Previously only case 1 was handled, so chained
                     *      recursive predicates were silently mis-evaluated
                     *      (the semi-naive loop only exposes an SCC's idb to
                     *      its own rules, so a same-stratum dependent reads a
                     *      stale relation). */
                    needed = from_s + 1;
                    if (needed > 1000000) needed = 1000000;
                } else {
                    /* Standard positive edge: >= */
                    needed = from_s;
                }

                if (to_s < needed) {
                    stratum[edges[i].to] = needed;
                    changed = 1;
                }
            }
            iter2++;
        } while (changed && iter2 < 10000);
    }

    free(comp);
    comp = NULL;

    /* Check for unstratifiable (after both fixpoint passes) */
    for (i = 0; i < n_edges; i++) {
        if (edges[i].is_neg) {
            if (stratum[edges[i].to] <= stratum[edges[i].from]) {
                fprintf(stderr,
                    "compile error: unstratifiable program — negation "
                    "through recursion: '%s' depends negatively on '%s' "
                    "in the same SCC\n",
                    db_rel_name(db, edges[i].from),
                    db_rel_name(db, edges[i].to));
                free(self_loop); free(edges); free(stratum);
                return -1;
            }
        }
    }

    free(self_loop);
    free(edges);

    /* Copy out */
    for (ri = 0; ri < nrels; ri++)
        out_strata[ri] = stratum[ri];

    free(stratum);
    return 0;
#undef ADD_EDGE
}

/* ─── Compile one rule ──────────────────────────────────────────────── */

static compiled_rule *compile_one(dl_db *db, rule *r, int *rel_strata)
{
    v_tab vt; i_buf ib; compiled_rule *cr = NULL;
    int head_ri, *bri = NULL, bi, i, j;
    int cc = 0;  /* counter for unique constant slot names */
    int tc = 0;  /* M9: counter for unique arithmetic temp slot names */
    int agg_body_idx = -1;

    v_init(&vt); i_init(&ib);

    /* M5: compiled regex patterns for body atoms with ~ */
    regex_dfa **pat_dfa = NULL;
    int         n_pat   = 0;
    int         pat_cap = 0;
    uint8_t    *pat_idx = NULL;  /* pat_idx[bi] = pattern index, or 0xFF if none */

    /* ── M5: compile patterns from body atoms ────────────────────────── */
    pat_idx = malloc((size_t)r->nbody * sizeof(uint8_t));
    if (!pat_idx) goto fail;
    for (i = 0; i < r->nbody; i++)
        pat_idx[i] = 0xFF;

    for (bi = 0; bi < r->nbody; bi++) {
        atom *ba = r->body[bi];
        if (!ba->pattern) continue;
        if (ba->negated) {
            fprintf(stderr, "compile error: negated pattern atom not supported "
                    "(rule '%s')\n", r->head->pred); goto fail;
        }
        regex_dfa *dfa = regex_compile(ba->pattern);
        if (dfa->errmsg) {
            fprintf(stderr, "compile error: bad regex pattern '%s': %s "
                    "(rule '%s')\n",
                    ba->pattern, dfa->errmsg, r->head->pred);
            regex_dfa_free(dfa);
            goto fail;
        }
        if (n_pat >= pat_cap) {
            int nc = pat_cap ? pat_cap * 2 : 4;
            regex_dfa **np = realloc(pat_dfa,
                                     (size_t)nc * sizeof(regex_dfa *));
            if (!np) { regex_dfa_free(dfa); goto fail; }
            pat_dfa = np;
            pat_cap = nc;
        }
        pat_idx[bi] = (uint8_t)n_pat;
        pat_dfa[n_pat++] = dfa;
    }

    /* ── 1. detect M3 aggregate body atom ─────────────────────────── */
    for (bi = 0; bi < r->nbody; bi++) {
        if (r->body[bi]->aggregate) {
            if (agg_body_idx >= 0) {
                fprintf(stderr, "compile error: multiple aggregates not supported "
                        "(rule '%s')\n", r->head->pred); goto fail;
            }
            agg_body_idx = bi;
        }
    }
    atom *agg = (agg_body_idx >= 0) ? r->body[agg_body_idx] : NULL;
    if (agg && agg->negated) {
        fprintf(stderr, "compile error: aggregate inside negation not supported "
                "(rule '%s')\n", r->head->pred); goto fail;
    }
    int agg_op_code = -1;
    const char *agg_src_var = NULL;
    if (agg) {
        const char *opname = agg->agg_op ? agg->agg_op->text : "";
        if (!strcmp(opname, "count"))       agg_op_code = 0;
        else if (!strcmp(opname, "sum"))    agg_op_code = 1;
        else if (!strcmp(opname, "min"))    agg_op_code = 2;
        else if (!strcmp(opname, "max"))    agg_op_code = 3;
        else {
            fprintf(stderr, "compile error: unknown aggregate '%s' (rule '%s')\n",
                    opname, r->head->pred); goto fail;
        }
        if (agg_op_code == 0) {
            if (agg->nargs != 0) {
                fprintf(stderr, "compile error: 'count' takes no arguments (rule '%s')\n",
                        r->head->pred); goto fail;
            }
        } else {
            if (agg->nargs != 1 || agg->args[0]->kind != TOK_VAR) {
                fprintf(stderr, "compile error: 'sum/min/max' require a source variable "
                        "(rule '%s')\n", r->head->pred); goto fail;
            }
            agg_src_var = agg->args[0]->text;
        }
    }

    /* ── 2. resolve head ─────────────────────────────────────────── */
    head_ri = db_find_rel(db, r->head->pred);
    if (head_ri < 0) {
        if (r->head->nargs < 1 || r->head->nargs > MAX_ARITY) {
            fprintf(stderr, "compile error: head arity %d for '%s'\n",
                    r->head->nargs, r->head->pred); goto fail;
        }
        if (dl_declare_relation(db, r->head->pred, (uint8_t)r->head->nargs))
        { fprintf(stderr, "compile error: cannot declare '%s/%d'\n",
                   r->head->pred, r->head->nargs); goto fail; }
        head_ri = db_find_rel(db, r->head->pred);
        if (head_ri < 0) goto fail;
    } else {
        if (db_rel_arity(db, head_ri) != (uint8_t)r->head->nargs) {
            fprintf(stderr, "compile error: arity mismatch for '%s': %d vs %d\n",
                    r->head->pred, db_rel_arity(db, head_ri), r->head->nargs);
            goto fail;
        }
    }

    /* ── 3. resolve body ─────────────────────────────────────────── */
    if (r->nbody == 0) {
        fprintf(stderr, "compile error: rule '%s' has no body\n", r->head->pred);
        goto fail;
    }
    /* v1: reject constants in the head of an aggregate rule — all head
     * args must be the result var or group-by vars. */
    if (agg) {
        for (i = 0; i < r->head->nargs; i++) {
            if (r->head->args[i]->kind != TOK_VAR) {
                fprintf(stderr, "compile error: constants in head of aggregate rule not "
                        "supported (rule '%s')\n", r->head->pred); goto fail;
            }
        }
    }
    bri = malloc((size_t)r->nbody * sizeof(int));
    if (!bri) goto fail;
    for (bi = 0; bi < r->nbody; bi++) {
        atom *ba = r->body[bi];
        /* aggregates and builtins (equality/comparison/arithmetic/string) are
         * not relation references */
        if (ba->aggregate) { bri[bi] = -1; continue; }
        if (is_builtin_pred(ba)) {
            if (ba->negated) {
                fprintf(stderr, "compile error: negated builtin not supported "
                        "(rule '%s')\n", r->head->pred); goto fail;
            }
            /* M9-strings: validate arity/operand/result kinds so a malformed
             * string builtin (bad arity, INT operand) is rejected loudly
             * rather than silently mis-evaluated. */
            if (is_str_builtin(ba) && !str_builtin_valid(ba)) {
                fprintf(stderr, "compile error: malformed string builtin "
                        "'%s' (bad arity or non-symbol operand) "
                        "(rule '%s')\n", ba->pred, r->head->pred);
                goto fail;
            }
            bri[bi] = -1; continue;
        }
        int ri = db_find_rel(db, ba->pred);
        if (ri < 0) {
            fprintf(stderr, "compile error: unknown predicate '%s'\n",
                    ba->pred); goto fail;
        }
        if (db_rel_arity(db, ri) != (uint8_t)ba->nargs) {
            fprintf(stderr, "compile error: arity mismatch for '%s'\n",
                    ba->pred); goto fail;
        }
        bri[bi] = ri;
    }

    /* ── 4. collect vars ─────────────────────────────────────────── */
    for (i = 0; i < r->head->nargs; i++)
        if (r->head->args[i]->kind == TOK_VAR && v_add(&vt, r->head->args[i]->text) < 0) goto fail;
    for (i = 0; i < r->nbody; i++)
        for (j = 0; j < r->body[i]->nargs; j++)
            if (r->body[i]->args[j]->kind == TOK_VAR && v_add(&vt, r->body[i]->args[j]->text) < 0) goto fail;
    /* M9: arithmetic operand vars referenced only inside an expr tree must
     * also be in the var table (so v_find resolves them during safety checks
     * and lowering). */
    for (i = 0; i < r->nbody; i++)
        if (is_arith(r->body[i]))
            collect_expr_vars(r->body[i]->arith, &vt);
    if (agg) {
        /* aggregate result var and source var are vars like any other */
        if (v_add(&vt, agg->pred) < 0) goto fail;
        if (agg_src_var && v_add(&vt, agg_src_var) < 0) goto fail;
    }
    if (vt.err) goto fail;

    /* ── 5. negation safety check ────────────────────────────────── */
    {
        int bound_vars[256] = {0};
        /* Only positive body atoms (in left-to-right order) ground
         * variables for negation safety.  Head variables are NOT
         * considered bound — a variable must appear in a positive
         * body atom BEFORE any negated atom that references it.
         * Fixes B1 (negation safety bug). */
        for (bi = 0; bi < r->nbody; bi++) {
            atom *ba = r->body[bi];
            if (ba->aggregate) continue;          /* computed after body */
            if (is_equality(ba)) {
                /* equality X = Y binds at most ONE new side (OP_EQ binds Y
                 * from X when X is bound, and is a NO-OP when both are
                 * unbound).  So propagate binding only when at least one side
                 * is already bound — otherwise a later negated atom reading X
                 * would silently run NEG_CHECK against the UNBOUND sentinel
                 * (0xFFFFFFFF) instead of being rejected as unsafe. */
                int vi0 = (ba->nargs >= 1 && ba->args[0]->kind == TOK_VAR)
                              ? v_find(&vt, ba->args[0]->text) : -1;
                int vi1 = (ba->nargs >= 2 && ba->args[1]->kind == TOK_VAR)
                              ? v_find(&vt, ba->args[1]->text) : -1;
                int b0 = (vi0 >= 0) && bound_vars[vi0];
                int b1 = (vi1 >= 0) && bound_vars[vi1];
                if (b0 || b1) {
                    if (vi0 >= 0) bound_vars[vi0] = 1;
                    if (vi1 >= 0) bound_vars[vi1] = 1;
                }
                continue;
            }
            if (is_arith(ba) || is_comparison(ba) || is_str_builtin(ba)) {
                /* builtins execute AFTER the relational phase (NEG_CHECK runs
                 * during it), so their vars are NOT bound for negation-safety
                 * purposes — a negated atom cannot safely read an arithmetic /
                 * string-builtin result var or a comparison / string-filter
                 * operand. */
                continue;
            }
            if (ba->negated) {
                for (j = 0; j < ba->nargs; j++) {
                    if (ba->args[j]->kind == TOK_VAR) {
                        int vi = v_find(&vt, ba->args[j]->text);
                        if (vi < 0 || !bound_vars[vi]) {
                            fprintf(stderr,
                                "compile error: unsafe negation — variable "
                                "'%s' in negated atom '%s' is not bound "
                                "by a positive body atom (rule '%s')\n",
                                ba->args[j]->text, ba->pred,
                                r->head->pred);
                            goto fail;
                        }
                    }
                }
            } else {
                for (j = 0; j < ba->nargs; j++) {
                    if (ba->args[j]->kind == TOK_VAR) {
                        int vi = v_find(&vt, ba->args[j]->text);
                        if (vi >= 0) bound_vars[vi] = 1;
                    }
                }
            }
        }
    }

    /* ── 5b. M9 builtin-safety pass ──────────────────────────────── */
    /* Bound-set model mirrors the runtime emission order: relational atoms
     * all bind first (pre-seeded), then equality (runs before arithmetic),
     * then arithmetic in body order, then comparisons.  An ungrounded
     * comparison/arithmetic operand is rejected LOUDLY (never silently
     * mis-evaluated). */
    {
        int bound_vars[256] = {0};

        /* (a) seed from ALL positive RELATIONAL atoms (regardless of their
         *     text position — they all execute before any builtin). */
        for (bi = 0; bi < r->nbody; bi++) {
            atom *ba = r->body[bi];
            if (ba->negated || ba->aggregate || is_builtin_pred(ba)) continue;
            for (j = 0; j < ba->nargs; j++)
                if (ba->args[j]->kind == TOK_VAR) {
                    int vi = v_find(&vt, ba->args[j]->text);
                    if (vi >= 0) bound_vars[vi] = 1;
                }
        }
        /* (b) equality propagation (equality runs before arithmetic).  An
         * equality X = Y binds at most ONE new side: OP_EQ binds Y from X when
         * X is already bound (or X from Y when Y is bound), and is a NO-OP when
         * both are unbound.  So propagate binding only when at least one side
         * is already bound — otherwise leave both unbound so a downstream
         * arithmetic/comparison consumer is rejected as ungrounded instead of
         * silently reading the UNBOUND sentinel (0xFFFFFFFF). */
        for (bi = 0; bi < r->nbody; bi++) {
            atom *ba = r->body[bi];
            int vi0, vi1, b0, b1;
            if (!is_equality(ba)) continue;
            vi0 = (ba->nargs >= 1 && ba->args[0]->kind == TOK_VAR)
                      ? v_find(&vt, ba->args[0]->text) : -1;
            vi1 = (ba->nargs >= 2 && ba->args[1]->kind == TOK_VAR)
                      ? v_find(&vt, ba->args[1]->text) : -1;
            b0 = (vi0 >= 0) && bound_vars[vi0];
            b1 = (vi1 >= 0) && bound_vars[vi1];
            if (b0 || b1) {
                if (vi0 >= 0) bound_vars[vi0] = 1;
                if (vi1 >= 0) bound_vars[vi1] = 1;
            }
        }
        /* (c) PRODUCERS in body order: arithmetic + string-producing.  Require
         *     every operand var bound, then ADD the result var (left-to-right
         *     dependency among producers). */
        for (bi = 0; bi < r->nbody; bi++) {
            atom *ba = r->body[bi];
            if (is_arith(ba)) {
                if (!expr_vars_bound(ba->arith, &vt, bound_vars, r->head->pred))
                    goto fail;
                if (ba->nargs >= 1 && ba->args[0]->kind == TOK_VAR) {
                    int vi = v_find(&vt, ba->args[0]->text);
                    if (vi >= 0) bound_vars[vi] = 1;
                }
            } else if (is_str_producing(ba)) {
                if (!str_producing_operands_bound(ba, &vt, bound_vars,
                                                  r->head->pred))
                    goto fail;
                {
                    int vi = v_find(&vt, ba->args[0]->text);
                    if (vi >= 0) bound_vars[vi] = 1;
                }
            }
        }
        /* (d) FILTERS: comparisons + string filters.  All variable operands
         *     must be bound (by a relational atom, equality, or an earlier
         *     producer result). */
        for (bi = 0; bi < r->nbody; bi++) {
            atom *ba = r->body[bi];
            if (is_comparison(ba)) {
                for (j = 0; j < ba->nargs; j++) {
                    token *t = ba->args[j];
                    if (t->kind != TOK_VAR) continue;
                    int vi = v_find(&vt, t->text);
                    if (vi < 0 || !bound_vars[vi]) {
                        fprintf(stderr,
                            "compile error: ungrounded comparison — variable "
                            "'%s' in comparison '%s' is not bound by a positive "
                            "body atom (rule '%s')\n",
                            t->text, ba->pred, r->head->pred);
                        goto fail;
                    }
                }
            } else if (is_str_filter(ba)) {
                if (!str_filter_operands_bound(ba, &vt, bound_vars,
                                               r->head->pred))
                    goto fail;
            }
        }
    }

    /* ── 5c. M9: reject division/modulo by a literal 0 at compile time. ── */
    for (bi = 0; bi < r->nbody; bi++) {
        atom *ba = r->body[bi];
        if (is_arith(ba) && expr_has_div0(ba->arith)) {
            fprintf(stderr, "compile error: division/modulo by literal 0 "
                    "(rule '%s')\n", r->head->pred);
            goto fail;
        }
    }

    /* ── 6. grounding ────────────────────────────────────────────── */
    for (i = 0; i < r->head->nargs; i++) {
        token *a = r->head->args[i];
        if (a->kind != TOK_VAR) continue;
        /* aggregate result var is produced by the aggregate, so it is
         * grounded without appearing in a positive body atom */
        if (agg && !strcmp(a->text, agg->pred)) continue;
        /* M9: an arithmetic or string-producing result var is produced by
         * OP_ARITH / OP_STR_* + OP_EQ, so it is grounded without appearing in
         * a positive body atom (the same exemption as the aggregate result
         * var). */
        {
            int ares = 0;
            for (bi = 0; bi < r->nbody; bi++) {
                atom *ba = r->body[bi];
                if (ba->nargs >= 1 && ba->args[0]->kind == TOK_VAR &&
                    !strcmp(ba->args[0]->text, a->text) &&
                    (is_arith(ba) || is_str_producing(ba)))
                    { ares = 1; break; }
            }
            if (ares) continue;
        }
        int ok = 0;
        for (bi = 0; bi < r->nbody && !ok; bi++)
            for (j = 0; j < r->body[bi]->nargs; j++)
                if (r->body[bi]->args[j]->kind == TOK_VAR &&
                    !strcmp(r->body[bi]->args[j]->text, a->text)) { ok = 1; break; }
        if (!ok) {
            fprintf(stderr, "compile error: ungrounded variable '%s' in head of '%s'\n",
                    a->text, r->head->pred); goto fail;
        }
    }

    /* ── 8. emit bytecode ────────────────────────────────────────── */

    {
        int first_pos = -1;

        for (bi = 0; bi < r->nbody; bi++) {
            atom *ba = r->body[bi];
            if (ba->negated || ba->aggregate) continue;
            if (is_builtin_pred(ba)) continue;
            first_pos = bi; break;
        }
        if (first_pos < 0) {
            fprintf(stderr, "compile error: rule '%s' has no positive body atom\n",
                    r->head->pred);
            goto fail;
        }

        /* NEG_CHECK for negated atoms before first positive */
        for (bi = 0; bi < first_pos; bi++) {
            atom *na = r->body[bi];
            vm_instr *neg = i_emit(&ib);
            neg->op = OP_NEG_CHECK;
            neg->a = (uint8_t)bri[bi];
            neg->b = (uint8_t)na->nargs;
            neg->body_idx = (uint8_t)bi;
            for (j = 0; j < na->nargs; j++) {
                if (na->args[j]->kind == TOK_VAR) {
                    int vi = v_find(&vt, na->args[j]->text);
                    neg->slots[j] = vt.e[vi].slot;
                } else {
                    char cname[16];
                    v_fresh_name(&vt, cname, sizeof(cname), &cc, 'k');
                    int vi = v_add(&vt, cname);
                    neg->slots[j] = vt.e[vi].slot;
                    uint32_t cv;
                    if (token_const(db, na->args[j], &cv)) goto fail;
                    vm_instr *eq = i_emit(&ib);
                    eq->op = OP_EQ_CONST; eq->a = neg->slots[j]; eq->imm = cv;
                }
            }
        }

        /* First positive body atom → SCAN or WALK */
        {
            atom *a0 = r->body[first_pos];
            vm_instr *ip = i_emit(&ib);
            if (pat_idx[first_pos] != 0xFF) {
                ip->op = OP_WALK;
                ip->imm = pat_idx[first_pos];  /* pattern index */
            } else {
                ip->op = OP_SCAN;
            }
            ip->a = (uint8_t)bri[first_pos]; ip->b = (uint8_t)a0->nargs;
            ip->body_idx = (uint8_t)first_pos;
            uint8_t ts[8];
            for (j = 0; j < a0->nargs; j++) {
                if (a0->args[j]->kind == TOK_VAR) {
                    int vi = v_find(&vt, a0->args[j]->text);
                    ts[j] = vt.e[vi].slot;
                } else {
                    char cname[16];
                    v_fresh_name(&vt, cname, sizeof(cname), &cc, 'k');
                    int vi = v_add(&vt, cname);
                    ts[j] = vt.e[vi].slot;
                }
            }
            for (j = 0; j < a0->nargs; j++) ip->slots[j] = ts[j];

            for (j = 0; j < a0->nargs; j++) {
                token *a = a0->args[j];
                if (a->kind == TOK_VAR) continue;
                uint32_t cv;
                if (token_const(db, a, &cv)) { fprintf(stderr, "compile: bad constant\n"); goto fail; }
                vm_instr *eq = i_emit(&ib);
                eq->op = OP_EQ_CONST; eq->a = ts[j]; eq->imm = cv;
            }
        }

        /* Remaining body atoms */
        for (bi = first_pos + 1; bi < r->nbody; bi++) {
            atom *curr = r->body[bi];

            if (curr->aggregate) continue;          /* AGG_ACC emitted below */
            if (is_builtin_pred(curr)) continue; /* equality/builtins (arith, comparison, string) emitted below */

            if (curr->negated) {
                vm_instr *neg = i_emit(&ib);
                neg->op = OP_NEG_CHECK;
                neg->a = (uint8_t)bri[bi];
                neg->b = (uint8_t)curr->nargs;
                neg->body_idx = (uint8_t)bi;
                for (j = 0; j < curr->nargs; j++) {
                    if (curr->args[j]->kind == TOK_VAR) {
                        int vi = v_find(&vt, curr->args[j]->text);
                        neg->slots[j] = vt.e[vi].slot;
                    } else {
                        char cname[16];
                        v_fresh_name(&vt, cname, sizeof(cname), &cc, 'k');
                        int vi = v_add(&vt, cname);
                        neg->slots[j] = vt.e[vi].slot;
                        uint32_t cv;
                        if (token_const(db, curr->args[j], &cv)) goto fail;
                        vm_instr *eq = i_emit(&ib);
                        eq->op = OP_EQ_CONST; eq->a = neg->slots[j]; eq->imm = cv;
                    }
                }
                continue;
            }

            /* Positive atom: LOOKUP, OP_LOOKUP_PERM, or SCAN */
            int sc[8] = {0}, k = 0;
            for (j = 0; j < curr->nargs; j++) {
                if (curr->args[j]->kind != TOK_VAR) continue;
                int bp;
                for (bp = 0; bp < bi; bp++) {
                    if (r->body[bp]->negated || r->body[bp]->aggregate) continue;
                    atom *p = r->body[bp];
                    if (is_builtin_pred(p)) continue;
                    int q;
                    for (q = 0; q < p->nargs; q++)
                        if (p->args[q]->kind == TOK_VAR &&
                            !strcmp(p->args[q]->text, curr->args[j]->text))
                            { sc[j] = 1; break; }
                    if (sc[j]) break;
                }
            }
            while (k < curr->nargs && sc[k]) k++;

            vm_instr *ip = i_emit(&ib);
            if (pat_idx[bi] != 0xFF) {
                /* Pattern atom: always full scan with regex filter */
                ip->op = OP_WALK;
                ip->imm = pat_idx[bi];
                ip->a = (uint8_t)bri[bi];
                ip->b = (uint8_t)curr->nargs;
                ip->body_idx = (uint8_t)bi;
                uint8_t ts[8];
                for (j = 0; j < curr->nargs; j++) {
                    if (curr->args[j]->kind == TOK_VAR) {
                        int vi = v_find(&vt, curr->args[j]->text);
                        ts[j] = vt.e[vi].slot;
                    } else {
                        char cname[16];
                        v_fresh_name(&vt, cname, sizeof(cname), &cc, 'k');
                        int vi = v_add(&vt, cname);
                        ts[j] = vt.e[vi].slot;
                    }
                }
                for (j = 0; j < curr->nargs; j++) ip->slots[j] = ts[j];
                for (j = 0; j < curr->nargs; j++) {
                    if (curr->args[j]->kind == TOK_VAR) continue;
                    uint32_t cv;
                    if (token_const(db, curr->args[j], &cv)) goto fail;
                    vm_instr *eq = i_emit(&ib);
                    eq->op = OP_EQ_CONST; eq->a = ts[j]; eq->imm = cv;
                }
            } else if (k > 0) {
                /* Leading shared columns: OP_LOOKUP */
                ip->op = OP_LOOKUP; ip->a = (uint8_t)bri[bi]; ip->b = (uint8_t)k; ip->c = (uint8_t)curr->nargs;
                ip->body_idx = (uint8_t)bi;
                uint8_t slot_map[8]; int si = 0;
                for (j = 0; j < k; j++) {
                    int vi = v_find(&vt, curr->args[j]->text);
                    ip->slots[j] = vt.e[vi].slot;
                    slot_map[j] = vt.e[vi].slot;
                }
                si = k;
                for (j = k; j < curr->nargs; j++) {
                    if (curr->args[j]->kind == TOK_VAR) {
                        int vi = v_find(&vt, curr->args[j]->text);
                        ip->slots[si] = vt.e[vi].slot;
                        slot_map[j] = vt.e[vi].slot;
                    } else {
                        char cname[16];
                        v_fresh_name(&vt, cname, sizeof(cname), &cc, 'k');
                        int vi = v_add(&vt, cname);
                        ip->slots[si] = vt.e[vi].slot;
                        slot_map[j] = vt.e[vi].slot;
                    }
                    si++;
                }
                for (j = 0; j < curr->nargs; j++) {
                    if (curr->args[j]->kind == TOK_VAR) continue;
                    uint32_t cv;
                    if (token_const(db, curr->args[j], &cv)) goto fail;
                    vm_instr *eq = i_emit(&ib);
                    eq->op = OP_EQ_CONST; eq->a = slot_map[j]; eq->imm = cv;
                }
            } else {
                /* No leading shared columns.  Check for non-leading join. */
                int any_shared = 0;
                for (j = 0; j < curr->nargs; j++) if (sc[j]) { any_shared = 1; break; }

                if (any_shared) {
                    /* M6: non-leading-column join → OP_LOOKUP_PERM */
                    uint8_t perm_arr[8], n_join = 0, n_other = 0;
                    uint8_t join_cols[8], other_cols[8];
                    int pj;

                    /* Collect join columns (sc[j]==1) in increasing order */
                    for (j = 0; j < curr->nargs; j++) {
                        if (sc[j]) join_cols[n_join++] = (uint8_t)j;
                        else       other_cols[n_other++] = (uint8_t)j;
                    }

                    /* Build perm: join cols first, then other cols */
                    for (pj = 0; pj < n_join; pj++)
                        perm_arr[pj] = join_cols[pj];
                    for (pj = 0; pj < n_other; pj++)
                        perm_arr[(int)n_join + pj] = other_cols[pj];

                    int perm_id = dl_db_declare_perm(db, bri[bi],
                        (uint8_t)curr->nargs, perm_arr);
                    if (perm_id < 0) {
                        fprintf(stderr, "compile error: too many permutation "
                                "indices (rule '%s')\n", r->head->pred);
                        goto fail;
                    }

                    ip->op = OP_LOOKUP_PERM;
                    ip->a   = (uint8_t)bri[bi];
                    ip->b   = n_join;  /* perm-prefix length */
                    ip->c   = (uint8_t)curr->nargs;
                    ip->imm = (uint32_t)perm_id;
                    ip->body_idx = (uint8_t)bi;

                    /* slots indexed by ORIGINAL column: slots[c] = var slot */
                    for (j = 0; j < curr->nargs; j++) {
                        if (curr->args[j]->kind == TOK_VAR) {
                            int vi = v_find(&vt, curr->args[j]->text);
                            ip->slots[j] = vt.e[vi].slot;
                        } else {
                            char cname[16];
                            v_fresh_name(&vt, cname, sizeof(cname), &cc, 'k');
                            int vi = v_add(&vt, cname);
                            ip->slots[j] = vt.e[vi].slot;
                        }
                    }

                    /* Emit EQ_CONST for any constant args */
                    for (j = 0; j < curr->nargs; j++) {
                        if (curr->args[j]->kind == TOK_VAR) continue;
                        uint32_t cv;
                        if (token_const(db, curr->args[j], &cv)) goto fail;
                        vm_instr *eq = i_emit(&ib);
                        eq->op = OP_EQ_CONST; eq->a = ip->slots[j]; eq->imm = cv;
                    }
                } else {
                    /* No shared columns at all: OP_SCAN */
                    ip->op = OP_SCAN; ip->a = (uint8_t)bri[bi]; ip->b = (uint8_t)curr->nargs;
                    ip->body_idx = (uint8_t)bi;
                    uint8_t ts[8];
                    for (j = 0; j < curr->nargs; j++) {
                        if (curr->args[j]->kind == TOK_VAR) {
                            int vi = v_find(&vt, curr->args[j]->text);
                            ts[j] = vt.e[vi].slot;
                        } else {
                            char cname[16];
                            v_fresh_name(&vt, cname, sizeof(cname), &cc, 'k');
                            int vi = v_add(&vt, cname);
                            ts[j] = vt.e[vi].slot;
                        }
                    }
                    for (j = 0; j < curr->nargs; j++) ip->slots[j] = ts[j];
                    for (j = 0; j < curr->nargs; j++) {
                        if (curr->args[j]->kind == TOK_VAR) continue;
                        uint32_t cv; if (token_const(db, curr->args[j], &cv)) goto fail;
                        vm_instr *eq = i_emit(&ib);
                        eq->op = OP_EQ_CONST; eq->a = ts[j]; eq->imm = cv;
                    }
                }
            }
        }
    }

    /* Equality body atoms → OP_EQ.  Emitted after all body atoms so every
     * variable is bound; OP_EQ then acts as a pure filter (both sides bound)
     * or binds a head-only variable from its already-bound counterpart. */
    for (bi = 0; bi < r->nbody; bi++) {
        atom *ba = r->body[bi];
        if (ba->aggregate) continue;
        if (!is_equality(ba)) continue;
        if (ba->negated) continue;  /* negated builtin already rejected */
        int vi_l = v_find(&vt, ba->args[0]->text);
        int vi_r = v_find(&vt, ba->args[1]->text);
        if (vi_l < 0 || vi_r < 0) goto fail;
        vm_instr *eq = i_emit(&ib);
        eq->op = OP_EQ;
        eq->a = vt.e[vi_l].slot;
        eq->b = vt.e[vi_r].slot;
        eq->body_idx = (uint8_t)bi;
    }

    /* PRODUCERS in body order → arithmetic (OP_ARITH) + string-producing
     * (OP_STR_BIND / OP_STR_LEN).  Each producer lowers into a fresh temp,
     * then a final OP_EQ(result_slot, temp_slot) reusing the bind-or-filter
     * semantics: a pre-bound result var filters (never silently overwrites)
     * and an unbound one binds — preserving the b_try never-overwrite
     * invariant.  Emitted in BODY ORDER after equality (which runs first)
     * and before filters (which may reference producer results). */
    for (bi = 0; bi < r->nbody; bi++) {
        atom *ba = r->body[bi];
        if (is_arith(ba)) {
            if (ba->negated) continue;  /* negated builtin already rejected */
            int rs = lower_expr(db, ba->arith, &vt, &ib, &cc, &tc, bi);
            if (rs < 0) goto fail;
            int rvi = v_find(&vt, ba->args[0]->text);
            if (rvi < 0) goto fail;
            vm_instr *eq = i_emit(&ib);
            eq->op = OP_EQ;
            eq->a = vt.e[rvi].slot;
            eq->b = (uint8_t)rs;
            eq->body_idx = (uint8_t)bi;
        } else if (is_str_producing(ba)) {
            if (ba->negated) continue;  /* negated builtin already rejected */
            char cname[16];
            int vi, ls, rs;
            vm_instr *op;
            v_fresh_name(&vt, cname, sizeof(cname), &tc, 't');
            vi = v_add(&vt, cname);
            if (vi < 0) goto fail;
            if (str_bind_imm(ba->pred) >= 0) {
                /* OP_STR_BIND: concat (2 operands, imm 0) or lower/upper
                 * (1 operand, imm 1/2). */
                ls = cmp_operand_slot(db, ba->args[1], &vt, &ib, &cc);
                if (ls < 0) goto fail;
                if (!strcmp(ba->pred, "concat")) {
                    rs = cmp_operand_slot(db, ba->args[2], &vt, &ib, &cc);
                    if (rs < 0) goto fail;
                } else {
                    rs = ls;  /* b unused for unary lower/upper */
                }
                op = i_emit(&ib);
                if (!op) goto fail;
                op->op = OP_STR_BIND;
                op->a = (uint8_t)ls;
                op->b = (uint8_t)rs;
                op->c = vt.e[vi].slot;
                op->imm = (uint32_t)str_bind_imm(ba->pred);
                op->body_idx = (uint8_t)bi;
            } else {  /* length */
                ls = cmp_operand_slot(db, ba->args[1], &vt, &ib, &cc);
                if (ls < 0) goto fail;
                op = i_emit(&ib);
                if (!op) goto fail;
                op->op = OP_STR_LEN;
                op->a = (uint8_t)ls;
                op->c = vt.e[vi].slot;
                op->imm = 0;
                op->body_idx = (uint8_t)bi;
            }
            int rvi = v_find(&vt, ba->args[0]->text);
            if (rvi < 0) goto fail;
            vm_instr *eq = i_emit(&ib);
            eq->op = OP_EQ;
            eq->a = vt.e[rvi].slot;
            eq->b = vt.e[vi].slot;
            eq->body_idx = (uint8_t)bi;
        }
    }

    /* FILTERS in body order → comparisons (OP_CMP) + string filters
     * (OP_STR_FILTER).  All operand slots are bound by now (compiler
     * guarantees; constants were materialized by cmp_operand_slot's
     * OP_EQ_CONST). */
    for (bi = 0; bi < r->nbody; bi++) {
        atom *ba = r->body[bi];
        if (is_comparison(ba)) {
            if (ba->negated) continue;  /* negated builtin already rejected */
            int ls = cmp_operand_slot(db, ba->args[0], &vt, &ib, &cc);
            int rs = cmp_operand_slot(db, ba->args[1], &vt, &ib, &cc);
            if (ls < 0 || rs < 0) goto fail;
            vm_instr *c = i_emit(&ib);
            c->op = OP_CMP;
            c->a = (uint8_t)ls;
            c->b = (uint8_t)rs;
            c->imm = (uint32_t)cmp_op_code(ba->pred);
            c->body_idx = (uint8_t)bi;
        } else if (is_str_filter(ba)) {
            if (ba->negated) continue;  /* negated builtin already rejected */
            int ls = cmp_operand_slot(db, ba->args[0], &vt, &ib, &cc);
            int rs = cmp_operand_slot(db, ba->args[1], &vt, &ib, &cc);
            if (ls < 0 || rs < 0) goto fail;
            vm_instr *f = i_emit(&ib);
            f->op = OP_STR_FILTER;
            f->a = (uint8_t)ls;
            f->b = (uint8_t)rs;
            f->imm = (uint32_t)str_filter_code(ba->pred);
            f->body_idx = (uint8_t)bi;
        }
    }

    /* Aggregate: emit AGG_ACC to accumulate all body bindings into
     * group-keyed buckets.  Group-by vars = head vars except the result var
     * (Soufflé convention). */
    if (agg) {
        int group_vars[8];
        int n_group = 0;
        for (i = 0; i < r->head->nargs; i++) {
            token *a = r->head->args[i];
            if (a->kind != TOK_VAR) continue;
            if (!strcmp(a->text, agg->pred)) continue;  /* result var */
            int vi = v_find(&vt, a->text);
            if (vi < 0) goto fail;
            int k2, dup = 0;
            for (k2 = 0; k2 < n_group; k2++)
                if (group_vars[k2] == (int)vt.e[vi].slot) { dup = 1; break; }
            if (!dup) group_vars[n_group++] = (int)vt.e[vi].slot;
        }
        if (n_group > 7) {
            fprintf(stderr, "compile error: too many group-by columns (rule '%s')\n",
                    r->head->pred); goto fail;
        }
        int rvi = v_find(&vt, agg->pred);
        vm_instr *aip = i_emit(&ib);
        aip->op = OP_AGG_ACC;
        aip->a = (uint8_t)n_group;
        aip->b = (uint8_t)agg_op_code;
        aip->c = vt.e[rvi].slot;          /* aggregate result var slot */
        for (j = 0; j < n_group; j++) aip->slots[j] = (uint8_t)group_vars[j];
        if (agg_src_var) {
            int svi = v_find(&vt, agg_src_var);
            aip->slots[n_group] = vt.e[svi].slot;
        } else {
            aip->slots[n_group] = 0xFF;   /* count: no source var */
        }
        aip->body_idx = (uint8_t)agg_body_idx;
    }

    /* PROJECT (or AGG_EMIT for aggregate rules) */
    {
        uint8_t head_slots[8];
        for (j = 0; j < r->head->nargs; j++) {
            token *a = r->head->args[j];
            if (a->kind == TOK_VAR) {
                int vi = v_find(&vt, a->text);
                head_slots[j] = vt.e[vi].slot;
            } else {
                uint32_t cv;
                if (token_const(db, a, &cv)) goto fail;
                char cname[16];
                v_fresh_name(&vt, cname, sizeof(cname), &cc, 'k');
                int vi = v_add(&vt, cname);
                if (vi < 0) goto fail;
                head_slots[j] = vt.e[vi].slot;
                vm_instr *eq = i_emit(&ib);
                eq->op = OP_EQ_CONST; eq->a = head_slots[j]; eq->imm = cv;
            }
        }

        if (agg) {
            /* Aggregate rule: emit OP_AGG_EMIT (all head args are vars,
             * since constants in aggregate heads are rejected above). */
            int rvi = v_find(&vt, agg->pred);
            vm_instr *aip = i_emit(&ib);
            aip->op = OP_AGG_EMIT;
            aip->a = (uint8_t)head_ri;
            aip->b = (uint8_t)r->head->nargs;
            aip->c = vt.e[rvi].slot;   /* aggregate result var slot */
            for (j = 0; j < r->head->nargs; j++)
                aip->slots[j] = head_slots[j];
        } else {
            vm_instr *ip = i_emit(&ib);
            ip->op = OP_PROJECT; ip->a = (uint8_t)head_ri; ip->b = (uint8_t)r->head->nargs;
            for (j = 0; j < r->head->nargs; j++)
                ip->slots[j] = head_slots[j];
        }
    }

    /* HALT */
    { vm_instr *ip = i_emit(&ib); ip->op = OP_HALT; }

    if (ib.err) goto fail;

    /* ── 9. build result ─────────────────────────────────────────── */
    cr = calloc(1, sizeof(*cr)); if (!cr) goto fail;
    cr->head_pred = strdup(r->head->pred);
    cr->head_rel_id = (uint8_t)head_ri;
    cr->n_vars = (uint8_t)vt.n; cr->n_instrs = ib.n;
    cr->instrs = ib.b; ib.b = NULL; ib.n = 0;
    cr->stratum = 0;
    cr->is_recursive = 0;
    cr->has_aggregate = (agg_body_idx >= 0);

    /* M5: transfer compiled patterns */
    cr->n_patterns = n_pat;
    cr->patterns   = pat_dfa;  pat_dfa = NULL;
    n_pat = 0;

    if (rel_strata && head_ri >= 0)
        cr->stratum = rel_strata[head_ri];

    if (cr->n_vars > 0) {
        cr->vars = malloc((size_t)cr->n_vars * sizeof(var_info));
        if (!cr->vars) { compiled_rule_free(cr); cr = NULL; goto fail; }
        for (i = 0; i < vt.n; i++) {
            cr->vars[i].name = vt.e[i].name; vt.e[i].name = NULL;
            cr->vars[i].slot = vt.e[i].slot;
        }
    }

fail:
    if (vt.err) {
        fprintf(stderr, "compile error: rule '%s' exceeds the maximum of %d "
                "distinct variables / temps / constants in a single rule\n",
                r->head->pred, MAX_VARS);
    }
    v_free(&vt); i_free(&ib); free(bri);
    free(pat_idx);
    if (pat_dfa) {
        for (i = 0; i < n_pat; i++) regex_dfa_free(pat_dfa[i]);
        free(pat_dfa);
    }
    return cr;
}

/* ─── Public API ────────────────────────────────────────────────────── */

int compile_rules(dl_db *db, rule **rules, int n_rules,
                  compiled_rule ***out_rules, int *out_n)
{
    int i;
    compiled_rule **c;
    int *rel_strata = NULL;
    size_t nrels;

    if (!db || !rules || n_rules <= 0 || !out_rules || !out_n) return -1;

    /* Declare any missing head relations so stratification sees all nodes */
    for (i = 0; i < n_rules; i++) {
        rule *r = rules[i];
        int ri = db_find_rel(db, r->head->pred);
        if (ri < 0) {
            if (r->head->nargs < 1 || r->head->nargs > MAX_ARITY) {
                fprintf(stderr, "compile error: head arity %d for '%s'\n",
                        r->head->nargs, r->head->pred);
                return -1;
            }
            if (dl_declare_relation(db, r->head->pred, (uint8_t)r->head->nargs)) {
                fprintf(stderr, "compile error: cannot declare '%s/%d'\n",
                        r->head->pred, r->head->nargs);
                return -1;
            }
        }
    }

    /* Compute strata and SCC-based recursion */
    nrels = db_rel_count(db);
    rel_strata = calloc(nrels, sizeof(int));
    if (!rel_strata) return -1;
    {
        int *recursive = calloc(nrels, sizeof(int));
        if (!recursive) { free(rel_strata); return -1; }

        if (compute_strata(db, rules, n_rules, rel_strata,
                           recursive, (int)nrels) != 0) {
            free(recursive); free(rel_strata);
            return -1;
        }

        /* Compile each rule */
        c = calloc((size_t)n_rules, sizeof(compiled_rule *));
        if (!c) { free(recursive); free(rel_strata); return -1; }
        *out_n = 0;
        for (i = 0; i < n_rules; i++) {
            c[i] = compile_one(db, rules[i], rel_strata);
            if (!c[i]) {
                int j; for (j = 0; j < i; j++) compiled_rule_free(c[j]);
                free(c); free(recursive); free(rel_strata); return -1;
            }
            /* SCC-based recursion (fixes B3): recursive if head_rel is
             * in an SCC with >1 node or has a self-loop */
            c[i]->is_recursive =
                (c[i]->head_rel_id < (uint8_t)nrels
                 && recursive[c[i]->head_rel_id]) ? 1 : 0;

            /* M3: aggregates only allowed in non-recursive rules */
            if (c[i]->has_aggregate && c[i]->is_recursive) {
                fprintf(stderr, "compile error: aggregate in recursive rule "
                        "not supported (rule '%s')\n", c[i]->head_pred);
                int j; for (j = 0; j <= i; j++) compiled_rule_free(c[j]);
                free(c); free(recursive); free(rel_strata); return -1;
            }
            (*out_n)++;
        }

        free(recursive);
    }

    free(rel_strata);
    *out_rules = c;
    return 0;
}

void compiled_rule_free(compiled_rule *cr)
{
    int i;
    if (!cr) return;
    free(cr->head_pred);
    if (cr->vars) { for (i = 0; i < cr->n_vars; i++) free(cr->vars[i].name); free(cr->vars); }
    free(cr->instrs);
    if (cr->patterns) {
        for (i = 0; i < cr->n_patterns; i++)
            regex_dfa_free(cr->patterns[i]);
        free(cr->patterns);
    }
    free(cr);
}
