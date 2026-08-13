/*
 * magic.c — Magic-sets AST→AST adornment rewrite (M8 v2 first slice)
 *
 * Pure transformation: no dl_db access.  Reads the retained AST (a const
 * array of rule*) and synthesizes a fresh adorned + magic program.
 *
 * The synthesized AST is built with the SAME allocation discipline as
 * parser.c (calloc'd atom/rule, strdup'd pred/text, one token* per arg), so
 * the program can be freed with rule_free() (parser.h) and compiled by the
 * untouched compile_rules().
 *
 * First-slice conservative REJECTS (never silently mis-evaluate):
 *   - negation in any goal-headed rule
 *   - aggregate in any goal-headed rule
 *   - k == 0 (caller routes to dl_query)
 *   - a body atom that names a DIFFERENT IDB predicate (multi-predicate
 *     dependency / mutual-recursion closure)
 *   - a recursive call to the goal whose SIPS adornment differs from the
 *     head adornment (covers non-leading adornments)
 */
#include "magic.h"
#include "intern.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ─── Growable buffers ────────────────────────────────────────────────── */

typedef struct { rule **v; int n, cap; } rule_vec;
typedef struct { magic_decl *v; int n, cap; } decl_vec;
typedef struct { char **v; int n, cap; } str_vec;       /* owned strings */
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

static int sv_contains(const str_vec *v, const char *name)
{
    int i;
    for (i = 0; i < v->n; i++)
        if (strcmp(v->v[i], name) == 0) return 1;
    return 0;
}

static int sv_push(str_vec *v, const char *name)
{
    char *dup;
    if (sv_contains(v, name)) return 0;
    dup = strdup(name);
    if (!dup) return -1;
    if (v->n >= v->cap) {
        int nc = v->cap ? v->cap * 2 : 8;
        char **nv = realloc(v->v, (size_t)nc * sizeof(char *));
        if (!nv) { free(dup); return -1; }
        v->v = nv; v->cap = nc;
    }
    v->v[v->n++] = dup;
    return 0;
}

