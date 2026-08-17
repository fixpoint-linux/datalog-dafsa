/*
 * typecheck.c — S3 per-rule Datalog typechecker (occurrence-consistency)
 *
 * Given a dl_schema and the parser's rule** from dl_load_rules, verify each
 * rule's variables are used with a CONSISTENT type across every occurrence
 * (head + body).  v1 does NO polymorphism/unification: each variable maps to a
 * single dl_colspec for the whole rule; the first conflicting occurrence is
 * reported with both sites (file:line:col via the S1 line/col fields).
 *
 * Atom dispatch mirrors compiler.c's name-based recognition (the builtin
 * predicate-name sets are COPIED here — the compiler's helpers are static, so
 * this module must not depend on compiler internals).  List/range builtins are
 * rejected in v1 ("not yet typed").  Stratification is the compiler's job, so a
 * negated atom is typed exactly like its positive form.
 *
 * Because dl_typecheck_rules is called by dl_load_rules which knows no source
 * filename, the "file" component of every diagnostic is the literal `<input>`.
 */

#include "typecheck.h"
#include "parser.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ─── Copy of the builtin predicate-name classification (from compiler.c,
 *    which keeps these static — do NOT depend on compiler internals) ───── */

static int is_comparison_pred(const char *p)
{
    if (!p) return 0;
    return strcmp(p, "<") == 0 || strcmp(p, "<=") == 0 ||
           strcmp(p, ">") == 0 || strcmp(p, ">=") == 0 ||
           strcmp(p, "!=") == 0;
}

static int is_str_producing_pred(const char *p)
{
    if (!p) return 0;
    return strcmp(p, "concat") == 0 || strcmp(p, "length") == 0 ||
           strcmp(p, "lower")  == 0 || strcmp(p, "upper")  == 0;
}

static int is_str_filter_pred(const char *p)
{
    if (!p) return 0;
    return strcmp(p, "prefix")   == 0 ||
           strcmp(p, "suffix")   == 0 ||
           strcmp(p, "contains") == 0;
}

static int is_list_builtin_pred(const char *p)
{
    if (!p) return 0;
    return strcmp(p, "cons")   == 0 || strcmp(p, "car")    == 0 ||
           strcmp(p, "cdr")    == 0 || strcmp(p, "append") == 0 ||
           strcmp(p, "member") == 0;
}

static int is_range_builtin_pred(const char *p)
{
    if (!p) return 0;
    return strcmp(p, "range") == 0;
}

static int is_reserved_builtin_name(const char *name)
{
    if (!name) return 0;
    return strcmp(name, "member") == 0 || strcmp(name, "car") == 0 ||
           strcmp(name, "cons") == 0  || strcmp(name, "cdr") == 0 ||
           strcmp(name, "append") == 0 || strcmp(name, "concat") == 0 ||
           strcmp(name, "length") == 0 || strcmp(name, "lower") == 0 ||
           strcmp(name, "upper") == 0  || strcmp(name, "prefix") == 0 ||
           strcmp(name, "suffix") == 0 || strcmp(name, "contains") == 0 ||
           strcmp(name, "range") == 0;
}

/* ─── Per-rule variable table ─────────────────────────────────────────── */

typedef struct varent {
    char       *name;      /* owned copy of the variable name */
    dl_colspec  type;      /* tag 0 = untyped so far */
    int         line;      /* first site that typed it (S1 line:col) */
    int         col;
    const char *site;      /* human-readable label of that first site */
} varent;

typedef struct {
    varent *v;
    int     n;
    int     cap;
} vtab;

/* Recursive helper for arithmetic expr trees (defined below type_arith). */
static int type_expr(vtab *t, const expr *e, int line, int col,
                     const char *site, char *errbuf, size_t errcap);

static void vtab_free(vtab *t)
{
    int i;
    if (!t) return;
    for (i = 0; i < t->n; i++) free(t->v[i].name);
    free(t->v);
    memset(t, 0, sizeof(*t));
}

static varent *vtab_find(vtab *t, const char *name)
{
    int i;
    for (i = 0; i < t->n; i++)
        if (strcmp(t->v[i].name, name) == 0) return &t->v[i];
    return NULL;
}

