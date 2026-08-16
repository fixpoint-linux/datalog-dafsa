/*
 * dafsa.c -- Carrasco & Forcada (2002) Incremental Minimal Acyclic DFA
 *
 * Implements incremental addition and deletion of strings from a minimal
 * deterministic acyclic finite-state automaton (DAFSA) using the
 * clone-on-write + register + confluence algorithm described in:
 *
 *   Carrasco, R.C. & Forcada, M.L. (2002)
 *   "Incremental Construction and Maintenance of Minimal
 *    Acyclic Finite-State Automata"
 *   Computational Linguistics, 28(2), pp. 207-216.
 *
 * Refactored from dawg.c: heap-allocated/growable arrays, opaque handle,
 * length-delimited key API.
 */
#include "dafsa_internal.h"

/* ─── Prime helpers (for register growth) ──────────────────────────────── */

int is_prime(size_t n)
{
    size_t d;
    if (n < 2) return 0;
    if (n % 2 == 0) return n == 2;
    for (d = 3; d * d <= n; d += 2) {
        if (n % d == 0) return 0;
    }
    return 1;
}

size_t next_prime(size_t n)
{
    if (n < 2) return 2;
    if (n % 2 == 0) n++;
    while (!is_prime(n))
        n += 2;
    return n;
}

/* ─── Lifecycle ────────────────────────────────────────────────────────── */

dafsa *dafsa_create(void)
{
    dafsa *d;

    d = calloc(1, sizeof(*d));
    if (!d) return NULL;

    d->states_cap = 4096;
    d->states = calloc(d->states_cap, sizeof(State));
    if (!d->states) goto fail;

    d->inodes_cap = 4096;
    d->inodes = calloc(d->inodes_cap, sizeof(Inode));
    if (!d->inodes) goto fail;

    d->reg_cap = 4093;   /* prime */
    d->reg_keys = calloc(d->reg_cap, sizeof(uint64_t));
    if (!d->reg_keys) goto fail;
    d->reg_vals = calloc(d->reg_cap, sizeof(uint32_t));
    if (!d->reg_vals) goto fail;

    d->nstates = 1;            /* state 0 is "no state" sentinel */
    d->initial = state_new(d);  /* state 1 is the initial state */
    d->inodes_used = 0;
    d->reg_used = 0;
    d->reg_probes = 0;

    return d;

fail:
    dafsa_free(d);
    return NULL;
}

void dafsa_free(dafsa *d)
{
    if (!d) return;
    free(d->spath);
    free(d->schars);
    free(d->sparents);
    if (d->states) {
        unsigned int i;
        for (i = 0; i < d->nstates; i++)
            free(d->states[i].trans_heap);
    }
    free(d->states);
    free(d->inodes);
    free(d->reg_keys);
    free(d->reg_vals);
    free(d->subtree);
    free(d);
}

/* ─── ABI version probe ──────────────────────────────────────────────────── */

uint32_t dafsa_abi_version(void)
{
    return DAFSA_ABI_VERSION;
}

/* ─── Statistics ───────────────────────────────────────────────────────── */

void dafsa_stats(const dafsa *d, dafsa_stats_out *out)
{
    unsigned char *visited;
    unsigned int *queue;
    unsigned int head, tail;
    unsigned int reachable, finals, transitions;

    if (!d || !out) return;

    /* No lazy cache: compute fresh on every call.  The struct is never
     * mutated here, so dafsa_stats is const-correct and safe to call
     * concurrently on a shared read-only dafsa handle (search views are
     * shareable; this function reads only states[]/trans[]/is_final which
     * are immutable after load).  register_probes is read once below.
     *
     * Recomputing costs one BFS (O(nstates + ntrans)); dafsa_stats is a
     * diagnostic API, not a hot path, so the lost caching is acceptable
     * in exchange for dropping the const-cast data race. */
    visited = (unsigned char *)calloc(d->nstates, 1);
    queue   = (unsigned int *)malloc(d->nstates * sizeof(unsigned int));
    if (!visited || !queue) {
        free(visited);
        free(queue);
        memset(out, 0, sizeof(*out));   /* OOM: degrade to zeros */
        return;
    }

    head = tail = 0;
    reachable = finals = transitions = 0;
    queue[tail++] = d->initial;
    visited[d->initial] = 1;

    while (head < tail) {
        unsigned int sid = queue[head++];
        const State *s = &d->states[sid];
        unsigned int j;

        reachable++;
        if (s->is_final) finals++;
        transitions += s->ntrans;
        for (j = 0; j < s->ntrans; j++) {
            unsigned int tgt = trans_arr_c(s)[j].target;
            if (!visited[tgt]) {
                visited[tgt] = 1;
                queue[tail++] = tgt;
            }
        }
    }

    out->n_states_total     = d->nstates - 1;   /* exclude sink 0 */
    out->n_states_reachable = reachable;
    out->n_final            = finals;
    out->n_trans            = transitions;
    out->register_probes    = d->reg_probes;

    free(visited);
    free(queue);
}

