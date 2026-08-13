/*
 * magic.c — Magic-sets AST→AST adornment rewrite (M8 v2 multi-predicate slice)
 *
 * Pure transformation: no dl_db access.  Reads the retained AST (a const
 * array of rule*) and synthesizes a fresh adorned + magic program.
 *
 * The synthesized AST is built with the SAME allocation discipline as
 * parser.c (calloc'd atom/rule, strdup'd pred/text, one token* per arg), so
 * the program can be freed with rule_free() (parser.h) and compiled by the
 * untouched compile_rules().
 *
 * Multi-predicate slice: the goal IDB predicate's dependency closure is an
 * ACYCLIC DAG of IDB predicates (per-predicate self-recursion allowed).
 * Each reachable predicate P gets EXACTLY ONE adornment, derived by worklist
 * propagation from the goal's bound leading args.  For each adorned
 * predicate (P,alpha) we synthesize:
 *   (a) adorned rule          P__alpha(...) :- magic_P__alpha(bound head), body'
 *   (b) self-recursion magic  (body IDB atom Q == P):
 *       magic_P__alpha(bound args) :- magic_P__alpha(head bound), prefix'
 *   (c) dependency-seed magic (body IDB atom Q != P):
 *       magic_Q__gamma(bound args) :- magic_P__alpha(head bound), prefix'
 *
 * Conservative REJECTS (never silently mis-evaluate):
 *   - negation / aggregate in any reachable rule
 *   - k == 0 (caller routes to dl_query)
 *   - cross-predicate mutual recursion (predicate dep-graph SCC size > 1)
 *   - multiple distinct adornments for one predicate
 *   - a recursive call needing a different adornment (non-leading adornment)
 *   - MAX_RELS budget overflow / adorned-predicate name collision
 */
#include "magic.h"
#include "intern.h"
#include "dl_internal.h"   /* MAX_RELS */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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

/* ─── Per-predicate adornment table ───────────────────────────────────── */

typedef struct {
    char    pred[64];        /* original predicate name */
    uint8_t arity;           /* natural arity */
    char    adorn[9];        /* assigned adornment (empty until assigned) */
    int     assigned;        /* 1 once the worklist assigns an adornment */
    char    adorned_name[64];/* "<pred>__<adorn>" */
    char    magic_name[64];  /* "magic_<pred>__<adorn>" */
} pred_adorn;

typedef struct { pred_adorn *v; int n, cap; } pa_vec;

static int pa_find(const pa_vec *v, const char *pred)
{
    int i;
    if (!pred) return -1;
    for (i = 0; i < v->n; i++)
        if (strcmp(v->v[i].pred, pred) == 0) return i;
    return -1;
}

static int pa_push(pa_vec *v, const char *pred, uint8_t arity)
{
    pred_adorn *nv;
    if (pa_find(v, pred) >= 0) return 0;
    if (v->n >= v->cap) {
        int nc = v->cap ? v->cap * 2 : 8;
        nv = realloc(v->v, (size_t)nc * sizeof(pred_adorn));
        if (!nv) return -1;
        v->v = nv; v->cap = nc;
    }
    memset(&v->v[v->n], 0, sizeof(v->v[v->n]));
    snprintf(v->v[v->n].pred, sizeof(v->v[v->n].pred), "%s", pred);
    v->v[v->n].arity = arity;
    v->n++;
    return 0;
}

/* Fill in the derived adorned/magic predicate names; -1 on overflow. */
static int set_pred_names(pred_adorn *p)
{
    if (snprintf(p->adorned_name, sizeof(p->adorned_name), "%s__%s",
                 p->pred, p->adorn) >= (int)sizeof(p->adorned_name))
        return -1;
    if (snprintf(p->magic_name, sizeof(p->magic_name), "magic_%s__%s",
                 p->pred, p->adorn) >= (int)sizeof(p->magic_name))
        return -1;
    return 0;
}

