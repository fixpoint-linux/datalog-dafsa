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
 * this module must not depend on compiler internals).  List builtins and the
 * `range` builtin are typed here too (v2).  Stratification is the compiler's
 * job, so a negated atom is typed exactly like its positive form.
 *
 * Because dl_typecheck_rules is called by dl_load_rules which knows no source
 * filename, the default "file" component of every diagnostic is the literal
 * `<input>` — the dlp tool overrides it with the real rules-file path via the
 * `srcname` parameter.
 */

#include "typecheck.h"
#include "parser.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Source name used in diagnostics.  dl_typecheck_rules sets it from its
 * `srcname` parameter (NULL => `<input>`). */
static const char *g_srcname = "<input>";

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

/* Defined below (v2 list builtins).  type_list_assignment (above) uses it to
 * resolve the RHS of `[X|Xs] = L` to a List colspec. */
static int resolve_list_operand(vtab *t, const token *tok, const char *pred,
                                dl_colspec *lt, char *errbuf, size_t errcap);

/* TOK_LIST literal typing helpers (defined after token_inherent_type). */
static int type_list_literal_known(vtab *t, const token *lit, dl_coltype elem,
                                   const char *site, char *errbuf, size_t errcap);
static int infer_list_literal_elem(const token *lit, dl_coltype *elem,
                                   char *errbuf, size_t errcap);

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

/* A List colspec wrapping a flat element type (v1 elements are flat scalars). */
static dl_colspec list_of(dl_coltype elem)
{
    dl_colspec c;
    memset(&c, 0, sizeof c);
    c.tag = DLT_LIST;
    c.elem = elem;
    return c;
}

