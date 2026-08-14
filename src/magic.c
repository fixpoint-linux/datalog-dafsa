/*
 * magic.c — Magic-sets AST→AST adornment rewrite (M8 v2 multi-predicate slice,
 *           v3 SIPS body-reordering slice, v4 negation+aggregate slice,
 *           v5 adornment-closure fixpoint slice)
 *
 * SIPS slice: a deterministic greedy body-atom permutation is computed once
 * per (rule, variant) pair at transform time (sips_body_order) and shared
 * CONSISTENTLY between the adornment walk (compute_betas) and the synthesis
 * loops — the cached permutation is the single source of truth, never
 * recomputed in synthesis.
 *
 * Adornment-closure slice: one predicate may carry MULTIPLE distinct
 * adornments (e.g. tc^bf AND tc^bb).  Each (predicate, adornment) pair is a
 * distinct VARIANT with its own adorned + magic relation pair.  Phase B is a
 * fixpoint: pop a variant, walk every rule of its predicate under that
 * variant's adornment, and create a new variant for every body IDB atom's
 * computed adornment until no new (predicate, adornment) pair appears.
 *
 * Pure transformation: no dl_db access.  Reads the retained AST (a const
 * array of rule*) and synthesizes a fresh adorned + magic program.
 *
 * The synthesized AST is built with the SAME allocation discipline as
 * parser.c (calloc'd atom/rule, strdup'd pred/text, one token* per arg), so
 * the program can be freed with rule_free() (parser.h) and compiled by the
 * untouched compile_rules().
 *
 * For each adorned variant (P,alpha) we synthesize:
 *   (a) adorned rule          P__alpha(...) :- magic_P__alpha(bound head), body'
 *   (b) self-recursion magic  (body IDB atom Q == P under beta == alpha):
 *       magic_P__alpha(bound args) :- magic_P__alpha(head bound), prefix'
 *   (c) dependency-seed magic (body IDB atom Q != P, or beta != alpha):
 *       magic_Q__beta(bound args) :- magic_P__alpha(head bound), prefix'
 *
 * Negation / aggregates are SUPPORTED under a conservative soundness
 * boundary (never silently mis-evaluate):
 *   - a negated body atom is allowed only if its predicate is OUTSIDE the
 *     adorned closure (EDB, or an IDB predicate not reached by positive
 *     references).  Negation on an adorned-closure predicate is REJECTED:
 *     a magic-seeded slice is not the full complement.  The negated atom is
 *     evaluated against the FULL materialization of its predicate (the
 *     clone aliases src's relations, so dl_query_magic_adorn must ensure
 *     those IDB relations are compiled — see dl.c).
 *   - an aggregate is allowed only if the rule contains no positive body
 *     atom whose predicate is in the adorned closure (the aggregate must
 *     read the FULL group, not a bound slice).  Over-conservative; the
 *     precise soundness condition (group-by key fully bound) is a future
 *     relaxation documented here.
 * Conservative REJECTS (never silently mis-evaluate):
 *   - negation on an adorned-closure predicate (see above)
 *   - aggregate in a rule with an adorned-closure body atom (see above)
 *   - k == 0 (caller routes to dl_query)
 *   - cross-predicate mutual recursion (predicate dep-graph SCC size > 1)
 *   - adornment-closure blow-up (> MAX_ADORN_VARIANTS variants)
 *   - MAX_RELS budget overflow / adorned-predicate name collision
 */
#include "magic.h"
#include "intern.h"
#include "dl_internal.h"   /* MAX_RELS */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ─── M9: builtin atom classification (mirror compiler.c) ─────────────── */

static int is_comparison_atom(const atom *A)
{
    if (!A || !A->pred) return 0;
    return strcmp(A->pred, "<")  == 0 || strcmp(A->pred, "<=") == 0 ||
           strcmp(A->pred, ">")  == 0 || strcmp(A->pred, ">=") == 0 ||
           strcmp(A->pred, "!=") == 0;
}

static int is_arith_atom(const atom *A)
{
    return A && A->pred && strcmp(A->pred, "=") == 0 && A->arith != NULL;
}

static int is_equality_atom(const atom *A)
{
    return A && A->pred && strcmp(A->pred, "=") == 0 &&
           A->nargs == 2 && A->arith == NULL;
}

/* M9-strings: string builtin classification (mirror compiler.c). */
static int is_str_producing_atom(const atom *A)
{
    if (!A || !A->pred) return 0;
    return strcmp(A->pred, "concat") == 0 ||
           strcmp(A->pred, "length") == 0 ||
           strcmp(A->pred, "lower")  == 0 ||
           strcmp(A->pred, "upper")  == 0;
}

static int is_str_filter_atom(const atom *A)
{
    if (!A || !A->pred) return 0;
    return strcmp(A->pred, "prefix")   == 0 ||
           strcmp(A->pred, "suffix")   == 0 ||
           strcmp(A->pred, "contains") == 0;
}

/* ─── Growable buffers ────────────────────────────────────────────────── */

typedef struct { rule **v; int n, cap; } rule_vec;
typedef struct { magic_decl *v; int n, cap; } decl_vec;
typedef struct { const char **v; int n, cap; } name_set; /* borrowed var names */

static int rv_push(rule_vec *v, rule *r)
{
    if (v->n >= v->cap) {
        int nc = v->cap ? v->cap * 2 : 8;
        rule **nv = realloc(v->v, (size_t)nc * sizeof(rule *));
        if (!nv) return -1;
        v->v = nv; v->cap = nc;
    }
    v->v[v->n++] = r;
    return 0;
}

static int dv_push(decl_vec *v, const char *name, uint8_t arity)
{
    if (v->n >= v->cap) {
        int nc = v->cap ? v->cap * 2 : 4;
        magic_decl *nv = realloc(v->v, (size_t)nc * sizeof(magic_decl));
        if (!nv) return -1;
        v->v = nv; v->cap = nc;
    }
    snprintf(v->v[v->n].name, sizeof(v->v[v->n].name), "%s", name);
    v->v[v->n].arity = arity;
    v->n++;
    return 0;
}

static int dv_contains(const decl_vec *v, const char *name)
{
    int i;
    for (i = 0; i < v->n; i++)
        if (strcmp(v->v[i].name, name) == 0) return 1;
    return 0;
}

static int ns_contains(const name_set *s, const char *n)
{
    int i;
    for (i = 0; i < s->n; i++)
        if (strcmp(s->v[i], n) == 0) return 1;
    return 0;
}

static int ns_add(name_set *s, const char *n)
{
    if (ns_contains(s, n)) return 0;
    if (s->n >= s->cap) {
        int nc = s->cap ? s->cap * 2 : 8;
        const char **nv = realloc(s->v, (size_t)nc * sizeof(const char *));
        if (!nv) return -1;
        s->v = nv; s->cap = nc;
    }
    s->v[s->n++] = n;
    return 0;
}

/* Count bound/unbound variable operands of an arithmetic expr tree (mirrors
 * sips_atom_score's score/n_new semantics for arithmetic atoms). */
