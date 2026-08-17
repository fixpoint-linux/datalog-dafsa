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
#include <stdarg.h>

/* ─── LSP compile-error sink (parallel capture; stderr text unchanged) ── */
static uint32_t compile_err_off;
static char     compile_err_msg[256];
static int      compile_has_err;

/* Byte offset of the rule currently being compiled by compile_one().  Helper
 * functions (token_const, the *_operands_bound checks, cmp_operand_slot) don't
 * carry the `rule *` down, so they attribute their diagnostic to this rule. */
static uint32_t g_rule_off;

/* Write a compile error to stderr BYTE-IDENTICALLY to the pre-LSP compiler
 * (vfprintf), AND capture the offset + formatted message.  Only the FIRST
 * error of a compile_rules() call is kept. */
static void cerr(uint32_t off, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    if (!compile_has_err) {
        compile_has_err = 1;
        compile_err_off = off;
        va_start(ap, fmt);
        vsnprintf(compile_err_msg, sizeof(compile_err_msg), fmt, ap);
        va_end(ap);
    }
}

const char *compile_last_error(uint32_t *off)
{
    if (off) *off = compile_err_off;
    return compile_has_err ? compile_err_msg : NULL;
}

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

/* ─── BUSHY (v2) compile-time toggles ──────────────────────────────────── */
/* See compiler.h.  Default: bushy + reorder both ON. */
int g_bushy   = 1;
int g_reorder = 1;

/* M6-permsel: automatic perm-index selection.  See compiler.h. */
int g_perm_select         = 1;
int g_perm_card_threshold = 4;

static uint64_t db_rel_card(dl_db *db, int idx)
{
    if (idx < 0 || (size_t)idx >= db->nrels) return 0;
    /* v2: a variadic relation's join-order hint is the total across
     * variants (any value is semantically safe — perf hint only). */
    if (db->rels[idx].kind == RELK_VARIADIC)
        return vrel_count(db->rels[idx].vrel);
    return rel_count(db->rels[idx].rel);
}

/* M6-permsel: distinct-tuple estimate for the perm-selection cost gate. */
/* Uses rel_count_subtree (memoized distinct-key count on rel->d) not rel_count, */
/* whose dafsa_stats n_final is unreliable for fresh EDBs at compile time (~1). */
/* Unmaterialized IDB -> 0 -> cost model hash-joins (always semantically safe). */
static uint64_t db_rel_card_est(dl_db *db, int idx)
{
    if (idx < 0 || (size_t)idx >= db->nrels) return 0;
    if (db->rels[idx].kind == RELK_VARIADIC)
        return vrel_count(db->rels[idx].vrel);
    return rel_count_subtree(db->rels[idx].rel);
}

/* v2: 1 if the relation is variadic. */
static int db_rel_is_variadic(dl_db *db, int idx)
{
    if (idx < 0 || (size_t)idx >= db->nrels) return 0;
    return db->rels[idx].kind == RELK_VARIADIC;
}

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

/* ─── v2-lists: list-pattern helpers ──────────────────────────────────── */

/* 1 iff t is a list PATTERN: it has a '|' tail, or any element is a
 * variable, or (recursively) any nested element is a pattern.  A list with
 * no tail and only constant elements is a plain constant literal. */
static int list_is_pattern(const token *t)
{
    int i;
    if (!t || t->kind != TOK_LIST) return 0;
    if (t->tail) return 1;
    for (i = 0; i < t->nchildren; i++) {
        const token *e = t->children[i];
        if (e->kind == TOK_VAR) return 1;
        if (list_is_pattern(e)) return 1;
    }
    return 0;
}

/* OR every variable slot referenced by token t (recursing into list
 * patterns) into *m.  Used by atom_var_mask so a list-pattern atom connects
 * its element/tail vars to the bushy/left-deep join planner. */
static void mask_token_vars(uint64_t *m, const token *t, v_tab *vt)
{
    int i;
    if (!t) return;
    if (t->kind == TOK_VAR) {
        int vi = v_find(vt, t->text);
        if (vi >= 0 && vi < 64) *m |= (1ULL << vi);
        return;
    }
    if (t->kind == TOK_LIST) {
        for (i = 0; i < t->nchildren; i++)
            mask_token_vars(m, t->children[i], vt);
        mask_token_vars(m, t->tail, vt);
    }
}

/* 1 iff token t (recursing into list patterns) references variable `name`. */
static int token_contains_var(const token *t, const char *name)
{
    int i;
    if (!t) return 0;
    if (t->kind == TOK_VAR) return t->text && strcmp(t->text, name) == 0;
    if (t->kind == TOK_LIST) {
        for (i = 0; i < t->nchildren; i++)
            if (token_contains_var(t->children[i], name)) return 1;
        if (token_contains_var(t->tail, name)) return 1;
    }
    return 0;
}

/* v_add every variable referenced by token t (recursing into list patterns).
 * Returns 0 on success, -1 on OOM / MAX_VARS overflow. */
static int collect_token_vars(const token *t, v_tab *vt)
{
    int i;
    if (!t) return 0;
    if (t->kind == TOK_VAR) return v_add(vt, t->text) < 0 ? -1 : 0;
    if (t->kind == TOK_LIST) {
        for (i = 0; i < t->nchildren; i++)
            if (collect_token_vars(t->children[i], vt) < 0) return -1;
        if (collect_token_vars(t->tail, vt) < 0) return -1;
    }
    return 0;
}

/* Mark every variable referenced by token t (recursing into list patterns)
 * as bound in bound_vars[]. */
static void mark_token_vars_bound(const token *t, v_tab *vt, int *bound_vars)
{
    int i;
    if (!t) return;
    if (t->kind == TOK_VAR) {
        int vi = v_find(vt, t->text);
        if (vi >= 0) bound_vars[vi] = 1;
        return;
    }
    if (t->kind == TOK_LIST) {
        for (i = 0; i < t->nchildren; i++)
            mark_token_vars_bound(t->children[i], vt, bound_vars);
        mark_token_vars_bound(t->tail, vt, bound_vars);
    }
}

/* ─── Parse a constant from a token ─────────────────────────────────── */

static int token_const(dl_db *db, const token *t, uint32_t *out)
{
    if (t->kind == TOK_INT) { *out = t->ival; return 0; }
    if (t->kind == TOK_IDENT || t->kind == TOK_STRING) {
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

            cerr(g_rule_off, "compile error: failed to intern string constant "
                    "'%s' (out of memory, or string exceeds the %d-byte "
                    "interner key limit)\n", brief, 4096);
            return -1;
        }
        return 0;
    }
    if (t->kind == TOK_LIST) {
        /* A list PATTERN must never reach a constant position — the compiler
         * lowers patterns via emit_pattern during the relational phase.  This
         * is a safety net for a nested pattern or a pattern in a head/fact. */
        if (list_is_pattern(t)) {
            cerr(g_rule_off, "compile error: internal: list pattern reached a "
                    "constant position (nested list patterns are not "
                    "supported)\n");
            return -1;
        }
        /* [e1, e2, ..., en] = cons(e1, cons(e2, ... cons(en, NIL) ...)).
         * Right-fold to NIL so element order is preserved (head = e1). */
        uint32_t acc = TERM_NIL;
        int i;
        for (i = t->nchildren - 1; i >= 0; i--) {
            uint32_t eh;
            if (token_const(db, t->children[i], &eh)) return -1;
            acc = term_cons(db->terms, eh, acc);
            if (acc == 0) {
                cerr(g_rule_off, "compile error: out of memory interning a "
                        "list literal (term store)\n");
                return -1;
            }
        }
        *out = acc;
        return 0;
    }
    cerr(g_rule_off, "compile error: internal: unexpected token kind %d in a "
            "constant position\n", (int)t->kind);
    return -1;
}

/* Emit car/cdr/equality destructuring for a list PATTERN already bound to
 * src_slot (the column-value slot from SCAN/LOOKUP/LOOKUP_PERM).  Uses only
 * existing opcodes; each var element binds via b_try (bind-or-filter), each
 * constant element filters via OP_EQ_CONST, tail var binds via OP_EQ, and an
 * absent tail (empty-tail) requires cdr^n == TERM_NIL. */
static int emit_pattern(dl_db *db, const token *pat, uint8_t src_slot,
                        v_tab *vt, i_buf *ib, int *cc, const char *head_pred)
{
    uint8_t cur = src_slot;
    int i;
    for (i = 0; i < pat->nchildren; i++) {
        const token *e = pat->children[i];
        if (e->kind == TOK_VAR) {
            int vi = v_find(vt, e->text);
            vm_instr *op;
            if (vi < 0) return -1;
            op = i_emit(ib);
            if (!op) return -1;
            op->op = OP_LIST_CAR;
            op->a = cur;
            op->c = vt->e[vi].slot;   /* bind-or-filter head element */
        } else {
            /* constant element: car into a fresh temp, then EQ_CONST */
            char cname[16]; uint32_t cv;
            int vi; vm_instr *op, *eq;
            v_fresh_name(vt, cname, sizeof(cname), cc, 'p');
            vi = v_add(vt, cname);
            if (vi < 0) return -1;
            if (token_const(db, e, &cv)) return -1;
            op = i_emit(ib);
            if (!op) return -1;
            op->op = OP_LIST_CAR;
            op->a = cur;
            op->c = vt->e[vi].slot;
            eq = i_emit(ib);
            if (!eq) return -1;
            eq->op = OP_EQ_CONST;
            eq->a = vt->e[vi].slot;
            eq->imm = cv;
        }
        {   /* advance: cur = cdr(cur) into a fresh temp */
            char cname[16]; int vi; vm_instr *op;
            v_fresh_name(vt, cname, sizeof(cname), cc, 'p');
            vi = v_add(vt, cname);
            if (vi < 0) return -1;
            op = i_emit(ib);
            if (!op) return -1;
            op->op = OP_LIST_CDR;
            op->a = cur;
            op->c = vt->e[vi].slot;
            cur = vt->e[vi].slot;
        }
    }
    if (pat->tail) {
        int vi = v_find(vt, pat->tail->text);
        vm_instr *eq;
        if (vi < 0) return -1;
        eq = i_emit(ib);
        if (!eq) return -1;
        eq->op = OP_EQ;              /* bind-or-filter the tail var */
        eq->a = vt->e[vi].slot;
        eq->b = cur;
    } else {
        vm_instr *eq = i_emit(ib);
        if (!eq) return -1;
        eq->op = OP_EQ_CONST;        /* empty-tail: cdr^n must be NIL */
        eq->a = cur;
        eq->imm = TERM_NIL;
    }
    (void)head_pred;
    return 0;
}