/* Full human-readable type name (flat scalars + List<elem>/Optional<elem>/Enum). */
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
    case DLT_LIST:
    case DLT_OPTIONAL: {
        /* elem is a flat scalar (v1) — its name is short, so a small buffer
           is enough and the combined "<...>" always fits the caller's cap. */
        dl_colspec ec = scalar(c->elem);
        char en[16];
        type_name(&ec, en, sizeof en);
        snprintf(out, cap, "%s<%s>", c->tag == DLT_LIST ? "List" : "Optional", en);
        return;
    }
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
        set_err(errbuf, errcap, "%s: out of memory in typechecker\n",
                g_srcname);
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
                "%s:%d:%d: variable %s is %s here (%s) but %s at "
                "%s:%d:%d (%s)\n",
                g_srcname, line, col, name, wbuf, site,
                hbuf, g_srcname, e->line, e->col,
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
                    "%s:%d:%d: int constant %u in a %s column (%s)\n",
                    g_srcname, a->line, a->col, a->ival, wbuf, site);
            return -1;
        }
        return 0;
    case TOK_IDENT:
    case TOK_STRING:
        if (!dl_colspec_eq(want, scalar(DLT_TEXT))) {
            char wbuf[64];
            type_name(&want, wbuf, sizeof wbuf);
            set_err(errbuf, errcap,
                    "%s:%d:%d: constant '%s' is Text but a %s column "
                    "(%s) requires Natural\n", g_srcname, a->line, a->col, a->text,
                    wbuf, site);
            return -1;
        }
        return 0;
    case TOK_LIST:
        if (want.tag != DLT_LIST) {
            char wbuf[64];
            type_name(&want, wbuf, sizeof wbuf);
            set_err(errbuf, errcap,
                    "%s:%d:%d: list literal in a %s column (%s)\n",
                    g_srcname, a->line, a->col, wbuf, site);
            return -1;
        }
        return type_list_literal_known(t, a, want.elem, site, errbuf, errcap);
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
                "%s:%d:%d: '%s' is a reserved builtin predicate name and "
                "cannot be used as a rule head\n", g_srcname, a->line, a->col, a->pred);
        return -1;
    }

    rd = dl_schema_find(schema, a->pred);
    if (!rd) {
        set_err(errbuf, errcap,
                "%s:%d:%d: relation '%s' is not declared in schema.dhall\n",
                g_srcname, a->line, a->col, a->pred);
        return -1;
    }
    if (a->nargs != rd->arity) {
        set_err(errbuf, errcap,
                "%s:%d:%d: relation '%s' has arity %d but the rule uses "
                "%d argument(s)\n", g_srcname, a->line, a->col, a->pred, rd->arity,
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

/* ─── TOK_LIST literal typing (finish-dlp Item 1) ───────────────────────── */

/* Inherent flat element type of a constant list element, or 0. */
static dl_coltype elem_of_const(const token *a)
{
    if (!a) return 0;
    if (a->kind == TOK_INT) return DLT_NATURAL;
    if (a->kind == TOK_IDENT || a->kind == TOK_STRING) return DLT_TEXT;
    return 0;
}

/* Check one list-literal element against flat scalar elem.  var binds elem;
 * constant must int->Natural / ident|string->Text; nested list rejected. */
static int constrain_list_elem(vtab *t, const token *e, dl_coltype elem,
                               const char *site, char *errbuf, size_t errcap)
{
    char wbuf[32];
    if (!e) return 0;
    if (e->kind == TOK_VAR)
        return constrain_var(t, e->text, scalar(elem), e->line, e->col,
                             site, errbuf, errcap);
    if (e->kind == TOK_INT) {
        if (elem != DLT_NATURAL) {
            dl_colspec wc = scalar(elem);
            type_name(&wc, wbuf, sizeof wbuf);
            set_err(errbuf, errcap,
                    "%s:%d:%d: list literal int element in a List<%s> (%s)\n",
                    g_srcname, e->line, e->col, wbuf, site);
            return -1;
        }
        return 0;
    }
    if (e->kind == TOK_IDENT || e->kind == TOK_STRING) {
        if (elem != DLT_TEXT) {
            dl_colspec wc = scalar(elem);
            type_name(&wc, wbuf, sizeof wbuf);
            set_err(errbuf, errcap,
                    "%s:%d:%d: list literal '%s' element in a List<%s> (%s)\n",
                    g_srcname, e->line, e->col, e->text, wbuf, site);
            return -1;
        }
        return 0;
    }
    if (e->kind == TOK_LIST) {
        set_err(errbuf, errcap,
                "%s:%d:%d: nested list literal element is not supported "
                "(flat element type only) (%s)\n",
                g_srcname, e->line, e->col, site);
        return -1;
    }
    return 0;
}

/* Type a TOK_LIST against a KNOWN flat element type. */
static int type_list_literal_known(vtab *t, const token *lit, dl_coltype elem,
                                   const char *site, char *errbuf, size_t errcap)
{
    int i;
    if (!lit || lit->kind != TOK_LIST) return 0;
    for (i = 0; i < lit->nchildren; i++)
        if (constrain_list_elem(t, lit->children[i], elem, site, errbuf, errcap) != 0)
            return -1;
    if (lit->tail && lit->tail->kind == TOK_VAR)
        if (constrain_var(t, lit->tail->text, list_of(elem), lit->tail->line,
                          lit->tail->col, site, errbuf, errcap) != 0)
            return -1;
    return 0;
}

/* Infer element type of an all-constant list literal (list-builtin operand). */
static int infer_list_literal_elem(const token *lit, dl_coltype *elem,
                                   char *errbuf, size_t errcap)
{
    int i;
    dl_coltype got = 0;
    if (!lit || lit->kind != TOK_LIST) return -1;
    if (lit->tail) {
        set_err(errbuf, errcap,
                "%s:%d:%d: list pattern is not allowed as a list-builtin operand\n",
                g_srcname, lit->line, lit->col);
        return -1;
    }
    for (i = 0; i < lit->nchildren; i++) {
        const token *e = lit->children[i];
        dl_coltype ec;
        if (e->kind == TOK_VAR) {
            set_err(errbuf, errcap,
                    "%s:%d:%d: list literal with a variable element is not "
                    "allowed as a list-builtin operand (it is a pattern)\n",
                    g_srcname, e->line, e->col);
            return -1;
        }
        if (e->kind == TOK_LIST) {
            set_err(errbuf, errcap,
                    "%s:%d:%d: nested list literal element is not supported\n",
                    g_srcname, e->line, e->col);
            return -1;
        }
        ec = elem_of_const(e);
        if (ec == 0) continue;
        if (got == 0) got = ec;
        else if (got != ec) {
            set_err(errbuf, errcap,
                    "%s:%d:%d: list literal has mixed element types "
                    "(%s and %s)\n",
                    g_srcname, lit->line, lit->col,
                    got == DLT_NATURAL ? "Natural" : "Text",
                    ec == DLT_NATURAL ? "Natural" : "Text");
            return -1;
        }
    }
    if (got == 0) {
        set_err(errbuf, errcap,
                "%s:%d:%d: cannot infer list element type from an empty list "
                "literal [] (bind it via another operand or a column)\n",
                g_srcname, lit->line, lit->col);
        return -1;
    }
    *elem = got;
    return 0;
}

/* List assignment `[X|Xs] = L` (the parser builds an '=' atom with nargs==2,
 * args[0] a TOK_LIST pattern, args[1] the RHS list value).  Resolve the RHS
 * to a List<elem> (reusing resolve_list_operand, defined below type_equality),
 * then bind the pattern per emit_pattern semantics (compiler.c): each head
 * element var (children[i]) := elem, the tail var (tail) := List<elem>.
 * Constant head elements (TOK_INT/TOK_IDENT) have no var to constrain — skip.
 * `[X] = L` (single head child, no tail) still binds X := elem (car pattern). */
static int type_list_assignment(vtab *t, const atom *a, char *errbuf, size_t errcap)
{
    const token *pat = a->nargs > 0 ? a->args[0] : NULL;
    const token *rhs = a->nargs > 1 ? a->args[1] : NULL;
    dl_colspec lt;
    int i;

    if (!resolve_list_operand(t, rhs, a->pred, &lt, errbuf, errcap)) return -1;
    dl_colspec ec = scalar(lt.elem);
    dl_colspec lr = list_of(lt.elem);

    if (pat) {
        for (i = 0; i < pat->nchildren; i++) {
            const token *el = pat->children[i];
            if (el && el->kind == TOK_VAR) {
                if (constrain_var(t, el->text, ec, el->line, el->col,
                                  a->pred, errbuf, errcap) != 0)
                    return -1;
            }
        }
        if (pat->tail && pat->tail->kind == TOK_VAR) {
            if (constrain_var(t, pat->tail->text, lr, pat->tail->line,
                              pat->tail->col, a->pred, errbuf, errcap) != 0)
                return -1;
        }
    }
    return 0;
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
     * args[0] is a TOK_LIST pattern): type the RHS as a List and bind the
     * pattern vars (head elements := elem, tail := List<elem>). */
    if (l && l->kind == TOK_LIST)
        return type_list_assignment(t, a, errbuf, errcap);

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
        set_err(errbuf, errcap, "%s:%d:%d: mismatched types in equality\n",
                g_srcname, a->line, a->col);
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
                        "%s:%d:%d: %s operand is not yet typed "
                        "(min/max needs a Natural/Timestamp/Date column)\n",
                        g_srcname, a->args[0]->line, a->args[0]->col, op);
                return -1;
            }
            if (ot.tag != DLT_NATURAL && ot.tag != DLT_TIMESTAMP &&
                ot.tag != DLT_DATE) {
                char obuf[64];
                type_name(&ot, obuf, sizeof obuf);
                set_err(errbuf, errcap,
                        "%s:%d:%d: %s over a %s column is not supported "
                        "(min/max needs Natural/Timestamp/Date)\n",
                        g_srcname, a->args[0]->line, a->args[0]->col, op, obuf);
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

/* ─── v2 list builtins (member/car/cdr/cons/append) ─────────────────────── */

/* Resolve the LIST OPERAND of a list builtin to its List colspec.
 *
 *   - a TOK_LIST literal          -> v1 reject (lists as literals not typed);
 *   - a variable typed as a List  -> write its List colspec into *lt, return 1;
 *   - an UNTYPED variable         -> 'cannot infer list element type' (v1
 *                                    left-to-right boundary — a two-pass /
 *                                    unification typechecker removes this);
 *   - a variable typed non-List   -> type-conflict diagnostic;
 *   - any other token             -> 'requires a List operand'.
 * Returns 1 on success, 0 on failure (message written to errbuf). */
static int resolve_list_operand(vtab *t, const token *tok, const char *pred,
                                dl_colspec *lt, char *errbuf, size_t errcap)
{
    if (!tok) {
        set_err(errbuf, errcap, "%s: '%s' is missing its list operand\n",
                g_srcname, pred);
        return 0;
    }
    if (tok->kind == TOK_LIST) {
        dl_coltype elem;
        if (infer_list_literal_elem(tok, &elem, errbuf, errcap) != 0) return 0;
        *lt = list_of(elem);
        return 1;
    }
    if (tok->kind == TOK_VAR) {
        varent *e = vtab_find(t, tok->text);
        if (e && e->type.tag == DLT_LIST) { *lt = e->type; return 1; }
        if (e && e->type.tag == 0) {
            set_err(errbuf, errcap,
                    "%s:%d:%d: cannot infer list element type in '%s' "
                    "(list operand '%s' is untyped)\n",
                    g_srcname, tok->line, tok->col, pred, tok->text);
            return 0;
        }
        if (e) {
            char wbuf[64];
            type_name(&e->type, wbuf, sizeof wbuf);
            set_err(errbuf, errcap,
                    "%s:%d:%d: '%s' in '%s' is %s, not a List\n",
                    g_srcname, tok->line, tok->col, tok->text, pred, wbuf);
            return 0;
        }
        set_err(errbuf, errcap,
                "%s:%d:%d: cannot infer list element type in '%s' "
                "(list operand '%s' is untyped)\n",
                g_srcname, tok->line, tok->col, pred, tok->text);
        return 0;
    }
    set_err(errbuf, errcap,
            "%s:%d:%d: '%s' requires a List operand, got a constant\n",
            g_srcname, tok->line, tok->col, pred);
    return 0;
}

/* Type one v2 list builtin.  Arg order matches the compiler's
 * list_builtin_valid: car/cdr have 2 args (Result, List); cons/append have 3
 * (Result, Head/A, Tail/B); member is a filter (X, List).  args[0] is always
 * a result/member variable.
 *
 *   member(X,L):  L typed List<elem> -> X := elem.
 *   car(R,L):     L List<elem>       -> R := elem.
 *   cdr(R,L):     L List<elem>       -> R := List<elem>.
 *   cons(R,H,T):  T List<elem>       -> H := elem, R := List<elem>.
 *   append(R,A,B):A List<elem>       -> B := List<elem>, R := List<elem>.
 */
static int type_list_builtin(vtab *t, const atom *a, char *errbuf, size_t errcap)
{
    const char *p = a->pred;
    dl_colspec lt;

    if (strcmp(p, "member") == 0) {
        const token *mx = a->nargs > 0 ? a->args[0] : NULL;
        const token *ml = a->nargs > 1 ? a->args[1] : NULL;
        if (!resolve_list_operand(t, ml, p, &lt, errbuf, errcap)) return -1;
        dl_colspec ec = scalar(lt.elem);
        if (mx && mx->kind == TOK_VAR)
            return constrain_var(t, mx->text, ec, mx->line, mx->col, p,
                                 errbuf, errcap);
        return 0;
    }

    if (strcmp(p, "car") == 0) {
        const token *res = a->nargs > 0 ? a->args[0] : NULL;
        const token *ll  = a->nargs > 1 ? a->args[1] : NULL;
        if (!resolve_list_operand(t, ll, p, &lt, errbuf, errcap)) return -1;
        dl_colspec ec = scalar(lt.elem);
        if (res && res->kind == TOK_VAR)
            return constrain_var(t, res->text, ec, res->line, res->col, p,
                                 errbuf, errcap);
        return 0;
    }

    if (strcmp(p, "cdr") == 0) {
        const token *res = a->nargs > 0 ? a->args[0] : NULL;
        const token *ll  = a->nargs > 1 ? a->args[1] : NULL;
        if (!resolve_list_operand(t, ll, p, &lt, errbuf, errcap)) return -1;
        dl_colspec lr = list_of(lt.elem);
        if (res && res->kind == TOK_VAR)
            return constrain_var(t, res->text, lr, res->line, res->col, p,
                                 errbuf, errcap);
        return 0;
    }

    if (strcmp(p, "cons") == 0) {
        const token *res = a->nargs > 0 ? a->args[0] : NULL;
        const token *hh  = a->nargs > 1 ? a->args[1] : NULL;
        const token *tt  = a->nargs > 2 ? a->args[2] : NULL;
        if (!resolve_list_operand(t, tt, p, &lt, errbuf, errcap)) return -1;
        dl_colspec ec = scalar(lt.elem);
        dl_colspec lr = list_of(lt.elem);
        if (hh) {
            if (hh->kind == TOK_VAR) {
                if (constrain_var(t, hh->text, ec, hh->line, hh->col, p,
                                  errbuf, errcap) != 0)
                    return -1;
            } else if (hh->kind == TOK_LIST) {
                set_err(errbuf, errcap,
                        "%s:%d:%d: cons head must be a flat scalar, got a "
                        "list literal\n", g_srcname, hh->line, hh->col);
                return -1;
            } else if (constrain_list_elem(t, hh, lt.elem, p, errbuf, errcap) != 0) {
                return -1;
            }
        }
        if (res && res->kind == TOK_VAR)
            return constrain_var(t, res->text, lr, res->line, res->col, p,
                                 errbuf, errcap);
        return 0;
    }

    /* append */
    {
        const token *res = a->nargs > 0 ? a->args[0] : NULL;
        const token *aa  = a->nargs > 1 ? a->args[1] : NULL;
        const token *bb  = a->nargs > 2 ? a->args[2] : NULL;
        if (!resolve_list_operand(t, aa, p, &lt, errbuf, errcap)) return -1;
        dl_colspec lr = list_of(lt.elem);
        if (bb) {
            if (bb->kind == TOK_VAR) {
                if (constrain_var(t, bb->text, lr, bb->line, bb->col, p,
                                  errbuf, errcap) != 0)
                    return -1;
            } else if (bb->kind == TOK_LIST) {
                if (type_list_literal_known(t, bb, lt.elem, p, errbuf, errcap) != 0)
                    return -1;
            } else {
                set_err(errbuf, errcap,
                        "%s:%d:%d: append's second operand must be a List, "
                        "got a constant\n", g_srcname, bb->line, bb->col);
                return -1;
            }
        }
        if (res && res->kind == TOK_VAR)
            return constrain_var(t, res->text, lr, res->line, res->col, p,
                                 errbuf, errcap);
        return 0;
    }
    return 0;
}

/* `range(X, Rel, Lo, Hi)` (compiler.c range_builtin_valid): args[0] is the
 * member variable X := Natural, args[1] is the relation NAME (TOK_IDENT — a
 * name, NOT a value: it is resolved by the compiler against declared
 * relations, so it carries no column type), args[2]/args[3] are the half-open
 * bounds (TOK_VAR|TOK_INT) := Natural. */
static int type_range(vtab *t, const atom *a, char *errbuf, size_t errcap)
{
    const token *x  = a->nargs > 0 ? a->args[0] : NULL;
    const token *rl = a->nargs > 1 ? a->args[1] : NULL;
    const token *lo = a->nargs > 2 ? a->args[2] : NULL;
    const token *hi = a->nargs > 3 ? a->args[3] : NULL;

    if (a->nargs != 4) {
        set_err(errbuf, errcap,
                "%s:%d:%d: 'range' expects 4 arguments "
                "(range(X, Rel, Lo, Hi))\n", g_srcname, a->line, a->col);
        return -1;
    }
    /* Rel is a relation NAME (TOK_IDENT), not a value operand. */
    if (!rl || rl->kind != TOK_IDENT) {
        set_err(errbuf, errcap,
                "%s:%d:%d: range relation must be a name (got a value or "
                "variable)\n", g_srcname,
                rl ? rl->line : a->line, rl ? rl->col : a->col);
        return -1;
    }
    if (x && x->kind == TOK_VAR) {
        if (constrain_var(t, x->text, scalar(DLT_NATURAL), x->line, x->col,
                          "range", errbuf, errcap) != 0)
            return -1;
    }
    if (constrain_arg(t, lo, scalar(DLT_NATURAL), "range", errbuf, errcap) != 0)
        return -1;
    if (constrain_arg(t, hi, scalar(DLT_NATURAL), "range", errbuf, errcap) != 0)
        return -1;
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

    /* v2 list builtins: real typing. */
    if (is_list_builtin_pred(a->pred))
        return type_list_builtin(t, a, errbuf, errcap);
    /* range(X, Rel, Lo, Hi): real typing (X/Lo/Hi Natural, Rel a name). */
    if (is_range_builtin_pred(a->pred))
        return type_range(t, a, errbuf, errcap);

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
                    "%s: untyped variable %s in rule '%s' "
                    "(no column constrains it)\n", g_srcname, t.v[i].name,
                    r->head ? r->head->pred : "");
            vtab_free(&t);
            return -1;
        }
    }

    vtab_free(&t);
    return 0;
}

int dl_typecheck_rules(const dl_schema *schema, void *rules, int n_rules,
                       const char *srcname, char *errbuf, size_t errcap)
{
    rule **rr = (rule **)rules;
    int i;

    g_srcname = srcname ? srcname : "<input>";

    if (!schema || !rr || n_rules <= 0)
        return 0; /* nothing to check */

    for (i = 0; i < n_rules; i++) {
        if (!rr[i]) continue;
        if (type_rule(schema, rr[i], errbuf, errcap) != 0)
            return -1;
    }
    return 0;
}