/* Get-or-create the entry for `name`; returns NULL on OOM. */
static varent *vtab_get(vtab *t, const char *name)
{
    varent *e = vtab_find(t, name);
    if (e) return e;
    if (t->n == t->cap) {
        int nc = t->cap ? t->cap * 2 : 16;
        varent *nv = realloc(t->v, (size_t)nc * sizeof(varent));
        if (!nv) return NULL;
        t->v = nv;
        t->cap = nc;
    }
    e = &t->v[t->n];
    memset(e, 0, sizeof(*e));
    e->name = strdup(name);
    if (!e->name) return NULL;
    t->n++;
    return e;
}

/* ─── Diagnostic writing ──────────────────────────────────────────────── */

static void set_err(char *errbuf, size_t errcap, const char *fmt, ...)
{
    va_list ap;
    if (!errbuf || errcap == 0) return;
    va_start(ap, fmt);
    vsnprintf(errbuf, errcap, fmt, ap);
    va_end(ap);
}

/* A flat scalar colspec (tag only; elem/evalues left zero). */
static dl_colspec scalar(dl_coltype tag)
{
    dl_colspec c;
    memset(&c, 0, sizeof c);
    c.tag = tag;
    return c;
}

/* Full human-readable type name (Stage A: flat scalars; List/Optional/Enum
 * render bare names until Stage B adds their parameter rendering). */
static void type_name(const dl_colspec *c, char *out, size_t cap)
{
    if (!c) { snprintf(out, cap, "?"); return; }
    switch (c->tag) {
    case DLT_NATURAL:   snprintf(out, cap, "Natural");   return;
    case DLT_TEXT:      snprintf(out, cap, "Text");      return;
    case DLT_BOOL:      snprintf(out, cap, "Bool");      return;
    case DLT_CHAR:      snprintf(out, cap, "Char");      return;
    case DLT_DATE:      snprintf(out, cap, "Date");      return;
    case DLT_TIMESTAMP: snprintf(out, cap, "Timestamp"); return;
    case DLT_SIGNED:    snprintf(out, cap, "Signed");    return;
    case DLT_LIST:      snprintf(out, cap, "List");      return;
    case DLT_OPTIONAL:  snprintf(out, cap, "Optional");  return;
    case DLT_ENUM:      snprintf(out, cap, "Enum");      return;
    default:            snprintf(out, cap, "?");         return;
    }
}

/* Is `t` an ORDERABLE scalar?  Raw u32 order == semantic order for these;
 * Signed is EXCLUDED (zigzag breaks order) and Text/List/Optional/Enum are
 * not orderable. */
static int is_orderable(dl_colspec t)
{
    return t.tag == DLT_NATURAL || t.tag == DLT_TIMESTAMP ||
           t.tag == DLT_DATE    || t.tag == DLT_BOOL     ||
           t.tag == DLT_CHAR;
}

/* ─── Constraining ────────────────────────────────────────────────────── */

/* Record that variable `name` must be type `t` at (line,col) of site `site`.
 * Returns 0 on success, -1 on a type conflict (message written to errbuf). */
static int constrain_var(vtab *t, const char *name, dl_colspec want,
                         int line, int col, const char *site,
                         char *errbuf, size_t errcap)
{
    varent *e = vtab_get(t, name);
    if (!e) {
        set_err(errbuf, errcap, "<input>: out of memory in typechecker\n");
        return -1;
    }
    if (e->type.tag == 0) {
        e->type = want;
        e->line = line;
        e->col  = col;
        e->site = site;
        return 0;
    }
    if (!dl_colspec_eq(e->type, want)) {
        char wbuf[64], hbuf[64];
        type_name(&want, wbuf, sizeof wbuf);
        type_name(&e->type, hbuf, sizeof hbuf);
        set_err(errbuf, errcap,
                "<input>:%d:%d: variable %s is %s here (%s) but %s at "
                "<input>:%d:%d (%s)\n",
                line, col, name, wbuf, site,
                hbuf, e->line, e->col,
                e->site ? e->site : "");
        return -1;
    }
    return 0;
}

/* Constrain a relational/operand token to type `want` at site `site`:
 *   TOK_VAR   -> constrain_var
 *   TOK_INT   -> require Natural (an int constant in a Text column is a type
 *                error: the value is a raw u32).
 *   TOK_IDENT / TOK_STRING -> require Text (a symbol/string constant is not a
 *                Natural number).
 *   TOK_LIST  -> v1 reject: lists are not yet in the typed universe.
 * Returns 0 on success, -1 on conflict (message written to errbuf). */
