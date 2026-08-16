/*
 * topdown.c — Top-down / QSQ (query-subquery) SLG-style demand-driven evaluator
 *
 * Runs the magic-sets ADORNED PROGRAM (magic_transform_adorn output) as an
 * SLG worklist over subqueries (variant + bound tuple) instead of as a forward
 * semi-naive fixpoint.  The "magic relation" for a variant is its bound_set
 * (all subquery bound tuples, arity nbound); the "adorned relation" is its
 * memo (all answers, full arity).  Discovery is driven by the MAGIC rules
 * (their heads are child bound tuples); answers are derived by the ADORNED
 * rules with the bound_set as the guard override.
 *
 * Scheduling (correct + efficient, no C recursion):
 *   Phase A (bound queue): a variant whose bound_set grew fires its adorned
 *     rules with the NEW bounds as guard (base answers + late-join against
 *     already-computed child memos) and its magic rules (discovery).
 *   Phase B (prop queue): semi-naive delta propagation — a variant whose
 *     memo grew fires each consumer rule with the WHOLE bound_set as guard
 *     and the DELTA as the changed body atom (delta-join), so each derived
 *     fact is produced once and the chain TC stays Θ(N²).
 *
 * The goal variant's memo is the UNION of all its subquery answers (a
 * self-recursive goal's bound_set can grow past the single user seed), so we
 * stream it via a per-bound-position filter against the user seed rather than
 * treating the memo as the answer verbatim.
 */
#include "topdown.h"
#include "vm.h"
#include "permindex.h"
#include "tupleset.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#define TD_MAX_VARIANTS 64   /* == MAX_ADORN_VARIANTS in magic.c */
#define TD_MAX_OV       32   /* guard + up to 31 IDB body atoms per rule */

/* ─── Per-variant state ────────────────────────────────────────────────── */

typedef struct {
    char    adorned_name[80];
    char    magic_name[86];
    char    adorn[9];          /* e.g. "bf" — length == arity */
    uint8_t arity;             /* full arity */
    uint8_t nbound;            /* count of 'b' */
    uint8_t bound_pos[8];      /* full-arity positions of 'b' (0-based) */

    int    *adorned_rules; int n_adorned; int cap_adorned;
    int    *magic_rules;   int n_magic;   int cap_magic;

    tuple_set bound_set;       /* all subquery bound tuples (arity nbound) */
    tuple_set bound_new;       /* bound tuples not yet processed */
    tuple_set bound_late;      /* bounds discovered during Phase B (need late-join) */
    tuple_set memo;            /* all answers (full arity) */
    tuple_set delta;           /* answers not yet propagated */
    int       memo_sorted;
} td_variant;

/* ─── Per-rule meta (indexed by crules index == prog.rules index) ──────── */

typedef struct {
    int  is_magic;             /* 1 = head is magic_name (discovery rule) */
    int  head_variant;         /* adorned rule: V;  magic rule: target Q */
    int  guard_variant;        /* adorned rule: V;  magic rule: parent P */
    int  has_idb_body;         /* any idb_map[j] != -1 for j >= 1 */
    int  nbody;
    int *idb_map;              /* [nbody]: variant_id for IDB body atom j, or -1 */
    int *op_at;                /* [nbody]: opcode of the relational atom at j */
    int *perm_at;              /* [nbody]: perm_id (OP_LOOKUP_PERM / OP_HASH_JOIN) */
    int *rel_at;               /* [nbody]: rel_id (for dl_db_get_perm) */
} td_rule_meta;

typedef struct { int rule; int body; } td_consumer;
typedef struct { td_consumer *v; int n, cap; } consumer_vec;

struct td_ctx {
    dl_db          *edb;
    compiled_rule **crules;
    int             n_crules;

    td_variant     *variants;
    int             n_variants;
    td_rule_meta   *rmeta;
    consumer_vec   *consumers;

    int    *bound_queue; size_t bound_head, bound_tail, bound_cap;
    int    *prop_queue;  size_t prop_head,  prop_tail,  prop_cap;
    uint8_t *bound_pending;
    uint8_t *prop_pending;
    int     in_phase_b;         /* 1 while draining the prop queue (Phase B) */

