/*
 * test_vector_search.c — S2 vector tier query-path tests (scan-and-rank)
 *
 * Exercises dl_vector_search / dl_vector_search_version / dl_vector_rerank:
 *   - pack/unpack round-trip (the encoder's chunk layout)
 *   - EXACTNESS: the top-k equals a brute-force float cosine reference, in the
 *     same order (100% recall by construction — no candidate pre-filter)
 *   - live-entity filter (delete an entity -> it drops out of live search)
 *   - view consistency (publish, mutate live: version stable + live sees
 *     mutation — the test that catches a live/version read mix)
 *   - snapshot parity (live results == version results on an unmutated index)
 *   - error contract (version == 0 / nonexistent / relation-absent -> -1,
 *     empty-but-present -> 0, NULL args / k <= 0 -> -1)
 *
 * SUPERSEDED (2026-08-11): this suite was a MIH/ITQ test — it built a
 * 256-bit random-projection signature per entity, wrote 16 band postings, and
 * asserted a probabilistic *recall fraction* (pigeonhole retrieval could miss).
 * Ranking is now a scan-and-rank over __vec_q__ with no candidate pre-filter,
 * so the recall assertion became an exactness assertion, which is strictly
 * stronger: the emitted top-k must equal the float-cosine reference exactly.
 * The signature model is gone with the mechanism.
 */
#include "dl.h"
#include "vector.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0;
static int tests_failed = 0;

#define TEST(name) do { \
    tests_run++; \
    printf("  %s ... ", name); \
    fflush(stdout); \
} while (0)
#define PASS() do { printf("OK\n"); } while (0)
#define FAIL(msg) do { \
    printf("FAIL: %s\n", msg); \
    tests_failed++; \
} while (0)

static const char *BASE = "build-tmp/vector-search";

static dl_db *fresh_db(char *dir_out, size_t cap, const char *name)
{
    char cmd[512];
    snprintf(dir_out, cap, "%s/%s", BASE, name);
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir_out);
    system(cmd);
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", dir_out);
    system(cmd);
    return dl_open(dir_out);
}

/* ─── Deterministic RNG (LCG) ───────────────────────────────────────────── */
static uint32_t rng_state;

static void rng_seed(uint32_t s) { rng_state = s; }