static void expr_operand_vars(const expr *e, const name_set *bound,
                              int *s, int *nn)
{
    if (!e) return;
    switch (e->kind) {
    case EX_VAR:
        if (ns_contains(bound, e->var)) (*s)++;
        else (*nn)++;
        break;
    case EX_BINOP:
        expr_operand_vars(e->l, bound, s, nn);
        expr_operand_vars(e->r, bound, s, nn);
        break;
    default:
        break;
    }
}

/* Score the VARIABLE operands of a string builtin.  `start` selects the
 * operand window: 1 for producing builtins (args[0] is the result var, never
 * scored), 0 for filters (args[0..1] are both operands).  Mirrors
 * expr_operand_vars/sips_atom_score semantics. */
static void str_operand_vars(const atom *A, int start, const name_set *bound,
                             int *s, int *nn)
{
    int j;
    for (j = start; j < A->nargs; j++) {
        const token *t = A->args[j];
        if (t->kind != TOK_VAR) continue;
        if (ns_contains(bound, t->text)) (*s)++;
        else (*nn)++;
    }
}

/* ─── AST allocation helpers (mirror parser.c discipline) ─────────────── */

static token *tok_dup(const token *t)
{
    token *n = calloc(1, sizeof(*n));
    if (!n) return NULL;
    n->kind = t->kind;
    n->ival = t->ival;
    if (t->text) {
        n->text = strdup(t->text);
        if (!n->text) { free(n); return NULL; }
    }
    return n;
}

/* Reclaim an atom exactly like parser.c's static atom_free. */
static void atom_free_local(atom *a)
{
    int i;
    if (!a) return;
    free(a->pred);
    free(a->pattern);
    if (a->args) {
        for (i = 0; i < a->nargs; i++) {
            if (a->args[i]) { free(a->args[i]->text); free(a->args[i]); }
        }
        free(a->args);
    }
    if (a->aggregate) {
        if (a->agg_op) { free(a->agg_op->text); free(a->agg_op); }
    }
    expr_free(a->arith);
    free(a);
}

/* Free an array of atoms (and the array itself). */
static void atoms_free(atom **arr, int n)
{
    int i;
    if (!arr) return;
    for (i = 0; i < n; i++) atom_free_local(arr[i]);
    free(arr);
}

/* Free an array of tokens (and the array itself). */
static void tokens_free(token **arr, int n)
{
    int i;
    if (!arr) return;
    for (i = 0; i < n; i++) {
        if (arr[i]) { free(arr[i]->text); free(arr[i]); }
    }
    free(arr);
}

/* Deep-copy an atom, optionally renaming its predicate.
 * Mirrors parser.c atom layout so rule_free() can reclaim it. */
static atom *atom_copy(const atom *src, const char *new_pred)
{
    atom *a = calloc(1, sizeof(*a));
    int i;
    if (!a) return NULL;
    a->pred = strdup(new_pred ? new_pred : src->pred);
    if (!a->pred) { free(a); return NULL; }
    a->negated   = src->negated;
    a->aggregate = src->aggregate;
    if (src->pattern) {
        a->pattern = strdup(src->pattern);
        if (!a->pattern) { atom_free_local(a); return NULL; }
    }
    if (src->agg_op) {
        a->agg_op = tok_dup(src->agg_op);
        if (!a->agg_op) { atom_free_local(a); return NULL; }
    }
    if (src->arith) {
        a->arith = expr_clone(src->arith);
        if (!a->arith) { atom_free_local(a); return NULL; }
    }
    if (src->nargs > 0) {
        a->args = calloc((size_t)src->nargs, sizeof(token *));
        if (!a->args) { atom_free_local(a); return NULL; }
        a->nargs = src->nargs;
        for (i = 0; i < src->nargs; i++) {
            a->args[i] = tok_dup(src->args[i]);
            if (!a->args[i]) { atom_free_local(a); return NULL; }
        }
    }
    return a;
}

/* Build a fresh atom "pred" whose args are the 'b' positions of `src`
 * under `adorn`.  Used for magic guards and magic-rule heads. */
static atom *build_bound_atom(const atom *src, const char *pred,
                              const char *adorn)
{
    atom *a;
    token **args;
    int nb = 0, i, k = 0;

    for (i = 0; i < src->nargs; i++)
        if (adorn[i] == 'b') nb++;

    a = calloc(1, sizeof(*a));
    if (!a) return NULL;
    a->pred = strdup(pred);
    if (!a->pred) { free(a); return NULL; }

    if (nb > 0) {
        args = calloc((size_t)nb, sizeof(token *));
        if (!args) { free(a->pred); free(a); return NULL; }
        for (i = 0; i < src->nargs; i++) {
            if (adorn[i] == 'b') {
                args[k] = tok_dup(src->args[i]);
                if (!args[k]) { tokens_free(args, k); free(a->pred); free(a); return NULL; }
                k++;
            }
        }
        a->args = args;
        a->nargs = nb;
    }
    return a;
}

static rule *make_rule(atom *head, atom **body, int nbody,
                       int has_neg, int has_agg)
{
    rule *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->head = head;
    r->body = body;
    r->nbody = nbody;
    r->has_negation = has_neg;
    r->has_aggregate = has_agg;
    return r;
}

/* ─── Adornment helpers ───────────────────────────────────────────────── */

/* Write "b"*k ++ "f"*(arity-k) into buf (buf >= arity+1). */
static void make_adornment(char *buf, uint8_t arity, uint8_t k)
{
    uint8_t i;
    for (i = 0; i < k; i++) buf[i] = 'b';
    for (i = k; i < arity; i++) buf[i] = 'f';
    buf[arity] = '\0';
}

/* Compute the left-to-right SIPS adornment of atom A given the current
 * bound variable set.  Constants are 'b'; vars in bound are 'b'. */
static void atom_adornment(const atom *A, const name_set *bound, char *beta)
{
    int j;
    for (j = 0; j < A->nargs; j++) {
        const token *t = A->args[j];
        if (t->kind == TOK_INT || t->kind == TOK_IDENT)
            beta[j] = 'b';
        else if (t->kind == TOK_VAR && ns_contains(bound, t->text))
            beta[j] = 'b';
        else
            beta[j] = 'f';
    }
    beta[A->nargs] = '\0';
}

/* ─── IDB predicate table (name + arity) ──────────────────────────────── */
/* Distinct from the variant table: this is the set of rule-head predicates,
 * used for the dep graph, the SCC check, and the EDB-vs-IDB distinction in
 * SIPS ordering. */

typedef struct { char pred[64]; uint8_t arity; } pred_info;
typedef struct { pred_info *v; int n, cap; } pred_vec;

static int pred_find(const pred_vec *v, const char *pred)
{
    int i;
    if (!pred) return -1;
    for (i = 0; i < v->n; i++)
        if (strcmp(v->v[i].pred, pred) == 0) return i;
    return -1;
}

static int pred_push(pred_vec *v, const char *pred, uint8_t arity)
{
    pred_info *nv;
    if (pred_find(v, pred) >= 0) return 0;
    if (v->n >= v->cap) {
        int nc = v->cap ? v->cap * 2 : 8;
        nv = realloc(v->v, (size_t)nc * sizeof(pred_info));
        if (!nv) return -1;
        v->v = nv; v->cap = nc;
    }
    memset(&v->v[v->n], 0, sizeof(v->v[v->n]));
    snprintf(v->v[v->n].pred, sizeof(v->v[v->n].pred), "%s", pred);
    v->v[v->n].arity = arity;
    v->n++;
    return 0;
}

