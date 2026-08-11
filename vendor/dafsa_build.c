/* dafsa_build.c — Bulk minimal DAFSA construction (Daciuk et al. 2000)
 *
 * Builds a MINIMAL DAFSA from a SORTED, DEDUPLICATED key list in ~linear
 * time via the construction algorithm described in:
 *
 *   Daciuk, J., Mihov, S., Watson, B.W., & Watson, R.E. (2000)
 *   "Incremental Construction of Minimal Acyclic Finite-State Automata"
 *   Computational Linguistics, 26(1), pp. 3-16.
 *
 * Uses the engine's existing primitives (state_new, trans_add, incoming_add,
 * replace_or_register) without modifying any vendor file or adding new
 * internal helpers.
 *
 * IMPLEMENTATION NOTE: Because we freeze states bottom-up via the register
 * as soon as they can no longer change (suffix of a prefix that won't be
 * seen again), and dafsa_save BFS-renumbers reachable states and serializes
 * only sym-ascending transitions + final bits, ANY minimal DAFSA with the
 * same language will serialize byte-identical.  We do NOT need to reproduce
 * the incremental path's internal numbering/refcounts/inodes/register/free-list.
 */
#include "dafsa_internal.h"

/* Length of longest common prefix between two length-delimited keys.
 * Returns the byte position of the first difference (0..min(alen,blen)). */
static size_t lcp(const unsigned char *a, size_t alen,
                   const unsigned char *b, size_t blen)
{
    size_t n = (alen < blen) ? alen : blen;
    size_t i;
    for (i = 0; i < n; i++)
        if (a[i] != b[i]) break;
    return i;
}

dafsa *dafsa_build_sorted(const unsigned char *const *keys,
                          const size_t *lens, size_t nkeys)
{
    dafsa *d;
    const unsigned char *prev;
    size_t prev_len;
    size_t i;
    unsigned int sp;   /* path stack depth */

    if (nkeys == 0) return dafsa_create();

    /* Validate: keys + lens must be non-NULL */
    if (!keys || !lens) return NULL;

    d = dafsa_create();
    if (!d) return NULL;

    /* Find max key length and ensure scratch */
    {
        size_t max_len = 0;
        for (i = 0; i < nkeys; i++) {
            if (lens[i] > max_len)
                max_len = lens[i];
        }
        if (max_len > MAX_WORD_LEN) {
            dafsa_free(d);
            return NULL;
        }
        /* Always ensure scratch is allocated — even for empty keys,
         * we need the spath array for the root entry. */
        if (dafsa_ensure_scratch(d, max_len > 0 ? max_len : 1) != 0) {
            dafsa_free(d);
            return NULL;
        }
    }

    /* Active path stack: spath = state ids, sparents = parent state ids,
     * schars = transition symbols.  Indexing: path[0] = root (initial),
     * path[1..sp-1] = suffix states.  sp is the depth (number of states). */
    sp = 0;
    d->spath[sp]    = d->initial;
    d->sparents[sp] = 0;
    d->schars[sp]   = 0;
    sp = 1;

    prev     = NULL;
    prev_len = 0;

    for (i = 0; i < nkeys; i++) {
        const unsigned char *w     = keys[i];
        size_t               wlen  = lens[i];
        size_t j;

        /* DEFENSIVE: key must be non-NULL unless len==0 */
        if (wlen > 0 && !w) {
            dafsa_free(d);
            return NULL;
        }

        j = lcp(prev, prev_len, w, wlen);  /* common-prefix length */

        /* --- Freeze suffix below depth j: register bottom-up ---
         * States below the divergence point can never change again
         * (sorted input guarantees no later key shares this suffix).
         * Each state on the suffix path has refcount==1 (guaranteed
         * by the Daciuk construction: the only parent is the state
         * above it on the active path). */
        while (sp > j + 1) {
            sp--;
            replace_or_register(d, d->spath[sp], d->sparents[sp]);
        }

        /* --- Extend suffix w[j..wlen-1] from path[sp-1] --- */
        {
            unsigned int cur = d->spath[sp - 1];
            size_t k;

            for (k = j; k < wlen; k++) {
                unsigned int nxt = state_new(d);  /* may realloc states */

                /* Re-fetch via index — state_new may have realloc'd */
                trans_add(&d->states[cur], w[k], nxt);
                d->states[cur].sig = 0;

                incoming_add(d, cur, w[k], nxt);  /* may realloc inodes */

                d->spath[sp]    = nxt;
                d->sparents[sp] = cur;
                d->schars[sp]   = w[k];
                sp++;
                cur = nxt;
            }

            /* Mark final and dirty signature */
            d->states[cur].is_final = 1;
            d->states[cur].sig = 0;
        }

        /* Special case: wlen==0 — mark initial as final (must be first key) */
        if (wlen == 0) {
            d->states[d->initial].is_final = 1;
            d->states[d->initial].sig = 0;
        }

        prev     = w;
        prev_len = wlen;
    }

    /* --- Final flush: register remaining path bottom-up --- */
    while (sp > 1) {
        sp--;
        replace_or_register(d, d->spath[sp], d->sparents[sp]);
    }

    /* Register root */
    replace_or_register(d, d->initial, 0);

    return d;
}