    int goal_variant;
};

/* ─── Small helpers ────────────────────────────────────────────────────── */

static int int_vec_push(int **v, int *n, int *cap, int val)
{
    if (*n >= *cap) {
        int nc = *cap ? *cap * 2 : 8;
        int *nv = realloc(*v, (size_t)nc * sizeof(int));
        if (!nv) return -1;
        *v = nv; *cap = nc;
    }
    (*v)[(*n)++] = val;
    return 0;
}

static int consumer_push(consumer_vec *cv, int rule, int body)
{
    if (cv->n >= cv->cap) {
        int nc = cv->cap ? cv->cap * 2 : 8;
        td_consumer *nv = realloc(cv->v, (size_t)nc * sizeof(td_consumer));
        if (!nv) return -1;
        cv->v = nv; cv->cap = nc;
    }
    cv->v[cv->n].rule = rule;
    cv->v[cv->n].body = body;
    cv->n++;
    return 0;
}

static int variant_by_adorned_name(const td_ctx *c, const char *name)
{
    int i;
    for (i = 0; i < c->n_variants; i++)
        if (!strcmp(c->variants[i].adorned_name, name)) return i;
    return -1;
}

static int variant_by_magic_name(const td_ctx *c, const char *name)
{
    int i;
    for (i = 0; i < c->n_variants; i++)
        if (!strcmp(c->variants[i].magic_name, name)) return i;
    return -1;
}

/* ─── Build the variant table from prog.decls ──────────────────────────── */

static int build_variants(td_ctx *c, const magic_program *prog)
{
    int i, j, nb;

    c->n_variants = prog->n_decls / 2;
    if (c->n_variants <= 0 || c->n_variants > TD_MAX_VARIANTS) return -1;
    c->variants = calloc((size_t)c->n_variants, sizeof(td_variant));
    if (!c->variants) return -1;

    for (i = 0; i < c->n_variants; i++) {
        td_variant *v = &c->variants[i];
        const magic_decl *ad = &prog->decls[2 * i];
        const magic_decl *mg = &prog->decls[2 * i + 1];
        size_t alen;

        snprintf(v->adorned_name, sizeof(v->adorned_name), "%s", ad->name);
        snprintf(v->magic_name, sizeof(v->magic_name), "%s", mg->name);
        v->arity  = ad->arity;
        v->nbound = mg->arity;

        /* adorned_name is "<pred>__<adorn>"; the adorn is its last `arity`
         * chars (adorn chars are only 'b'/'f', so this is unambiguous). */
        alen = strlen(v->adorned_name);
        if (alen < v->arity) return -1;
        memcpy(v->adorn, v->adorned_name + alen - v->arity, v->arity);
        v->adorn[v->arity] = '\0';

        nb = 0;
        for (j = 0; j < v->arity; j++)
            if (v->adorn[j] == 'b') v->bound_pos[nb++] = (uint8_t)j;
        if (nb != v->nbound) return -1;   /* cross-check decls vs adorn */

        if (ts_init(&v->bound_set, v->nbound) != 0) return -1;
        if (ts_init(&v->bound_new, v->nbound) != 0) return -1;
        if (ts_init(&v->bound_late, v->nbound) != 0) return -1;
        if (ts_init(&v->memo, v->arity) != 0) return -1;
        if (ts_init(&v->delta, v->arity) != 0) return -1;
        v->memo_sorted = 1;
    }
    return 0;
}

/* ─── Build per-rule meta + variant rule lists ─────────────────────────── */

static int append_rule_idx(td_ctx *c, int variant, int ri, int adorned)
{
    td_variant *v = &c->variants[variant];
    if (adorned)
        return int_vec_push(&v->adorned_rules, &v->n_adorned, &v->cap_adorned, ri);
    return int_vec_push(&v->magic_rules, &v->n_magic, &v->cap_magic, ri);
}