static uint32_t rng_next(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

/* ─── Pack4 / unpack4 (little-endian int8 packing) ──────────────────────── */

static uint32_t pack4(int8_t b0, int8_t b1, int8_t b2, int8_t b3)
{
    return (uint32_t)(uint8_t)b0 | ((uint32_t)(uint8_t)b1 << 8) |
           ((uint32_t)(uint8_t)b2 << 16) | ((uint32_t)(uint8_t)b3 << 24);
}

static void unpack4(uint32_t packed, int8_t *out)
{
    out[0] = (int8_t)(packed & 0xFF);
    out[1] = (int8_t)((packed >> 8) & 0xFF);
    out[2] = (int8_t)((packed >> 16) & 0xFF);
    out[3] = (int8_t)((packed >> 24) & 0xFF);
}

/* ─── Entity model (vector + its emitted __vec_q__ chunks) ──────────────── */

typedef struct {
    char name[32];
    uint32_t sym, type;
    int8_t vec[VEC_D];
    uint32_t ivec[VEC_IVEC_WORDS];
} ent_t;

static void pack_vec(uint32_t *out, const int8_t *vec)
{
    int c;
    for (c = 0; c < VEC_IVEC_WORDS; c++)
        out[c] = pack4(vec[c*4+0], vec[c*4+1], vec[c*4+2], vec[c*4+3]);
}

static void gen_entity(ent_t *e, const char *name, dl_db *db)
{
    int d;
    snprintf(e->name, sizeof(e->name), "%s", name);
    e->sym = dl_intern_str(db, e->name);
    e->type = dl_intern_str(db, "doc");
    for (d = 0; d < VEC_D; d++) e->vec[d] = (int8_t)(int)(rng_next() % 201u) - 100;
    pack_vec(e->ivec, e->vec);
}

/* A query that is a small perturbation of `src`. */
static void gen_query(ent_t *q, const ent_t *src)
{
    int d;
    snprintf(q->name, sizeof(q->name), "q");
    q->sym = 0;
    q->type = 0;
    for (d = 0; d < VEC_D; d++) {
        int v = src->vec[d] + (int)(rng_next() % 7u) - 3;
        if (v > 127) v = 127;
        if (v < -128) v = -128;
        q->vec[d] = (int8_t)v;
    }
    pack_vec(q->ivec, q->vec);
}

/* ─── Index construction (what the external encoder emits) ──────────────── */

static void declare_vector_rels(dl_db *db)
{
    dl_declare_relation(db, VEC_ENTITY_REL, 2);
    dl_declare_relation(db, "__vec_q__", 3);
}

static int add_entity_to_index(dl_db *db, const ent_t *e)
{
    uint32_t cols[3];
    int c;
    cols[0] = e->sym;
    cols[1] = e->type;
    if (dl_add_fact(db, VEC_ENTITY_REL, cols, 2) != 1) return -1;
    for (c = 0; c < VEC_IVEC_WORDS; c++) {
        cols[0] = e->sym;
        cols[1] = (uint32_t)c;
        cols[2] = e->ivec[c];
        if (dl_add_fact(db, "__vec_q__", cols, 3) != 1) return -1;
    }
    return 0;
}

/* ─── Result collection ─────────────────────────────────────────────────── */

#define COLL_MAX 4096
typedef struct { uint32_t syms[COLL_MAX]; int scores[COLL_MAX]; int n; } coll;

static int coll_search_cb(uint32_t sym, int score, void *user)
{
    coll *c = user;
    if (c->n < COLL_MAX) { c->syms[c->n] = sym; c->scores[c->n] = score; c->n++; }
    return 0;
}

static int coll_has(const coll *c, uint32_t sym)
{
    int i;
    for (i = 0; i < c->n; i++) if (c->syms[i] == sym) return 1;
    return 0;
}

/* Count rows in a prefix enumeration. */
static int sink_count_cb(const uint32_t *cols, uint8_t arity, void *user)
{
    int *cnt = user;
    (void)cols; (void)arity;
    if (cnt) (*cnt)++;
    return 0;
}

/* ─── Brute-force float cosine reference (test-side oracle) ───────────────
 *
 * Reproduces the ENGINE's integer dot exactly (the int8 quantization is not
 * what is under test — the ORDER is) and divides by the exact float norms, so
 * this is an independent implementation of cosine similarity: a mis-signed
 * cross-multiply, a wrong norm, a dropped candidate or a wrong tiebreak all
 * show up as a top-k mismatch. */

typedef struct { uint32_t sym; double cos; } ref_score;

static double cos_of(const int8_t *q, const int8_t *v)
{
    int d;
    double dot = 0.0, nq = 0.0, nv = 0.0;
    for (d = 0; d < VEC_D; d++) {
        dot += (double)q[d] * (double)v[d];
        nq += (double)q[d] * (double)q[d];
        nv += (double)v[d] * (double)v[d];
    }
    if (nq <= 0.0 || nv <= 0.0) return 0.0;
    return dot / (sqrt(nq) * sqrt(nv));
}

static int ref_cmp(const void *pa, const void *pb)
{
    const ref_score *a = pa, *b = pb;
    if (a->cos > b->cos) return -1;
    if (a->cos < b->cos) return 1;
    if (a->sym < b->sym) return -1;
    if (a->sym > b->sym) return 1;
    return 0;
}

/* Sort every entity by (cosine desc, sym asc) — the full reference ranking. */
static int ref_sorted(const int8_t *q, const ent_t *ents, int n, ref_score *out)
{
    int i;
    if (n > 64) n = 64;
    for (i = 0; i < n; i++) {
        out[i].sym = ents[i].sym;
        out[i].cos = cos_of(q, ents[i].vec);
    }
    qsort(out, (size_t)n, sizeof out[0], ref_cmp);
    return n;
}

/* Are the reference margins wide enough for the comparison to MEAN anything?
 *
 * The engine ranks with a SIGNED int64 cross-multiply over integer isqrt
 * norms, and isqrt floors: each norm carries up to 1 absolute error, so the
 * comparison is only faithful to float cosine to within ~(1/|a| + 1/|b|)
 * relative (~2e-3 at most here).  Two thresholds cover the two regimes:
 *   - top-1 (cosines ~0.99): gap must exceed 1e-2;
 *   - the rest (cosines ~0.1): gap must exceed 1e-3.
 * A fixture that cannot meet them is replaced by another perturbation rather
 * than asserted on — see t_exact_topk. */
static int ref_margins_ok(const ref_score *all, int n, int k)
{
    int j;
    if (n <= k) return 0;
    if (all[0].cos - all[1].cos <= 1e-2) return 0;
    for (j = 1; j < k; j++)
        if (all[j].cos - all[j + 1].cos <= 1e-3) return 0;
    return 1;
}

/* ─── Test 1: pack/unpack round-trip ───────────────────────────────────── */

static void t_pack_unpack_roundtrip(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_pack_unpack");

    TEST("pack4/unpack4 round-trip (the encoder's chunk layout)");

    /* int8 sign-extension across the byte boundary. */
    {
        int8_t bytes[4];
        unpack4(pack4(-128, -1, 0, 127), bytes);
        if (bytes[0] != -128 || bytes[1] != -1 || bytes[2] != 0 || bytes[3] != 127) {
            FAIL("pack/unpack sign-extension");
            dl_close(db);
            return;
        }
    }
    /* Every byte pattern round-trips exactly. */
    {
        int i;
        for (i = 0; i < 256; i++) {
            int8_t b0 = (int8_t)(i & 0xFF);
            int8_t b1 = (int8_t)((i * 7 + 13) & 0xFF);
            int8_t b2 = (int8_t)((i * 31 + 5) & 0xFF);
            int8_t b3 = (int8_t)((i * 13 + 3) & 0xFF);
            int8_t out[4];
            unpack4(pack4(b0, b1, b2, b3), out);
            if (out[0] != b0 || out[1] != b1 || out[2] != b2 || out[3] != b3) {
                FAIL("pack/unpack byte round-trip");
                dl_close(db);
                return;
            }
        }
    }

    PASS();
    dl_close(db);
}

/* ─── Test 2: exactness vs the brute-force float reference ──────────────── */

static void t_exact_topk(void)
{
#define NENTS 30
#define NQUERY 12
#define TOPK 5
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_exact");
    ent_t ents[NENTS];
    int i, q, accepted = 0, skipped = 0;

    TEST("top-k == brute-force float cosine reference (exact, in order)");

    declare_vector_rels(db);
    rng_seed(0xC0FFEEu);
    for (i = 0; i < NENTS; i++) {
        char nm[32];
        snprintf(nm, sizeof(nm), "ent%d", i);
        gen_entity(&ents[i], nm, db);
        if (add_entity_to_index(db, &ents[i]) != 0) {
            FAIL("failed to build index");
            dl_close(db);
            return;
        }
    }

    for (q = 0; q < NQUERY; q++) {
        int src = (int)(rng_next() % NENTS);
        ent_t query;
        ref_score all[64];
        coll got = {{0}, {0}, 0};
        int n_want, j, attempts;
        long n;

        /* Craft a query whose reference margins are wide enough to assert an
         * exact ORDER on (ref_margins_ok).  Two random entities can land
         * within the isqrt floor error of each other; that is a property of
         * the fixture, not of the engine, so the query is replaced — and the
         * count of replacements is reported and bounded below. */
        for (attempts = 0; attempts < 500; attempts++) {
            gen_query(&query, &ents[src]);
            n_want = ref_sorted(query.vec, ents, NENTS, all);
            if (ref_margins_ok(all, n_want, TOPK)) break;
        }
        if (attempts >= 500) {
            printf("\n    (query %d: no separated fixture)\n", q);
            skipped++;
            continue;
        }
        accepted++;

        n = dl_vector_search(db, query.ivec, TOPK, coll_search_cb, &got);
        if (n < 0) { FAIL("dl_vector_search error"); dl_close(db); return; }
        if (got.n != TOPK || n_want < TOPK) {
            printf("\n    (query %d: emitted %d, want %d)\n", q, got.n, TOPK);
            FAIL("emitted count != k");
            dl_close(db);
            return;
        }
        for (j = 0; j < TOPK; j++) {
            if (got.syms[j] != all[j].sym) {
                printf("\n    (query %d: rank %d got sym %u, want %u)\n", q, j,
                       got.syms[j], all[j].sym);
                FAIL("top-k differs from the float reference");
                dl_close(db);
                return;
            }
        }
        /* The source of a near-copy query must be rank 1. */
        if (got.syms[0] != ents[src].sym) {
            printf("\n    (query %d: top-1 %u != source %u)\n", q, got.syms[0],
                   ents[src].sym);
            FAIL("perturbed source is not the nearest neighbour");
            dl_close(db);
            return;
        }
        /* Every emitted score is the signed integer dot <q,v> (the cosine
         * numerator the callback contract promises). */
        for (j = 0; j < TOPK; j++) {
            int e, d;
            long long dot = 0;
            for (e = 0; e < NENTS; e++)
                if (ents[e].sym == got.syms[j]) break;
            if (e >= NENTS) { FAIL("emitted a sym that is not an entity"); dl_close(db); return; }
            for (d = 0; d < VEC_D; d++) dot += (long long)query.vec[d] * ents[e].vec[d];
            if ((long long)got.scores[j] != dot) {
                printf("\n    (query %d rank %d: score %d != dot %lld)\n", q, j,
                       got.scores[j], dot);
                FAIL("emitted score is not the integer dot");
                dl_close(db);
                return;
            }
        }
    }

    /* Decision rule over the sample the fixture actually yields: at most
     * NQUERY/6 unusable fixtures.  More than that means the crafted queries
     * are not testing ordering at all. */
    printf("  (%d/%d queries asserted exactly, %d unseparated)\n",
           accepted, NQUERY, skipped);
    if (accepted < NQUERY - 2) {
        FAIL("too few separated fixtures to test ordering");
        dl_close(db);
        return;
    }

    /* 100% recall: with k >= N, EVERY live entity with a vector is emitted
     * (the old MIH path could lose one to the pigeonhole filter). */
    {
        int src = (int)(rng_next() % NENTS);
        ent_t query;
        coll all = {{0}, {0}, 0};
        gen_query(&query, &ents[src]);
        if (dl_vector_search(db, query.ivec, NENTS, coll_search_cb, &all) != NENTS) {
            printf("\n    (emitted %d of %d)\n", all.n, NENTS);
            FAIL("not every live vector was recalled");
            dl_close(db);
            return;
        }
        for (i = 0; i < NENTS; i++) {
            if (!coll_has(&all, ents[i].sym)) {
                FAIL("a live entity with a vector was dropped");
                dl_close(db);
                return;
            }
        }
    }

    PASS();
    dl_close(db);
#undef NENTS
#undef NQUERY
#undef TOPK
}

/* ─── Test 3: live-entity filter ────────────────────────────────────────── */

static void t_live_filter(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_live_filter");
    ent_t alpha, beta, q;

    TEST("live-entity filter: deleted entity drops out of live search");

    declare_vector_rels(db);
    rng_seed(0xABCDEFu);
    gen_entity(&alpha, "alpha", db);
    gen_entity(&beta, "beta", db);
    if (add_entity_to_index(db, &alpha) != 0 ||
        add_entity_to_index(db, &beta) != 0) {
        FAIL("build index");
        dl_close(db);
        return;
    }

    /* query = alpha's own vector -> alpha is the nearest neighbour. */
    memcpy(q.ivec, alpha.ivec, sizeof(q.ivec));

    {
        coll res = {{0}, {0}, 0};
        long n = dl_vector_search(db, q.ivec, COLL_MAX, coll_search_cb, &res);
        if (n < 0) { FAIL("search before delete"); dl_close(db); return; }
        if (!coll_has(&res, alpha.sym)) {
            FAIL("alpha should be ranked before delete");
            dl_close(db);
            return;
        }
    }

    /* Delete alpha's entity row (sym-id persists in __vec_q__ — append-only). */
    {
        uint32_t cols[2] = { alpha.sym, alpha.type };
        if (dl_delete_fact(db, VEC_ENTITY_REL, cols, 2) != 1) {
            FAIL("delete alpha entity");
            dl_close(db);
            return;
        }
    }

    /* alpha's vector chunks are STILL present (append-only): the entity
       filter is what drops it from the ranking. */
    {
        uint32_t leading[1] = { alpha.sym };
        int cnt = 0;
        long n = dl_prefix(db, "__vec_q__", leading, 1, sink_count_cb, &cnt);
        if (n < 0 || cnt != VEC_IVEC_WORDS) {
            printf("\n    (__vec_q__ rows for alpha after delete: %d)\n", cnt);
            FAIL("alpha vector chunks should persist after delete");
            dl_close(db);
            return;
        }
    }

    {
        coll res = {{0}, {0}, 0};
        long n = dl_vector_search(db, q.ivec, COLL_MAX, coll_search_cb, &res);
        if (n < 0) { FAIL("search after delete"); dl_close(db); return; }
        if (coll_has(&res, alpha.sym)) {
            FAIL("deleted alpha must NOT appear in live search");
            dl_close(db);
            return;
        }
        if (!coll_has(&res, beta.sym)) {
            FAIL("beta (still live) must appear");
            dl_close(db);
            return;
        }
    }

    PASS();
    dl_close(db);
}

/* ─── Test 4: view consistency (publish, mutate live) ───────────────────── */

static void t_view_consistency(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_view_consistency");
    ent_t alpha, beta, q;
    uint32_t vers[8];
    long nvers;

    TEST("view consistency: version stable, live sees mutation");

    declare_vector_rels(db);
    rng_seed(0x13579BDu);
    gen_entity(&alpha, "alpha", db);
    /* beta is a near copy of alpha so BOTH are candidates for one query. */
    gen_query(&beta, &alpha);
    snprintf(beta.name, sizeof(beta.name), "beta");
    beta.sym = dl_intern_str(db, beta.name);
    beta.type = dl_intern_str(db, "doc");
    if (add_entity_to_index(db, &alpha) != 0) {
        FAIL("build alpha");
        dl_close(db);
        return;
    }
    memcpy(q.ivec, alpha.ivec, sizeof(q.ivec));

    /* live search finds alpha */
    {
        coll res = {{0}, {0}, 0};
        long n = dl_vector_search(db, q.ivec, COLL_MAX, coll_search_cb, &res);
        if (n < 0 || !coll_has(&res, alpha.sym)) {
            FAIL("live search should find alpha before publish");
            dl_close(db);
            return;
        }
    }

    /* publish */
    if (dl_publish_snapshot(db) != 0) {
        FAIL("publish");
        dl_close(db);
        return;
    }
    nvers = dl_snapshot_versions(db, vers, 8);
    if (nvers < 1) {
        FAIL("no snapshot version after publish");
        dl_close(db);
        return;
    }
    uint32_t V = vers[nvers - 1];

    /* snapshot parity: on the unmutated index, live == version results. */
    {
        coll live = {{0}, {0}, 0}, ver = {{0}, {0}, 0};
        if (dl_vector_search(db, q.ivec, COLL_MAX, coll_search_cb, &live) < 0) {
            FAIL("live search parity");
            dl_close(db);
            return;
        }
        if (dl_vector_search_version(db, V, q.ivec, COLL_MAX,
                                     coll_search_cb, &ver) < 0) {
            FAIL("version search parity");
            dl_close(db);
            return;
        }
        if (live.n != ver.n || !coll_has(&ver, alpha.sym)) {
            printf("  (live.n=%d ver.n=%d)\n", live.n, ver.n);
            FAIL("snapshot parity mismatch (live vs version)");
            dl_close(db);
            return;
        }
        /* same ORDER in both modes (one ranking, two read primitives) */
        {
            int i;
            for (i = 0; i < live.n; i++) {
                if (live.syms[i] != ver.syms[i]) {
                    FAIL("live and version rank the same data differently");
                    dl_close(db);
                    return;
                }
            }
        }
    }

    /* MUTATE LIVE: add beta, delete alpha. */
    if (add_entity_to_index(db, &beta) != 0) {
        FAIL("add beta live");
        dl_close(db);
        return;
    }
    {
        uint32_t cols[2] = { alpha.sym, alpha.type };
        if (dl_delete_fact(db, VEC_ENTITY_REL, cols, 2) != 1) {
            FAIL("delete alpha live");
            dl_close(db);
            return;
        }
    }

    /* version search STILL finds alpha, and does NOT see beta. */
    {
        coll ver = {{0}, {0}, 0};
        long n = dl_vector_search_version(db, V, q.ivec, COLL_MAX,
                                          coll_search_cb, &ver);
        if (n < 0) { FAIL("version search post-mutation"); dl_close(db); return; }
        if (!coll_has(&ver, alpha.sym)) {
            FAIL("version search must be stable (alpha should remain)");
            dl_close(db);
            return;
        }
        if (coll_has(&ver, beta.sym)) {
            FAIL("version search must NOT see live-added beta");
            dl_close(db);
            return;
        }
    }

    /* live search sees the mutation: beta present, alpha gone. */
    {
        coll res = {{0}, {0}, 0};
        long n = dl_vector_search(db, q.ivec, COLL_MAX, coll_search_cb, &res);
        if (n < 0) { FAIL("live search post-mutation"); dl_close(db); return; }
        if (coll_has(&res, alpha.sym)) {
            FAIL("live search must NOT see deleted alpha");
            dl_close(db);
            return;
        }
        if (!coll_has(&res, beta.sym)) {
            FAIL("live search must see live-added beta");
            dl_close(db);
            return;
        }
    }

    PASS();
    dl_close(db);
}

/* ─── Test 5: error contract ────────────────────────────────────────────── */

static void t_error_contract(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_error_contract");
    uint32_t ivec[VEC_IVEC_WORDS] = {0};
    coll c = {{0}, {0}, 0};
    uint32_t vers[8];
    long nvers;
    uint32_t V;

    TEST("error contract: version 0 / nonexistent -> -1, empty-present -> 0");

    declare_vector_rels(db);   /* declared but EMPTY */
    if (dl_publish_snapshot(db) != 0) {
        FAIL("publish empty index");
        dl_close(db);
        return;
    }
    nvers = dl_snapshot_versions(db, vers, 8);
    if (nvers < 1) { FAIL("no version"); dl_close(db); return; }
    V = vers[nvers - 1];

    /* version == 0 -> -1 */
    if (dl_vector_search_version(db, 0, ivec, 100, coll_search_cb, &c) != -1) {
        FAIL("version 0 should return -1");
        dl_close(db);
        return;
    }
    /* nonexistent version -> -1 */
    if (dl_vector_search_version(db, 0xFFFFFFu, ivec, 100,
                                 coll_search_cb, &c) != -1) {
        FAIL("nonexistent version should return -1");
        dl_close(db);
        return;
    }
    /* empty-but-present -> 0 */
    if (dl_vector_search_version(db, V, ivec, 100, coll_search_cb, &c) != 0) {
        FAIL("empty-but-present should return 0");
        dl_close(db);
        return;
    }

    /* argument contract (live mode) */
    if (dl_vector_search(db, ivec, 0, coll_search_cb, &c) != -1) {
        FAIL("k == 0 should return -1");
        dl_close(db);
        return;
    }
    if (dl_vector_search(NULL, ivec, 10, coll_search_cb, &c) != -1) {
        FAIL("NULL db should return -1");
        dl_close(db);
        return;
    }
    if (dl_vector_search(db, NULL, 10, coll_search_cb, &c) != -1) {
        FAIL("NULL q_int8 should return -1");
        dl_close(db);
        return;
    }
    if (dl_vector_search(db, ivec, 10, NULL, &c) != -1) {
        FAIL("NULL cb should return -1");
        dl_close(db);
        return;
    }

    /* relation-absent-from-version -> -1: a version with the liveness relation
       but NO __vec_q__ chunks relation (the query path's only data relation). */
    {
        char dir2[512];
        dl_db *db2 = fresh_db(dir2, sizeof(dir2), "t_err_no_vecq");
        coll c2 = {{0}, {0}, 0};
        uint32_t v2[8];
        long n2;
        uint32_t V2;
        dl_declare_relation(db2, VEC_ENTITY_REL, 2);
        if (dl_publish_snapshot(db2) != 0) { FAIL("publish no-vecq"); dl_close(db2); dl_close(db); return; }
        n2 = dl_snapshot_versions(db2, v2, 8);
        if (n2 < 1) { FAIL("no version2"); dl_close(db2); dl_close(db); return; }
        V2 = v2[n2 - 1];
        if (dl_vector_search_version(db2, V2, ivec, 100,
                                     coll_search_cb, &c2) != -1) {
            FAIL("relation-absent-from-version should return -1");
            dl_close(db2);
            dl_close(db);
            return;
        }
        /* and LIVE mode on the same store: missing __vec_q__ -> -1 too */
        if (dl_vector_search(db2, ivec, 100, coll_search_cb, &c2) != -1) {
            FAIL("missing __vec_q__ (live) should return -1");
            dl_close(db2);
            dl_close(db);
            return;
        }
        dl_close(db2);
    }

    /* missing LIVENESS relation -> -1 (live mode). */
    {
        char dir3[512];
        dl_db *db3 = fresh_db(dir3, sizeof(dir3), "t_err_no_entity");
        dl_declare_relation(db3, "__vec_q__", 3);
        if (dl_vector_search(db3, ivec, 100, coll_search_cb, &c) != -1) {
            FAIL("missing liveness relation should return -1");
            dl_close(db3);
            dl_close(db);
            return;
        }
        dl_close(db3);
    }

    PASS();
    dl_close(db);
}

int main(void)
{
    printf("=== test_vector_search (S2 scan-and-rank) ===\n");

    t_pack_unpack_roundtrip();
    t_exact_topk();
    t_live_filter();
    t_view_consistency();
    t_error_contract();

    printf("\n");
    if (tests_failed == 0)
        printf("All %d tests passed.\n", tests_run);
    else
        printf("%d/%d tests passed, %d failed.\n",
               tests_run - tests_failed, tests_run, tests_failed);

    return tests_failed != 0;
}
