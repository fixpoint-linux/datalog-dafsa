/* dafsa_view_rank.c — Tier-2 order-statistics over a zero-copy dafsa_view.
 * (reference implementation for the plan; see handoff-range-snap-plan) */
#include "dafsa_internal.h"
#include <stdlib.h>

static inline int view_is_final(const dafsa_view *v, uint32_t s)
{
    return (v->final_bits[s / 8] & (uint8_t)(1u << (s % 8))) ? 1 : 0;
}

static uint64_t view_count_recurse(const dafsa_view *v, uint32_t s,
                                   uint64_t *memo, uint8_t *computed)
{
    const uint8_t *cur;
    unsigned char sym;
    uint32_t tgt;
    uint64_t c;

    if (computed[s]) return memo[s];
    c = view_is_final(v, s) ? 1 : 0;
    cur = v->csr + v->state_off[s];
    while (view_edge_next(v, s, &cur, &sym, &tgt) == 0)
        c += view_count_recurse(v, tgt, memo, computed);
    memo[s] = c;
    computed[s] = 1;
    return c;
}

uint64_t dafsa_view_subtree_counts(const dafsa_view *v, uint64_t **counts_out)
{
    uint64_t *memo;
    uint8_t  *computed;
    if (!v || !counts_out) return 0;
    *counts_out = NULL;
    memo = calloc((size_t)v->n_states + 1, sizeof(uint64_t));
    if (!memo) return 0;
    computed = calloc((size_t)v->n_states + 1, 1);
    if (!computed) { free(memo); return 0; }
    view_count_recurse(v, v->initial, memo, computed);
    free(computed);
    *counts_out = memo;
    return memo[v->initial];
}

static uint64_t view_rank_core(const dafsa_view *v, uint32_t s,
                               const uint64_t *counts,
                               const unsigned char *key, size_t len)
{
    uint64_t r = 0;
    size_t i;
    for (i = 0; i < len; i++) {
        unsigned char c = key[i];
        const uint8_t *cur = v->csr + v->state_off[s];
        unsigned char sym;
        uint32_t tgt;
        int matched = 0;
        uint32_t match_tgt = 0;

        if (view_is_final(v, s)) r += 1;
        while (view_edge_next(v, s, &cur, &sym, &tgt) == 0) {
            if (sym < c)      r += counts[tgt];
            else if (sym == c) { matched = 1; match_tgt = tgt; break; }
            else break;
        }
        if (!matched) return r;
        s = match_tgt;
    }
    return r;
}

uint64_t dafsa_view_rank_n(const dafsa_view *v, const unsigned char *key, size_t len)
{
    uint64_t *counts = NULL;
    uint64_t r;
    if (!v || !key) return 0;
    dafsa_view_subtree_counts(v, &counts);
    if (!counts) return 0;
    r = view_rank_core(v, v->initial, counts, key, len);
    free(counts);
    return r;
}

static int view_select_core(const dafsa_view *v, uint32_t s, const uint64_t *counts,
                            uint64_t k, unsigned char *key_out, size_t key_cap)
{
    size_t pos = 0;
    uint64_t total = counts[s];
    if (k >= total) return -1;
    for (;;) {
        const uint8_t *cur = v->csr + v->state_off[s];
        unsigned char sym;
        uint32_t tgt;
        int descended = 0;

        if (view_is_final(v, s)) {
            if (k == 0) return (int)pos;
            k -= 1;
        }
        while (view_edge_next(v, s, &cur, &sym, &tgt) == 0) {
            uint64_t cnt = counts[tgt];
            if (k < cnt) {
                if (pos >= key_cap) return -1;
                key_out[pos++] = sym;
                s = tgt;
                descended = 1;
                break;
            }
            k -= cnt;
        }
        if (!descended) return -1;
    }
}

int dafsa_view_select_n(const dafsa_view *v, uint64_t k,
                        unsigned char *key_out, size_t key_cap)
{
    uint64_t *counts = NULL;
    int r;
    if (!v || !key_out) return -1;
    dafsa_view_subtree_counts(v, &counts);
    if (!counts) return -1;
    r = view_select_core(v, v->initial, counts, k, key_out, key_cap);
    free(counts);
    return r;
}

uint64_t dafsa_view_range_count_n(const dafsa_view *v,
                                  const unsigned char *lo, size_t lo_len,
                                  const unsigned char *hi, size_t hi_len)
{
    uint64_t *counts = NULL;
    uint64_t rhi, rlo;
    if (!v || !lo || !hi) return 0;
    dafsa_view_subtree_counts(v, &counts);
    if (!counts) return 0;
    rhi = view_rank_core(v, v->initial, counts, hi, hi_len);
    rlo = view_rank_core(v, v->initial, counts, lo, lo_len);
    free(counts);
    return (rhi > rlo) ? (rhi - rlo) : 0;
}