static int build_rule_meta(td_ctx *c, const magic_program *prog)
{
    int i, j;

    c->rmeta = calloc((size_t)c->n_crules, sizeof(td_rule_meta));
    if (!c->rmeta) return -1;

    for (i = 0; i < c->n_crules; i++) {
        rule *r = prog->rules[i];
        td_rule_meta *m = &c->rmeta[i];
        int nbody = r->nbody > 0 ? r->nbody : 1;
        int hv;

        m->nbody = r->nbody;
        m->idb_map = calloc((size_t)nbody, sizeof(int));
        m->op_at   = calloc((size_t)nbody, sizeof(int));
        m->perm_at = calloc((size_t)nbody, sizeof(int));
        m->rel_at  = calloc((size_t)nbody, sizeof(int));
        if (!m->idb_map || !m->op_at || !m->perm_at || !m->rel_at) return -1;

        hv = variant_by_adorned_name(c, r->head->pred);
        if (hv >= 0) {
            /* adorned rule: P^a :- magic_P^a(bound), body'. */
            m->is_magic      = 0;
            m->head_variant  = hv;
            m->guard_variant = hv;
            if (append_rule_idx(c, hv, i, 1) != 0) return -1;
        } else {
            int qv = variant_by_magic_name(c, r->head->pred);
            if (qv < 0) return -1;   /* unknown head predicate */
            /* magic rule: magic_Q^b :- magic_P^a(bound), prefix. */
            m->is_magic = 1;
            m->head_variant = qv;
            if (r->nbody < 1) return -1;
            m->guard_variant = variant_by_magic_name(c, r->body[0]->pred);
            if (m->guard_variant < 0) return -1;
            if (append_rule_idx(c, m->guard_variant, i, 0) != 0) return -1;
        }

        m->has_idb_body = 0;
        for (j = 0; j < r->nbody; j++) {
            m->idb_map[j] = -1;
            m->op_at[j]   = -1;
            m->perm_at[j] = -1;
            m->rel_at[j]  = -1;
            if (j == 0) continue;   /* magic guard (body 0) — handled specially */

            m->idb_map[j] = variant_by_adorned_name(c, r->body[j]->pred);
            if (m->idb_map[j] >= 0) m->has_idb_body = 1;

            /* find the relational opcode emitted for this body atom */
            {
                compiled_rule *cr = c->crules[i];
                int k;
                for (k = 0; k < cr->n_instrs; k++) {
                    const vm_instr *in = &cr->instrs[k];
                    if ((int)in->body_idx != j) continue;
                    if (in->op == OP_SCAN || in->op == OP_LOOKUP ||
                        in->op == OP_LOOKUP_PERM || in->op == OP_HASH_JOIN ||
                        in->op == OP_WALK) {
                        m->op_at[j] = in->op;
                        m->rel_at[j] = in->a;
                        /* OP_HASH_JOIN's imm is now a PACKED permutation, not a
                         * perm_id, so it must never be read back as one. */
                        m->perm_at[j] = (in->op == OP_LOOKUP_PERM)
                            ? (int)in->imm : -1;
                        break;
                    }
                }
            }
        }
    }
    return 0;
}

/* ─── Build reverse consumer lists ─────────────────────────────────────── */

static int build_consumers(td_ctx *c)
{
    int i, j;

    c->consumers = calloc((size_t)c->n_variants, sizeof(consumer_vec));
    if (!c->consumers) return -1;
    for (i = 0; i < c->n_crules; i++) {
        td_rule_meta *m = &c->rmeta[i];
        for (j = 1; j < m->nbody; j++) {
            int Q = m->idb_map[j];
            if (Q < 0) continue;
            if (consumer_push(&c->consumers[Q], i, j) != 0) return -1;
        }
    }
    return 0;
}

/* ─── Queues (dedup via pending flags) ─────────────────────────────────── */

