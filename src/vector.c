/*
 * vector.c — Vector tier query path (S2)
 *
 * MIH candidate retrieval (dl_vector_search / dl_vector_search_version) and
 * integer int8 cosine re-ranking (dl_vector_rerank).
 *
 * LIVE vs VERSION read discipline (F1/F2, the load-bearing invariant):
 *   - dl_prefix / dl_lookup are LIVE-ONLY (dl.c) — they never route to the
 *     snapshot view.  VERSION reads MUST use dl_query_bound_version.
 *   - LIVE search reads BOTH sig_j and entity via dl_prefix.
 *   - VERSION search reads BOTH via dl_query_bound_version.
 * The two search entry points share ONE implementation that branches ONLY on
 * the read primitive (mirrors dl_search / dl_search_version in index.c), so
 * a publish between a sig_j read and an entity read can never produce an
 * inconsistent candidate set within a single mode.
 *
 * int8 re-rank is pure integer arithmetic (F5): no float, no sqrtf.  Integer
 * dot + integer norm, ranked by exact cosine order via a SIGNED int64
 * cross-multiply dot_a*|b| vs dot_b*|a| (integer isqrt for |v|).  This is
 * the sign-preserving, non-overflowing equivalent of the plan's
 * "dot_a^2*|b|^2 vs dot_b^2*|a|^2" form, which — even after the int32->int64
 * fix — overflows int64 (dot^2 * |v|^2 reaches ~2.4e20 > INT64_MAX).
 */
#include "vector.h"
#include "dl.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ─── Band slicing (C1: MUST match embed.py's slice formula) ────────────── */

/* Band j of a c-bit code.  Word layout MSB-first (sig[0] = bits 255..224).
   VEC_W=16 divides 32, so band j sits in one word: even j -> high 16 of word
   j/2 (shift 16), odd j -> low 16 (shift 0).  Band 0 = high 16 of sig[0]. */
static uint32_t band_slice(const uint32_t *sig, int j) {
    return (sig[j / 2] >> ((1u - (j % 2u)) * 16u)) & 0xFFFFu;
}

static void sig_rel_name(int j, char *out) { snprintf(out, 16, "__sig%d__", j); }

/* Every VEC_W-bit value within Hamming `budget` of `sub`, streamed via cb.
   Popcount scan (2^16 = 65536 iters) is trivially correct at w=16; a
   combinatorial generator is a later optimization, not a correctness change.
   Returns 1 if cb requested an early stop, else 0. */
static int variants_within(uint32_t sub, int budget,
                           int (*cb)(uint32_t, void *), void *user) {
    uint32_t mask;
    for (mask = 0; mask < (1u << VEC_W); mask++)
        if (__builtin_popcount(mask) <= budget)
            if (cb(sub ^ mask, user)) return 1;
    return 0;
}

/* ─── Candidate set: (entity sym, band-match count), dedup ──────────────── */

typedef struct { uint32_t sym; int bands; } vec_cand;

typedef struct { vec_cand *d; size_t n, cap; } vec_cand_set;

static void cs_init(vec_cand_set *s) { s->d = NULL; s->n = s->cap = 0; }
static void cs_free(vec_cand_set *s) { free(s->d); s->d = NULL; s->n = s->cap = 0; }

static vec_cand *cs_find(vec_cand_set *s, uint32_t sym) {
    size_t i;
    for (i = 0; i < s->n; i++)
        if (s->d[i].sym == sym) return &s->d[i];
    return NULL;
}

/* Ensure `sym` has an entry; *out points at it (new entries have bands = 0).
 * Returns 0 on success, -1 on OOM. */
static int cs_ensure(vec_cand_set *s, uint32_t sym, vec_cand **out) {
    vec_cand *c = cs_find(s, sym);
    if (c) { *out = c; return 0; }
    if (s->n >= s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 64;
        vec_cand *nd = realloc(s->d, nc * sizeof(*nd));
        if (!nd) return -1;
        s->d = nd; s->cap = nc;
    }
    c = &s->d[s->n++];
    c->sym = sym; c->bands = 0;
    *out = c;
    return 0;
}

static int cand_cmp(const void *pa, const void *pb) {
    const vec_cand *a = pa, *b = pb;
    if (a->bands != b->bands) return (a->bands > b->bands) ? -1 : 1;
    return (a->sym < b->sym) ? -1 : (a->sym > b->sym ? 1 : 0);
}

/* ─── Mode dispatch read primitive ──────────────────────────────────────── */

typedef struct {
    dl_db *db;
    uint32_t version;
    int use_version;
} vec_reader;

/* Read a prefix: LIVE -> dl_prefix; VERSION -> dl_query_bound_version.
   A negative return sets *err (dl_prefix: missing relation; version mode:
   version == 0 / nonexistent / relation absent from that version). */
