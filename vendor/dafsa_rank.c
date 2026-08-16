/*
 * dafsa_rank.c — Tier-2 order-statistics (rank / select / range_count).
 *
 * Implements the per-state DISTINCT-complete-keys-reachable subtree counts
 * for a minimized DAFSA, plus order-statistics primitives built on them:
 *
 *   dafsa_ensure_subtree — lazily (re)builds d->subtree once, memoizing each
 *                          state's count; O(n_states + n_trans).
 *   dafsa_rank_n         — number of complete keys strictly < key.
 *   dafsa_select_n       — k-th complete key (0-indexed, lex order).
 *   dafsa_range_count_n  — half-open [lo, hi) key count.
 *
 * Each order-statistic has a PREFIX-BOUND (start-state) twin used by the
 * relation layer's *_bound API: dafsa_rank_from / dafsa_select_from /
 * dafsa_range_count_from take a start state `s` (the state reached by walking
 * a bound prefix) and rank/select/count within that state's subtree.  The
 * plain *_n variants are thin wrappers over the same core with s = d->initial.
 *
 * Correctness of the recurrence (count(s) = is_final(s) + sum over s's
 * OUTGOING transitions of count(target)): within a state every transition has
 * a DISTINCT symbol, so the per-transition key sets are disjoint; a state
 * shared by N parents is counted once per parent edge, which is CORRECT
 * because each parent edge carries a distinct symbol => disjoint full-key
 * sets.  The DFA's determinism guarantees #walks == #keys, so summing counts
 * never double-counts.  All counts are u64: a cross-product relation with
 * N x M distinct keys has only ~N+M+2 states, so counts can exceed 2^32 long
 * before the DAFSA is huge (the brief's u32 would silently mis-evaluate
 * range_count above 4B keys).
 *
 * The counts are a pure cached diagnostic in the same spirit as dafsa_stats:
 * NOT persisted, rebuilt lazily on first use.  The array is freed in
 * dafsa_free and realloc'd in dafsa_ensure_subtree when nstates grows
 * (state_new can push nstates up between builds), so there is no stale-size
 * bug.
 */
#include "dafsa_internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ---- subtree path-count: number of DISTINCT complete keys reachable from s ----
 * For a MINIMIZED DAG this is memoized bottom-up; a state shared by N parents
 * contributes its count to each parent edge, which is CORRECT (each parent edge
 * is a distinct symbol => disjoint key sets). count(s) = is_final(s) + sum over
 * outgoing transitions of count(target). Recursion depth <= MAX_WORD_LEN (4096),
 * safe on the C stack. */
static uint64_t count_recurse(const dafsa *d, unsigned int s, uint64_t *memo,
                              unsigned char *visiting)
{
    const State *st = &d->states[s];
    uint64_t c;
    unsigned int j;
    if (visiting[s]) return memo[s];   /* DAG revisit: already computed */
    c = st->is_final ? 1 : 0;
    for (j = 0; j < st->ntrans; j++)
        c += count_recurse(d, trans_arr_c(st)[j].target, memo, visiting);
    memo[s] = c;
    visiting[s] = 1;
    return c;
}

/* Lazily (re)build the per-state subtree-count array into d->subtree.
 * Returns the root count (== number of distinct keys == n_final semantics).
 * On OOM degrades to 0 (d->subtree_valid stays 0 so the next call retries);
 * never aborts. */
uint64_t dafsa_ensure_subtree(dafsa *d)
{
    if (d->subtree_valid) return d->subtree[d->initial];
    if (d->subtree_cap < d->nstates) {
        free(d->subtree);
        d->subtree = calloc(d->nstates, sizeof(uint64_t));
        if (!d->subtree) { d->subtree_cap = 0; return 0; } /* OOM: degrade */
        d->subtree_cap = d->nstates;
    } else {
        memset(d->subtree, 0, d->nstates * sizeof(uint64_t));
    }
    {
        unsigned char *vis = calloc(d->nstates, 1);
        if (!vis) return 0;
        count_recurse(d, d->initial, d->subtree, vis);
        free(vis);
    }
    d->subtree_valid = 1;
    return d->subtree[d->initial];
}

/* rank within the subtree rooted at state `s` = number of complete keys
 * reachable from `s` strictly lexicographically < key.  O(len * ntrans).
 * Correct for absent keys (returns insertion position).  OOM during the lazy
 * build degrades to 0 (never abort). */