static int enqueue_bound(td_ctx *c, int V)
{
    if (c->bound_pending[V]) return 0;
    if (c->bound_tail >= c->bound_cap) {
        size_t nc = c->bound_cap ? c->bound_cap * 2 : 16;
        int *nv = realloc(c->bound_queue, nc * sizeof(int));
        if (!nv) return -1;
        c->bound_queue = nv; c->bound_cap = nc;
    }
    c->bound_queue[c->bound_tail++] = V;
    c->bound_pending[V] = 1;
    return 0;
}

static int enqueue_prop(td_ctx *c, int V)
{
    if (c->prop_pending[V]) return 0;
    if (c->prop_tail >= c->prop_cap) {
        size_t nc = c->prop_cap ? c->prop_cap * 2 : 16;
        int *nv = realloc(c->prop_queue, nc * sizeof(int));
        if (!nv) return -1;
        c->prop_queue = nv; c->prop_cap = nc;
    }
    c->prop_queue[c->prop_tail++] = V;
    c->prop_pending[V] = 1;
    return 0;
}

/* ─── Tuple collector + perm shadow ────────────────────────────────────── */

typedef struct { tuple_set *ts; int err; } collect_ctx;

static int collect_cb(const uint32_t *cols, uint8_t arity, void *user)
{
    collect_ctx *cx = (collect_ctx *)user;
    if (cx->ts->arity == 0) {
        if (ts_init(cx->ts, arity) != 0) { cx->err = 1; return 1; }
    }
    if (ts_add(cx->ts, cols) < 0) { cx->err = 1; return 1; }
    return 0;
}

/* Build the permuted shadow of `src` under `perm` (position p -> original
 * column perm[p]), sorted in permuted order (what OP_LOOKUP_PERM's override
 * path expects).  `out` must be zero-initialised. */
static int build_perm_shadow(const tuple_set *src, const uint8_t *perm,
                             uint8_t arity, tuple_set *out)
{
    long i;
    int p;
    uint32_t row[8];

    if (src->count == 0) return 0;   /* leave out empty (arity 0) */
    if (ts_init(out, arity) != 0) return -1;
    for (i = 0; i < src->count; i++) {
        const uint32_t *r = src->data + (size_t)i * arity;
        for (p = 0; p < arity; p++) row[p] = r[perm[p]];
        if (ts_add(out, row) < 0) { ts_free(out); return -1; }
    }
    ts_sort(out);
    return 0;
}

/* ─── Fire one compiled rule with overrides ────────────────────────────── */

/* Fire rule `ri` with body atom 0 (the magic guard) overridden by `guard_ts`
 * (enumerated in full by OP_SCAN), the IDB body atom `delta_body` (if >= 0)
 * overridden by `delta_ts`, and every other IDB body atom overridden by its
 * variant memo (full).  Collects head tuples into `out`.  Returns 0/-1. */
