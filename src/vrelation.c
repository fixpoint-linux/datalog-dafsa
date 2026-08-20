/*
 * vrelation.c — Variable-arity relation dispatch layer (v2)
 *
 * Implements the per-arity-variant family described in vrelation.h.  Every
 * operation delegates VERBATIM to the existing fixed-width relation.c code on
 * the chosen variant — this file contains no DAFSA logic of its own, which is
 * exactly the property that keeps the proven walker, exact lookup, perm
 * indices, WAL and snapshot byte-identical for fixed relations (a fixed
 * relation never goes through here at all).
 */

#include "vrelation.h"
#include "regexwalk.h"

#include <stdlib.h>

struct vrelation {
    relation *variants[MAX_VAR_ARITY + 1];  /* [0] unused */
};

vrelation *vrel_create(void)
{
    return calloc(1, sizeof(vrelation));
}

void vrel_free(vrelation *v)
{
    uint8_t a;
    if (!v) return;
    for (a = 1; a <= MAX_VAR_ARITY; a++)
        rel_free(v->variants[a]);
    free(v);
}

relation *vrel_variant_or_null(const vrelation *v, uint8_t arity)
{
    if (!v || arity == 0 || arity > MAX_VAR_ARITY) return NULL;
    return v->variants[arity];
}

relation *vrel_variant(vrelation *v, uint8_t arity)
{
    if (!v || arity == 0 || arity > MAX_VAR_ARITY) return NULL;
    if (!v->variants[arity]) {
        v->variants[arity] = rel_create(arity);
        /* On OOM the slot stays NULL and the caller sees failure. */
    }
    return v->variants[arity];
}

int vrel_attach(vrelation *v, uint8_t arity, relation *r)
{
    if (!v || !r || arity == 0 || arity > MAX_VAR_ARITY) return -1;
    if (v->variants[arity]) return -1;   /* already present */
    v->variants[arity] = r;
    return 0;
}

void vrel_foreach(vrelation *v, vrel_iter_cb cb, void *user)
{
    uint8_t a;
    if (!v || !cb) return;
    for (a = 1; a <= MAX_VAR_ARITY; a++)
        if (v->variants[a])
            cb(v->variants[a], a, user);
}

int vrel_any_idb(const vrelation *v)
{
    uint8_t a;
    if (!v) return 0;
    for (a = 1; a <= MAX_VAR_ARITY; a++)
        if (v->variants[a] && rel_is_idb(v->variants[a]))
            return 1;
    return 0;
}

int vrel_reset_views(vrelation *v)
{
    uint8_t a;
    if (!v) return -1;
    for (a = 1; a <= MAX_VAR_ARITY; a++) {
        if (!v->variants[a]) continue;
        if (rel_reset_view(v->variants[a]) != 0)
            return -1;   /* earlier variants stay reset; loud failure */
    }
    return 0;
}

int vrel_exact(const vrelation *v, const uint32_t *cols, uint8_t arity)
{
    relation *r = vrel_variant_or_null(v, arity);
    return r ? rel_exact(r, cols) : 0;
}

int vrel_exact_base(const vrelation *v, const uint32_t *cols, uint8_t arity)
{
    relation *r = vrel_variant_or_null(v, arity);
    return r ? rel_exact_base(r, cols) : 0;
}

/* Shared fan-out: rel_prefix or rel_prefix_base over every present variant
 * a >= max(k,1).  Each variant walk is the EXISTING fixed-width prefix
 * walker; the cb's arity parameter disambiguates tuples across variants. */
static long vrel_prefix_impl(const vrelation *v, const uint32_t *leading,
                             uint8_t k, int use_base,
                             rel_enum_cb cb, void *user)
{
    long total = 0;
    uint8_t a;

    if (!v || !cb) return -1;
    if (k > MAX_VAR_ARITY) return -1;
    if (k > 0 && !leading) return -1;

    for (a = (k > 0 ? k : 1); a <= MAX_VAR_ARITY; a++) {
        relation *r = v->variants[a];
        long n;
        if (!r) continue;
        n = use_base ? rel_prefix_base(r, leading, k, cb, user)
                     : rel_prefix(r, leading, k, cb, user);
        if (n < 0) return -1;
        total += n;
        /* Early-stop: rel_prefix returns a SHORT count-then-stop only via the
         * cb; a cb stop means "enough" — abort the remaining variants and
         * report what was produced (never an error).  We cannot observe the
         * stop directly, so we simply keep walking the (typically few)
         * remaining variants; their cb calls return non-zero immediately at
         * the first tuple and the per-variant walk stops there.  Total count
         * semantics for early-stop are best-effort by design (same class as
         * rel_prefix's single-variant early stop). */
    }
    return total;
}

long vrel_prefix(const vrelation *v, const uint32_t *leading, uint8_t k,
                 rel_enum_cb cb, void *user)
{
    return vrel_prefix_impl(v, leading, k, 0, cb, user);
}

long vrel_prefix_base(const vrelation *v, const uint32_t *leading, uint8_t k,
                      rel_enum_cb cb, void *user)
{
    return vrel_prefix_impl(v, leading, k, 1, cb, user);
}

long vrel_pattern(const vrelation *v, const struct regex_dfa *dfa,
                  rel_enum_cb cb, void *user)
{
    long total = 0;
    uint8_t a;

    if (!v || !dfa || !cb) return -1;

    for (a = 1; a <= MAX_VAR_ARITY; a++) {
        relation *r = v->variants[a];
        long n;
        if (!r) continue;
        n = rel_pattern(r, dfa, cb, user);
        if (n < 0) return -1;
        total += n;
    }
    return total;
}

long vrel_filter_col(const vrelation *v, uint8_t col,
                     const struct sym_set *set,
                     rel_enum_cb cb, void *user)
{
    long total = 0;
    uint8_t a;

    if (!v || !set || !cb) return -1;

    for (a = 1; a <= MAX_VAR_ARITY; a++) {
        relation *r = v->variants[a];
        long n;
        if (!r) continue;
        /* Only filter if the variant has enough columns */
        if (col < rel_arity(r)) {
            n = rel_filter_col(r, col, set, cb, user);
            if (n < 0) return -1;
            total += n;
        }
    }
    return total;
}

uint64_t vrel_count(const vrelation *v)
{
    uint64_t total = 0;
    uint8_t a;
    if (!v) return 0;
    for (a = 1; a <= MAX_VAR_ARITY; a++)
        if (v->variants[a])
            total += rel_count(v->variants[a]);
    return total;
}