/* ─── Adornment-variant multimap (predicate, adornment) → names ────────── */

/* Closure blow-up cap: the adornment lattice of one predicate is bounded by
 * 2^arity <= 2^MAX_ARITY = 256; this caps the TOTAL variant count across all
 * predicates.  A program exceeding it is pathological (never silently
 * mis-evaluated). */
#define MAX_ADORN_VARIANTS 64

typedef struct {
    char    pred[64];        /* original predicate name */
    uint8_t arity;           /* natural arity */
    char    adorn[9];        /* this variant's adornment */
    char    adorned_name[80];/* "<pred>__<adorn>" */
    char    magic_name[86];  /* "magic_<pred>__<adorn>" */
    int     is_goal_variant; /* 1 if this is the goal's user-requested variant */
} pred_adorn_variant;

typedef struct { pred_adorn_variant *v; int n, cap; } pa_vec;

/* Fill in the derived adorned/magic predicate names; -1 on overflow. */
static int set_pred_names(pred_adorn_variant *p)
{
    if (snprintf(p->adorned_name, sizeof(p->adorned_name), "%s__%s",
                 p->pred, p->adorn) >= (int)sizeof(p->adorned_name))
        return -1;
    if (snprintf(p->magic_name, sizeof(p->magic_name), "magic_%s__%s",
                 p->pred, p->adorn) >= (int)sizeof(p->magic_name))
        return -1;
    return 0;
}

/* Exact (pred, adorn) pair lookup → index, or -1.  Doubles as the
 * "goal variant" lookup (the goal's user-requested variant is the entry
 * seeded from the user's adornment string). */
static int pa_find(const pa_vec *v, const char *pred, const char *adorn)
{
    int i;
    if (!pred || !adorn) return -1;
    for (i = 0; i < v->n; i++)
        if (strcmp(v->v[i].pred, pred) == 0 &&
            strcmp(v->v[i].adorn, adorn) == 0)
            return i;
    return -1;
}

/* 1 if the predicate has ANY assigned variant (i.e. is in the closure). */
static int pa_find_any(const pa_vec *v, const char *pred)
{
    int i;
    if (!pred) return 0;
    for (i = 0; i < v->n; i++)
        if (strcmp(v->v[i].pred, pred) == 0) return 1;
    return 0;
}

/* Add a (pred, adorn) variant (dedup on the pair); returns its index, or -1
 * on allocation/name-overflow failure.  Idempotent: returning the existing
 * index lets the fixpoint re-discover a pair harmlessly. */
static int pa_push(pa_vec *v, const char *pred, uint8_t arity, const char *adorn)
{
    pred_adorn_variant *nv;
    int qi = pa_find(v, pred, adorn);
    if (qi >= 0) return qi;
    if (v->n >= v->cap) {
        int nc = v->cap ? v->cap * 2 : 8;
        nv = realloc(v->v, (size_t)nc * sizeof(pred_adorn_variant));
        if (!nv) return -1;
        v->v = nv; v->cap = nc;
    }
    memset(&v->v[v->n], 0, sizeof(v->v[v->n]));
    snprintf(v->v[v->n].pred, sizeof(v->v[v->n].pred), "%s", pred);
    v->v[v->n].arity = arity;
    snprintf(v->v[v->n].adorn, sizeof(v->v[v->n].adorn), "%s", adorn);
    if (set_pred_names(&v->v[v->n]) != 0) return -1;
    return v->n++;
}

/* Adorned name for a body atom, or its own pred if it has no variant (EDB
 * atom, negated atom, aggregate atom).  NEGATED ATOMS MUST NEVER BE RENAMED:
 * negation tests the FULL materialization of its predicate, never a
 * magic-seeded slice.  (A negated closure-IDB atom is REJECTED upstream; a
 * negated EDB or non-closure-IDB atom keeps its original predicate.)  An
 * aggregate atom's `pred` is its result variable name, never a relation.
 * `beta` is the SPECIFIC adornment recorded for this atom during the walk;
 * the rename must target that variant exactly, not any variant of A->pred. */
static const char *renamed_pred(const atom *A, const char *beta,
                                const pa_vec *adorns)
{
    int qi;
    if (!A) return NULL;
    if (A->negated || A->aggregate) return A->pred;
    if (!beta || beta[0] == '\0') return A->pred;
    qi = pa_find(adorns, A->pred, beta);
    if (qi < 0) return A->pred;   /* defensive: EDB atom / no such variant */
    return adorns->v[qi].adorned_name;
}

/* ─── Predicate dependency graph + acyclic-SCC check ──────────────────── */

typedef struct { int from, to; } dep_edge;
typedef struct { dep_edge *v; int n, cap; } edge_vec;

static int ev_push(edge_vec *v, int from, int to)
{
    if (v->n >= v->cap) {
        int nc = v->cap ? v->cap * 2 : 16;
        dep_edge *nv = realloc(v->v, (size_t)nc * sizeof(dep_edge));
        if (!nv) return -1;
        v->v = nv; v->cap = nc;
    }
    v->v[v->n].from = from;
    v->v[v->n].to = to;
    v->n++;
    return 0;
}

typedef struct {
    int n;
    unsigned char **adj;
    int *index, *low, *onstack, *stack;
    int idx, top;
    int has_big;   /* set when an SCC of size > 1 is found */
} scc_ctx;

static void scc_visit(scc_ctx *c, int u)
{
    int v, w;
    c->index[u] = c->low[u] = c->idx++;
    c->stack[c->top++] = u;
    c->onstack[u] = 1;
    for (v = 0; v < c->n; v++) {
        if (!c->adj[u][v]) continue;
        if (c->index[v] == -1) {
            scc_visit(c, v);
            if (c->low[v] < c->low[u]) c->low[u] = c->low[v];
        } else if (c->onstack[v]) {
            if (c->index[v] < c->low[u]) c->low[u] = c->index[v];
        }
    }
    if (c->low[u] == c->index[u]) {
        int size = 0;
        do {
            w = c->stack[--c->top];
            c->onstack[w] = 0;
            size++;
        } while (w != u);
        if (size > 1) c->has_big = 1;
    }
}

/* Returns 1 if the predicate dep graph has an SCC with >1 node
 * (cross-predicate mutual recursion), 0 if every SCC is size 1 (self-loops
 * allowed), -1 on allocation failure. */
static int dep_has_multi_node_scc(int n, const edge_vec *E)
{
    scc_ctx c;
    int i;
    int rc = 0;

    if (n <= 1) return 0;

    memset(&c, 0, sizeof(c));
    c.n = n;
    c.adj     = calloc((size_t)n, sizeof(unsigned char *));
    c.index   = malloc((size_t)n * sizeof(int));
    c.low     = malloc((size_t)n * sizeof(int));
    c.onstack = calloc((size_t)n, sizeof(int));
    c.stack   = malloc((size_t)n * sizeof(int));
    if (!c.adj || !c.index || !c.low || !c.onstack || !c.stack) {
        rc = -1;
        goto out;
    }
    for (i = 0; i < n; i++) {
        c.adj[i] = calloc((size_t)n, sizeof(unsigned char));
        if (!c.adj[i]) { rc = -1; goto out; }
        c.index[i] = -1;
    }
    for (i = 0; i < E->n; i++)
        c.adj[E->v[i].from][E->v[i].to] = 1;

    for (i = 0; i < n; i++)
        if (c.index[i] == -1)
            scc_visit(&c, i);

    rc = c.has_big;
out:
    if (c.adj) {
        for (i = 0; i < n; i++) free(c.adj[i]);
        free(c.adj);
    }
    free(c.index); free(c.low); free(c.onstack); free(c.stack);
    return rc;
}