static int fire_rule(td_ctx *c, int ri, const tuple_set *guard_ts,
                     int delta_body, const tuple_set *delta_ts, tuple_set *out)
{
    td_rule_meta *m = &c->rmeta[ri];
    compiled_rule *cr = c->crules[ri];
    vm_override ov[TD_MAX_OV];
    tuple_set shadows[TD_MAX_OV];
    int shadow_used[TD_MAX_OV];
    int n_ov = 0, j;
    collect_ctx cx;
    long rc;

    if (m->nbody + 1 > TD_MAX_OV) return -1;

    memset(shadow_used, 0, sizeof(shadow_used));
    memset(&cx, 0, sizeof(cx));
    cx.ts = out;

    /* body 0 = magic guard → OP_SCAN over the bound set */
    ov[n_ov].body_idx = 0;
    ov[n_ov].ts = guard_ts;
    ov[n_ov].perm_id = -1;
    n_ov++;

    for (j = 1; j < m->nbody; j++) {
        int Q = m->idb_map[j];
        const tuple_set *src;
        int op;
        if (Q < 0) continue;

        src = (delta_body == j) ? delta_ts : &c->variants[Q].memo;
        if (src->count == 0) continue;   /* empty → DAFSA read (empty rel) */

        op = m->op_at[j];
        if (op == OP_LOOKUP_PERM) {
            const uint8_t *perm =
                dl_db_get_perm(c->edb, m->rel_at[j], m->perm_at[j]);
            tuple_set *sh;
            if (!perm || src->arity > 8) return -1;
            sh = &shadows[n_ov];
            if (build_perm_shadow(src, perm, src->arity, sh) != 0) {
                int t;
                for (t = 0; t < n_ov; t++)
                    if (shadow_used[t]) ts_free(&shadows[t]);
                return -1;
            }
            shadow_used[n_ov] = 1;
            ov[n_ov].body_idx = j;
            ov[n_ov].ts = sh;
            ov[n_ov].perm_id = -1;
            n_ov++;
        } else {
            /* OP_SCAN / OP_LOOKUP / OP_HASH_JOIN.  Only OP_LOOKUP reads via
             * ts_prefix and therefore needs the source SORTED. */
            if (op == OP_LOOKUP && src == &c->variants[Q].memo &&
                !c->variants[Q].memo_sorted) {
                ts_sort(&c->variants[Q].memo);
                c->variants[Q].memo_sorted = 1;
            }
            ov[n_ov].body_idx = j;
            ov[n_ov].ts = src;
            ov[n_ov].perm_id = -1;
            n_ov++;
        }
    }

    rc = vm_exec_rule(c->edb, cr, ov, n_ov, 1 /* dry */, collect_cb, &cx);

    {
        int t;
        for (t = 0; t < n_ov; t++)
            if (shadow_used[t]) ts_free(&shadows[t]);
    }
    if (cx.err || rc < 0) return -1;
    return 0;
}

/* ─── Memo / bound bookkeeping ─────────────────────────────────────────── */

static int merge_into(td_ctx *c, int V, const tuple_set *out)
{
    td_variant *var = &c->variants[V];
    long i;
    int added = 0;

    if (out->arity == 0 || out->count == 0) return 0;
    for (i = 0; i < out->count; i++) {
        const uint32_t *t = out->data + (size_t)i * out->arity;
        int r = ts_add(&var->memo, t);
        if (r < 0) return -1;
        if (r == 1) {
            added = 1;
            if (ts_add(&var->delta, t) < 0) return -1;
        }
    }
    if (added) var->memo_sorted = 0;
    if (var->delta.count > 0) {
        if (enqueue_prop(c, V) != 0) return -1;
    }
    return 0;
}

static int add_bound(td_ctx *c, int Q, const uint32_t *t)
{
    td_variant *var = &c->variants[Q];
    int r = ts_add(&var->bound_set, t);
    if (r < 0) return -1;
    if (r == 1) {
        if (ts_add(&var->bound_new, t) < 0) return -1;
        if (c->in_phase_b && ts_add(&var->bound_late, t) < 0) return -1;
        if (enqueue_bound(c, Q) != 0) return -1;
    }
    return 0;
}

/* ─── Phase A: process new bounds (base answers + late join + discovery) ── */