static long vec_read_prefix(const vec_reader *rd, const char *rel,
                            const uint32_t *leading, uint8_t k,
                            dl_tuple_cb cb, void *user, int *err) {
    long n = rd->use_version
        ? dl_query_bound_version(rd->db, rd->version, rel, leading, k, cb, user)
        : dl_prefix(rd->db, rel, leading, k, cb, user);
    if (n < 0) { *err = 1; return -1; }
    return n;
}

static int noop_cb(const uint32_t *cols, uint8_t arity, void *user) {
    (void)cols; (void)arity; (void)user; return 0;
}

/* ─── Variant probing ───────────────────────────────────────────────────── */

typedef struct { vec_cand_set *set; int error; } add_entity_ctx;

/* Collect cb: record cols[1] (entity sym) into the per-band set (dedup). */
static int add_entity_cb(const uint32_t *cols, uint8_t arity, void *user) {
    add_entity_ctx *c = user;
    vec_cand *tmp;
    (void)arity;
    if (cs_ensure(c->set, cols[1], &tmp) < 0) { c->error = 1; return 1; }
    return 0;
}

typedef struct {
    const vec_reader *rd;
    char rel[16];
    vec_cand_set *per_band;
    int error;
} variant_ctx;

/* One variant of a query band: prefix-probe sig_j and collect its entities. */
static int variant_probe_cb(uint32_t variant, void *user) {
    variant_ctx *v = user;
    uint32_t leading[1] = { variant };
    add_entity_ctx ac = { v->per_band, 0 };
    long n = vec_read_prefix(v->rd, v->rel, leading, 1, add_entity_cb, &ac,
                             &v->error);
    if (ac.error) v->error = 1;                    /* propagate OOM to caller */
    if (v->error || ac.error || n < 0) return 1;   /* stop the variant loop */
    return 0;
}

/* ─── Shared search body (LIVE / VERSION differ only in the read primitive) */

static long vector_search_impl(const vec_reader *rd, const uint32_t *q_sig,
                               int k, int r, dl_vec_cb cb, void *user) {
    int budget, j, err = 0;
    vec_cand_set cand, band;

    if (!rd->db || !q_sig || k <= 0 || !cb) return -1;
    if (r < 0) r = 0;

    /* VERSION mode up-front gate: the entity relation must be present in the
       version (relation-absent-from-version -> -1, per the error contract).
       leading=[0] touches no row if the relation exists (even empty). */
    if (rd->use_version) {
        uint32_t probe[1] = { 0 };
        if (vec_read_prefix(rd, VEC_ENTITY_REL, probe, 1, noop_cb, NULL,
                            &err) < 0)
            return -1;
    }

    cs_init(&cand);
    cs_init(&band);

    budget = r / VEC_M;              /* pigeonhole budget per band */
    if (budget > VEC_W) budget = VEC_W;

    for (j = 0; j < VEC_M && !err; j++) {
        variant_ctx vc;
        sig_rel_name(j, vc.rel);
        vc.rd = rd;
        vc.per_band = &band;
        vc.error = 0;
        cs_free(&band);
        cs_init(&band);
        variants_within(band_slice(q_sig, j), budget, variant_probe_cb, &vc);
        if (vc.error) { err = 1; break; }
        /* merge per-band (distinct within the band) into the global set. */
        {
            size_t i;
            for (i = 0; i < band.n && !err; i++) {
                vec_cand *g;
                if (cs_ensure(&cand, band.d[i].sym, &g) < 0) { err = 1; break; }
                g->bands++;
            }
        }
    }
    cs_free(&band);
    if (err) { cs_free(&cand); return -1; }

    /* live-entity filter: drop candidates whose entity row is absent. */
    {
        size_t w = 0, i;
        for (i = 0; i < cand.n; i++) {
            uint32_t leading[1] = { cand.d[i].sym };
            long n = vec_read_prefix(rd, VEC_ENTITY_REL, leading, 1, noop_cb,
                                     NULL, &err);
            if (err) { cs_free(&cand); return -1; }
            if (n > 0) cand.d[w++] = cand.d[i];
        }
        cand.n = w;
    }

    /* coarse relevance: most matched bands first, sym asc as the tiebreak. */
    qsort(cand.d, cand.n, sizeof(cand.d[0]), cand_cmp);

    /* emit at most k (count before the cb so an early stop still counts). */
    {
        long emitted = 0;
        size_t i;
        for (i = 0; i < cand.n && emitted < k; i++) {
            emitted++;
            if (cb(cand.d[i].sym, cand.d[i].bands, user) != 0) break;
        }
        cs_free(&cand);
        return emitted;
    }
}

long dl_vector_search(dl_db *db, const uint32_t *q_sig,
                      int k, int r, dl_vec_cb cb, void *user) {
    vec_reader rd = { db, 0, 0 };
    return vector_search_impl(&rd, q_sig, k, r, cb, user);
}