/* ─── SIPS body ordering (deterministic greedy) ───────────────────────── */

/* Score a non-equality body atom: `score` = #bound var args + #constant args;
 * `n_new` = #var args not yet bound.  Constants (TOK_INT/TOK_IDENT) contribute
 * to the score but are NEVER added to the bound set (they do not propagate). */
static void sips_atom_score(const atom *A, const name_set *bound,
                            int *score, int *n_new)
{
    int j, s = 0, nn = 0;
    for (j = 0; j < A->nargs; j++) {
        const token *t = A->args[j];
        if (t->kind == TOK_VAR) {
            if (ns_contains(bound, t->text)) s++;
            else nn++;
        } else if (t->kind == TOK_INT || t->kind == TOK_IDENT) {
            s++;
        }
    }
    *score = s;
    *n_new = nn;
}

/* Strictly-better candidate, lexicographic and total (no partial comparator):
 *   (score desc), then zero-new-vars first, then EDB before IDB, then lowest
 *   ORIGINAL body index (the stable tie-break key). */
static int sips_atom_better(int score, int n_new, int edb, int idx,
                            int b_score, int b_new, int b_edb, int b_idx)
{
    if (score != b_score) return score > b_score;
    {
        int z = (n_new == 0), bz = (b_new == 0);
        if (z != bz) return z > bz;
    }
    if (edb != b_edb) return edb > b_edb;
    return idx < b_idx;
}

/* Compute a deterministic greedy SIPS permutation of [0..R->nbody) into
 * body_order_out (pre-sized R->nbody).  Returns 0 on success, -1 on allocation
 * failure.  Greedy loop: (1) fire any equality ("=", 2 var args) with one side
 * already bound — place it next and propagate the other side; (2) else pick the
 * best-scoring non-equality atom and add its vars to bound; (3) re-sweep
 * equalities.  Negated, aggregate, and comparison atoms are eligible only once
 * ALL their variable args are already bound; when placed, they are
 * zero-propagation — their vars are NOT added to bound.  Arithmetic atoms
 * (`X = E`) are eligible once all operand vars are bound and PROPAGATE their
 * result var to bound (mirroring positive atoms). */
static int sips_body_order(const rule *R, const char *alpha,
                           const pred_vec *preds, int *body_order_out,
                           char *err, size_t errsz)
{
    name_set bound = {0, 0, 0};
    unsigned char *placed = NULL;
    int n = R->nbody;
    int out_n = 0;
    int j;

    if (n <= 0) return 0;   /* parser never emits this; defensive */

    placed = calloc((size_t)n, sizeof(unsigned char));
    if (!placed) {
        if (err && errsz > 0) snprintf(err, errsz, "out of memory");
        return -1;
    }

    /* Init bound from head 'b'-position TOK_VAR args (constants don't
     * propagate — they're positional). */
    for (j = 0; j < R->head->nargs; j++)
        if (alpha[j] == 'b' && R->head->args[j]->kind == TOK_VAR)
            if (ns_add(&bound, R->head->args[j]->text) != 0)
                goto oom;

    while (out_n < n) {
        int i, fired = 0;

        /* (1) Fire any equality with one side already bound. */
        for (i = 0; i < n; i++) {
            const atom *A = R->body[i];
            if (placed[i] || !A || !A->pred) continue;
            if (!is_equality_atom(A)) continue;
            if (A->nargs == 2 &&
                A->args[0]->kind == TOK_VAR &&
                A->args[1]->kind == TOK_VAR) {
                int b0 = ns_contains(&bound, A->args[0]->text);
                int b1 = ns_contains(&bound, A->args[1]->text);
                if (b0 || b1) {
                    if (b0 && !b1) {
                        if (ns_add(&bound, A->args[1]->text) != 0) goto oom;
                    } else if (b1 && !b0) {
                        if (ns_add(&bound, A->args[0]->text) != 0) goto oom;
                    }
                    body_order_out[out_n++] = i;
                    placed[i] = 1;
                    fired = 1;
                }
            }
        }
        if (fired) continue;  /* re-sweep: propagation may enable another '=' */

        /* (2) Score remaining non-equality atoms; pick the best. */
        {
            int best = -1, best_score = 0, best_new = 0, best_edb = 0;
            int best_nonprop = 0;   /* best atom is negated/aggregate/cmp */
            for (i = 0; i < n; i++) {
                const atom *A = R->body[i];
                int score, n_new, edb, nonprop, is_ar, is_cp, is_sp, is_sf;
                if (placed[i] || !A || !A->pred) continue;
                if (is_equality_atom(A)) continue;  /* none fireable now */
                is_ar = is_arith_atom(A);
                is_cp = is_comparison_atom(A);
                is_sp = is_str_producing_atom(A);
                is_sf = is_str_filter_atom(A);
                if (is_ar) {
                    /* arithmetic: eligible once all operand vars bound;
                     * score over operand vars; propagates its result var */
                    expr_operand_vars(A->arith, &bound, &score, &n_new);
                    nonprop = 0;
                    if (n_new > 0) continue;   /* defer until operands bound */
                } else if (is_sp) {
                    /* string-producing: eligible once all operand vars
                     * (args[1..]) bound; score over operands; propagates its
                     * result var (args[0]) */
                    str_operand_vars(A, 1, &bound, &score, &n_new);
                    nonprop = 0;
                    if (n_new > 0) continue;
                } else if (is_cp) {
                    /* comparison: zero-propagation; eligible only when every
                     * variable operand is bound */
                    sips_atom_score(A, &bound, &score, &n_new);
                    nonprop = 1;
                    if (n_new > 0) continue;   /* defer */
                } else if (is_sf) {
                    /* string filter: zero-propagation; eligible only when
                     * every variable operand (args[0..1]) is bound */
                    str_operand_vars(A, 0, &bound, &score, &n_new);
                    nonprop = 1;
                    if (n_new > 0) continue;
                } else {
                    nonprop = (A->aggregate || A->negated);
                    sips_atom_score(A, &bound, &score, &n_new);
                    if (nonprop && n_new > 0) continue;
                }
                edb = (pred_find(preds, A->pred) < 0);
                if (best < 0 ||
                    sips_atom_better(score, n_new, edb, i,
                                     best_score, best_new, best_edb, best)) {
                    best = i;
                    best_score = score;
                    best_new = n_new;
                    best_edb = edb;
                    best_nonprop = nonprop;
                }
            }
            if (best >= 0) {
                const atom *A = R->body[best];
                body_order_out[out_n++] = best;
                placed[best] = 1;
                if (is_arith_atom(A)) {
                    /* propagate the arithmetic result var */
                    if (A->nargs >= 1 && A->args[0]->kind == TOK_VAR)
                        if (ns_add(&bound, A->args[0]->text) != 0) goto oom;
                } else if (is_str_producing_atom(A)) {
                    /* propagate the string-producing result var (args[0]) */
                    if (A->nargs >= 1 && A->args[0]->kind == TOK_VAR)
                        if (ns_add(&bound, A->args[0]->text) != 0) goto oom;
                } else if (!best_nonprop) {
                    for (j = 0; j < A->nargs; j++)
                        if (A->args[j]->kind == TOK_VAR)
                            if (ns_add(&bound, A->args[j]->text) != 0) goto oom;
                }
                continue;
            }
        }

        /* (3) Defensive fallback: nothing fireable (e.g. an equality with
         * neither side bound and no other atom left).  Place the remainder in
         * original order to guarantee termination. */
        for (i = 0; i < n; i++)
            if (!placed[i]) body_order_out[out_n++] = i;
        break;
    }

    free(placed);
    free(bound.v);
    return 0;

oom:
    if (err && errsz > 0) snprintf(err, errsz, "out of memory");
    free(placed);
    free(bound.v);
    return -1;
}