static int init_variant(td_ctx *c, int V)
{
    td_variant *var = &c->variants[V];
    tuple_set nb, nl;
    int a, mi;

    if (var->bound_new.count == 0) return 0;

    nb = var->bound_new;                     /* take ownership */
    memset(&var->bound_new, 0, sizeof(var->bound_new));
    var->bound_new.arity = var->nbound;      /* keep arity: ts_add re-allocs data */
    nl = var->bound_late;                    /* take ownership (Phase-B bounds) */
    memset(&var->bound_late, 0, sizeof(var->bound_late));
    var->bound_late.arity = var->nbound;

    /* Answer: fire only the BASE adorned rules (no IDB body atoms) with
     * guard = new bounds.  Recursive/dependency rules are derived entirely by
     * Phase-B semi-naive delta propagation (a delta-join never reads the full
     * memo for a single-IDB-atom rule), which keeps the chain TC Θ(N²) —
     * firing them here with full memos would re-sort the memo once per new
     * bound (Θ(N² log N) total) for no derived tuples. */
    for (a = 0; a < var->n_adorned; a++) {
        int ri = var->adorned_rules[a];
        tuple_set out;
        if (c->rmeta[ri].has_idb_body) continue;
        memset(&out, 0, sizeof(out));
        if (fire_rule(c, ri, &nb, -1, NULL, &out) != 0) {
            ts_free(&out); ts_free(&nl); ts_free(&nb); return -1;
        }
        if (merge_into(c, V, &out) != 0) { ts_free(&out); ts_free(&nl); ts_free(&nb); return -1; }
        ts_free(&out);
    }

    /* Late-join: bounds discovered during Phase B may have had their child
     * memos already populated (deltas consumed) — fire the recursive/dependency
     * rules for THOSE bounds with full child memos so they see existing answers.
     * (Early Phase-A bounds get their recursive answers via delta propagation,
     * since their children are computed later.) */
    if (nl.count > 0) {
        for (a = 0; a < var->n_adorned; a++) {
            int ri = var->adorned_rules[a];
            tuple_set out;
            if (!c->rmeta[ri].has_idb_body) continue;
            memset(&out, 0, sizeof(out));
            if (fire_rule(c, ri, &nl, -1, NULL, &out) != 0) {
                ts_free(&out); ts_free(&nl); ts_free(&nb); return -1;
            }
            if (merge_into(c, V, &out) != 0) { ts_free(&out); ts_free(&nl); ts_free(&nb); return -1; }
            ts_free(&out);
        }
    }

    /* Discovery: fire every magic rule (parent V) with guard = new bounds. */
    for (mi = 0; mi < var->n_magic; mi++) {
        int ri = var->magic_rules[mi];
        int Q = c->rmeta[ri].head_variant;
        tuple_set out;
        long i;
        memset(&out, 0, sizeof(out));
        if (fire_rule(c, ri, &nb, -1, NULL, &out) != 0) {
            ts_free(&out); ts_free(&nb); return -1;
        }
        for (i = 0; i < out.count; i++) {
            const uint32_t *t = out.data + (size_t)i * out.arity;
            if (add_bound(c, Q, t) != 0) { ts_free(&out); ts_free(&nb); return -1; }
        }
        ts_free(&out);
    }

    ts_free(&nl);
    ts_free(&nb);
    return 0;
}

/* ─── Phase B: semi-naive delta propagation ────────────────────────────── */

static int propagate_delta(td_ctx *c, int V)
{
    td_variant *var = &c->variants[V];
    tuple_set cur;
    consumer_vec *cv;
    int k;

    if (var->delta.count == 0) return 0;

    cur = var->delta;                          /* take ownership */
    memset(&var->delta, 0, sizeof(var->delta));
    var->delta.arity = var->arity;             /* keep arity: ts_add re-allocs data */
    if (cur.arity > 0) ts_sort(&cur);

    cv = &c->consumers[V];
    for (k = 0; k < cv->n; k++) {
        int ri = cv->v[k].rule;
        int body = cv->v[k].body;
        td_rule_meta *m = &c->rmeta[ri];
        int parent = m->is_magic ? m->guard_variant : m->head_variant;
        td_variant *pvar = &c->variants[parent];
        tuple_set out;
        long i;

        if (pvar->bound_set.count == 0) continue;

        memset(&out, 0, sizeof(out));
        if (fire_rule(c, ri, &pvar->bound_set, body, &cur, &out) != 0) {
            ts_free(&out); ts_free(&cur); return -1;
        }

        if (m->is_magic) {
            int Q = m->head_variant;
            for (i = 0; i < out.count; i++) {
                const uint32_t *t = out.data + (size_t)i * out.arity;
                if (add_bound(c, Q, t) != 0) { ts_free(&out); ts_free(&cur); return -1; }
            }
        } else {
            if (merge_into(c, parent, &out) != 0) { ts_free(&out); ts_free(&cur); return -1; }
        }
        ts_free(&out);
    }

    ts_free(&cur);

    /* self-recursion may have re-grown delta[V] during this pass */
    if (var->delta.count > 0) {
        if (enqueue_prop(c, V) != 0) return -1;
    }
    return 0;
}