/* ─── Dot output for visualization ─────────────────────────────────────── */

void dafsa_dot(const dafsa *d, FILE *f)
{
    unsigned int i;

    if (!d || !f) return;

    fprintf(f, "digraph DAFSA {\n");
    fprintf(f, "  rankdir=LR;\n");
    fprintf(f, "  node [shape=circle,fontsize=10];\n");
    /* mark the initial state */
    fprintf(f, "  start [shape=point];\n");
    fprintf(f, "  start -> %u;\n", d->initial);

    for (i = 1; i < d->nstates; i++) {
        const State *s = &d->states[i];
        const char *shape;
        unsigned int j;

        if (s->refcount == 0 && i != d->initial) continue; /* skip orphans */

        shape = s->is_final ? "doublecircle" : "circle";
        fprintf(f, "  %u [shape=%s,label=\"%u (rc=%u)\"];\n",
                i, shape, i, s->refcount);

        for (j = 0; j < s->ntrans; j++) {
            const Edge *e = &trans_arr_c(s)[j];
            fprintf(f, "  %u -> %u [label=\"%c\"];\n",
                    i, e->target,
                    e->sym >= 32 && e->sym < 127
                        ? e->sym : '?');
        }
    }
    fprintf(f, "}\n");
}

/* ─── DAFSA_DEBUG invariant checker ─────────────────────────────────────── */

#ifdef DAFSA_DEBUG
void dafsa_check_invariants(const dafsa *d)
{
    unsigned int i;
    unsigned char *visited;
    unsigned int *queue;
    unsigned int head, tail;

    /* Allocate BFS workspace */
    visited = (unsigned char *)calloc(d->nstates, 1);
    queue   = (unsigned int *)malloc(d->nstates * sizeof(unsigned int));
    if (!visited || !queue) {
        free(visited);
        free(queue);
        return;  /* cannot check; skip gracefully */
    }

    /* BFS from initial to find reachable states */
    head = 0; tail = 0;
    queue[tail++] = d->initial;
    visited[d->initial] = 1;

    while (head < tail) {
        unsigned int sid = queue[head++];
        const State *s = &d->states[sid];

        /* (a) no orphan reachable from initial */
        assert(s->refcount > 0 || sid == d->initial);

        /* (b) ntrans matches the actual trans[] entries — trans[] is
         *     sorted by sym and has no duplicate sym */
        for (i = 0; i < s->ntrans; i++) {
            if (i > 0)
                assert(trans_arr_c(s)[i - 1].sym < trans_arr_c(s)[i].sym);
        }

        /* (c) refcount == number of inodes pointing at this state */
        {
            unsigned int ni = s->in_head;
            unsigned int cnt = 0;
            while (ni != 0) {
                cnt++;
                ni = d->inodes[ni].next;
            }
            assert(cnt == s->refcount);
        }

        /* enqueue children */
        for (i = 0; i < s->ntrans; i++) {
            unsigned int tgt = trans_arr_c(s)[i].target;
            if (!visited[tgt]) {
                visited[tgt] = 1;
                queue[tail++] = tgt;
            }
        }
    }

    /* (d) register: for each reachable state, reg_lookup must return
     *     either this state or a valid equivalent (matching sig, live).
     *     Stale entries pointing to dead states (refcount==0, id!=initial)
     *     are a known benign artefact of clone-on-write — they are
     *     validated away in replace_or_register. */
    for (i = 1; i < d->nstates; i++) {
        const State *s = &d->states[i];
        unsigned int eq;
        if (s->refcount == 0 && i != d->initial) continue; /* dead slot */
        /* Only check states that have a valid signature (transitions
         * or is_final).  Freshly-created states with sig==0 are dirty. */
        if (s->sig == 0) continue;
        eq = reg_lookup_no_count(d, s->sig);  /* does not perturb reg_probes */
        /* eq == 0 means no register entry — that's OK for dirty states
         * whose old sig was evicted.  If non-zero, it must be live. */
        if (eq != 0)
            assert(eq == d->initial || d->states[eq].refcount > 0);
    }

    free(visited);
    free(queue);
}
#endif /* DAFSA_DEBUG */