/* ─── Per-(rule, variant) SIPS cache ──────────────────────────────────── */
/* One permutation per (rule_idx, variant_idx): the single source of truth
 * shared by the fixpoint walk (Phase B) and the synthesis loops (Phase C).
 * With multi-adornment the SIPS order depends on the head's adornment, so a
 * per-rule cache is insufficient — different variants of one predicate may
 * order a rule's body differently and thus assign different body-atom
 * adornments. */

typedef struct { int rule_idx; int variant_idx; int *order; } sips_entry;
typedef struct { sips_entry *v; int n, cap; } sips_cache;

static int *sips_get(sips_cache *sc, int rule_idx, int variant_idx,
                     const rule *R, const char *alpha, const pred_vec *preds,
                     char *err, size_t errsz)
{
    int i, *ord;
    for (i = 0; i < sc->n; i++)
        if (sc->v[i].rule_idx == rule_idx && sc->v[i].variant_idx == variant_idx)
            return sc->v[i].order;
    if (sc->n >= sc->cap) {
        int nc = sc->cap ? sc->cap * 2 : 8;
        sips_entry *nv = realloc(sc->v, (size_t)nc * sizeof(sips_entry));
        if (!nv) {
            if (err && errsz > 0) snprintf(err, errsz, "out of memory");
            return NULL;
        }
        sc->v = nv; sc->cap = nc;
    }
    ord = malloc((size_t)(R->nbody > 0 ? R->nbody : 1) * sizeof(int));
    if (!ord) {
        if (err && errsz > 0) snprintf(err, errsz, "out of memory");
        return NULL;
    }
    if (sips_body_order(R, alpha, preds, ord, err, errsz) != 0) {
        free(ord);
        return NULL;
    }
    sc->v[sc->n].rule_idx = rule_idx;
    sc->v[sc->n].variant_idx = variant_idx;
    sc->v[sc->n].order = ord;
    sc->n++;
    return ord;
}

static void sips_cache_free(sips_cache *sc)
{
    int i;
    for (i = 0; i < sc->n; i++) free(sc->v[i].order);
    free(sc->v);
}

/* ─── Bound-set walk (adornment capture) ───────────────────────────────── */

/* Walk R's body in SIPS order under head adornment `alpha`, maintaining the
 * SIPS bound-variable set, and fill betas[j] (indexed by ORIGINAL body
 * position j) with each positive IDB atom's adornment, "" for every other
 * atom (equality, comparison, arithmetic, negated, aggregate, EDB).  The
 * bound-set propagation exactly mirrors sips_body_order (equalities propagate;
 * positive atoms propagate all their var args; arithmetic propagates its result
 * var; comparisons/negated/aggregate atoms propagate nothing), so betas are the
 * single source of truth for BOTH variant discovery (Phase B) and synthesis
 * renaming (Phase C).  Returns 0 / -1 (OOM, err set). */
static int compute_betas(const rule *R, const char *alpha,
                         const pred_vec *preds, const int *body_order,
                         char (*betas)[9], char *err, size_t errsz)
{
    name_set bound = {0, 0, 0};
    int j, a;

    for (j = 0; j < R->nbody; j++) betas[j][0] = '\0';

    for (j = 0; j < R->head->nargs; j++)
        if (alpha[j] == 'b' && R->head->args[j]->kind == TOK_VAR)
            if (ns_add(&bound, R->head->args[j]->text) != 0) goto oom;

    for (j = 0; j < R->nbody; j++) {
        const atom *A = R->body[body_order ? body_order[j] : j];
        int bi = body_order ? body_order[j] : j;
        if (!A) continue;

        if (is_equality_atom(A)) {
            /* equality X = Y: propagate binding in either direction */
            if (A->nargs == 2 &&
                A->args[0]->kind == TOK_VAR &&
                A->args[1]->kind == TOK_VAR) {
                const char *a0 = A->args[0]->text;
                const char *a1 = A->args[1]->text;
                int b0 = ns_contains(&bound, a0);
                int b1 = ns_contains(&bound, a1);
                if (b0 && !b1) { if (ns_add(&bound, a1) != 0) goto oom; }
                else if (b1 && !b0) { if (ns_add(&bound, a0) != 0) goto oom; }
            }
            continue;
        }

        if (is_arith_atom(A)) {
            /* arithmetic X = E: propagate the result var (mirror equality
             * propagation); betas stay "" (arithmetic is never an IDB atom) */
            if (A->nargs >= 1 && A->args[0]->kind == TOK_VAR)
                if (ns_add(&bound, A->args[0]->text) != 0) goto oom;
            continue;
        }

        if (is_str_producing_atom(A)) {
            /* string-producing: propagate the result var (args[0]); betas
             * stay "" (never an IDB atom) */
            if (A->nargs >= 1 && A->args[0]->kind == TOK_VAR)
                if (ns_add(&bound, A->args[0]->text) != 0) goto oom;
            continue;
        }

        if (is_comparison_atom(A)) {
            /* comparison: zero-propagation */
            continue;
        }

        if (is_str_filter_atom(A)) {
            /* string filter: zero-propagation */
            continue;
        }

        /* Negated and aggregate atoms neither propagate adornment nor add
         * vars to bound (SIPS schedules them only once all their var args
         * are bound).  The negation/aggregate soundness REJECTs are enforced
         * in a post-pass after the fixpoint (see Phase B) so they are
         * independent of rule/atom ordering. */
        if (A->negated || A->aggregate) continue;

        if (A->pred && pred_find(preds, A->pred) >= 0)
            atom_adornment(A, &bound, betas[bi]);

        for (a = 0; a < A->nargs; a++)
            if (A->args[a]->kind == TOK_VAR)
                if (ns_add(&bound, A->args[a]->text) != 0) goto oom;
    }
    free(bound.v);
    return 0;

oom:
    if (err && errsz > 0) snprintf(err, errsz, "out of memory");
    free(bound.v);
    return -1;
}

/* ─── Rule ordering for single-pass non-recursive evaluation ──────────── */