static int constrain_arg(vtab *t, const token *a, dl_colspec want,
                         const char *site, char *errbuf, size_t errcap)
{
    if (!a) return 0;
    switch (a->kind) {
    case TOK_VAR:
        return constrain_var(t, a->text, want, a->line, a->col, site,
                             errbuf, errcap);
    case TOK_INT:
        if (!dl_colspec_eq(want, scalar(DLT_NATURAL))) {
            char wbuf[64];
            type_name(&want, wbuf, sizeof wbuf);
            set_err(errbuf, errcap,
                    "<input>:%d:%d: int constant %u in a %s column (%s)\n",
                    a->line, a->col, a->ival, wbuf, site);
            return -1;
        }
        return 0;
    case TOK_IDENT:
    case TOK_STRING:
        if (!dl_colspec_eq(want, scalar(DLT_TEXT))) {
            char wbuf[64];
            type_name(&want, wbuf, sizeof wbuf);
            set_err(errbuf, errcap,
                    "<input>:%d:%d: constant '%s' is Text but a %s column "
                    "(%s) requires Natural\n", a->line, a->col, a->text,
                    wbuf, site);
            return -1;
        }
        return 0;
    case TOK_LIST:
        set_err(errbuf, errcap,
                "<input>:%d:%d: lists are not yet in the typed universe\n",
                a->line, a->col);
        return -1;
    default:
        return 0;
    }
}

/* ─── Atom typing ─────────────────────────────────────────────────────── */

/* Relational atom (a pred that is NOT a builtin): closed-world check against
 * the schema (declared + arity match), then per-arg column typing.  Also used
 * for the rule HEAD.  A reserved builtin name can never be a rule head. */
static int type_relational(vtab *t, const dl_schema *schema, const atom *a,
                           int is_head, char *errbuf, size_t errcap)
{
    const dl_reldef *rd;
    int j;

    if (is_head && is_reserved_builtin_name(a->pred)) {
        set_err(errbuf, errcap,
                "<input>:%d:%d: '%s' is a reserved builtin predicate name and "
                "cannot be used as a rule head\n", a->line, a->col, a->pred);
        return -1;
    }

    rd = dl_schema_find(schema, a->pred);
    if (!rd) {
        set_err(errbuf, errcap,
                "<input>:%d:%d: relation '%s' is not declared in schema.dhall\n",
                a->line, a->col, a->pred);
        return -1;
    }
    if (a->nargs != rd->arity) {
        set_err(errbuf, errcap,
                "<input>:%d:%d: relation '%s' has arity %d but the rule uses "
                "%d argument(s)\n", a->line, a->col, a->pred, rd->arity,
                a->nargs);
        return -1;
    }
    for (j = 0; j < a->nargs; j++) {
        if (constrain_arg(t, a->args[j], rd->cols[j], a->pred,
                          errbuf, errcap) != 0)
            return -1;
    }
    return 0;
}

/* `X = E` arithmetic: result var + every variable in E are Natural. */
static int type_arith(vtab *t, const atom *a, char *errbuf, size_t errcap)
{
    const token *res = a->nargs > 0 ? a->args[0] : NULL;
    /* result var */
    if (res && res->kind == TOK_VAR) {
        if (constrain_var(t, res->text, scalar(DLT_NATURAL), res->line, res->col,
                          a->pred, errbuf, errcap) != 0)
            return -1;
    }
    /* every variable inside the expr tree is Natural */
    return type_expr(t, a->arith, a->line, a->col, a->pred, errbuf, errcap);
}

/* Inherent type of a constant token (tag 0 = not a constant / unknown). */
static dl_colspec token_inherent_type(const token *a)
{
    if (!a) return scalar(0);
    switch (a->kind) {
    case TOK_INT:      return scalar(DLT_NATURAL);
    case TOK_IDENT:
    case TOK_STRING:   return scalar(DLT_TEXT);
    default:           return scalar(0); /* TOK_VAR / TOK_LIST / punctuation */
    }
}

/* `X = Y` plain equality: both sides must have the same type.  Variables take
 * their already-typed value (or stay untyped); a constant contributes its
 * inherent type (int -> Natural, symbol/string -> Text). */