/* Emit post-join arg handling for atom a: each non-var arg j is either a
 * constant (OP_EQ_CONST filter) or a list PATTERN (destructured from the
 * column-value slot slots[j] via emit_pattern).  slots[] is indexed by
 * ORIGINAL column.  Returns 0 on success, -1 on error. */
static int emit_const_or_pattern(dl_db *db, const atom *a, const uint8_t *slots,
                                 v_tab *vt, i_buf *ib, int *cc,
                                 const char *head_pred)
{
    int j;
    for (j = 0; j < a->nargs; j++) {
        const token *t = a->args[j];
        if (t->kind == TOK_VAR) continue;
        if (t->kind == TOK_LIST && list_is_pattern(t)) {
            if (emit_pattern(db, t, slots[j], vt, ib, cc, head_pred))
                return -1;
            continue;
        }
        {
            uint32_t cv;
            if (token_const(db, t, &cv)) return -1;
            vm_instr *eq = i_emit(ib);
            if (!eq) return -1;
            eq->op = OP_EQ_CONST; eq->a = slots[j]; eq->imm = cv;
        }
    }
    return 0;
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

/* v2-lists: list ASSIGNMENT form `[X|Xs] = L` — an equality atom whose LHS
 * (args[0]) is a TOK_LIST pattern.  The compiler lowers it to emit_pattern
 * (car/cdr/equality destructuring) against the RHS value, binding the pattern
 * vars.  Distinct from a plain `VAR = VAR` equality (both sides TOK_VAR). */
static int is_list_assign(const atom *a)
{
    return is_equality(a) &&
           a->args[0] && a->args[0]->kind == TOK_LIST;
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

/* v2-lists: LIST-producing builtin (`VAR = cons(H,T)` / `VAR = car(L)` /
 * `VAR = cdr(L)` / `VAR = append(A,B)`).  args[0] is the result var,
 * args[1..] are the operand tokens.  These lowercase names are RESERVED. */
static int is_list_producing(const atom *a)
{
    if (!a || !a->pred) return 0;
    return strcmp(a->pred, "cons")   == 0 ||
           strcmp(a->pred, "car")    == 0 ||
           strcmp(a->pred, "cdr")    == 0 ||
           strcmp(a->pred, "append") == 0;
}

/* v2-lists: list FILTER builtin `member(X, L)` — a bare function-call atom
 * (NOT `VAR = member(...)`).  args[0] is the member variable X (which the VM
 * either tests or GENERATES), args[1] is the list operand L. */
static int is_list_filter(const atom *a)
{
    if (!a || !a->pred) return 0;
    return strcmp(a->pred, "member") == 0;
}

static int is_list_builtin(const atom *a)
{
    return is_list_producing(a) || is_list_filter(a);
}

/* v2-range: RANGE builtin `range(X, Rel, Lo, Hi)` — a bare function-call atom
 * (NOT `VAR = range(...)`).  args[0] is the member variable X (which the VM
 * either tests or GENERATES from the distinct leading-column values of Rel in
 * [Lo,Hi)), args[1] is a TOK_IDENT RELATION NAME resolved via db_find_rel,
 * args[2]/args[3] are the half-open bounds (TOK_VAR|TOK_INT). */
static int is_range_builtin(const atom *a)
{
    if (!a || !a->pred) return 0;
    return strcmp(a->pred, "range") == 0;
}

/* any non-relational builtin (equality / comparison / arithmetic / string /
 * list / range) */
static int is_builtin_pred(const atom *a)
{
    return is_equality(a) || is_comparison(a) || is_arith(a) ||
           is_str_builtin(a) || is_list_builtin(a) || is_range_builtin(a);
}

/* v2-lists FIX-2: is `name` a reserved BUILTIN predicate name?  These names
 * are always classified as builtins in body position (never a relation
 * reference), so a rule/fact HEAD with one of these names is a user error —
 * the body would silently ignore any declared relation of the same name (or,
 * for a fact, produce the misleading "rule X has no body").  Reserve them
 * loudly at the head so the diagnostic is clear. */
static int is_reserved_builtin_name(const char *name)
{
    if (!name) return 0;
    return strcmp(name, "member") == 0 ||
           strcmp(name, "car") == 0 || strcmp(name, "cons") == 0 ||
           strcmp(name, "cdr") == 0 || strcmp(name, "append") == 0 ||
           strcmp(name, "concat") == 0 || strcmp(name, "length") == 0 ||
           strcmp(name, "lower") == 0 || strcmp(name, "upper") == 0 ||
           strcmp(name, "prefix") == 0 || strcmp(name, "suffix") == 0 ||
           strcmp(name, "contains") == 0 || strcmp(name, "range") == 0;
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
        for (i = 1; i < need; i++) {
            token *t = a->args[i];
            /* length(S) additionally accepts a CONSTANT list literal
             * (v2 list-length); a list PATTERN is rejected (never a string). */
            if (strcmp(a->pred, "length") == 0 && t->kind == TOK_LIST) {
                if (list_is_pattern(t)) return 0;
                continue;
            }
            if (!str_operand_ok(t)) return 0;
        }
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

/* A list-builtin operand token must be a variable or a constant of any kind
 * (int / symbol / string / list literal).  The runtime is_list check is the
 * definitive list-ness test (term_cons / term_append / car / cdr backtrack on
 * a non-list) — a constant is never "wrong", just possibly non-list. */
static int list_operand_ok(const token *t)
{
    if (!t) return 0;
    if (t->kind == TOK_VAR || t->kind == TOK_INT ||
        t->kind == TOK_IDENT || t->kind == TOK_STRING)
        return 1;
    /* a list literal operand must be a CONSTANT list — never a [X|Xs]
     * pattern (patterns are destructured only in relational atom args). */
    if (t->kind == TOK_LIST)
        return !list_is_pattern(t);
    return 0;
}

/* Validate the arity + operand/result kinds of a list builtin.  Returns 1 if
 * well-formed, 0 otherwise (caller rejects loudly). */
static int list_builtin_valid(const atom *a)
{
    int i, need;
    if (!a || !a->pred) return 0;
    if (is_list_filter(a)) {
        /* member(X, L): 2 args, X a variable, L a variable-or-constant. */
        if (a->nargs != 2) return 0;
        if (a->args[0]->kind != TOK_VAR) return 0;
        if (!list_operand_ok(a->args[1])) return 0;
        return 1;
    }
    need = (strcmp(a->pred, "cons") == 0 ||
            strcmp(a->pred, "append") == 0) ? 3 : 2;
    if (a->nargs != need) return 0;
    if (a->args[0]->kind != TOK_VAR) return 0;
    for (i = 1; i < need; i++)
        if (!list_operand_ok(a->args[i])) return 0;
    return 1;
}

/* A range builtin operand token must be a variable or an integer constant. */
static int range_operand_ok(const token *t)
{
    return t && (t->kind == TOK_VAR || t->kind == TOK_INT);
}

/* Validate the shape of a `range(X, Rel, Lo, Hi)` builtin: exactly 4 args;
 * X a variable; Rel a TOK_IDENT resolved (via db_find_rel) to a known,
 * non-variadic relation of arity >= 1; Lo/Hi a variable or int constant.
 * Returns 1 if well-formed, 0 otherwise (caller rejects loudly). */
static int range_builtin_valid(dl_db *db, const atom *a)
{
    int ri;
    if (!a || !a->pred || !db) return 0;
    if (a->nargs != 4) return 0;
    if (a->args[0]->kind != TOK_VAR) return 0;      /* X must be a variable */
    if (a->args[1]->kind != TOK_IDENT) return 0;    /* Rel must be a NAME */
    if (!range_operand_ok(a->args[2])) return 0;    /* Lo bound */
    if (!range_operand_ok(a->args[3])) return 0;    /* Hi bound */
    ri = db_find_rel(db, a->args[1]->text);
    if (ri < 0) return 0;                           /* unknown relation */
    if (db_rel_is_variadic(db, ri)) return 0;       /* RELK_VARIADIC */
    if (db_rel_arity(db, ri) < 1) return 0;         /* range needs col0 */
    return 1;
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
            cerr(g_rule_off, 
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
 * __kN slot via OP_EQ_CONST.  Returns the slot, or -1 on error.
 *
 * `allow_list` (v2-lists): 1 permits a TOK_LIST literal operand (interned via
 * token_const — used by the LIST builtins, whose operands may be constants);
 * 0 REJECTS a list literal — used by comparisons and string builtins, where a
 * list operand is meaningless (lists have no order) or never produced. */
static int cmp_operand_slot(dl_db *db, const token *t, v_tab *vt, i_buf *ib,
                            int *cc, int allow_list)
{
    if (t->kind == TOK_LIST && !allow_list) {
        cerr(g_rule_off, 
                "compile error: list literal not allowed in this position "
                "(lists have no meaningful order; use = / != via variables)\n");
        return -1;
    }
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
                cerr(g_rule_off, 
                    "compile error: ungrounded string operand — variable "
                    "'%s' in '%s' is not bound by a positive body atom "
                    "(rule '%s')\n", t->text, a->pred, head_pred);
                return 0;
            }
        }
    }
    return 1;
}

/* require every VARIABLE operand of a producing LIST builtin (args[1..]) to
 * be bound — same shape as str_producing_operands_bound, distinct diagnostic. */
static int list_producing_operands_bound(const atom *a, v_tab *vt,
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
                cerr(g_rule_off, 
                    "compile error: ungrounded list operand — variable "
                    "'%s' in '%s' is not bound by a positive body atom "
                    "(rule '%s')\n", t->text, a->pred, head_pred);
                return 0;
            }
        }
    }
    return 1;
}

/* require the LIST operand (args[1]) of `member(X, L)` to be bound.  X
 * (args[0]) may be unbound — the VM then GENERATES it from L's elements. */
static int member_operand_bound(const atom *a, v_tab *vt,
                                const int *bound_vars,
                                const char *head_pred)
{
    token *t = a->args[1];
    int vi;
    if (t->kind != TOK_VAR) return 1;   /* constant L is always bound */
    vi = v_find(vt, t->text);
    if (vi < 0 || !bound_vars[vi]) {
        cerr(g_rule_off, 
                "compile error: ungrounded member operand — list variable "
                "'%s' in 'member' is not bound by a positive body atom "
                "(rule '%s')\n", t->text, head_pred);
        return 0;
    }
    return 1;
}