/* The VM (vm.c: eval_nonrecursive) evaluates a non-recursive stratum in a
 * SINGLE PASS over its rules in emit order — it does not do a fixpoint or a
 * topological sort.  The synthesized magic program is a dependency DAG
 * (magic seeds propagate goal→leaf, adorned relations propagate leaf→goal),
 * so emitting rules in goal-first order leaves a dependent's adorned relation
 * empty (e.g. p^fb :- magic_p^fb, q^fb emitted before q^fb is populated).
 *
 * Fix: topologically sort the synthesized rules so every rule's dependencies
 * (body atoms whose predicate is itself a rule head) are emitted first.
 * Self-references are ignored — recursion is handled by the compiler's
 * stratum/SCC machinery, not by emit order.  Cross-predicate cycles are
 * already REJECTED, so the graph is a DAG. */
static void topo_sort_rules(rule_vec *v)
{
    int n = v->n;
    int i, j, k;
    unsigned char **adj;
    int *indeg;
    int *q;
    rule **sorted;
    int sorted_n = 0;
    int qh = 0, qt = 0;

    if (n <= 1) return;

    adj = calloc((size_t)n, sizeof(unsigned char *));
    indeg = calloc((size_t)n, sizeof(int));
    q = malloc((size_t)n * sizeof(int));
    if (!adj || !indeg || !q) goto out_early;
    for (i = 0; i < n; i++) {
        adj[i] = calloc((size_t)n, sizeof(unsigned char));
        if (!adj[i]) goto out_free_adj;
    }

    /* Edge r1 -> r2 when r1's head predicate appears in r2's body. */
    for (i = 0; i < n; i++) {
        const rule *r2 = v->v[i];
        for (j = 0; j < r2->nbody; j++) {
            const char *bp = r2->body[j]->pred;
            if (!bp) continue;
            for (k = 0; k < n; k++) {
                if (k == i) continue;  /* ignore self-reference */
                if (strcmp(v->v[k]->head->pred, bp) == 0 && !adj[k][i]) {
                    adj[k][i] = 1;
                    indeg[i]++;
                }
            }
        }
    }

    /* Kahn's algorithm. */
    for (i = 0; i < n; i++)
        if (indeg[i] == 0) q[qt++] = i;
    sorted = malloc((size_t)n * sizeof(rule *));
    if (!sorted) goto out_free_adj;
    while (qh < qt) {
        int u = q[qh++];
        sorted[sorted_n++] = v->v[u];
        for (i = 0; i < n; i++) {
            if (adj[u][i] && --indeg[i] == 0)
                q[qt++] = i;
        }
    }

    /* Reorder only on a complete sort (a cycle — which the mutual-recursion
     * REJECT already rules out — would leave sorted_n < n; keep order then). */
    if (sorted_n == n) {
        for (i = 0; i < n; i++) v->v[i] = sorted[i];
    }
    free(sorted);

out_free_adj:
    for (i = 0; i < n; i++) free(adj[i]);
    free(adj);
out_early:
    free(indeg);
    free(q);
}

/* ─── The transformation ──────────────────────────────────────────────── */

int magic_transform(const rule *const *ast_rules, int n_ast,
                    const char *goal_pred, uint8_t goal_arity,
                    const uint32_t *leading, uint8_t k, size_t src_nrels,
                    interner *ir, magic_program *out,
                    char *reject_reason, size_t reject_sz)
{
    /* Leading-prefix shorthand: synthesize adorn = "b"*k + "f"*(arity-k) and
     * route to the general transform.  Guard the args that affect the local
     * adorn[] buffer (goal_arity ≤ 8, k ≤ goal_arity); everything else is
     * validated inside magic_transform_adorn. */
    char adorn[9];
    if (reject_reason && reject_sz > 0) reject_reason[0] = '\0';
    if (goal_arity > 8) {
        if (reject_reason && reject_sz > 0)
            snprintf(reject_reason, reject_sz,
                     "goal arity %u out of range 1..8", goal_arity);
        memset(out, 0, sizeof(*out));
        return -1;
    }
    if (k > goal_arity) {
        if (reject_reason && reject_sz > 0)
            snprintf(reject_reason, reject_sz, "k=%u exceeds goal arity %u",
                     k, goal_arity);
        memset(out, 0, sizeof(*out));
        return -1;
    }
    make_adornment(adorn, goal_arity, k);
    return magic_transform_adorn(ast_rules, n_ast, goal_pred, goal_arity,
                                 adorn, leading, k, src_nrels, ir, out,
                                 reject_reason, reject_sz);
}