static int type_equality(vtab *t, const atom *a, char *errbuf, size_t errcap)
{
    const token *l = a->nargs > 0 ? a->args[0] : NULL;
    const token *r = a->nargs > 1 ? a->args[1] : NULL;
    dl_colspec lt = scalar(0), rt = scalar(0);

    /* List assignment `[X|Xs] = L` (parser builds an equality atom whose
     * args[0] is a TOK_LIST pattern).  v1 does not type lists, so reject it
     * EXPLICITLY (consistent with the list/range builtin rejection) rather
     * than silently registering only the RHS and skipping the pattern vars. */
    if (l && l->kind == TOK_LIST) {
        set_err(errbuf, errcap,
                "<input>:%d:%d: list assignment is not yet in the typed "
                "universe\n", l->line, l->col);
        return -1;
    }

    if (l && l->kind == TOK_VAR) {
        varent *le = vtab_find(t, l->text);
        lt = le ? le->type : scalar(0);
    } else {
        lt = token_inherent_type(l);
    }
    if (r && r->kind == TOK_VAR) {
        varent *re = vtab_find(t, r->text);
        rt = re ? re->type : scalar(0);
    } else {
        rt = token_inherent_type(r);
    }

    /* Both sides typed and differ: report against the second occurrence. */
    if (lt.tag != 0 && rt.tag != 0 && !dl_colspec_eq(lt, rt)) {
        if (r && r->kind == TOK_VAR)
            return constrain_var(t, r->text, lt, r->line, r->col, a->pred,
                                 errbuf, errcap);
        if (l && l->kind == TOK_VAR)
            return constrain_var(t, l->text, rt, l->line, l->col, a->pred,
                                 errbuf, errcap);
        /* both constants of different types */
        set_err(errbuf, errcap, "<input>:%d:%d: mismatched types in equality\n",
                a->line, a->col);
        return -1;
    }
    /* One side typed, the other an untyped variable: propagate the type. */
    if (lt.tag != 0 && rt.tag == 0) {
        if (r && r->kind == TOK_VAR)
            return constrain_var(t, r->text, lt, r->line, r->col, a->pred,
                                 errbuf, errcap);
        return 0;
    }
    if (rt.tag != 0 && lt.tag == 0) {
        if (l && l->kind == TOK_VAR)
            return constrain_var(t, l->text, rt, l->line, l->col, a->pred,
                                 errbuf, errcap);
        return 0;
    }
    /* Neither side has a type yet: register both (untyped) so the rule-level
     * untyped-variable check can report them if no other atom types them. */
    if (lt.tag == 0 && rt.tag == 0) {
        if (l && l->kind == TOK_VAR) vtab_get(t, l->text);
        if (r && r->kind == TOK_VAR) vtab_get(t, r->text);
    }
    return 0;
}

/* Aggregate atom: a->pred is the result VAR name, a->agg_op->text is the op.
 *   count      -> result Natural
 *   sum(V)     -> V Natural + result Natural
 *   min(V)/max(V) -> result = operand's ESTABLISHED type; operand must be
 *                     Natural / Timestamp / Date (orderable, NOT Signed). */
static int type_aggregate(vtab *t, const atom *a, char *errbuf, size_t errcap)
{
    const char *op = a->agg_op ? a->agg_op->text : "";

    /* min/max: result takes the operand's established type. */
    if (strcmp(op, "min") == 0 || strcmp(op, "max") == 0) {
        if (a->nargs > 0 && a->args[0]->kind == TOK_VAR) {
            varent *e = vtab_find(t, a->args[0]->text);
            dl_colspec ot = e ? e->type : scalar(0);
            if (ot.tag == 0) {
                set_err(errbuf, errcap,
                        "<input>:%d:%d: %s operand is not yet typed "
                        "(min/max needs a Natural/Timestamp/Date column)\n",
                        a->args[0]->line, a->args[0]->col, op);
                return -1;
            }
            if (ot.tag != DLT_NATURAL && ot.tag != DLT_TIMESTAMP &&
                ot.tag != DLT_DATE) {
                char obuf[64];
                type_name(&ot, obuf, sizeof obuf);
                set_err(errbuf, errcap,
                        "<input>:%d:%d: %s over a %s column is not supported "
                        "(min/max needs Natural/Timestamp/Date)\n",
                        a->args[0]->line, a->args[0]->col, op, obuf);
                return -1;
            }
            return constrain_var(t, a->pred, ot, a->line, a->col, op,
                                 errbuf, errcap);
        }
        /* non-var operand: keep Natural (v1) */
        return constrain_var(t, a->pred, scalar(DLT_NATURAL), a->line, a->col,
                             op, errbuf, errcap);
    }

    /* count / sum: result Natural. */
    if (constrain_var(t, a->pred, scalar(DLT_NATURAL), a->line, a->col, op,
                      errbuf, errcap) != 0)
        return -1;
    if (strcmp(op, "count") == 0) {
        return 0;
    }
    /* sum: the source var is Natural. */
    if (a->nargs > 0 && a->args[0]->kind == TOK_VAR) {
        return constrain_var(t, a->args[0]->text, scalar(DLT_NATURAL),
                             a->args[0]->line, a->args[0]->col, op,
                             errbuf, errcap);
    }
    return 0;
}