static uint64_t rank_core(dafsa *d, unsigned int s, const unsigned char *key,
                          size_t len)
{
    size_t i;
    uint64_t r = 0;

    if (!d || !key || s == 0 || s >= d->nstates) return 0;
    dafsa_ensure_subtree(d);
    if (!d->subtree_valid) return 0;   /* OOM during build: degrade to 0 */

    for (i = 0; i < len; i++) {
        unsigned char c = key[i];
        const State *st = &d->states[s];
        unsigned int j;
        int matched = -1;

        /* If `s` is final, the key key[0..i-1] is itself a complete key that is
         * a strict prefix of `key`, hence strictly < it: count it. */
        if (st->is_final) r += 1;

        /* Transitions are sym-ascending; accumulate every subtree whose first
         * byte is < c (those keys diverge earlier and are < key), stop at the
         * == c edge. */
        for (j = 0; j < st->ntrans; j++) {
            const Edge *e = &trans_arr_c(st)[j];
            if (e->sym < c) {
                r += d->subtree[e->target];
            } else if (e->sym == c) {
                matched = (int)j;
                break;
            } else {
                break;   /* sorted: no later entry can equal c */
            }
        }
        if (matched < 0) return r;   /* key diverges here: insertion position */
        s = trans_arr_c(st)[(unsigned int)matched].target;
    }
    /* Consumed all `len` bytes.  If s is final the key is present and must NOT
     * count itself (strictly <); if not final the key is an absent prefix, and
     * all strictly-smaller keys were already accumulated above. */
    return r;
}

/* rank(key) = number of complete keys strictly lexicographically < key
 * (start state = d->initial).  See rank_core. */
uint64_t dafsa_rank_n(dafsa *d, const unsigned char *key, size_t len)
{
    if (!d) return 0;
    return rank_core(d, d->initial, key, len);
}

/* Prefix-bound form: rank within the subtree rooted at `s` (the state reached
 * by walking the bound prefix).  See rank_core. */
uint64_t dafsa_rank_from(dafsa *d, unsigned int s, const unsigned char *key,
                         size_t len)
{
    return rank_core(d, s, key, len);
}

/* select within the subtree rooted at state `s`: the k-th complete key in lex
 * order (0-indexed), written to key_out (up to key_cap bytes).  Returns the key
 * length on success, or -1 if k >= total count of the subtree or on OOM (never
 * aborts). */
static int select_core(dafsa *d, unsigned int s, uint64_t k,
                       unsigned char *key_out, size_t key_cap)
{
    size_t pos = 0;
    uint64_t total;

    if (!d || !key_out || s == 0 || s >= d->nstates) return -1;
    dafsa_ensure_subtree(d);
    if (!d->subtree_valid) return -1;   /* OOM during build: degrade to -1 */

    total = d->subtree[s];
    if (k >= total) return -1;          /* out of range */

    for (;;) {
        const State *st = &d->states[s];
        unsigned int j;

        if (st->is_final) {
            if (k == 0) return (int)pos;   /* key_out[0..pos-1] is the k-th key */
            k -= 1;                         /* skip the "ends here" key */
        }
        /* Descend into the smallest-sym transition whose subtree contains the
         * k-th key. */
        for (j = 0; j < st->ntrans; j++) {
            const Edge *e = &trans_arr_c(st)[j];
            uint64_t cnt = d->subtree[e->target];
            if (k < cnt) {
                if (pos >= key_cap) return -1;   /* key too long for buffer */
                key_out[pos++] = e->sym;
                s = e->target;
                break;
            }
            k -= cnt;
        }
        if (j == st->ntrans) return -1;  /* unreachable if k < total; defensive */
    }
}

/* select(k) = the k-th complete key in lex order (0-indexed), written to
 * key_out (up to key_cap bytes).  Returns the key length on success, or -1 if
 * k >= total count or on OOM (never aborts). */
int dafsa_select_n(dafsa *d, uint64_t k, unsigned char *key_out, size_t key_cap)
{
    if (!d) return -1;
    return select_core(d, d->initial, k, key_out, key_cap);
}

/* Prefix-bound form: select within the subtree rooted at `s`.  See select_core. */
int dafsa_select_from(dafsa *d, unsigned int s, uint64_t k,
                      unsigned char *key_out, size_t key_cap)
{
    return select_core(d, s, k, key_out, key_cap);
}

/* range_count within the subtree rooted at `s` = number of complete keys in the
 * half-open interval [lo, hi), i.e. rank(hi) - rank(lo).  Returns 0 on
 * error/OOM (never abort). */
static uint64_t range_count_core(dafsa *d, unsigned int s,
                                 const unsigned char *lo, size_t lo_len,
                                 const unsigned char *hi, size_t hi_len)
{
    uint64_t rhi, rlo;

    if (!d || !lo || !hi) return 0;
    rhi = rank_core(d, s, hi, hi_len);
    rlo = rank_core(d, s, lo, lo_len);
    return (rhi > rlo) ? (rhi - rlo) : 0;
}

/* range_count = number of complete keys in the half-open interval [lo, hi),
 * i.e. rank(hi) - rank(lo).  Returns 0 on error/OOM (never abort). */
uint64_t dafsa_range_count_n(dafsa *d, const unsigned char *lo, size_t lo_len,
                             const unsigned char *hi, size_t hi_len)
{
    if (!d) return 0;
    return range_count_core(d, d->initial, lo, lo_len, hi, hi_len);
}

/* Prefix-bound form: range_count within the subtree rooted at `s`. */
uint64_t dafsa_range_count_from(dafsa *d, unsigned int s,
                                const unsigned char *lo, size_t lo_len,
                                const unsigned char *hi, size_t hi_len)
{
    return range_count_core(d, s, lo, lo_len, hi, hi_len);
}