/* require the Lo (args[2]) and Hi (args[3]) bound operands of `range` to be
 * bound.  X (args[0]) may be unbound — the VM then GENERATES it from Rel's
 * distinct col0 values in [Lo,Hi).  args[1] is a relation NAME (never a var). */
static int range_operands_bound(const atom *a, v_tab *vt,
                                const int *bound_vars,
                                const char *head_pred)
{
    int j;
    for (j = 2; j <= 3; j++) {
        token *t = a->args[j];
        int vi;
        if (t->kind != TOK_VAR) continue;   /* int constant is always bound */
        vi = v_find(vt, t->text);
        if (vi < 0 || !bound_vars[vi]) {
            cerr(g_rule_off, 
                    "compile error: ungrounded range bound — variable "
                    "'%s' in 'range' is not bound by a positive body atom "
                    "(rule '%s')\n", t->text, head_pred);
            return 0;
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
                cerr(g_rule_off, 
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
            /* v2-range: `range(X, Rel, Lo, Hi)` reads Rel via an ARGUMENT,
             * so ba->pred ("range") is not the relation and the dependency on
             * Rel is invisible to the edge collection below.  Add a STRICT
             * (is_neg=1) edge Rel -> head so the range rule lands in a
             * strictly higher stratum than Rel: Rel is always fully
             * materialized (rel->d) before OP_RANGE reads it, never stale. */
            if (is_range_builtin(ba) && ba->nargs >= 2 &&
                ba->args[1] && ba->args[1]->kind == TOK_IDENT) {
                int rel_ri = db_find_rel(db, ba->args[1]->text);
                if (rel_ri >= 0)
                    ADD_EDGE(rel_ri, head_ri, 1);
                continue;   /* range is a builtin, not a relational atom */
            }
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

    /* v2-range / deep-review FIX: the fixpoint only fails to converge when a
     * STRICT edge (negation, or range's Rel->head edge) sits on a CYCLE —
     * the ping-pong then runs to the iteration cap and exits with changed=1.
     * The post-hoc stratum[to]<=stratum[from] check below only catches this
     * for SOME rule orders (parity of the last update), so a strict cycle
     * can slip through and be silently mis-evaluated.  Exiting at the cap
     * with changed still set IS unstratifiable: report it loudly. */
    if (changed) {
        cerr(0, 
                "compile error: unstratifiable program — stratification "
                "fixpoint did not converge (strict dependency cycle through "
                "negation or range)\n");
        free(self_loop); free(edges); free(stratum);
        return -1;
    }

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

        /* v2-range / deep-review FIX (second strict-cycle cap): same as the
         * first loop — exiting the M2.1 bump at the iteration cap with
         * changed still set is an unstratifiable strict cycle; report it
         * loudly rather than silently under-stratifying. */
        if (changed) {
            cerr(0, 
                    "compile error: unstratifiable program — stratification "
                    "fixpoint did not converge (strict dependency cycle "
                    "through negation or range)\n");
            free(comp); comp = NULL;
            free(self_loop); free(edges); free(stratum);
            return -1;
        }
    }

    free(comp);
    comp = NULL;

    /* Check for unstratifiable (after both fixpoint passes) */
    for (i = 0; i < n_edges; i++) {
        if (edges[i].is_neg) {
            if (stratum[edges[i].to] <= stratum[edges[i].from]) {
                cerr(0, 
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

/* ─── BUSHY (v2): natural-partition join planning ─────────────────────── */

#define BUSHY_MAX_CUT   2   /* min cut width that still justifies bushy      */
#define BUSHY_MAX_ATOMS 16  /* cap on the 2^n partition enumeration (safety) */

/* Emission context threaded through the recursive bushy/left-deep emitters. */
typedef struct {
    dl_db         *db;
    const rule    *r;
    const int     *bri;       /* body atom -> rel id (-1 for non-relational) */
    const uint8_t *pat_idx;   /* body atom -> pattern index, 0xFF if none     */
    v_tab         *vt;
    i_buf         *ib;
    int           *cc;        /* constant-slot counter                        */
    const uint64_t *mask;     /* var mask per positive atom (by body idx)     */
    const int      *recursive;/* [nrels]: 1 for predicates in a recursive SCC */
    int            do_bushy;  /* 1 = may emit binary-tree plans               */
    int            next_buf;  /* next free intermediate buffer index          */
} emit_ctx;

static int popcount64(uint64_t x)
{
    int c = 0;
    while (x) { x &= x - 1; c++; }
    return c;
}

/* Bitmask of the var slots referenced by body atom `bi` (constants
 * contribute nothing).  Bits 0..MAX_VARS-1 map to var slots. */
static uint64_t atom_var_mask(const rule *r, int bi, v_tab *vt)
{
    const atom *a = r->body[bi];
    uint64_t m = 0;
    int j;
    for (j = 0; j < a->nargs; j++)
        mask_token_vars(&m, a->args[j], vt);
    return m;
}

/* Are atoms[0..n-1] connected via shared variables?  Edge between two atoms
 * iff their var masks intersect. */
static int atoms_connected(const int *atoms, int n, const uint64_t *mask)
{
    uint8_t visited[64] = {0};
    int stack[64], sp = 0;
    int i;
    if (n <= 1) return 1;
    visited[0] = 1;
    stack[sp++] = 0;
    while (sp > 0) {
        int u = stack[--sp];
        int v;
        for (v = 0; v < n; v++) {
            if (visited[v]) continue;
            if (mask[atoms[u]] & mask[atoms[v]]) { visited[v] = 1; stack[sp++] = v; }
        }
    }
    for (i = 0; i < n; i++) if (!visited[i]) return 0;
    return 1;
}

/* Enumerate 2-partitions of atoms[0..n-1] into L/R with |L|>=2, |R|>=2 and
 * BOTH induced subgraphs connected; return the partition with minimal cut
 * width w = popcount(vars(L) & vars(R)).  On success sets best_l and best_r to
 * bitmasks over positions [0,n) and *best_w=w, returning 1; else 0. */
static int min_cut_split(const int *atoms, int n, const uint64_t *mask,
                         uint32_t *best_l, uint32_t *best_r, int *best_w)
{
    uint32_t total = (n >= 32) ? 0xFFFFFFFFu : ((1u << n) - 1);
    uint32_t sub;
    int found = 0;
    int best = 1 << 20;
    uint32_t bL = 0, bR = 0;
    for (sub = 1; sub < total; sub++) {
        uint32_t comp = total & ~sub;
        int nl = popcount64((uint64_t)sub);
        int nr = n - nl;
        int L[64], R[64];
        int il = 0, ir = 0, i;
        uint64_t mL = 0, mR = 0;
        if (nl < 2 || nr < 2) continue;
        for (i = 0; i < n; i++) {
            if (sub & (1u << i)) { L[il++] = atoms[i]; mL |= mask[atoms[i]]; }
            else                 { R[ir++] = atoms[i]; mR |= mask[atoms[i]]; }
        }
        if (!atoms_connected(L, il, mask)) continue;
        if (!atoms_connected(R, ir, mask)) continue;
        {
            int w = popcount64(mL & mR);
            if (w < best) { best = w; bL = sub; bR = comp; found = 1; }
        }
    }
    if (!found) return 0;
    *best_l = bL; *best_r = bR; *best_w = best;
    return 1;
}

/* Ascending-slot-order list of the vars in mask m.  Returns count (<=64). */
static int mask_to_slots(uint64_t m, uint8_t *out)
{
    int n = 0, s;
    for (s = 0; s < 64; s++)
        if (m & (1ULL << s)) out[n++] = (uint8_t)s;
    return n;
}

/* Greedy left-deep reorder: ascending rel_count, tie-broken by most vars
 * shared with the already-placed prefix.  `order` holds body indices of the
 * positive atoms (body order in), rewritten in place.  OOM leaves it
 * unchanged (still a correct body-order sequence). */
static void reorder_pos_atoms(dl_db *db, const int *bri, const uint64_t *mask,
                              int *order, int n)
{
    int *used = calloc((size_t)(n > 0 ? n : 1), sizeof(int));
    int *res  = malloc((size_t)(n > 0 ? n : 1) * sizeof(int));
    uint64_t placed_mask = 0;
    int out;
    if (!used || !res) { free(used); free(res); return; }
    for (out = 0; out < n; out++) {
        int best = -1, k;
        uint64_t bc = 0;
        int bsh = -1;
        for (k = 0; k < n; k++) {
            uint64_t cnt;
            int s;
            if (used[k]) continue;
            cnt = db_rel_card(db, bri[order[k]]);
            s = popcount64(mask[order[k]] & placed_mask);
            if (best < 0 || cnt < bc || (cnt == bc && s > bsh)) {
                best = k; bc = cnt; bsh = s;
            }
        }
        used[best] = 1;
        res[out] = order[best];
        placed_mask |= mask[order[best]];
    }
    memcpy(order, res, (size_t)n * sizeof(int));
    free(used); free(res);
}

/* ─── M6-permsel: non-leading join emission ─────────────────────────── */

/* Pack a permutation into a uint32_t: 3 bits per column, LSB-first —
 * perm[0] occupies bits 0-2, perm[1] bits 3-5, etc.  arity <= 8 => <= 24 bits,
 * so this is a lossless bijection over every arity-1..8 permutation.  Must
 * match the unpack in the VM's case OP_HASH_JOIN exactly. */
static uint32_t pack_perm(const uint8_t *perm, uint8_t arity)
{
    uint32_t p = 0;
    int i;
    for (i = 0; i < (int)arity; i++)
        p |= (uint32_t)(perm[i] & 7u) << (3u * (uint32_t)i);
    return p;
}

/* Cost-gated emit for a non-leading-column join atom.  Emits either
 * OP_LOOKUP_PERM (perm index) or OP_HASH_JOIN (slot-free fallback) onto the
 * already-emitted `ip`, fills slots[] by ORIGINAL column, then emits
 * EQ_CONST/destructure for any constant/pattern args (mirroring the old
 * OP_LOOKUP_PERM blocks this replaces).  Returns 0 on success, -1 on a hard
 * compile error (recursive carve-out with a full perm table).
 *
 * Decision model (g_perm_select / g_perm_card_threshold in compiler.h):
 *   recursive[rel_id]    -> OP_LOOKUP_PERM, MUST declare; declare<0 -> -1.
 *                           A recursive body atom CANNOT hash-join: the
 *                           semi-naive override loop only rewrites
 *                           OP_LOOKUP_PERM, so an OP_HASH_JOIN recursive atom
 *                           would read a stale DAFSA and silently mis-evaluate.
 *                           Cap exhaustion here is a genuine no-fallback, so
 *                           the compile error is kept.
 *   g_perm_select == 0   -> OP_HASH_JOIN (oracle OFF: never build an index).
 *   dl_db_find_perm()>=0 -> OP_LOOKUP_PERM reusing an existing index.
 *   rel_card >= threshold-> declare; >=0 -> OP_LOOKUP_PERM; <0 (cap full)
 *                           -> OP_HASH_JOIN (slot-free fallback).
 *   else                 -> OP_HASH_JOIN (small rel; index not worth building). */
static int emit_nonleading_join(dl_db *db, const rule *r, const atom *curr,
                                int rel_id, const int *sc,
                                const int *recursive, v_tab *vt, i_buf *ib,
                                int *cc, vm_instr *ip, int bi)
{
    uint8_t perm_arr[8], n_join = 0, n_other = 0;
    uint8_t join_cols[8], other_cols[8];
    int pj, j;

    for (j = 0; j < curr->nargs; j++) {
        if (sc[j]) join_cols[n_join++] = (uint8_t)j;
        else       other_cols[n_other++] = (uint8_t)j;
    }
    for (pj = 0; pj < n_join; pj++) perm_arr[pj] = join_cols[pj];
    for (pj = 0; pj < n_other; pj++)
        perm_arr[(int)n_join + pj] = other_cols[pj];

    int use_perm = 0;
    int perm_id = -1;

    if (recursive && recursive[rel_id]) {
        /* Recursive carve-out: never hash-join a recursive IDB body atom. */
        perm_id = dl_db_declare_perm(db, rel_id, (uint8_t)curr->nargs,
                                     perm_arr);
        if (perm_id < 0) {
            cerr(r->off, "compile error: too many permutation "
                    "indices (rule '%s')\n", r->head->pred);
            return -1;
        }
        use_perm = 1;
    } else if (g_perm_select == 0) {
        /* Oracle OFF: always hash join, never build an index. */
        use_perm = 0;
    } else {
        int existing = dl_db_find_perm(db, rel_id, (uint8_t)curr->nargs,
                                       perm_arr);
        if (existing >= 0) {
            perm_id = existing;
            use_perm = 1;
        } else if (db_rel_card_est(db, rel_id) >=
                   (uint64_t)g_perm_card_threshold) {
            perm_id = dl_db_declare_perm(db, rel_id, (uint8_t)curr->nargs,
                                         perm_arr);
            if (perm_id >= 0) use_perm = 1;
            /* else cap full -> use_perm stays 0 -> OP_HASH_JOIN fallback */
        }
        /* else: small rel, never declare -> OP_HASH_JOIN */
    }
    ip->op   = use_perm ? OP_LOOKUP_PERM : OP_HASH_JOIN;
    ip->a    = (uint8_t)rel_id;
    ip->b    = n_join;               /* perm-prefix length / join-col count */
    ip->c    = (uint8_t)curr->nargs;
    ip->body_idx = (uint8_t)bi;
    ip->imm  = use_perm ? (uint32_t)perm_id
                        : pack_perm(perm_arr, (uint8_t)curr->nargs);

    /* slots indexed by ORIGINAL column: slots[c] = var slot (constants get a
     * fresh temp slot; a trailing EQ_CONST enforces the value). */
    for (j = 0; j < curr->nargs; j++) {
        if (curr->args[j]->kind == TOK_VAR) {
            int vi = v_find(vt, curr->args[j]->text);
            ip->slots[j] = vt->e[vi].slot;
        } else {
            char cname[16];
            v_fresh_name(vt, cname, sizeof(cname), cc, 'k');
            int vi = v_add(vt, cname);
            ip->slots[j] = vt->e[vi].slot;
        }
    }
    if (emit_const_or_pattern(db, curr, ip->slots, vt, ib, cc, r->head->pred))
        return -1;
    return 0;
}

/* Emit the join instruction for positive body atom `bi`.  `bound` is the
 * bitmask of var slots already bound in the CURRENT scope (subtree): an atom
 * whose only join column is sibling-bound compiles to SCAN not LOOKUP — the
 * parent hash join enforces the equality (the cross-subtree-probe
 * correctness rule).  `is_first` forces a full scan (first atom of a scope). */
static int emit_pos_atom(dl_db *db, const rule *r, int bi, const int *bri,
                         const uint8_t *pat_idx, uint64_t bound, int is_first,
                         v_tab *vt, i_buf *ib, int *cc, const int *recursive)
{
    const atom *curr = r->body[bi];
    int j;
    int sc[8] = {0};
    int k = 0;
    vm_instr *ip;

    if (!is_first) {
        for (j = 0; j < curr->nargs; j++) {
            if (curr->args[j]->kind != TOK_VAR) continue;
            int vi = v_find(vt, curr->args[j]->text);
            if (vi >= 0 && vi < 64 && (bound & (1ULL << vi))) sc[j] = 1;
        }
        while (k < curr->nargs && sc[k]) k++;
    }

    ip = i_emit(ib);
    if (!ip) return -1;

    if (pat_idx[bi] != 0xFF) {
        /* Pattern atom: always full scan with regex filter. */
        uint8_t ts[8];
        ip->op = OP_WALK;
        ip->imm = pat_idx[bi];
        ip->a = (uint8_t)bri[bi];
        ip->b = (uint8_t)curr->nargs;
        ip->body_idx = (uint8_t)bi;
        for (j = 0; j < curr->nargs; j++) {
            if (curr->args[j]->kind == TOK_VAR) {
                int vi = v_find(vt, curr->args[j]->text);
                ts[j] = vt->e[vi].slot;
            } else {
                char cname[16];
                v_fresh_name(vt, cname, sizeof(cname), cc, 'k');
                int vi = v_add(vt, cname);
                ts[j] = vt->e[vi].slot;
            }
        }
        for (j = 0; j < curr->nargs; j++) ip->slots[j] = ts[j];
        for (j = 0; j < curr->nargs; j++) {
            if (curr->args[j]->kind == TOK_VAR) continue;
            uint32_t cv;
            if (token_const(db, curr->args[j], &cv)) return -1;
            vm_instr *eq = i_emit(ib);
            if (!eq) return -1;
            eq->op = OP_EQ_CONST; eq->a = ts[j]; eq->imm = cv;
        }
        return 0;
    }

    if (is_first || k == 0) {
        int any_shared = 0;
        if (!is_first) {
            for (j = 0; j < curr->nargs; j++) if (sc[j]) { any_shared = 1; break; }
        }
        if (!is_first && any_shared) {
            /* M6-permsel: non-leading-column join — cost-gated perm vs hash */
            if (emit_nonleading_join(db, r, curr, bri[bi], sc, recursive,
                                     vt, ib, cc, ip, bi))
                return -1;
        } else {
            /* OP_SCAN (first atom, or no shared columns at all) */
            uint8_t ts[8];
            ip->op = OP_SCAN;
            ip->a = (uint8_t)bri[bi];
            ip->b = (uint8_t)curr->nargs;
            ip->body_idx = (uint8_t)bi;
            for (j = 0; j < curr->nargs; j++) {
                if (curr->args[j]->kind == TOK_VAR) {
                    int vi = v_find(vt, curr->args[j]->text);
                    ts[j] = vt->e[vi].slot;
                } else {
                    char cname[16];
                    v_fresh_name(vt, cname, sizeof(cname), cc, 'k');
                    int vi = v_add(vt, cname);
                    ts[j] = vt->e[vi].slot;
                }
            }
            for (j = 0; j < curr->nargs; j++) ip->slots[j] = ts[j];
            if (emit_const_or_pattern(db, curr, ts, vt, ib, cc, r->head->pred))
                return -1;
        }
    } else {
        /* Leading shared columns: OP_LOOKUP */
        uint8_t slot_map[8];
        int si;
        ip->op = OP_LOOKUP;
        ip->a = (uint8_t)bri[bi];
        ip->b = (uint8_t)k;
        ip->c = (uint8_t)curr->nargs;
        ip->body_idx = (uint8_t)bi;
        for (j = 0; j < k; j++) {
            int vi = v_find(vt, curr->args[j]->text);
            ip->slots[j] = vt->e[vi].slot;
            slot_map[j] = vt->e[vi].slot;
        }
        si = k;
        for (j = k; j < curr->nargs; j++) {
            if (curr->args[j]->kind == TOK_VAR) {
                int vi = v_find(vt, curr->args[j]->text);
                ip->slots[si] = vt->e[vi].slot;
                slot_map[j] = vt->e[vi].slot;
            } else {
                char cname[16];
                v_fresh_name(vt, cname, sizeof(cname), cc, 'k');
                int vi = v_add(vt, cname);
                ip->slots[si] = vt->e[vi].slot;
                slot_map[j] = vt->e[vi].slot;
            }
            si++;
        }
        if (emit_const_or_pattern(db, curr, slot_map, vt, ib, cc,
                                  r->head->pred))
            return -1;
    }
    return 0;
}

/* Emit the interface-capture OP_PROJECT terminating a subtree range.
 * a=0xFF marks capture mode (never a real head rel id). */
static int emit_iface_project(emit_ctx *ec, const uint8_t *slots, int ar)
{
    vm_instr *pr = i_emit(ec->ib);
    int j;
    if (!pr) return -1;
    pr->op = OP_PROJECT;
    pr->a = 0xFF;
    pr->b = (uint8_t)ar;
    for (j = 0; j < ar; j++) pr->slots[j] = slots[j];
    return 0;
}

static int emit_join(emit_ctx *ec, const int *atoms, int n,
                     const uint8_t *iface_slots, int iface_ar);

/* Left-deep join of atoms[0..n-1] (a leaf subtree or the fallback).  Binding
 * scope is LOCAL to this sequence (bound starts empty). */
static int emit_leftdeep_seq(emit_ctx *ec, const int *atoms, int n,
                             const uint8_t *iface_slots, int iface_ar)
{
    uint64_t bound = 0;
    int i;
    for (i = 0; i < n; i++) {
        if (emit_pos_atom(ec->db, ec->r, atoms[i], ec->bri, ec->pat_idx,
                          bound, i == 0, ec->vt, ec->ib, ec->cc, ec->recursive) != 0)
            return -1;
        bound |= ec->mask[atoms[i]];
    }
    if (iface_slots)
        return emit_iface_project(ec, iface_slots, iface_ar);
    return 0;
}

/* Binary-tree (bushy) emission for atoms[0..n-1]: post-order
 * [eval left][eval right][OP_MAT_JOIN], each child materialized via
 * OP_MAT_BEGIN into a per-rule intermediate buffer. */
static int emit_bushy_tree(emit_ctx *ec, const int *atoms, int n,
                           const uint8_t *iface_slots, int iface_ar)
{
    uint32_t bl, br;
    int w, i, j;
    int L[64], R[64];
    int nl = 0, nr = 0;
    uint64_t mL = 0, mR = 0;
    uint8_t sh_slots[8], lp_slots[8], rp_slots[8];
    int nsh, nlp, nrp;
    uint8_t liface[8], riface[8], out_slots[8];
    int la = 0, ra = 0, oa = 0;
    int bL, bR, left_begin, right_begin;
    vm_instr *mb, *mj;

    if (!min_cut_split(atoms, n, ec->mask, &bl, &br, &w))
        return -1;   /* caller guarantees a split exists */

    for (i = 0; i < n; i++) {
        if (bl & (1u << i)) { L[nl++] = atoms[i]; mL |= ec->mask[atoms[i]]; }
        else                { R[nr++] = atoms[i]; mR |= ec->mask[atoms[i]]; }
    }
    nsh = mask_to_slots(mL & mR, sh_slots);
    nlp = mask_to_slots(mL & ~mR, lp_slots);
    nrp = mask_to_slots(mR & ~mL, rp_slots);

    for (j = 0; j < nsh; j++) liface[la++] = sh_slots[j];
    for (j = 0; j < nlp; j++) liface[la++] = lp_slots[j];
    for (j = 0; j < nsh; j++) riface[ra++] = sh_slots[j];
    for (j = 0; j < nrp; j++) riface[ra++] = rp_slots[j];
    for (j = 0; j < nsh; j++) out_slots[oa++] = sh_slots[j];
    for (j = 0; j < nlp; j++) out_slots[oa++] = lp_slots[j];
    for (j = 0; j < nrp; j++) out_slots[oa++] = rp_slots[j];

    bL = ec->next_buf++;
    bR = ec->next_buf++;

    mb = i_emit(ec->ib);
    if (!mb) return -1;
    mb->op = OP_MAT_BEGIN;
    mb->a = (uint8_t)bL;
    mb->b = (uint8_t)la;
    mb->imm = 0;   /* patched below */
    for (j = 0; j < la; j++) mb->slots[j] = liface[j];
    left_begin = ec->ib->n - 1;
    if (emit_join(ec, L, nl, liface, la) != 0) return -1;
    ec->ib->b[left_begin].imm = (uint32_t)ec->ib->n;

    mb = i_emit(ec->ib);
    if (!mb) return -1;
    mb->op = OP_MAT_BEGIN;
    mb->a = (uint8_t)bR;
    mb->b = (uint8_t)ra;
    mb->imm = 0;
    for (j = 0; j < ra; j++) mb->slots[j] = riface[j];
    right_begin = ec->ib->n - 1;
    if (emit_join(ec, R, nr, riface, ra) != 0) return -1;
    ec->ib->b[right_begin].imm = (uint32_t)ec->ib->n;

    mj = i_emit(ec->ib);
    if (!mj) return -1;
    mj->op = OP_MAT_JOIN;
    mj->a = (uint8_t)bL;
    mj->b = (uint8_t)bR;
    mj->c = (uint8_t)nsh;
    for (j = 0; j < oa; j++) mj->slots[j] = out_slots[j];

    if (iface_slots)
        return emit_iface_project(ec, iface_slots, iface_ar);
    return 0;
}

/* Emit a join for atoms[0..n-1]: bushy iff (n>=4, a split with cut width
 * <= BUSHY_MAX_CUT exists, and the subtree has <= MAX_ARITY distinct vars —
 * which bounds every descendant buffer arity by subset).  Otherwise
 * left-deep.  iface_slots (NULL for the root) is the interface projection
 * emitted at the end of a child subtree range. */
static int emit_join(emit_ctx *ec, const int *atoms, int n,
                     const uint8_t *iface_slots, int iface_ar)
{
    uint64_t all = 0;
    int i;
    for (i = 0; i < n; i++) all |= ec->mask[atoms[i]];
    if (ec->do_bushy && n >= 4 && n <= BUSHY_MAX_ATOMS &&
        popcount64(all) <= MAX_ARITY) {
        uint32_t bl, br;
        int w;
        if (min_cut_split(atoms, n, ec->mask, &bl, &br, &w) &&
            w <= BUSHY_MAX_CUT)
            return emit_bushy_tree(ec, atoms, n, iface_slots, iface_ar);
    }
    return emit_leftdeep_seq(ec, atoms, n, iface_slots, iface_ar);
}

/* ─── Compile one rule ──────────────────────────────────────────────── */

static compiled_rule *compile_one(dl_db *db, rule *r, int *rel_strata,
                                  const int *recursive)
{
    v_tab vt; i_buf ib; compiled_rule *cr = NULL;
    g_rule_off = r->off;

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
            cerr(r->off, "compile error: negated pattern atom not supported "
                    "(rule '%s')\n", r->head->pred); goto fail;
        }
        regex_dfa *dfa = regex_compile(ba->pattern);
        if (dfa->errmsg) {
            cerr(r->off, "compile error: bad regex pattern '%s': %s "
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
                cerr(r->off, "compile error: multiple aggregates not supported "
                        "(rule '%s')\n", r->head->pred); goto fail;
            }
            agg_body_idx = bi;
        }
    }
    atom *agg = (agg_body_idx >= 0) ? r->body[agg_body_idx] : NULL;
    if (agg && agg->negated) {
        cerr(r->off, "compile error: aggregate inside negation not supported "
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
            cerr(r->off, "compile error: unknown aggregate '%s' (rule '%s')\n",
                    opname, r->head->pred); goto fail;
        }
        if (agg_op_code == 0) {
            if (agg->nargs != 0) {
                cerr(r->off, "compile error: 'count' takes no arguments (rule '%s')\n",
                        r->head->pred); goto fail;
            }
        } else {
            if (agg->nargs != 1 || agg->args[0]->kind != TOK_VAR) {
                cerr(r->off, "compile error: 'sum/min/max' require a source variable "
                        "(rule '%s')\n", r->head->pred); goto fail;
            }
            agg_src_var = agg->args[0]->text;
        }
    }

    /* ── 1b. v2-lists: reject list PATTERNS in the head/fact.  A head arg may
     * be a CONSTANT list literal (interning a list value) but never a
     * [X|Xs] pattern — construct lists with cons(...) instead.  Reject here,
     * BEFORE collect-vars, so a pattern var never enters the var table. */
    for (i = 0; i < r->head->nargs; i++) {
        if (list_is_pattern(r->head->args[i])) {
            cerr(r->off, "compile error: list pattern in head/fact of '%s' "
                    "— use cons(...) to construct lists\n", r->head->pred);
            goto fail;
        }
    }

    /* 1c. v2-lists FIX-2: a reserved BUILTIN name cannot be a rule HEAD.
     * These names are always classified as builtins in body position, so a
     * head of the same name would either silently shadow a declared relation
     * or (for a fact) fall through to the misleading "rule X has no body".
     * Reject loudly here with a clear diagnostic BEFORE head resolution. */
    if (is_reserved_builtin_name(r->head->pred)) {
        cerr(r->off, "compile error: '%s' is a reserved builtin predicate "
                "name and cannot be used as a rule head (rule '%s')\n",
                r->head->pred, r->head->pred);
        goto fail;
    }

    /* 2. resolve head */
    head_ri = db_find_rel(db, r->head->pred);
    if (head_ri < 0) {
        if (r->head->nargs < 1 || r->head->nargs > MAX_ARITY) {
            cerr(r->off, "compile error: head arity %d for '%s'\n",
                    r->head->nargs, r->head->pred); goto fail;
        }
        if (dl_declare_relation(db, r->head->pred, (uint8_t)r->head->nargs))
        { cerr(r->off, "compile error: cannot declare '%s/%d'\n",
                   r->head->pred, r->head->nargs); goto fail; }
        head_ri = db_find_rel(db, r->head->pred);
        if (head_ri < 0) goto fail;
    } else if (db_rel_is_variadic(db, head_ri)) {
        /* v2: a variadic head accepts any static nargs in 1..8 — the rule
         * PROJECTS into variant[nargs].  Materialize the variant up front
         * (durable, WAL-backed) so the fixpoint's view-reset and the
         * OP_PROJECT write both see it.  To make a head variadic at all,
         * the user must dl_declare_relation_variadic BEFORE dl_load_rules
         * (an undeclared head is declared fixed, as in v1). */
        if (r->head->nargs < 1 || r->head->nargs > MAX_ARITY) {
            cerr(r->off, "compile error: head arity %d for variadic '%s'\n",
                    r->head->nargs, r->head->pred); goto fail;
        }
        if (!dl_ensure_variant(db, head_ri, (uint8_t)r->head->nargs)) {
            cerr(r->off, "compile error: cannot materialize variant %d "
                    "of variadic '%s'\n",
                    r->head->nargs, r->head->pred); goto fail;
        }
    } else {
        if (db_rel_arity(db, head_ri) != (uint8_t)r->head->nargs) {
            cerr(r->off, "compile error: arity mismatch for '%s': %d vs %d\n",
                    r->head->pred, db_rel_arity(db, head_ri), r->head->nargs);
            goto fail;
        }
    }

    /* ── 3. resolve body ─────────────────────────────────────────── */
    if (r->nbody == 0) {
        cerr(r->off, "compile error: rule '%s' has no body\n", r->head->pred);
        goto fail;
    }
    /* v1: reject constants in the head of an aggregate rule — all head
     * args must be the result var or group-by vars. */
    if (agg) {
        for (i = 0; i < r->head->nargs; i++) {
            if (r->head->args[i]->kind != TOK_VAR) {
                cerr(r->off, "compile error: constants in head of aggregate rule not "
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
                cerr(r->off, "compile error: negated builtin not supported "
                        "(rule '%s')\n", r->head->pred); goto fail;
            }
            /* M9-strings: validate arity/operand/result kinds so a malformed
             * string builtin (bad arity, INT operand) is rejected loudly
             * rather than silently mis-evaluated. */
            if (is_str_builtin(ba) && !str_builtin_valid(ba)) {
                cerr(r->off, "compile error: malformed string builtin "
                        "'%s' (bad arity or non-symbol operand) "
                        "(rule '%s')\n", ba->pred, r->head->pred);
                goto fail;
            }
            /* v2-lists: same validation for list builtins (bad arity / result
             * not a variable / operand not a variable-or-constant). */
            if (is_list_builtin(ba) && !list_builtin_valid(ba)) {
                cerr(r->off, "compile error: malformed list builtin "
                        "'%s' (bad arity or non-variable result / non-constant "
                        "operand) (rule '%s')\n", ba->pred, r->head->pred);
                goto fail;
            }
            /* v2-range: validate `range(X, Rel, Lo, Hi)` — 4 args, X a
             * variable, Rel a known non-variadic arity>=1 relation name,
             * Lo/Hi variable-or-int bounds.  Rejects unknown/variadic/empty
             * relations loudly (never silently mis-evaluate). */
            if (is_range_builtin(ba) && !range_builtin_valid(db, ba)) {
                cerr(r->off, "compile error: malformed range builtin "
                        "'%s' (expected range(X, Rel, Lo, Hi): X a variable, "
                        "Rel a known non-variadic arity>=1 relation, Lo/Hi "
                        "variable-or-int bounds) (rule '%s')\n",
                        ba->pred, r->head->pred);
                goto fail;
            }
            /* RECURSIVE-STALENESS GATE: OP_RANGE reads db->rels[rel].rel
             * directly, which the recursive fixpoint NEVER updates (the
             * idb grows in tuple_sets, not rel->d) — range over a RECURSIVE
             * relation would read a STALE view and silently mis-evaluate.
             * Range over EDB and non-recursive IDB is safe (materialized
             * before the range rule, same read-point as OP_SCAN). */
            if (is_range_builtin(ba)) {
                int rr = db_find_rel(db, ba->args[1]->text);
                if (rr >= 0 && recursive && recursive[rr]) {
                    cerr(r->off, "compile error: range over recursive "
                            "relation '%s' is not supported (OP_RANGE reads "
                            "the idb directly, which the fixpoint never "
                            "updates) (rule '%s')\n", ba->args[1]->text,
                            r->head->pred);
                    goto fail;
                }
            }
            bri[bi] = -1; continue;
        }
        /* v2-lists: a NEGATED relational atom cannot destructure a list
         * pattern (it never binds vars under negation) — reject loudly. */
        if (ba->negated) {
            for (j = 0; j < ba->nargs; j++) {
                if (list_is_pattern(ba->args[j])) {
                    cerr(r->off, "compile error: list pattern in negated "
                            "atom '%s' (patterns cannot bind vars under "
                            "negation) (rule '%s')\n",
                            ba->pred, r->head->pred);
                    goto fail;
                }
            }
        }
        int ri = db_find_rel(db, ba->pred);
        if (ri < 0) {
            cerr(r->off, "compile error: unknown predicate '%s'\n",
                    ba->pred); goto fail;
        }
        if (db_rel_is_variadic(db, ri)) {
            /* v2: a variadic body atom accepts any static nargs in 1..8;
             * the VM reads variant[nargs] (an absent variant reads as an
             * EMPTY relation — no materialization needed for reads). */
            if (ba->nargs < 1 || ba->nargs > MAX_ARITY) {
                cerr(r->off, "compile error: body arity %d for variadic "
                        "'%s'\n", ba->nargs, ba->pred);
                goto fail;
            }
        } else if (db_rel_arity(db, ri) != (uint8_t)ba->nargs) {
            cerr(r->off, "compile error: arity mismatch for '%s'\n",
                    ba->pred); goto fail;
        }
        bri[bi] = ri;
    }

    /* ── 3b. v2: reject aggregate rules that touch a variadic relation ──.
     * The group-by/leading-prefix structure of the aggregate machinery is
     * undefined across arities — reject at COMPILE time, never silently
     * mis-evaluate. */
    if (agg) {
        if (db_rel_is_variadic(db, head_ri)) {
            cerr(r->off, "compile error: aggregate over a variadic "
                    "relation is not supported (head '%s', rule '%s')\n",
                    r->head->pred, r->head->pred);
            goto fail;
        }
        for (bi = 0; bi < r->nbody; bi++) {
            if (bri[bi] < 0) continue;   /* builtin / aggregate atom */
            if (db_rel_is_variadic(db, bri[bi])) {
                cerr(r->off, "compile error: aggregate over a variadic "
                        "relation is not supported (body '%s', rule '%s')\n",
                        r->body[bi]->pred, r->head->pred);
                goto fail;
            }
        }
    }

    /* ── 4. collect vars ─────────────────────────────────────────── */
    for (i = 0; i < r->head->nargs; i++)
        if (r->head->args[i]->kind == TOK_VAR && v_add(&vt, r->head->args[i]->text) < 0) goto fail;
    for (i = 0; i < r->nbody; i++)
        for (j = 0; j < r->body[i]->nargs; j++)
            if (r->body[i]->args[j]->kind == TOK_VAR && v_add(&vt, r->body[i]->args[j]->text) < 0) goto fail;
    /* v2-lists: vars nested inside a list PATTERN (element vars + tail var)
     * must also be in the var table so v_find resolves them during the safety
     * passes and lowering. */
    for (i = 0; i < r->nbody; i++)
        for (j = 0; j < r->body[i]->nargs; j++)
            if (collect_token_vars(r->body[i]->args[j], &vt) < 0) goto fail;
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
                if (is_list_assign(ba)) {
                    /* [X|Xs] = L is lowered in the EQUALITY phase, which runs
                     * AFTER the relational join AND its interleaved NEG_CHECK
                     * instructions.  So the pattern vars (args[0]) are NOT
                     * bound during negation — a negated atom cannot safely
                     * read them (it would NEG_CHECK the UNBOUND sentinel and
                     * silently backtrack).  Do NOT mark them bound here: a
                     * negated atom reading a pattern var is rejected as unsafe
                     * negation, mirroring the car/cdr desugaring. */
                    continue;
                }
                /* Equality X = Y is lowered in the EQUALITY phase, which runs
                 * AFTER the relational join AND its interleaved NEG_CHECK
                 * instructions.  So its vars are NOT bound during negation —
                 * a negated atom cannot safely read a var that is only bound
                 * by an equality (it would NEG_CHECK the UNBOUND sentinel and
                 * silently backtrack to a wrong empty result).  Do NOT
                 * propagate binding here: a negated atom reading an
                 * equality-bound var is rejected as unsafe, matching the
                 * other post-relational builtins (arith/str/list).  (Note: a
                 * var that is ALSO bound by a positive relational atom is
                 * already marked bound by that atom's own pass.) */
                continue;
            }
            if (is_arith(ba) || is_comparison(ba) || is_str_builtin(ba) ||
                is_list_builtin(ba) || is_range_builtin(ba)) {
                /* builtins execute AFTER the relational phase (NEG_CHECK runs
                 * during it), so their vars are NOT bound for negation-safety
                 * purposes — a negated atom cannot safely read an arithmetic /
                 * string-builtin result var or a comparison / string-filter /
                 * range operand. */
                continue;
            }
            if (ba->negated) {
                for (j = 0; j < ba->nargs; j++) {
                    if (ba->args[j]->kind == TOK_VAR) {
                        int vi = v_find(&vt, ba->args[j]->text);
                        if (vi < 0 || !bound_vars[vi]) {
                            cerr(r->off, 
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
                for (j = 0; j < ba->nargs; j++)
                    mark_token_vars_bound(ba->args[j], &vt, bound_vars);
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
                mark_token_vars_bound(ba->args[j], &vt, bound_vars);
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
            if (!is_equality(ba)) continue;
            if (is_list_assign(ba)) {
                /* [X|Xs] = L: the RHS value L must be bound (it is the list
                 * being destructured; destructuring an unbound L would
                 * silently produce nothing).  When L is bound, the pattern
                 * vars (args[0]) become bound. */
                int vi1 = (ba->nargs >= 2 && ba->args[1]->kind == TOK_VAR)
                              ? v_find(&vt, ba->args[1]->text) : -1;
                int b1 = (vi1 >= 0) && bound_vars[vi1];
                if (ba->args[1]->kind == TOK_VAR) {
                    if (!b1) {
                        cerr(r->off, "compile error: ungrounded list "
                                "assignment — list variable '%s' in '[X|Xs] = "
                                "%s' is not bound by a positive body atom "
                                "(rule '%s')\n",
                                ba->args[1]->text, ba->args[1]->text,
                                r->head->pred);
                        goto fail;
                    }
                    bound_vars[vi1] = 1;
                }
                mark_token_vars_bound(ba->args[0], &vt, bound_vars);
                continue;
            }
            {
            int vi0, vi1, b0, b1;
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
            } else if (is_list_producing(ba)) {
                if (!list_producing_operands_bound(ba, &vt, bound_vars,
                                                   r->head->pred))
                    goto fail;
                {
                    int vi = v_find(&vt, ba->args[0]->text);
                    if (vi >= 0) bound_vars[vi] = 1;
                }
            } else if (is_list_filter(ba)) {
                /* member(X, L): L must be bound (X may be unbound → generated
                 * by the VM).  Member runs in the producer phase (it binds X)
                 * so X is bound for any LATER producer/filter. */
                if (!member_operand_bound(ba, &vt, bound_vars, r->head->pred))
                    goto fail;
                {
                    int vi = v_find(&vt, ba->args[0]->text);
                    if (vi >= 0) bound_vars[vi] = 1;
                }
            } else if (is_range_builtin(ba)) {
                /* range(X, Rel, Lo, Hi): Lo/Hi must be bound (X may be unbound
                 * → GENERATED by the VM from Rel's distinct col0 in [Lo,Hi)).
                 * Range runs in the producer phase (it binds X). */
                if (!range_operands_bound(ba, &vt, bound_vars, r->head->pred))
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
                        cerr(r->off, 
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
            cerr(r->off, "compile error: division/modulo by literal 0 "
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
                    (is_arith(ba) || is_str_producing(ba) ||
                     is_list_producing(ba)))
                    { ares = 1; break; }
            }
            if (ares) continue;
        }
        int ok = 0;
        for (bi = 0; bi < r->nbody && !ok; bi++)
            for (j = 0; j < r->body[bi]->nargs; j++)
                if (token_contains_var(r->body[bi]->args[j], a->text))
                    { ok = 1; break; }
        if (!ok) {
            cerr(r->off, "compile error: ungrounded variable '%s' in head of '%s'\n",
                    a->text, r->head->pred); goto fail;
        }
    }

    /* ── 8. emit bytecode ────────────────────────────────────────── */

    if (!r->has_negation) {
        /* BUSHY (v2): negation-free rules take the always-on left-deep
         * reorder, and optionally a binary-tree (bushy) join plan when a
         * natural 2-partition exists. */
        int *pos = malloc((size_t)(r->nbody > 0 ? r->nbody : 1) * sizeof(int));
        uint64_t *mask = calloc((size_t)(r->nbody > 0 ? r->nbody : 1),
                                sizeof(uint64_t));
        int n_pos = 0;
        int any_pattern = 0;

        if (!pos || !mask) { free(pos); free(mask); goto fail; }
        for (bi = 0; bi < r->nbody; bi++) {
            atom *ba = r->body[bi];
            if (ba->negated || ba->aggregate) continue;
            if (is_builtin_pred(ba)) continue;
            pos[n_pos++] = bi;
            mask[bi] = atom_var_mask(r, bi, &vt);
            /* FIX-3 (defense-in-depth): a list PATTERN in a positive atom also
             * gates bushy.  [X|Xs] destructuring binds vars INLINE during the
             * relational phase (emit_pattern), so bushy+pattern is CORRECT
             * today (the shared-column mask-to-slots math holds — pattern vars
             * are in atom_var_mask).  But that is an implicit invariant: if
             * pattern lowering ever became non-inline (deferred to a post-join
             * phase), a bushy plan could silently misjoin on a pattern-bound
             * var.  Disable bushy for any rule carrying a list pattern so a
             * future lowering change can never mis-evaluate. */
            for (j = 0; j < ba->nargs; j++)
                if (list_is_pattern(ba->args[j])) { any_pattern = 1; break; }
            if (pat_idx[bi] != 0xFF) any_pattern = 1;
        }
        if (n_pos == 0) {
            /* v2-lists FIX-1: a lone member(X,L) list-filter atom whose list
             * operand L is bound-or-constant is a legitimate positive DRIVER —
             * the producer phase emits OP_LIST_MEMBER, which GENERATES X from
             * L's elements.  Similarly, a lone [X|Xs] = L list ASSIGNMENT
             * whose RHS is a CONSTANT is a driver — the equality phase
             * materializes the constant RHS and emit_pattern destructures it,
             * binding X/Xs.  (An UNBOUND var L was already rejected loudly by
             * member_operand_bound / the 5b list-assign check.)  Emit NO
             * relational join: skip the join below and let the member
             * generator / constant-list destructuring drive the head. */
            int list_driver = 0;
            for (bi = 0; bi < r->nbody; bi++) {
                atom *ba = r->body[bi];
                if (is_list_filter(ba) || is_range_builtin(ba)) { list_driver = 1; break; }
                if (is_list_assign(ba) && ba->args[1]->kind != TOK_VAR) {
                    list_driver = 1; break;
                }
            }
            if (!list_driver) {
                free(pos); free(mask);
                cerr(r->off, "compile error: rule '%s' has no positive body atom\n",
                        r->head->pred);
                goto fail;
            }
        } else {
            emit_ctx ec;
            if (g_reorder)
                reorder_pos_atoms(db, bri, mask, pos, n_pos);

            memset(&ec, 0, sizeof(ec));
            ec.db = db; ec.r = r; ec.bri = bri; ec.pat_idx = pat_idx;
            ec.vt = &vt; ec.ib = &ib; ec.cc = &cc; ec.mask = mask;
            ec.do_bushy = g_bushy && (agg == NULL) && !any_pattern;
            ec.recursive = recursive;
            ec.next_buf = 0;
            if (emit_join(&ec, pos, n_pos, NULL, 0) != 0) {
                free(pos); free(mask);
                goto fail;
            }
        }
        free(pos); free(mask);
    } else {
        int first_pos = -1;

        for (bi = 0; bi < r->nbody; bi++) {
            atom *ba = r->body[bi];
            if (ba->negated || ba->aggregate) continue;
            if (is_builtin_pred(ba)) continue;
            first_pos = bi; break;
        }
        if (first_pos < 0) {
            /* v2-lists FIX-1: a lone member(X,L) list-filter atom with a
             * bound-or-constant L is a legitimate positive DRIVER even in a
             * rule with negation — the producer phase emits OP_LIST_MEMBER,
             * which GENERATES X from L's elements.  A lone [X|_] = [1,2] list
             * ASSIGNMENT with a constant RHS is likewise a driver (the
             * equality phase destructures the constant, binding the pattern
             * vars).  (An unbound var operand was already rejected loudly by
             * member_operand_bound / the 5b list-assign check.)
             * In this case there is NO positive relational atom to scan: we
             * still emit NEG_CHECK for the negated atoms (handled below by the
             * "remaining body atoms" loop, which starts at bi==0 when
             * first_pos<0), but we skip the first-positive SCAN/WALK block. */
            int list_driver = 0;
            for (bi = 0; bi < r->nbody; bi++) {
                atom *ba = r->body[bi];
                if (is_list_filter(ba) || is_range_builtin(ba)) { list_driver = 1; break; }
                if (is_list_assign(ba) && ba->args[1]->kind != TOK_VAR) {
                    list_driver = 1; break;
                }
            }
            if (!list_driver) {
                cerr(r->off, "compile error: rule '%s' has no positive body atom\n",
                        r->head->pred);
                goto fail;
            }
        }

        /* NEG_CHECK for negated atoms before first positive (none when a lone
         * member drives — first_pos stays -1, so the loop is empty). */
        for (bi = 0; bi < first_pos; bi++) {
            atom *na = r->body[bi];
            uint8_t cslot[8];
            vm_instr *neg;
            /* Materialize any CONSTANT arg into a fresh temp slot FIRST (an
             * OP_EQ_CONST), so OP_NEG_CHECK below reads the slot already
             * bound — otherwise it would read the UNBOUND sentinel and
             * silently backtrack to a wrong empty result. */
            for (j = 0; j < na->nargs; j++) {
                if (na->args[j]->kind != TOK_VAR) {
                    char cname[16];
                    v_fresh_name(&vt, cname, sizeof(cname), &cc, 'k');
                    int vi = v_add(&vt, cname);
                    if (vi < 0) goto fail;
                    cslot[j] = vt.e[vi].slot;
                    uint32_t cv;
                    if (token_const(db, na->args[j], &cv)) goto fail;
                    vm_instr *eq = i_emit(&ib);
                    if (!eq) goto fail;
                    eq->op = OP_EQ_CONST; eq->a = cslot[j]; eq->imm = cv;
                }
            }
            neg = i_emit(&ib);
            neg->op = OP_NEG_CHECK;
            neg->a = (uint8_t)bri[bi];
            neg->b = (uint8_t)na->nargs;
            neg->body_idx = (uint8_t)bi;
            for (j = 0; j < na->nargs; j++) {
                if (na->args[j]->kind == TOK_VAR) {
                    int vi = v_find(&vt, na->args[j]->text);
                    if (vi < 0) goto fail;
                    neg->slots[j] = vt.e[vi].slot;
                } else {
                    neg->slots[j] = cslot[j];
                }
            }
        }

        /* First positive body atom → SCAN or WALK (skipped when a lone member
         * drives the rule — there is no relational atom to scan; the member
         * generator in the producer phase drives X). */
        if (first_pos >= 0) {
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

            if (emit_const_or_pattern(db, a0, ts, &vt, &ib, &cc,
                                      r->head->pred))
                goto fail;
        }
        } /* if (first_pos >= 0) */

        /* Remaining body atoms */
        for (bi = first_pos + 1; bi < r->nbody; bi++) {
            atom *curr = r->body[bi];

            if (curr->aggregate) continue;          /* AGG_ACC emitted below */
            if (is_builtin_pred(curr)) continue; /* equality/builtins (arith, comparison, string) emitted below */

            if (curr->negated) {
                uint8_t cslot[8];
                vm_instr *neg;
                /* Materialize any CONSTANT arg into a fresh temp slot FIRST
                 * (an OP_EQ_CONST), so OP_NEG_CHECK below reads the slot
                 * already bound — otherwise it would read the UNBOUND
                 * sentinel and silently backtrack to a wrong empty result. */
                for (j = 0; j < curr->nargs; j++) {
                    if (curr->args[j]->kind != TOK_VAR) {
                        char cname[16];
                        v_fresh_name(&vt, cname, sizeof(cname), &cc, 'k');
                        int vi = v_add(&vt, cname);
                        if (vi < 0) goto fail;
                        cslot[j] = vt.e[vi].slot;
                        uint32_t cv;
                        if (token_const(db, curr->args[j], &cv)) goto fail;
                        vm_instr *eq = i_emit(&ib);
                        if (!eq) goto fail;
                        eq->op = OP_EQ_CONST; eq->a = cslot[j]; eq->imm = cv;
                    }
                }
                neg = i_emit(&ib);
                neg->op = OP_NEG_CHECK;
                neg->a = (uint8_t)bri[bi];
                neg->b = (uint8_t)curr->nargs;
                neg->body_idx = (uint8_t)bi;
                for (j = 0; j < curr->nargs; j++) {
                    if (curr->args[j]->kind == TOK_VAR) {
                        int vi = v_find(&vt, curr->args[j]->text);
                        if (vi < 0) goto fail;
                        neg->slots[j] = vt.e[vi].slot;
                    } else {
                        neg->slots[j] = cslot[j];
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
                if (emit_const_or_pattern(db, curr, slot_map, &vt, &ib, &cc,
                                          r->head->pred))
                    goto fail;
            } else {
                /* No leading shared columns.  Check for non-leading join. */
                int any_shared = 0;
                for (j = 0; j < curr->nargs; j++) if (sc[j]) { any_shared = 1; break; }

                if (any_shared) {
                    /* M6-permsel: non-leading-column join — cost-gated perm vs hash */
                    if (emit_nonleading_join(db, r, curr, bri[bi], sc, recursive,
                                             &vt, &ib, &cc, ip, bi))
                        goto fail;
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
                    if (emit_const_or_pattern(db, curr, ts, &vt, &ib, &cc,
                                              r->head->pred))
                        goto fail;
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
        if (is_list_assign(ba)) {
            /* [X|Xs] = L: materialize the RHS value into a slot (var → its
             * bound slot; constant/list-literal → interned via OP_EQ_CONST),
             * then destructure the LHS pattern from it with emit_pattern
             * (OP_LIST_CAR/CDR + equality + tail/NIL check).  Runs in the
             * equality phase (after the relational join), so a var RHS is
             * already bound. */
            int rs = cmp_operand_slot(db, ba->args[1], &vt, &ib, &cc, 1);
            if (rs < 0) goto fail;
            if (emit_pattern(db, ba->args[0], (uint8_t)rs, &vt, &ib, &cc,
                             r->head->pred))
                goto fail;
            continue;
        }
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
                ls = cmp_operand_slot(db, ba->args[1], &vt, &ib, &cc, 0);
                if (ls < 0) goto fail;
                if (!strcmp(ba->pred, "concat")) {
                    rs = cmp_operand_slot(db, ba->args[2], &vt, &ib, &cc, 0);
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
            } else {  /* length (string OR list — runtime dispatch) */
                ls = cmp_operand_slot(db, ba->args[1], &vt, &ib, &cc, 1);
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
        } else if (is_list_producing(ba)) {
            /* OP_LIST_CONS / CAR / CDR / APPEND: resolve operands FIRST
             * (a TOK_LIST / int / symbol constant materializes via an
             * OP_EQ_CONST that must precede the list opcode), then emit the
             * opcode into a fresh temp, then a final OP_EQ(result, temp)
             * reusing bind-or-filter semantics (never silently overwrites a
             * bound result var).  Encodings: CONS a=result b=head c=tail;
             * CAR/CDR a=operand c=result; APPEND a,b=operands c=result. */
            if (ba->negated) continue;  /* negated builtin already rejected */
            char cname[16];
            int vi, ls, rs, opcode;
            vm_instr *op;
            v_fresh_name(&vt, cname, sizeof(cname), &tc, 't');
            vi = v_add(&vt, cname);
            if (vi < 0) goto fail;

            ls = rs = -1;
            if (!strcmp(ba->pred, "cons")) {
                ls = cmp_operand_slot(db, ba->args[1], &vt, &ib, &cc, 1);
                rs = cmp_operand_slot(db, ba->args[2], &vt, &ib, &cc, 1);
                if (ls < 0 || rs < 0) goto fail;
                opcode = OP_LIST_CONS;
            } else if (!strcmp(ba->pred, "car")) {
                ls = cmp_operand_slot(db, ba->args[1], &vt, &ib, &cc, 1);
                if (ls < 0) goto fail;
                opcode = OP_LIST_CAR;
            } else if (!strcmp(ba->pred, "cdr")) {
                ls = cmp_operand_slot(db, ba->args[1], &vt, &ib, &cc, 1);
                if (ls < 0) goto fail;
                opcode = OP_LIST_CDR;
            } else {  /* append */
                ls = cmp_operand_slot(db, ba->args[1], &vt, &ib, &cc, 1);
                rs = cmp_operand_slot(db, ba->args[2], &vt, &ib, &cc, 1);
                if (ls < 0 || rs < 0) goto fail;
                opcode = OP_LIST_APPEND;
            }

            op = i_emit(&ib);
            if (!op) goto fail;
            op->op = (uint8_t)opcode;
            op->body_idx = (uint8_t)bi;
            if (opcode == OP_LIST_CONS) {
                op->a = vt.e[vi].slot;  /* result */
                op->b = (uint8_t)ls;    /* head */
                op->c = (uint8_t)rs;    /* tail */
            } else if (opcode == OP_LIST_CAR || opcode == OP_LIST_CDR) {
                op->a = (uint8_t)ls;    /* operand */
                op->c = vt.e[vi].slot;  /* result */
            } else {                    /* append */
                op->a = (uint8_t)ls;    /* first operand */
                op->b = (uint8_t)rs;    /* second operand */
                op->c = vt.e[vi].slot;  /* result */
            }

            int rvi = v_find(&vt, ba->args[0]->text);
            if (rvi < 0) goto fail;
            vm_instr *eq = i_emit(&ib);
            eq->op = OP_EQ;
            eq->a = vt.e[rvi].slot;
            eq->b = vt.e[vi].slot;
            eq->body_idx = (uint8_t)bi;
        } else if (is_list_filter(ba)) {
            /* member(X, L): a=the list operand slot, b=the member var slot.
             * L is materialized (a constant list/other constant) or bound by
             * an earlier relational atom; the VM tests X against L's elements
             * when X is bound, else generates X from each element. */
            if (ba->negated) continue;  /* negated builtin already rejected */
            int ls = cmp_operand_slot(db, ba->args[1], &vt, &ib, &cc, 1);
            if (ls < 0) goto fail;
            {
                int xvi = v_find(&vt, ba->args[0]->text);
                vm_instr *m;
                if (xvi < 0) goto fail;
                m = i_emit(&ib);
                if (!m) goto fail;
                m->op = OP_LIST_MEMBER;
                m->a = (uint8_t)ls;             /* list operand slot */
                m->b = vt.e[xvi].slot;          /* member var slot */
                m->slots[0] = vt.e[xvi].slot;   /* generator bind target */
                m->body_idx = (uint8_t)bi;
            }
        } else if (is_range_builtin(ba)) {
            /* range(X, Rel, Lo, Hi): a=lo slot, b=hi slot, c=X slot,
             * imm=rel_id.  Lo/Hi materialized (constants via OP_EQ_CONST)
             * or bound by an earlier relational atom; the VM tests X against
             * Rel's distinct col0 values in [Lo,Hi) when X is bound, else
             * GENERATES X from them (a producer, like member). */
            if (ba->negated) continue;  /* negated builtin already rejected */
            {
                int los = cmp_operand_slot(db, ba->args[2], &vt, &ib, &cc, 0);
                int his = cmp_operand_slot(db, ba->args[3], &vt, &ib, &cc, 0);
                int xvi, rri;
                vm_instr *m;
                if (los < 0 || his < 0) goto fail;
                xvi = v_find(&vt, ba->args[0]->text);
                rri = db_find_rel(db, ba->args[1]->text);
                if (xvi < 0 || rri < 0) goto fail;
                m = i_emit(&ib);
                if (!m) goto fail;
                m->op = OP_RANGE;
                m->a = (uint8_t)los;            /* lo slot */
                m->b = (uint8_t)his;            /* hi slot */
                m->c = vt.e[xvi].slot;          /* X var slot */
                m->imm = (uint32_t)rri;         /* rel_id */
                m->slots[0] = vt.e[xvi].slot;   /* generator bind target */
                m->body_idx = (uint8_t)bi;
            }
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
            int ls = cmp_operand_slot(db, ba->args[0], &vt, &ib, &cc, 0);
            int rs = cmp_operand_slot(db, ba->args[1], &vt, &ib, &cc, 0);
            if (ls < 0 || rs < 0) goto fail;
            vm_instr *c = i_emit(&ib);
            c->op = OP_CMP;
            c->a = (uint8_t)ls;
            c->b = (uint8_t)rs;
            c->imm = (uint32_t)cmp_op_code(ba->pred);
            c->body_idx = (uint8_t)bi;
        } else if (is_str_filter(ba)) {
            if (ba->negated) continue;  /* negated builtin already rejected */
            int ls = cmp_operand_slot(db, ba->args[0], &vt, &ib, &cc, 0);
            int rs = cmp_operand_slot(db, ba->args[1], &vt, &ib, &cc, 0);
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
            cerr(r->off, "compile error: too many group-by columns (rule '%s')\n",
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
        cerr(r->off, "compile error: rule '%s' exceeds the maximum of %d "
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

    compile_has_err = 0;
    compile_err_off = 0;
    compile_err_msg[0] = '\0';

    if (!db || !rules || n_rules <= 0 || !out_rules || !out_n) return -1;

    /* Declare any missing head relations so stratification sees all nodes */
    for (i = 0; i < n_rules; i++) {
        rule *r = rules[i];
        int ri = db_find_rel(db, r->head->pred);
        if (ri < 0) {
            if (r->head->nargs < 1 || r->head->nargs > MAX_ARITY) {
                cerr(r->off, "compile error: head arity %d for '%s'\n",
                        r->head->nargs, r->head->pred);
                return -1;
            }
            if (dl_declare_relation(db, r->head->pred, (uint8_t)r->head->nargs)) {
                cerr(r->off, "compile error: cannot declare '%s/%d'\n",
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
            c[i] = compile_one(db, rules[i], rel_strata, recursive);
            if (!c[i]) {
                int j; for (j = 0; j < i; j++) compiled_rule_free(c[j]);
                free(c); free(recursive); free(rel_strata); return -1;
            }
            /* SCC-based recursion (fixes B3): recursive if head_rel is
             * in an SCC with >1 node or has a self-loop */
            c[i]->is_recursive =
                (c[i]->head_rel_id < (uint8_t)nrels
                 && recursive[c[i]->head_rel_id]) ? 1 : 0;

            /* v2: a RECURSIVE variadic head is out of scope — the
             * semi-naive fixpoint tracks one single-arity idb/delta
             * tuple_set pair per recursive head, which cannot represent a
             * mixed-arity family.  Reject at compile time (never silently
             * mis-evaluate).  Recursive rules whose BODY merely READS
             * variadic EDB variants remain fine. */
            if (c[i]->is_recursive &&
                db_rel_is_variadic(db, c[i]->head_rel_id)) {
                cerr(rules[i]->off, "compile error: recursive rule over a "
                        "variadic head is not supported (rule '%s')\n",
                        c[i]->head_pred);
                int j; for (j = 0; j <= i; j++) compiled_rule_free(c[j]);
                free(c); free(recursive); free(rel_strata); return -1;
            }

            /* M3: aggregates only allowed in non-recursive rules */
            if (c[i]->has_aggregate && c[i]->is_recursive) {
                cerr(rules[i]->off, "compile error: aggregate in recursive rule "
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