/* A producing string builtin: concat(Res,A,B) all Text; length(Res,S) Res
 * Natural + S Text; lower/upper(Res,S) Text.  args[0] is the result var. */
static int type_str_producing(vtab *t, const atom *a, char *errbuf, size_t errcap)
{
    const char *p = a->pred;
    int j;

    if (strcmp(p, "length") == 0) {
        /* result Natural, operand Text */
        if (a->nargs > 0 && a->args[0]->kind == TOK_VAR) {
            if (constrain_var(t, a->args[0]->text, scalar(DLT_NATURAL),
                              a->args[0]->line, a->args[0]->col, p,
                              errbuf, errcap) != 0)
                return -1;
        }
        if (a->nargs > 1) {
            if (constrain_arg(t, a->args[1], scalar(DLT_TEXT), p, errbuf, errcap) != 0)
                return -1;
        }
        return 0;
    }
    /* concat / lower / upper: all args Text */
    for (j = 0; j < a->nargs; j++) {
        if (constrain_arg(t, a->args[j], scalar(DLT_TEXT), p, errbuf, errcap) != 0)
            return -1;
    }
    return 0;
}

/* A filter string builtin prefix/suffix/contains: both args Text. */
static int type_str_filter(vtab *t, const atom *a, char *errbuf, size_t errcap)
{
    int j;
    for (j = 0; j < a->nargs; j++) {
        if (constrain_arg(t, a->args[j], scalar(DLT_TEXT), a->pred, errbuf, errcap) != 0)
            return -1;
    }
    return 0;
}

int type_expr(vtab *t, const expr *e, int line, int col, const char *site,
              char *errbuf, size_t errcap)
{
    if (!e) return 0;
    if (e->kind == EX_VAR) {
        return constrain_var(t, e->var, scalar(DLT_NATURAL), line, col, site,
                             errbuf, errcap);
    }
    if (e->kind == EX_BINOP) {
        if (type_expr(t, e->l, line, col, site, errbuf, errcap) != 0)
            return -1;
        return type_expr(t, e->r, line, col, site, errbuf, errcap);
    }
    return 0;
}

