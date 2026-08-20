/*
 * test_vector_search.c — S2 vector tier query-path tests
 *
 * Exercises dl_vector_search / dl_vector_search_version / dl_vector_rerank:
 *   - pack/unpack + band round-trip (C1 gate)
 *   - MIH recall (probabilistic, seed-fixed)
 *   - live-entity filter (delete an entity -> it drops out of search)
 *   - view consistency (publish, mutate live: version stable + live sees
 *     mutation — the test that catches a live/version read mix)
 *   - snapshot parity (live results == version results on an unmutated index)
 *   - error contract (version == 0 / nonexistent / relation-absent -> -1,
 *     empty-but-present -> 0)
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

/* ─── Band layout (C1 formula — MUST match vector.c / embed.py) ─────────── */

static uint32_t band_slice(const uint32_t *sig, int j)
{
    return (sig[j / 2] >> ((1u - (j % 2u)) * 16u)) & 0xFFFFu;
}

static void band_set(uint32_t *sig, int j, uint32_t val16)
{
    int shift = (int)((1u - (j % 2u)) * 16u);
    sig[j / 2] = (sig[j / 2] & ~(0xFFFFu << shift)) | ((val16 & 0xFFFFu) << shift);
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

/* ─── Signature: sign of random ±1 projections (256 bits -> 8 u32) ──────── */

static int8_t P[VEC_C][VEC_D];

static void init_projection(void)
{
    static int done = 0;
    int b, d;
    if (done) return;
    rng_seed(0x5EED1234u);
    for (b = 0; b < VEC_C; b++)
        for (d = 0; d < VEC_D; d++)
            P[b][d] = (rng_next() & 1u) ? 1 : -1;
    done = 1;
}

/* Compute the 256-bit signature of an int8 vector, packed MSB-first so band
   j == bits [16j, 16j+15] (band 0 = high 16 of sig[0]). */
static void sig_of(const int8_t *vec, uint32_t *sig)
{
    int b, d;
    memset(sig, 0, (size_t)VEC_SIG_WORDS * sizeof(uint32_t));
    for (b = 0; b < VEC_C; b++) {
        int32_t acc = 0;
        for (d = 0; d < VEC_D; d++) acc += P[b][d] * vec[d];
        if (acc > 0) sig[b / 32] |= (1u << (31 - (b % 32)));
    }
}

static int sig_hamming(const uint32_t *a, const uint32_t *b)
{
    int h = 0, w;
    for (w = 0; w < VEC_SIG_WORDS; w++) h += __builtin_popcount(a[w] ^ b[w]);
    return h;
}

/* ─── Entity model ──────────────────────────────────────────────────────── */

typedef struct {
    char name[32];
    uint32_t sym, type;
    int8_t vec[VEC_D];
    uint32_t ivec[VEC_IVEC_WORDS];
    uint32_t sig[VEC_SIG_WORDS];
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
    sig_of(e->vec, e->sig);
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
    sig_of(q->vec, q->sig);
}

/* ─── Index construction ────────────────────────────────────────────────── */

static void declare_vector_rels(dl_db *db)
{
    int j;
    dl_declare_relation(db, VEC_ENTITY_REL, 2);
    dl_declare_relation(db, "__vec_q__", 3);
    for (j = 0; j < VEC_M; j++) {
        char rel[32];
        snprintf(rel, sizeof(rel), "__sig%d__", j);
        dl_declare_relation(db, rel, 2);
    }
}

static int add_entity_to_index(dl_db *db, const ent_t *e)
{
    uint32_t cols[3];
    int j, c;
    cols[0] = e->sym;
    cols[1] = e->type;
    if (dl_add_fact(db, VEC_ENTITY_REL, cols, 2) != 1) return -1;
    for (j = 0; j < VEC_M; j++) {
        char rel[32];
        uint32_t b[2];
        snprintf(rel, sizeof(rel), "__sig%d__", j);
        b[0] = band_slice(e->sig, j);
        b[1] = e->sym;
        if (dl_add_fact(db, rel, b, 2) != 1) return -1;
    }
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
typedef struct { uint32_t syms[COLL_MAX]; int n; } coll;

static int coll_search_cb(uint32_t sym, int score, void *user)
{
    coll *c = user;
    (void)score;
    if (c->n < COLL_MAX) c->syms[c->n++] = sym;
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

/* Exact-cosine nearest neighbour (test-side; float is fine in tests). */
static int true_nn(const int8_t *q, const ent_t *ents, int n)
{
    int best = -1, i, d;
    double bestcos = -2.0;
    for (i = 0; i < n; i++) {
        double dot = 0, nq = 0, nv = 0, c;
        for (d = 0; d < VEC_D; d++) {
            dot += (double)q[d] * ents[i].vec[d];
            nq += (double)q[d] * q[d];
            nv += (double)ents[i].vec[d] * ents[i].vec[d];
        }
        c = (nq > 0 && nv > 0) ? dot / (sqrt(nq) * sqrt(nv)) : 0.0;
        if (c > bestcos) { bestcos = c; best = i; }
    }
    return best;
}

/* ─── Test 1: pack/unpack + band round-trip (C1 gate) ──────────────────── */

static void t_pack_unpack_roundtrip(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_pack_unpack");

    TEST("pack4/unpack4 round-trip + band round-trip (C1)");

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
    /* Band slice/set round-trip: distinct 16-bit value per band. */
    {
        uint32_t sig[VEC_SIG_WORDS] = {0};
        uint32_t expected[VEC_M];
        int j;
        for (j = 0; j < VEC_M; j++) {
            expected[j] = (uint32_t)j * 0x111u + 1u;
            band_set(sig, j, expected[j]);
        }
        for (j = 0; j < VEC_M; j++)
            if (band_slice(sig, j) != expected[j]) {
                FAIL("band_slice mismatch");
                dl_close(db);
                return;
            }
    }

    PASS();
    dl_close(db);
}

/* ─── Test 2: MIH recall (probabilistic, seed-fixed) ───────────────────── */

static void t_recall(void)
{
#define NENTS 30
#define NQUERY 12
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_recall");
    ent_t ents[NENTS];
    int i, recalled = 0, top1 = 0;

    TEST("MIH recall + int8 rerank top-1 (seed-fixed)");

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

    for (i = 0; i < NQUERY; i++) {
        int src = (int)(rng_next() % NENTS);
        ent_t q;
        int attempts;
        /* Craft a query that is a near copy of src so MIH recall and rerank
         * top-1 are DETERMINISTIC: within the pigeonhole radius AND src is
         * the exact-cosine NN.  Deterministic because the RNG is seeded. */
        for (attempts = 0; attempts < 500; attempts++) {
            gen_query(&q, &ents[src]);
            if (sig_hamming(q.sig, ents[src].sig) <= 16 &&
                true_nn(q.vec, ents, NENTS) == src)
                break;
        }
        if (attempts >= 500) {
            FAIL("could not craft a guaranteed-recall query");
            dl_close(db);
            return;
        }

        /* retrieval (radius 16 -> budget 1 per band) */
        {
            coll res = {{0}, 0};
            long n = dl_vector_search(db, q.sig, COLL_MAX, 16, coll_search_cb, &res);
            if (n < 0) { FAIL("dl_vector_search error"); dl_close(db); return; }
            if (!coll_has(&res, ents[src].sym)) {
                printf("  (query %d missed source %d)\n", i, src);
                FAIL("MIH retrieval missed the true source");
                dl_close(db);
                return;
            }
            recalled++;
            /* rerank top-1 must be the source */
            {
                coll rk = {{0}, 0};
                long r = dl_vector_rerank(db, q.ivec, res.syms, res.n, 1,
                                          coll_search_cb, &rk);
                if (r < 0) { FAIL("dl_vector_rerank error"); dl_close(db); return; }
                if (rk.n > 0 && rk.syms[0] == ents[src].sym) top1++;
                else {
                    printf("  (query %d rerank top-1 != source %d)\n", i, src);
                    FAIL("rerank top-1 != source");
                    dl_close(db);
                    return;
                }
            }
        }
    }

    printf("  (recall %d/%d, top1 %d/%d)\n", recalled, NQUERY, top1, NQUERY);
    if (recalled != NQUERY || top1 != NQUERY) {
        FAIL("recall/top-1 not perfect");
        dl_close(db);
        return;
    }
    PASS();
    dl_close(db);
#undef NENTS
#undef NQUERY
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

    /* query = alpha's own signature/vector -> alpha is definitely a match. */
    memcpy(q.sig, alpha.sig, sizeof(q.sig));
    memcpy(q.ivec, alpha.ivec, sizeof(q.ivec));

    {
        coll res = {{0}, 0};
        long n = dl_vector_search(db, q.sig, COLL_MAX, 16, coll_search_cb, &res);
        if (n < 0) { FAIL("search before delete"); dl_close(db); return; }
        if (!coll_has(&res, alpha.sym)) {
            FAIL("alpha should be a candidate before delete");
            dl_close(db);
            return;
        }
    }

    /* Delete alpha's entity row (sym-id persists in sig_j — append-only). */
    {
        uint32_t cols[2] = { alpha.sym, alpha.type };
        if (dl_delete_fact(db, VEC_ENTITY_REL, cols, 2) != 1) {
            FAIL("delete alpha entity");
            dl_close(db);
            return;
        }
    }

    /* alpha's sig postings are STILL present (dead entity persists in the
       append-only interner/sig_j — the entity filter is what drops it). */
    {
        uint32_t leading[1] = { band_slice(alpha.sig, 0) };
        int cnt = 0;
        long n = dl_prefix(db, "__sig0__", leading, 1, sink_count_cb, &cnt);
        if (n < 0 || cnt == 0) {
            FAIL("alpha sig0 postings should persist after delete");
            dl_close(db);
            return;
        }
    }

    {
        coll res = {{0}, 0};
        long n = dl_vector_search(db, q.sig, COLL_MAX, 16, coll_search_cb, &res);
        if (n < 0) { FAIL("search after delete"); dl_close(db); return; }
        if (coll_has(&res, alpha.sym)) {
            FAIL("deleted alpha must NOT appear in live search");
            dl_close(db);
            return;
        }
        /* beta (unrelated but present in the band set) may or may not be a
         * candidate; what matters is alpha is gone. */
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
    {
        int attempts;
        for (attempts = 0; attempts < 500; attempts++) {
            gen_query(&beta, &alpha);
            if (sig_hamming(beta.sig, alpha.sig) <= 16) break;
        }
        if (attempts >= 500) {
            FAIL("could not craft a beta close to alpha");
            dl_close(db);
            return;
        }
        snprintf(beta.name, sizeof(beta.name), "beta");
        beta.sym = dl_intern_str(db, beta.name);
        beta.type = dl_intern_str(db, "doc");
    }
    if (add_entity_to_index(db, &alpha) != 0) {
        FAIL("build alpha");
        dl_close(db);
        return;
    }
    memcpy(q.sig, alpha.sig, sizeof(q.sig));
    memcpy(q.ivec, alpha.ivec, sizeof(q.ivec));

    /* live search finds alpha */
    {
        coll res = {{0}, 0};
        long n = dl_vector_search(db, q.sig, COLL_MAX, 16, coll_search_cb, &res);
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
        coll live = {{0}, 0}, ver = {{0}, 0};
        if (dl_vector_search(db, q.sig, COLL_MAX, 16, coll_search_cb, &live) < 0) {
            FAIL("live search parity");
            dl_close(db);
            return;
        }
        if (dl_vector_search_version(db, V, q.sig, COLL_MAX, 16,
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
    }

    /* version search finds alpha */
    {
        coll ver = {{0}, 0};
        long n = dl_vector_search_version(db, V, q.sig, COLL_MAX, 16,
                                          coll_search_cb, &ver);
        if (n < 0 || !coll_has(&ver, alpha.sym)) {
            FAIL("version search should find alpha");
            dl_close(db);
            return;
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
        coll ver = {{0}, 0};
        long n = dl_vector_search_version(db, V, q.sig, COLL_MAX, 16,
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
        coll res = {{0}, 0};
        long n = dl_vector_search(db, q.sig, COLL_MAX, 16, coll_search_cb, &res);
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
    uint32_t zeros[VEC_SIG_WORDS] = {0};
    coll c = {{0}, 0};
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
    if (dl_vector_search_version(db, 0, zeros, 100, 16, coll_search_cb, &c) != -1) {
        FAIL("version 0 should return -1");
        dl_close(db);
        return;
    }
    /* nonexistent version -> -1 */
    if (dl_vector_search_version(db, 0xFFFFFFu, zeros, 100, 16,
                                 coll_search_cb, &c) != -1) {
        FAIL("nonexistent version should return -1");
        dl_close(db);
        return;
    }
    /* empty-but-present -> 0 */
    if (dl_vector_search_version(db, V, zeros, 100, 16, coll_search_cb, &c) != 0) {
        FAIL("empty-but-present should return 0");
        dl_close(db);
        return;
    }

    /* relation-absent-from-version -> -1: a version with entity + __vec_q__
       but NO __sig0__..__sig15__. */
    {
        char dir2[512];
        dl_db *db2 = fresh_db(dir2, sizeof(dir2), "t_err_no_sig");
        coll c2 = {{0}, 0};
        uint32_t v2[8];
        long n2;
        uint32_t V2;
        dl_declare_relation(db2, VEC_ENTITY_REL, 2);
        dl_declare_relation(db2, "__vec_q__", 3);
        if (dl_publish_snapshot(db2) != 0) { FAIL("publish no-sig"); dl_close(db2); dl_close(db); return; }
        n2 = dl_snapshot_versions(db2, v2, 8);
        if (n2 < 1) { FAIL("no version2"); dl_close(db2); dl_close(db); return; }
        V2 = v2[n2 - 1];
        if (dl_vector_search_version(db2, V2, zeros, 100, 16,
                                     coll_search_cb, &c2) != -1) {
            FAIL("relation-absent-from-version should return -1");
            dl_close(db2);
            dl_close(db);
            return;
        }
        dl_close(db2);
    }

    PASS();
    dl_close(db);
}

int main(void)
{
    printf("=== test_vector_search (S2) ===\n");

    init_projection();

    t_pack_unpack_roundtrip();
    t_recall();
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