static void sv_free(str_vec *v)
{
    int i;
    for (i = 0; i < v->n; i++) free(v->v[i]);
    free(v->v);
    v->v = NULL; v->n = v->cap = 0;
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

/* ─── The transformation ──────────────────────────────────────────────── */

int magic_transform(const rule *const *ast_rules, int n_ast,
                    const char *goal_pred, uint8_t goal_arity,
                    const uint32_t *leading, uint8_t k,
                    interner *ir, magic_program *out,
                    char *reject_reason, size_t reject_sz)
{
    char adorn[9];
    char adorned_name[64];
    char magic_name[64];
    str_vec heads = {0, 0, 0};
    rule_vec modrules = {0, 0, 0};
    decl_vec decls = {0, 0, 0};
    int i, j;

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

    make_adornment(adorn, goal_arity, k);

    if (snprintf(adorned_name, sizeof(adorned_name), "%s__%s",
                 goal_pred, adorn) >= (int)sizeof(adorned_name))
        REJECT("adorned predicate name for '%s' too long", goal_pred);
    if (snprintf(magic_name, sizeof(magic_name), "magic_%s__%s",
                 goal_pred, adorn) >= (int)sizeof(magic_name))
        REJECT("magic predicate name for '%s' too long", goal_pred);

    /* Collect the IDB head set (predicates that appear as a rule head). */
    for (i = 0; i < n_ast; i++) {
        const rule *r = ast_rules[i];
        if (!r || !r->head || !r->head->pred) continue;
        if (sv_push(&heads, r->head->pred) != 0) REJECT("out of memory");
    }

    /* The goal must be an IDB predicate (EDB goals degenerate to prefix). */
    if (!sv_contains(&heads, goal_pred))
        REJECT("goal '%s' is not a rule head (EDB goal: use prefix lookup)",
               goal_pred);

    /* One pass over the goal-headed rules: validate, then synthesize. */
    for (i = 0; i < n_ast; i++) {
        const rule *R = ast_rules[i];
        name_set bound = {0, 0, 0};
        atom *guard, *mhead;
        atom **mbody;
        rule *mr;
        int total;

        if (!R || !R->head || !R->head->pred) continue;
        if (strcmp(R->head->pred, goal_pred) != 0) continue; /* other heads */

        if (R->has_negation)
            REJECT("negation in rule for '%s' not supported", goal_pred);
        if (R->has_aggregate)
            REJECT("aggregate in rule for '%s' not supported", goal_pred);

        /* ── Analysis: left-to-right SIPS bound-set + adornment checks ── */
        for (j = 0; j < R->head->nargs; j++)
            if (adorn[j] == 'b' && R->head->args[j]->kind == TOK_VAR)
                ns_add(&bound, R->head->args[j]->text);

        for (j = 0; j < R->nbody; j++) {
            const atom *A = R->body[j];
            char beta[9];
            int a;
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
                free(bound.v);
                REJECT("aggregate in rule for '%s' not supported", goal_pred);
            }

            if (sv_contains(&heads, A->pred)) {
                /* IDB body atom: single-predicate slice → must be goal. */
                if (strcmp(A->pred, goal_pred) != 0) {
                    free(bound.v);
                    REJECT("multi-predicate dependency '%s' in rule for "
                           "'%s' not supported", A->pred, goal_pred);
                }
                atom_adornment(A, &bound, beta);
                if (strcmp(beta, adorn) != 0) {
                    free(bound.v);
                    REJECT("recursive call to '%s' needs adornment '%s' "
                           "(head is '%s') not supported",
                           goal_pred, beta, adorn);
                }
            }

            for (a = 0; a < A->nargs; a++)
                if (A->args[a]->kind == TOK_VAR)
                    ns_add(&bound, A->args[a]->text);
        }
        free(bound.v);

        /* ── Build: modified rule H^a :- magic_H^a(bound head), body' ── */
        guard = build_bound_atom(R->head, magic_name, adorn);
        if (!guard) REJECT("out of memory");

        total = 1 + R->nbody;
        mbody = calloc((size_t)total, sizeof(atom *));
        if (!mbody) { atom_free_local(guard); REJECT("out of memory"); }
        mbody[0] = guard;
        for (j = 0; j < R->nbody; j++) {
            const atom *A = R->body[j];
            const char *np = (A->pred && strcmp(A->pred, goal_pred) == 0)
                             ? adorned_name : A->pred;
            mbody[1 + j] = atom_copy(A, np);
            if (!mbody[1 + j]) { atoms_free(mbody, 1 + j); REJECT("out of memory"); }
        }
        mhead = atom_copy(R->head, adorned_name);
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

        /* ── Build: magic rules for each recursive (goal) body atom ── */
        for (j = 0; j < R->nbody; j++) {
            const atom *A = R->body[j];
            atom **mgbody;
            atom *mghead;
            rule *mgr;
            int total2, jj;

            if (!(A->pred && strcmp(A->pred, goal_pred) == 0)) continue;

            mghead = build_bound_atom(A, magic_name, adorn);
            if (!mghead) REJECT("out of memory");

            total2 = 1 + j;  /* magic guard + prefix atoms A0..A_{j-1} */
            mgbody = calloc((size_t)total2, sizeof(atom *));
            if (!mgbody) { atom_free_local(mghead); REJECT("out of memory"); }
            mgbody[0] = build_bound_atom(R->head, magic_name, adorn);
            if (!mgbody[0]) {
                atom_free_local(mghead); free(mgbody);
                REJECT("out of memory");
            }
            for (jj = 0; jj < j; jj++) {
                const atom *B = R->body[jj];
                const char *np = (B->pred && strcmp(B->pred, goal_pred) == 0)
                                 ? adorned_name : B->pred;
                mgbody[1 + jj] = atom_copy(B, np);
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

    /* ── Decls: adorned goal + magic goal (always both) ─────────────── */
    if (dv_push(&decls, adorned_name, goal_arity) != 0) REJECT("out of memory");
    if (dv_push(&decls, magic_name, k) != 0) REJECT("out of memory");

    out->rules = modrules.v;
    out->n_rules = modrules.n;
    out->decls = decls.v;
    out->n_decls = decls.n;
    snprintf(out->adorned_goal, sizeof(out->adorned_goal), "%s", adorned_name);
    out->goal_arity = goal_arity;

    sv_free(&heads);
    return 0;

fail:
    sv_free(&heads);
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