/* Type a single body atom against the schema + var table. */
static int type_body_atom(vtab *t, const dl_schema *schema, const atom *a,
                          char *errbuf, size_t errcap)
{
    if (a->aggregate)
        return type_aggregate(t, a, errbuf, errcap);

    /* regex `~ 'pat'`: constrain the atom's columns to Text */
    if (a->pattern != NULL) {
        int j;
        for (j = 0; j < a->nargs; j++) {
            if (constrain_arg(t, a->args[j], scalar(DLT_TEXT), "~", errbuf, errcap) != 0)
                return -1;
        }
        return 0;
    }

    /* arithmetic `X = E` */
    if (a->arith != NULL)
        return type_arith(t, a, errbuf, errcap);

    /* `!=` is a raw u32 inequality (compiler OP_CMP on materialized u32
     * values): type-AGNOSTIC.  Forcing operands to Natural would falsely
     * reject valid `X != symbol` (test_m9_arith T8d) and `Text != Text`.
     * Register vars so the untyped-variable check can still flag a var
     * that nothing else types, but do NOT constrain them. */
    if (strcmp(a->pred, "!=") == 0) {
        int j;
        for (j = 0; j < a->nargs; j++) {
            const token *arg = a->args[j];
            if (arg && arg->kind == TOK_VAR)
                (void)vtab_get(t, arg->text);
        }
        return 0;
    }

    /* ordering comparison {<,<=,>=,>}: both args must be the SAME orderable
     * scalar (Natural/Timestamp/Date/Bool/Char — raw u32 order == semantic
     * order).  Signed is NOT orderable (zigzag breaks order); Text and the
     * parameterized types are not orderable.  If neither arg has an established
     * orderable type, default to Natural (v1). */
    if (is_comparison_pred(a->pred)) {
        dl_colspec lc = a->nargs > 0 ? token_inherent_type(a->args[0]) : scalar(DLT_NATURAL);
        dl_colspec rc = a->nargs > 1 ? token_inherent_type(a->args[1]) : lc;
        if (a->nargs > 0 && a->args[0]->kind == TOK_VAR) {
            varent *le = vtab_find(t, a->args[0]->text);
            if (le) lc = le->type;
        }
        if (a->nargs > 1 && a->args[1]->kind == TOK_VAR) {
            varent *re = vtab_find(t, a->args[1]->text);
            if (re) rc = re->type;
        }
        /* pick the first established orderable operand type; else Natural */
        dl_colspec want = is_orderable(lc) ? lc : (is_orderable(rc) ? rc : scalar(DLT_NATURAL));
        int j;
        for (j = 0; j < a->nargs; j++) {
            if (constrain_arg(t, a->args[j], want, a->pred,
                              errbuf, errcap) != 0)
                return -1;
        }
        return 0;
    }

    /* `X = Y` plain equality */
    if (strcmp(a->pred, "=") == 0 && a->nargs == 2 && a->arith == NULL)
        return type_equality(t, a, errbuf, errcap);

    /* string builtins */
    if (is_str_producing_pred(a->pred))
        return type_str_producing(t, a, errbuf, errcap);
    if (is_str_filter_pred(a->pred))
        return type_str_filter(t, a, errbuf, errcap);

    /* list + range builtins: v1 reject (not yet typed) */
    if (is_list_builtin_pred(a->pred) || is_range_builtin_pred(a->pred)) {
        set_err(errbuf, errcap,
                "<input>:%d:%d: '%s' is a list/range builtin and is not yet "
                "typed in v1\n", a->line, a->col, a->pred);
        return -1;
    }

    /* otherwise: relational atom (negation types as its positive form) */
    return type_relational(t, schema, a, 0, errbuf, errcap);
}

/* Type one rule: walk head + every body atom.  On the first conflict, write the
 * diagnostic and return -1.  After the walk, every variable must have a type
 * (occurrence-consistency is per-rule). */
static int type_rule(const dl_schema *schema, const rule *r,
                     char *errbuf, size_t errcap)
{
    vtab t;
    int i;

    memset(&t, 0, sizeof(t));

    if (r->head) {
        if (type_relational(&t, schema, r->head, 1, errbuf, errcap) != 0) {
            vtab_free(&t);
            return -1;
        }
    }
    for (i = 0; i < r->nbody; i++) {
        if (type_body_atom(&t, schema, r->body[i], errbuf, errcap) != 0) {
            vtab_free(&t);
            return -1;
        }
    }

    /* untyped-variable check: every var seen in this rule must have a type.
     * A var that never receives a relational/builtin constraint is an error. */
    for (i = 0; i < t.n; i++) {
        if (t.v[i].type.tag == 0) {
            set_err(errbuf, errcap,
                    "<input>: untyped variable %s in rule '%s' "
                    "(no column constrains it)\n", t.v[i].name,
                    r->head ? r->head->pred : "");
            vtab_free(&t);
            return -1;
        }
    }

    vtab_free(&t);
    return 0;
}

int dl_typecheck_rules(const dl_schema *schema, void *rules, int n_rules,
                       char *errbuf, size_t errcap)
{
    rule **rr = (rule **)rules;
    int i;

    if (!schema || !rr || n_rules <= 0)
        return 0; /* nothing to check */

    for (i = 0; i < n_rules; i++) {
        if (!rr[i]) continue;
        if (type_rule(schema, rr[i], errbuf, errcap) != 0)
            return -1;
    }
    return 0;
}