/* Adorned name for a body atom, or its own pred if it's an EDB atom. */
static const char *renamed_pred(const atom *A, const pa_vec *adorns)
{
    int qi = pa_find(adorns, A->pred);
    return (qi >= 0) ? adorns->v[qi].adorned_name : A->pred;
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

/* ─── Bound-set walk (adornment propagation) ──────────────────────────── */

/* Walk R's body left-to-right under head adornment `alpha`, maintaining the
 * SIPS bound-variable set and assigning/checking each IDB body atom's
 * adornment in `adorns`.  Newly-assigned predicates are appended to the
 * worklist queue (queue/qt).  On conflict, writes *err and returns -1. */
static int walk_rule_adorns(const rule *R, const char *alpha,
                            pa_vec *adorns, int *queue, int *qt,
                            char *err, size_t errsz)
{
    name_set bound = {0, 0, 0};
    int j, a;

    for (j = 0; j < R->head->nargs; j++)
        if (alpha[j] == 'b' && R->head->args[j]->kind == TOK_VAR)
            ns_add(&bound, R->head->args[j]->text);

    for (j = 0; j < R->nbody; j++) {
        const atom *A = R->body[j];
        char beta[9];
        int qi;
        if (!A) continue;

        if (A->pred && strcmp(A->pred, "=") == 0) {
            /* equality X = Y: propagate binding in either direction */
            if (A->nargs == 2 &&
                A->args[0]->kind == TOK_VAR &&
                A->args[1]->kind == TOK_VAR) {
                const char *a0 = A->args[0]->text;
                const char *a1 = A->args[1]->text;
                int b0 = ns_contains(&bound, a0);
                int b1 = ns_contains(&bound, a1);
                if (b0 && !b1) ns_add(&bound, a1);
                else if (b1 && !b0) ns_add(&bound, a0);
            }
            continue;
        }
        if (A->aggregate) {
            snprintf(err, errsz, "aggregate in rule for '%s' not supported",
                     R->head->pred);
            free(bound.v);
            return -1;
        }

        qi = pa_find(adorns, A->pred);
        if (qi >= 0) {
            atom_adornment(A, &bound, beta);
            if (adorns->v[qi].assigned) {
                if (strcmp(beta, adorns->v[qi].adorn) != 0) {
                    if (strcmp(A->pred, R->head->pred) == 0)
                        snprintf(err, errsz,
                            "recursive call to '%s' needs adornment '%s' "
                            "(head is '%s') not supported",
                            A->pred, beta, adorns->v[qi].adorn);
                    else
                        snprintf(err, errsz,
                            "predicate '%s' called with adornment '%s' "
                            "(already '%s') not supported",
                            A->pred, beta, adorns->v[qi].adorn);
                    free(bound.v);
                    return -1;
                }
            } else {
                snprintf(adorns->v[qi].adorn, sizeof(adorns->v[qi].adorn),
                         "%s", beta);
                adorns->v[qi].assigned = 1;
                if (set_pred_names(&adorns->v[qi]) != 0) {
                    snprintf(err, errsz,
                             "adorned predicate name for '%s' too long",
                             A->pred);
                    free(bound.v);
                    return -1;
                }
                queue[(*qt)++] = qi;
            }
        }

        for (a = 0; a < A->nargs; a++)
            if (A->args[a]->kind == TOK_VAR)
                ns_add(&bound, A->args[a]->text);
    }
    free(bound.v);
    return 0;
}

/* ─── The transformation ──────────────────────────────────────────────── */

int magic_transform(const rule *const *ast_rules, int n_ast,
                    const char *goal_pred, uint8_t goal_arity,
                    const uint32_t *leading, uint8_t k,
                    interner *ir, magic_program *out,
                    char *reject_reason, size_t reject_sz)
{
    pa_vec    adorns   = {0, 0, 0};
    edge_vec  edges    = {0, 0, 0};
    rule_vec  modrules = {0, 0, 0};
    decl_vec  decls    = {0, 0, 0};
    int      *queue    = NULL;
    int       goal_idx = -1;
    int       i, j;

    (void)ir;
    (void)leading;

    memset(out, 0, sizeof(*out));
    if (reject_reason && reject_sz > 0) reject_reason[0] = '\0';

#define REJECT(fmt, ...) do {                                              \
        if (reject_reason && reject_sz > 0)                                \
            snprintf(reject_reason, reject_sz, fmt, ##__VA_ARGS__);        \
        goto fail;                                                         \
    } while (0)

    if (!ast_rules || n_ast <= 0) REJECT("no rules loaded");
    if (!goal_pred) REJECT("null goal predicate");
    if (goal_arity == 0 || goal_arity > 8)
        REJECT("goal arity %u out of range 1..8", goal_arity);
    if (k == 0) REJECT("k==0 not supported (route to dl_query)");
    if (k > goal_arity)
        REJECT("k=%u exceeds goal arity %u", k, goal_arity);

    /* ── Collect the IDB head set (with natural arity) ── */
    for (i = 0; i < n_ast; i++) {
        const rule *r = ast_rules[i];
        if (!r || !r->head || !r->head->pred) continue;
        if (pa_push(&adorns, r->head->pred, (uint8_t)r->head->nargs) != 0)
            REJECT("out of memory");
    }

    /* The goal must be an IDB predicate (EDB goals degenerate to prefix). */
    goal_idx = pa_find(&adorns, goal_pred);
    if (goal_idx < 0)
        REJECT("goal '%s' is not a rule head (EDB goal: use prefix lookup)",
               goal_pred);

    make_adornment(adorns.v[goal_idx].adorn, goal_arity, k);
    adorns.v[goal_idx].assigned = 1;
    if (set_pred_names(&adorns.v[goal_idx]) != 0)
        REJECT("adorned predicate name for '%s' too long", goal_pred);

    /* ── Phase A: predicate dependency graph (body IDB -> head) ── */
    for (i = 0; i < n_ast; i++) {
        const rule *r = ast_rules[i];
        int hi;
        if (!r || !r->head || !r->head->pred) continue;
        hi = pa_find(&adorns, r->head->pred);
        for (j = 0; j < r->nbody; j++) {
            const atom *A = r->body[j];
            int bi;
            if (!A || !A->pred) continue;
            if (strcmp(A->pred, "=") == 0) continue;
            bi = pa_find(&adorns, A->pred);
            if (bi >= 0)               /* IDB body atom → edge bi -> hi */
                if (ev_push(&edges, bi, hi) != 0) REJECT("out of memory");
        }
    }

    /* Reject cross-predicate mutual recursion (SCC size > 1). */
    {
        int s = dep_has_multi_node_scc(adorns.n, &edges);
        if (s < 0) REJECT("out of memory");
        if (s > 0)
            REJECT("cross-predicate mutual recursion (dependency cycle "
                   "among IDB predicates) not supported");
    }
    free(edges.v);
    edges.v = NULL;

    /* ── Phase B: worklist adornment propagation ── */
    queue = malloc((size_t)adorns.n * sizeof(int));
    if (!queue) REJECT("out of memory");
    {
        int qh = 0, qt = 0;
        char ppred[64], pal[9];
        queue[qt++] = goal_idx;
        while (qh < qt) {
            int pi = queue[qh++];
            snprintf(ppred, sizeof(ppred), "%s", adorns.v[pi].pred);
            snprintf(pal, sizeof(pal), "%s", adorns.v[pi].adorn);
            for (i = 0; i < n_ast; i++) {
                const rule *R = ast_rules[i];
                if (!R || !R->head || !R->head->pred) continue;
                if (strcmp(R->head->pred, ppred) != 0) continue;
                if (R->has_negation)
                    REJECT("negation in rule for '%s' not supported", ppred);
                if (R->has_aggregate)
                    REJECT("aggregate in rule for '%s' not supported", ppred);
                if (walk_rule_adorns(R, pal, &adorns, queue, &qt,
                                     reject_reason, reject_sz) != 0)
                    goto fail;
            }
        }
    }
    free(queue);
    queue = NULL;

    /* ── MAX_RELS budget: 2 fresh rels per adorned predicate ── */
    {
        int n_adorned = 0;
        for (i = 0; i < adorns.n; i++)
            if (adorns.v[i].assigned) n_adorned++;
        if (2 * n_adorned > MAX_RELS)
            REJECT("adorned closure needs %d relations (2 x %d predicates) "
                   "exceeding MAX_RELS=%d", 2 * n_adorned, n_adorned,
                   MAX_RELS);
    }

    /* ── Phase C: per-(P,alpha) synthesis of all 3 rule classes ── */
    for (i = 0; i < adorns.n; i++) {
        const pred_adorn *P = &adorns.v[i];
        int r;
        if (!P->assigned) continue;

        for (r = 0; r < n_ast; r++) {
            const rule *R = ast_rules[r];
            atom *guard, *mhead;
            atom **mbody;
            rule *mr;
            int total;

            if (!R || !R->head || !R->head->pred) continue;
            if (strcmp(R->head->pred, P->pred) != 0) continue;

            /* (a) adorned rule: P^a :- magic_P^a(bound head), body' */
            guard = build_bound_atom(R->head, P->magic_name, P->adorn);
            if (!guard) REJECT("out of memory");
            total = 1 + R->nbody;
            mbody = calloc((size_t)total, sizeof(atom *));
            if (!mbody) { atom_free_local(guard); REJECT("out of memory"); }
            mbody[0] = guard;
            for (j = 0; j < R->nbody; j++) {
                mbody[1 + j] = atom_copy(R->body[j],
                                         renamed_pred(R->body[j], &adorns));
                if (!mbody[1 + j]) { atoms_free(mbody, 1 + j); REJECT("out of memory"); }
            }
            mhead = atom_copy(R->head, P->adorned_name);
            if (!mhead) { atoms_free(mbody, total); REJECT("out of memory"); }
            mr = make_rule(mhead, mbody, total, 0, 0);
            if (!mr) {
                atom_free_local(mhead);
                atoms_free(mbody, total);
                REJECT("out of memory");
            }
            if (rv_push(&modrules, mr) != 0) {
                rule_free(mr);
                REJECT("out of memory");
            }

            /* (b)/(c) magic rules for each IDB body atom (self-recursion
             * when Q==P, dependency-seed when Q!=P — same shape). */
            for (j = 0; j < R->nbody; j++) {
                const atom *A = R->body[j];
                atom **mgbody;
                atom *mghead;
                rule *mgr;
                int total2, jj, qi;
                const char *target_magic, *target_adorn;

                if (!A || !A->pred) continue;
                qi = pa_find(&adorns, A->pred);
                if (qi < 0) continue;   /* EDB body atom: no magic rule */

                target_magic = adorns.v[qi].magic_name;
                target_adorn = adorns.v[qi].adorn;

                mghead = build_bound_atom(A, target_magic, target_adorn);
                if (!mghead) REJECT("out of memory");

                total2 = 1 + j;   /* magic guard + prefix atoms A0..A_{j-1} */
                mgbody = calloc((size_t)total2, sizeof(atom *));
                if (!mgbody) { atom_free_local(mghead); REJECT("out of memory"); }
                mgbody[0] = build_bound_atom(R->head, P->magic_name, P->adorn);
                if (!mgbody[0]) {
                    atom_free_local(mghead); free(mgbody);
                    REJECT("out of memory");
                }
                for (jj = 0; jj < j; jj++) {
                    mgbody[1 + jj] = atom_copy(R->body[jj],
                                               renamed_pred(R->body[jj], &adorns));
                    if (!mgbody[1 + jj]) {
                        atom_free_local(mghead);
                        atoms_free(mgbody, 1 + jj);
                        REJECT("out of memory");
                    }
                }
                mgr = make_rule(mghead, mgbody, total2, 0, 0);
                if (!mgr) {
                    atom_free_local(mghead);
                    atoms_free(mgbody, total2);
                    REJECT("out of memory");
                }
                if (rv_push(&modrules, mgr) != 0) {
                    rule_free(mgr);
                    REJECT("out of memory");
                }
            }
        }
    }

    /* ── Phase D: decls — 2 per adorned predicate ── */
    for (i = 0; i < adorns.n; i++) {
        const pred_adorn *P = &adorns.v[i];
        uint8_t nb = 0;
        size_t alen;
        if (!P->assigned) continue;
        alen = strlen(P->adorn);
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

    out->rules = modrules.v;
    out->n_rules = modrules.n;
    out->decls = decls.v;
    out->n_decls = decls.n;
    snprintf(out->adorned_goal, sizeof(out->adorned_goal), "%s",
             adorns.v[goal_idx].adorned_name);
    out->goal_arity = goal_arity;

    free(adorns.v);
    return 0;

fail:
    free(queue);
    free(edges.v);
    free(adorns.v);
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