int magic_transform_adorn(const rule *const *ast_rules, int n_ast,
                          const char *goal_pred, uint8_t goal_arity,
                          const char *adorn, const uint32_t *vals,
                          uint8_t nvals, size_t src_nrels,
                          interner *ir, magic_program *out,
                          char *reject_reason, size_t reject_sz)
{
    pred_vec   preds    = {0, 0, 0};
    pa_vec     adorns   = {0, 0, 0};
    edge_vec   edges    = {0, 0, 0};
    rule_vec   modrules = {0, 0, 0};
    decl_vec   decls    = {0, 0, 0};
    sips_cache sips     = {0, 0, 0};
    int       *queue    = NULL;
    int        goal_vi  = -1;   /* index of the goal's user-requested variant */
    int        i, j;

    (void)ir;
    (void)vals;

    memset(out, 0, sizeof(*out));
    if (reject_reason && reject_sz > 0) reject_reason[0] = '\0';

#define REJECT(fmt, ...) do {                                              \
        if (reject_reason && reject_sz > 0)                                \
            snprintf(reject_reason, reject_sz, fmt, ##__VA_ARGS__);        \
        goto fail;                                                         \
    } while (0)

    if (!ast_rules || n_ast <= 0) REJECT("no rules loaded");
    if (!goal_pred) REJECT("null goal predicate");
    if (!adorn) REJECT("null goal adornment");
    if (goal_arity == 0 || goal_arity > 8)
        REJECT("goal arity %u out of range 1..8", goal_arity);
    {
        size_t alen = strlen(adorn);
        size_t xi;
        int nb = 0;
        if (alen != goal_arity)
            REJECT("adornment length %zu != goal arity %u", alen, goal_arity);
        for (xi = 0; xi < alen; xi++) {
            if (adorn[xi] != 'b' && adorn[xi] != 'f')
                REJECT("adornment char '%c' (not 'b'/'f')", adorn[xi]);
            if (adorn[xi] == 'b') nb++;
        }
        if (nvals == 0) REJECT("nvals==0 not supported (route to dl_query)");
        if (nvals > goal_arity)
            REJECT("nvals=%u exceeds goal arity %u", nvals, goal_arity);
        if (nb != (int)nvals)
            REJECT("nvals=%u != count_b(adorn)=%d", nvals, nb);
    }

    /* ── Collect the IDB head set (with natural arity) ── */
    for (i = 0; i < n_ast; i++) {
        const rule *r = ast_rules[i];
        if (!r || !r->head || !r->head->pred) continue;
        if (pred_push(&preds, r->head->pred, (uint8_t)r->head->nargs) != 0)
            REJECT("out of memory");
    }

    /* The goal must be an IDB predicate (EDB goals degenerate to prefix). */
    if (pred_find(&preds, goal_pred) < 0)
        REJECT("goal '%s' is not a rule head (EDB goal: use prefix lookup)",
               goal_pred);

    /* Seed the goal's user-requested variant (decision 4: this is the ONLY
     * relation the result stream in dl.c reads). */
    goal_vi = pa_push(&adorns, goal_pred, goal_arity, adorn);
    if (goal_vi < 0)
        REJECT("adorned predicate name for '%s' too long", goal_pred);
    adorns.v[goal_vi].is_goal_variant = 1;

    /* ── Phase A: predicate dependency graph (body IDB -> head) ── */
    for (i = 0; i < n_ast; i++) {
        const rule *r = ast_rules[i];
        int hi;
        if (!r || !r->head || !r->head->pred) continue;
        hi = pred_find(&preds, r->head->pred);
        for (j = 0; j < r->nbody; j++) {
            const atom *A = r->body[j];
            int bi;
            if (!A || !A->pred) continue;
            if (strcmp(A->pred, "=") == 0) continue;
            if (is_str_producing_atom(A) || is_str_filter_atom(A)) continue;
            bi = pred_find(&preds, A->pred);
            if (bi >= 0)               /* IDB body atom → edge bi -> hi */
                if (ev_push(&edges, bi, hi) != 0) REJECT("out of memory");
        }
    }

    /* Reject cross-predicate mutual recursion (SCC size > 1). */
    {
        int s = dep_has_multi_node_scc(preds.n, &edges);
        if (s < 0) REJECT("out of memory");
        if (s > 0)
            REJECT("cross-predicate mutual recursion (dependency cycle "
                   "among IDB predicates) not supported");
    }
    free(edges.v);
    edges.v = NULL;

    /* ── Phase B: adornment-closure fixpoint ──
     * Pop a variant (P,alpha) from the worklist; for every rule R with head
     * P, compute the SIPS order and per-atom betas under alpha; for each
     * positive IDB body atom A with beta != "", create the variant
     * (A.pred, beta) if absent and push it.  Terminates because the variant
     * set is bounded by the finite adornment lattice; capped by
     * MAX_ADORN_VARIANTS to reject pathological blow-up. */
    queue = malloc((size_t)MAX_ADORN_VARIANTS * sizeof(int));
    if (!queue) REJECT("out of memory");
    {
        int qh = 0, qt = 0;
        char pal[9];
        queue[qt++] = goal_vi;
        while (qh < qt) {
            int vi = queue[qh++];
            const pred_adorn_variant *V = &adorns.v[vi];
            snprintf(pal, sizeof(pal), "%s", V->adorn);
            for (i = 0; i < n_ast; i++) {
                const rule *R = ast_rules[i];
                int *ord;
                char (*betas)[9];
                int a;
                if (!R || !R->head || !R->head->pred) continue;
                if (strcmp(R->head->pred, V->pred) != 0) continue;
                ord = sips_get(&sips, i, vi, R, pal, &preds,
                               reject_reason, reject_sz);
                if (!ord) goto fail;
                betas = calloc((size_t)(R->nbody > 0 ? R->nbody : 1),
                               sizeof(char[9]));
                if (!betas) REJECT("out of memory");
                if (compute_betas(R, pal, &preds, ord, betas,
                                  reject_reason, reject_sz) != 0) {
                    free(betas);
                    goto fail;
                }
                for (a = 0; a < R->nbody; a++) {
                    const atom *A = R->body[a];
                    int nvi;
                    if (betas[a][0] == '\0') continue;
                    if (!A || !A->pred || strcmp(A->pred, "=") == 0) continue;
                    if (A->negated || A->aggregate) continue;  /* defensive */
                    nvi = pa_find(&adorns, A->pred, betas[a]);
                    if (nvi < 0) {
                        nvi = pa_push(&adorns, A->pred, (uint8_t)A->nargs,
                                      betas[a]);
                        if (nvi < 0) {
                            free(betas);
                            REJECT("adorned predicate name for '%s' too long",
                                   A->pred);
                        }
                        if (adorns.n > MAX_ADORN_VARIANTS) {
                            free(betas);
                            REJECT("adornment closure exceeds %d variants "
                                   "(predicate lattice blow-up)",
                                   MAX_ADORN_VARIANTS);
                        }
                        queue[qt++] = nvi;
                    }
                }
                free(betas);
            }
        }
    }
    free(queue);
    queue = NULL;

    /* ── Negation / aggregate soundness post-pass ──
     * The adorned closure is now fully assigned (fixpoint reached).  Check
     * the soundness boundary AFTER the worklist so the test is independent
     * of rule/atom ordering (a predicate may be assigned by a LATER positive
     * occurrence than a negated one that references it).  Never silently
     * mis-evaluate:
     *   (a) a negated atom whose predicate is IN the adorned closure would
     *       test a magic-seeded slice, not the full complement → REJECT.
     *   (b) an aggregate in a rule that also joins an adorned-closure
     *       predicate would read a bound slice instead of the full group →
     *       REJECT (over-conservative; see header doc). */
    for (i = 0; i < n_ast; i++) {
        const rule *R = ast_rules[i];
        int jj;
        if (!R || !R->head || !R->head->pred) continue;
        if (!pa_find_any(&adorns, R->head->pred)) continue;  /* unreachable */
        for (jj = 0; jj < R->nbody; jj++) {
            const atom *A = R->body[jj];
            if (!A || !A->pred) continue;
            if (A->negated) {
                if (pa_find_any(&adorns, A->pred))
                    REJECT("negation on adorned-closure predicate '%s' "
                           "not supported", A->pred);
            } else if (A->aggregate) {
                int a2;
                for (a2 = 0; a2 < R->nbody; a2++) {
                    const atom *B = R->body[a2];
                    if (!B || !B->pred) continue;
                    if (B->negated || B->aggregate) continue;
                    if (strcmp(B->pred, "=") == 0) continue;
                    if (pa_find_any(&adorns, B->pred))
                        REJECT("aggregate in rule with adorned-closure "
                               "body atom '%s' not supported", B->pred);
                }
            }
        }
    }

    /* ── MAX_RELS budget: src_nrels aliased + 2 fresh rels per variant ── */
    {
        size_t total = src_nrels + 2 * (size_t)adorns.n;
        if (total > MAX_RELS)
            REJECT("adorned closure needs %zu aliased + %d fresh relations "
                   "(%d variants x 2) = %zu total, exceeding MAX_RELS=%d",
                   src_nrels, 2 * adorns.n, adorns.n, total, MAX_RELS);
    }

    /* ── Phase C: per-(P,alpha) synthesis of all 3 rule classes ── */
    for (i = 0; i < adorns.n; i++) {
        const pred_adorn_variant *P = &adorns.v[i];
        int r;

        for (r = 0; r < n_ast; r++) {
            const rule *R = ast_rules[r];
            atom *guard, *mhead;
            atom **mbody;
            rule *mr;
            int *ord;
            char (*betas)[9];
            int total;

            if (!R || !R->head || !R->head->pred) continue;
            if (strcmp(R->head->pred, P->pred) != 0) continue;

            /* The cached SIPS permutation — the SAME one the fixpoint walk
             * used for this (rule, variant).  Single source of truth. */
            ord = sips_get(&sips, r, i, R, P->adorn, &preds,
                           reject_reason, reject_sz);
            if (!ord) goto fail;
            betas = calloc((size_t)(R->nbody > 0 ? R->nbody : 1),
                           sizeof(char[9]));
            if (!betas) REJECT("out of memory");
            if (compute_betas(R, P->adorn, &preds, ord, betas,
                              reject_reason, reject_sz) != 0) {
                free(betas);
                goto fail;
            }

            /* (a) adorned rule: P^a :- magic_P^a(bound head), body' */
            guard = build_bound_atom(R->head, P->magic_name, P->adorn);
            if (!guard) { free(betas); REJECT("out of memory"); }
            total = 1 + R->nbody;
            mbody = calloc((size_t)total, sizeof(atom *));
            if (!mbody) { atom_free_local(guard); free(betas); REJECT("out of memory"); }
            mbody[0] = guard;
            for (j = 0; j < R->nbody; j++) {
                int bj = ord ? ord[j] : j;
                mbody[1 + j] = atom_copy(R->body[bj],
                                         renamed_pred(R->body[bj], betas[bj],
                                                      &adorns));
                if (!mbody[1 + j]) {
                    free(betas);
                    atoms_free(mbody, 1 + j);
                    REJECT("out of memory");
                }
            }
            mhead = atom_copy(R->head, P->adorned_name);
            if (!mhead) { free(betas); atoms_free(mbody, total); REJECT("out of memory"); }
            /* Propagate the source rule's negation/aggregate flags so the
             * synthesized rule is self-consistent (the compiler re-derives
             * them from body atoms, but rule_free and future consumers rely
             * on the flags). */
            mr = make_rule(mhead, mbody, total, R->has_negation,
                           R->has_aggregate);
            if (!mr) {
                atom_free_local(mhead);
                free(betas);
                atoms_free(mbody, total);
                REJECT("out of memory");
            }
            if (rv_push(&modrules, mr) != 0) {
                free(betas);
                rule_free(mr);
                REJECT("out of memory");
            }

            /* (b)/(c) magic rules for each IDB body atom (self-recursion
             * when Q==P && beta==alpha, dependency-seed otherwise — same
             * shape; the target magic is keyed on the atom's recorded beta). */
            for (j = 0; j < R->nbody; j++) {
                const atom *A = R->body[ord ? ord[j] : j];
                int aorig = ord ? ord[j] : j;
                atom **mgbody;
                atom *mghead;
                rule *mgr;
                int total2, jj, qi, mg_neg, mg_agg;
                const char *target_magic, *target_adorn;

                if (!A || !A->pred) continue;
                /* Negated and aggregate atoms never seed magic: a negated
                 * atom's predicate is OUTSIDE the closure (a negated
                 * closure-IDB atom is REJECTED in the post-pass), and an
                 * aggregate atom's `pred` is its result variable name. */
                if (A->negated || A->aggregate) continue;
                if (betas[aorig][0] == '\0') continue;   /* EDB body atom */
                qi = pa_find(&adorns, A->pred, betas[aorig]);
                if (qi < 0) continue;   /* defensive: no such variant */

                /* The SPECIFIC variant A was called under (decision 10/T2),
                 * not "any variant of A->pred". */
                target_magic = adorns.v[qi].magic_name;
                target_adorn = adorns.v[qi].adorn;

                mghead = build_bound_atom(A, target_magic, target_adorn);
                if (!mghead) { free(betas); REJECT("out of memory"); }

                total2 = 1 + j;   /* magic guard + prefix atoms A0..A_{j-1} */
                mgbody = calloc((size_t)total2, sizeof(atom *));
                if (!mgbody) { atom_free_local(mghead); free(betas); REJECT("out of memory"); }
                mgbody[0] = build_bound_atom(R->head, P->magic_name, P->adorn);
                if (!mgbody[0]) {
                    atom_free_local(mghead);
                    free(mgbody);
                    free(betas);
                    REJECT("out of memory");
                }
                mg_neg = 0; mg_agg = 0;
                for (jj = 0; jj < j; jj++) {
                    int bjj = ord ? ord[jj] : jj;
                    const atom *B = R->body[bjj];
                    if (!B) continue;
                    if (B->negated) mg_neg = 1;
                    if (B->aggregate) mg_agg = 1;
                    mgbody[1 + jj] = atom_copy(B,
                                               renamed_pred(B, betas[bjj],
                                                            &adorns));
                    if (!mgbody[1 + jj]) {
                        atom_free_local(mghead);
                        atoms_free(mgbody, 1 + jj);
                        free(betas);
                        REJECT("out of memory");
                    }
                }
                /* Prefix may contain a negated atom (e.g. !blocked before an
                 * IDB join), so derive the flags from the prefix rather than
                 * hard-coding (0,0). */
                mgr = make_rule(mghead, mgbody, total2, mg_neg, mg_agg);
                if (!mgr) {
                    atom_free_local(mghead);
                    atoms_free(mgbody, total2);
                    free(betas);
                    REJECT("out of memory");
                }
                if (rv_push(&modrules, mgr) != 0) {
                    rule_free(mgr);
                    free(betas);
                    REJECT("out of memory");
                }
            }
            free(betas);
        }
    }

    /* ── Phase D: decls — 2 per variant ── */
    for (i = 0; i < adorns.n; i++) {
        const pred_adorn_variant *P = &adorns.v[i];
        uint8_t nb = 0;
        size_t alen = strlen(P->adorn);
        for (j = 0; j < (int)alen; j++)
            if (P->adorn[j] == 'b') nb++;

        if (dv_contains(&decls, P->adorned_name))
            REJECT("adorned/magic predicate name collision '%s'",
                   P->adorned_name);
        if (dv_push(&decls, P->adorned_name, P->arity) != 0)
            REJECT("out of memory");
        if (dv_contains(&decls, P->magic_name))
            REJECT("adorned/magic predicate name collision '%s'",
                   P->magic_name);
        if (dv_push(&decls, P->magic_name, nb) != 0)
            REJECT("out of memory");
    }

    /* Topological order so the VM's single-pass non-recursive evaluation
     * populates dependencies before dependents (goal->leaf magic, leaf->goal
     * adorned).  Recursive strata are unaffected (compiler strata handle them). */
    topo_sort_rules(&modrules);

    out->rules = modrules.v;
    out->n_rules = modrules.n;
    out->decls = decls.v;
    out->n_decls = decls.n;
    /* The goal's output relation is its USER-requested variant specifically
     * (decision 4), not any variant of the goal predicate. */
    snprintf(out->adorned_goal, sizeof(out->adorned_goal), "%s",
             adorns.v[goal_vi].adorned_name);
    out->goal_arity = goal_arity;

    free(preds.v);
    free(adorns.v);
    sips_cache_free(&sips);
    return 0;

fail:
    free(queue);
    free(edges.v);
    free(preds.v);
    free(adorns.v);
    sips_cache_free(&sips);
    {
        int x;
        for (x = 0; x < modrules.n; x++) rule_free(modrules.v[x]);
        free(modrules.v);
        free(decls.v);
    }
    return -1;
#undef REJECT
}

void magic_program_free(magic_program *p)
{
    int i;
    if (!p) return;
    for (i = 0; i < p->n_rules; i++) rule_free(p->rules[i]);
    free(p->rules);
    free(p->decls);
    memset(p, 0, sizeof(*p));
}