/* ─── Driver loop ──────────────────────────────────────────────────────── */

static int td_run(td_ctx *c)
{
    for (;;) {
        /* Phase A: drain the bound queue (discovery + base + late join). */
        while (c->bound_head < c->bound_tail) {
            int V = c->bound_queue[c->bound_head++];
            c->bound_pending[V] = 0;
            if (init_variant(c, V) != 0) return -1;
        }
        /* Phase B: drain the prop queue (semi-naive delta propagation). */
        while (c->prop_head < c->prop_tail) {
            int V = c->prop_queue[c->prop_head++];
            c->prop_pending[V] = 0;
            c->in_phase_b = 1;
            if (propagate_delta(c, V) != 0) { c->in_phase_b = 0; return -1; }
            c->in_phase_b = 0;
        }
        if (c->bound_head >= c->bound_tail && c->prop_head >= c->prop_tail)
            break;
    }
    return 0;
}

/* ─── Cleanup ──────────────────────────────────────────────────────────── */

static void td_free(td_ctx *c)
{
    int i;
    if (c->variants) {
        for (i = 0; i < c->n_variants; i++) {
            td_variant *v = &c->variants[i];
            free(v->adorned_rules);
            free(v->magic_rules);
            ts_free(&v->bound_set);
            ts_free(&v->bound_new);
            ts_free(&v->bound_late);
            ts_free(&v->memo);
            ts_free(&v->delta);
        }
        free(c->variants);
    }
    if (c->rmeta) {
        for (i = 0; i < c->n_crules; i++) {
            free(c->rmeta[i].idb_map);
            free(c->rmeta[i].op_at);
            free(c->rmeta[i].perm_at);
            free(c->rmeta[i].rel_at);
        }
        free(c->rmeta);
    }
    if (c->consumers) {
        for (i = 0; i < c->n_variants; i++) free(c->consumers[i].v);
        free(c->consumers);
    }
    free(c->bound_queue);
    free(c->prop_queue);
    free(c->bound_pending);
    free(c->prop_pending);
}

/* ─── Public entry ─────────────────────────────────────────────────────── */

long td_eval(dl_db *edb, const magic_program *prog,
             compiled_rule **crules, int n_crules,
             int goal_variant_id, const uint32_t *bound,
             dl_tuple_cb cb, void *user)
{
    td_ctx c;
    long result = -1;
    td_variant *gv;

    memset(&c, 0, sizeof(c));
    c.edb = edb;
    c.crules = crules;
    c.n_crules = n_crules;
    c.goal_variant = goal_variant_id;

    if (goal_variant_id < 0 || goal_variant_id >= prog->n_decls / 2) goto out;
    if (!bound || !cb) goto out;

    if (build_variants(&c, prog) != 0) goto out;
    if (build_rule_meta(&c, prog) != 0) goto out;
    if (build_consumers(&c) != 0) goto out;

    c.bound_pending = calloc((size_t)c.n_variants, 1);
    c.prop_pending  = calloc((size_t)c.n_variants, 1);
    if (!c.bound_pending || !c.prop_pending) goto out;

    /* seed the goal subquery */
    if (add_bound(&c, goal_variant_id, bound) != 0) goto out;

    if (td_run(&c) != 0) goto out;

    /* stream the goal variant's memo (all its tuples match the single seed
     * bound; the per-position check is a defensive no-op) */
    gv = &c.variants[goal_variant_id];
    if (!gv->memo_sorted) { ts_sort(&gv->memo); gv->memo_sorted = 1; }
    result = 0;
    if (gv->memo.arity > 0) {
        long i;
        for (i = 0; i < gv->memo.count; i++) {
            const uint32_t *t = gv->memo.data + (size_t)i * gv->memo.arity;
            int p, ok = 1;
            for (p = 0; p < gv->nbound; p++)
                if (t[gv->bound_pos[p]] != bound[p]) { ok = 0; break; }
            if (!ok) continue;
            if (cb(t, gv->memo.arity, user)) break;
            result++;
        }
    }

out:
    td_free(&c);
    return result;
}