long dl_vector_search_version(dl_db *db, uint32_t version, const uint32_t *q_sig,
                              int k, int r, dl_vec_cb cb, void *user) {
    vec_reader rd = { db, version, 1 };
    if (!db || version == 0) return -1;
    return vector_search_impl(&rd, q_sig, k, r, cb, user);
}

/* ─── Integer int8 cosine re-rank ───────────────────────────────────────── */

/* Integer square root (Newton), exact for the small norms here. */
static uint32_t isqrt_u64(uint64_t n) {
    uint64_t x, y;
    if (n == 0) return 0;
    x = n;
    y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + n / x) / 2; }
    return (uint32_t)x;
}

/* Load the stored int8 vector (96 chunks) for one entity into a buffer. */
typedef struct { int8_t v[VEC_D]; int present; } vec_load;

static int load_vec_cb(const uint32_t *cols, uint8_t arity, void *user) {
    vec_load *l = user;
    uint32_t chunk, packed;
    (void)arity;
    chunk = cols[1];
    packed = cols[2];
    if (chunk >= (uint32_t)VEC_IVEC_WORDS) return 0;  /* malformed, ignore */
    l->present = 1;
    l->v[chunk * 4 + 0] = (int8_t)(packed & 0xFFu);
    l->v[chunk * 4 + 1] = (int8_t)((packed >> 8) & 0xFFu);
    l->v[chunk * 4 + 2] = (int8_t)((packed >> 16) & 0xFFu);
    l->v[chunk * 4 + 3] = (int8_t)((packed >> 24) & 0xFFu);
    return 0;
}

typedef struct {
    uint32_t sym;
    int32_t dot;     /* <q, v>, |dot| <= 384*127^2 = 6.2e6 < 2^24 */
    uint32_t norm2;  /* |v|^2, same bound */
    uint32_t norm;   /* integer isqrt(norm2) */
} rank_cand;

/* a ranks above b iff dot_a/|a| > dot_b/|b|  <=>  dot_a*|b| > dot_b*|a|.
   All four factors fit int64 (|dot|*norm <= 6.2e6 * 2490 ~ 1.5e10), and the
   signed comparison is correct for every sign combination. */
static int rank_cand_cmp(const void *pa, const void *pb) {
    const rank_cand *a = pa, *b = pb;
    int64_t lhs = (int64_t)a->dot * (int64_t)b->norm;
    int64_t rhs = (int64_t)b->dot * (int64_t)a->norm;
    if (lhs != rhs) return (lhs > rhs) ? -1 : 1;
    return (a->sym < b->sym) ? -1 : (a->sym > b->sym ? 1 : 0);
}

long dl_vector_rerank(dl_db *db, const uint32_t *q_int8,
                      const uint32_t *cand_syms, int n_cand,
                      int k, dl_vec_cb cb, void *user) {
    int32_t q[VEC_D];
    rank_cand *rc;
    int i, err = 0;
    int n_valid = 0;

    if (!db || !q_int8 || !cand_syms || n_cand <= 0 || k <= 0 || !cb)
        return -1;

    /* unpack the query int8 vector (96 u32, 4 int8 little-endian packed). */
    for (i = 0; i < VEC_D; i++)
        q[i] = (int8_t)(q_int8[i / 4] >> (8u * (i % 4u)));

    rc = calloc((size_t)n_cand, sizeof(*rc));
    if (!rc) return -1;

    for (i = 0; i < n_cand; i++) {
        uint32_t sym = cand_syms[i];
        uint32_t leading[1] = { sym };
        vec_load l;
        long n;
        int d;
        int64_t dot = 0, norm2 = 0;

        memset(&l, 0, sizeof l);
        n = dl_prefix(db, "__vec_q__", leading, 1, load_vec_cb, &l);
        if (n < 0) { err = 1; break; }
        if (!l.present) continue;   /* no vector on record: skip candidate */

        for (d = 0; d < VEC_D; d++) {
            int32_t v = l.v[d];
            dot += (int64_t)v * q[d];
            norm2 += (int64_t)v * v;
        }
        rc[n_valid].sym = sym;
        rc[n_valid].dot = (int32_t)dot;
        rc[n_valid].norm2 = (uint32_t)norm2;
        rc[n_valid].norm = isqrt_u64((uint64_t)norm2);
        n_valid++;
    }

    if (err) { free(rc); return -1; }

    qsort(rc, (size_t)n_valid, sizeof(rc[0]), rank_cand_cmp);

    {
        long emitted = 0;
        for (i = 0; i < n_valid && emitted < k; i++) {
            emitted++;
            if (cb(rc[i].sym, (int)rc[i].dot, user) != 0) break;
        }
        free(rc);
        return emitted;
    }
}
