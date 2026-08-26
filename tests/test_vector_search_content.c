/*
 * test_vector_search_content.c — S2 content-corpus query-path tests
 *
 * Exercises the corpus-parameterized entry points:
 *   dl_vector_search_corpus / dl_vector_rerank_corpus with
 *   DL_VEC_CORPUS_OBSERVATION_CONTENT (liveness = observation col 1, sig =
 *   __obssig{j}__, vec = __vec_obs__).  Verifies that:
 *   - content vectors are searched under the corpus namespaced relations
 *   - the liveness filter uses col 1 of `observation` (content sym)
 *   - empty content corpus -> 0 candidates
 *   - entity and content corpora do not cross-contaminate
 */
#include "dl.h"
#include "vector.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0;
static int tests_failed = 0;

#define TEST(name) do { tests_run++; \
    printf("  %s ... ", name); fflush(stdout); } while (0)
#define PASS() do { printf("OK\n"); } while (0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); tests_failed++; } while (0)

static const char *BASE = "build-tmp/vector-content";

static dl_db *fresh_db(char *dir_out, size_t cap, const char *name)
{
    char cmd[512];
    snprintf(dir_out, cap, "%s/%s", BASE, name);
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir_out);
    system(cmd);
    snprintf(cmd, sizeof cmd, "mkdir -p %s", dir_out);
    system(cmd);
    return dl_open(dir_out);
}

/* ─── Band layout (C1 formula — MUST match vector.c / embed.py) ─────────── */
static uint32_t band_slice(const uint32_t *sig, int j)
{
    return (sig[j / 2] >> ((1u - (j % 2u)) * 16u)) & 0xFFFFu;
}

static uint32_t pack4(int8_t b0, int8_t b1, int8_t b2, int8_t b3)
{
    return (uint32_t)(uint8_t)b0 | ((uint32_t)(uint8_t)b1 << 8) |
           ((uint32_t)(uint8_t)b2 << 16) | ((uint32_t)(uint8_t)b3 << 24);
}

static void declare_content_rels(dl_db *db)
{
    int j;
    dl_declare_relation(db, "observation", 2);   /* entity_sym, content_sym */
    dl_declare_relation(db, "__vec_obs__", 3);
    for (j = 0; j < VEC_M; j++) {
        char rel[32];
        snprintf(rel, sizeof rel, "__obssig%d__", j);
        dl_declare_relation(db, rel, 2);
    }
}

static void add_obs(dl_db *db, uint32_t entity_sym, uint32_t content_sym)
{
    uint32_t cols[2] = { entity_sym, content_sym };
    dl_add_fact(db, "observation", cols, 2);
}

/* One content item: a vector + its signature, stored under the content rels. */
static void add_content_ivec(dl_db *db, uint32_t content_sym,
                             const uint32_t *sig, const int8_t *vec)
{
    int j, c;
    for (j = 0; j < VEC_M; j++) {
        char rel[32];
        uint32_t b[2];
        snprintf(rel, sizeof rel, "__obssig%d__", j);
        b[0] = band_slice(sig, j);
        b[1] = content_sym;
        dl_add_fact(db, rel, b, 2);
    }
    for (c = 0; c < VEC_IVEC_WORDS; c++) {
        uint32_t cols[3];
        cols[0] = content_sym;
        cols[1] = (uint32_t)c;
        cols[2] = pack4(vec[c*4+0], vec[c*4+1], vec[c*4+2], vec[c*4+3]);
        dl_add_fact(db, "__vec_obs__", cols, 3);
    }
}

/* deterministic vector that is highly self-similar (band 0 = 0xAAAA). */
static void fill_vec(uint32_t seed, int8_t *vec)
{
    int d;
    for (d = 0; d < VEC_D; d++)
        vec[d] = (int8_t)(int)(((seed * 2654435761u) ^ (uint32_t)d * 40503u) % 127u) - 63;
}

static void fill_sig(uint32_t band0, uint32_t *sig)
{
    int j;
    memset(sig, 0, (size_t)VEC_SIG_WORDS * sizeof(uint32_t));
    sig[0] = band0 << 16;   /* band 0 = high 16 of sig[0] */
    for (j = 1; j < VEC_M; j++) {
        uint32_t v = (uint32_t)(0x100 + j);
        sig[j / 2] |= v << ((1u - (j % 2u)) * 16u);
    }
}

#define COLL_MAX 4096
typedef struct { uint32_t syms[COLL_MAX]; int n; } coll;
static int coll_cb(uint32_t sym, int score, void *user)
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

/* ─── content search finds content syms under observation liveness ─────── */

static void t_content_search(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof dir, "t_content");
    uint32_t ent = dl_intern_str(db, "ent0");
    uint32_t c1 = dl_intern_str(db, "content one");
    uint32_t c2 = dl_intern_str(db, "content two");
    uint32_t qsig[VEC_SIG_WORDS];
    int8_t vec[VEC_D];
    struct dl_vec_corpus corpus = DL_VEC_CORPUS_OBSERVATION_CONTENT;

    TEST("content corpus search returns content syms");

    declare_content_rels(db);
    fill_vec(1, vec);
    fill_sig(0xAAAA, qsig);           /* matches band 0 of both items */
    fill_vec(2, vec);
    add_obs(db, ent, c1);
    add_obs(db, ent, c2);
    add_content_ivec(db, c1, qsig, vec);
    add_content_ivec(db, c2, qsig, vec);

    {
        coll res = {{0}, 0};
        long n = dl_vector_search_corpus(db, &corpus, qsig, COLL_MAX, 16,
                                         coll_cb, &res);
        if (n < 0) { FAIL("content search error"); dl_close(db); return; }
        if (!coll_has(&res, c1) || !coll_has(&res, c2)) {
            FAIL("content search missed live content syms");
            dl_close(db);
            return;
        }
        if (coll_has(&res, ent)) {
            FAIL("entity sym must NOT surface in content corpus");
            dl_close(db);
            return;
        }
    }

    PASS();
    dl_close(db);
}

/* ─── liveness filter: delete the observation -> content drops out ──────── */

static void t_liveness(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof dir, "t_liveness");
    uint32_t ent = dl_intern_str(db, "ent0");
    uint32_t c1 = dl_intern_str(db, "content one");
    uint32_t qsig[VEC_SIG_WORDS];
    int8_t vec[VEC_D];
    struct dl_vec_corpus corpus = DL_VEC_CORPUS_OBSERVATION_CONTENT;

    TEST("content liveness: deleting the observation drops the content");

    declare_content_rels(db);
    fill_vec(1, vec);
    fill_sig(0xBBBB, qsig);
    add_obs(db, ent, c1);
    add_content_ivec(db, c1, qsig, vec);

    /* delete the observation row (content sym persists in obssig/vec_obs) */
    {
        uint32_t cols[2] = { ent, c1 };
        if (dl_delete_fact(db, "observation", cols, 2) != 1) {
            FAIL("delete observation");
            dl_close(db);
            return;
        }
    }
    {
        coll res = {{0}, 0};
        long n = dl_vector_search_corpus(db, &corpus, qsig, COLL_MAX, 16,
                                         coll_cb, &res);
        if (n < 0) { FAIL("content search after delete"); dl_close(db); return; }
        if (coll_has(&res, c1)) {
            FAIL("deleted observation content must NOT appear");
            dl_close(db);
            return;
        }
    }

    PASS();
    dl_close(db);
}

/* ─── empty content corpus -> 0 candidates (no error) ───────────────────── */

static void t_empty(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof dir, "t_empty");
    uint32_t qsig[VEC_SIG_WORDS] = {0};
    struct dl_vec_corpus corpus = DL_VEC_CORPUS_OBSERVATION_CONTENT;

    TEST("empty content corpus -> 0 candidates");

    declare_content_rels(db);         /* declared but no observation rows */
    {
        coll res = {{0}, 0};
        long n = dl_vector_search_corpus(db, &corpus, qsig, COLL_MAX, 16,
                                         coll_cb, &res);
        if (n < 0) { FAIL("empty content search errored"); dl_close(db); return; }
        if (n != 0 || res.n != 0) {
            FAIL("empty content corpus should yield 0 candidates");
            dl_close(db);
            return;
        }
    }

    PASS();
    dl_close(db);
}

/* ─── entity vs content isolation: same band value does not mix ─────────── */

static void t_isolation(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof dir, "t_isol");
    uint32_t ent = dl_intern_str(db, "entity_name");
    uint32_t c1 = dl_intern_str(db, "content_text");
    int8_t vec[VEC_D];
    struct dl_vec_corpus oc = DL_VEC_CORPUS_OBSERVATION_CONTENT;
    struct dl_vec_corpus ec = DL_VEC_CORPUS_ENTITY;

    TEST("entity/content corpora do not cross-contaminate a shared band value");

    /* entity path: __sig0__ + __vec_q__ + entity relation */
    dl_declare_relation(db, "entity", 2);
    dl_declare_relation(db, "__vec_q__", 3);
    for (int j = 0; j < VEC_M; j++) {
        char rel[32];
        snprintf(rel, sizeof rel, "__sig%d__", j);
        dl_declare_relation(db, rel, 2);
    }
    declare_content_rels(db);
    {
        int j, c;
        dl_add_fact(db, "entity", (uint32_t[]){ ent, dl_intern_str(db, "doc") }, 2);
        fill_vec(1, vec);
        for (j = 0; j < VEC_M; j++) {
            char rel[32]; uint32_t b[2];
            snprintf(rel, sizeof rel, "__sig%d__", j);
            b[0] = 0x1111; b[1] = ent;
            dl_add_fact(db, rel, b, 2);
        }
        for (c = 0; c < VEC_IVEC_WORDS; c++)
            dl_add_fact(db, "__vec_q__", (uint32_t[]){ ent, (uint32_t)c, 0x7F7F7F7Fu }, 3);
    }

    /* content path: __obssig*__ + __vec_obs__ + observation, SAME band value */
    {
        int j, c;
        uint32_t csig[VEC_SIG_WORDS];
        fill_sig(0x1111, csig);
        fill_vec(3, vec);
        add_obs(db, dl_intern_str(db, "some_ent"), c1);
        for (j = 0; j < VEC_M; j++) {
            char rel[32]; uint32_t b[2];
            snprintf(rel, sizeof rel, "__obssig%d__", j);
            b[0] = band_slice(csig, j);
            b[1] = c1;
            dl_add_fact(db, rel, b, 2);
        }
        for (c = 0; c < VEC_IVEC_WORDS; c++)
            dl_add_fact(db, "__vec_obs__", (uint32_t[]){ c1, (uint32_t)c, 0x01010101u }, 3);
    }

    /* A query with band 0 = 0x1111 matches BOTH postings; the entity corpus
       must return only ent and the content corpus only c1. */
    {
        uint32_t qsig[VEC_SIG_WORDS] = {0};
        coll er = {{0}, 0}, cr = {{0}, 0};
        fill_sig(0x1111, qsig);
        if (dl_vector_search_corpus(db, &ec, qsig, COLL_MAX, 16, coll_cb, &er) < 0 ||
            dl_vector_search_corpus(db, &oc, qsig, COLL_MAX, 16, coll_cb, &cr) < 0) {
            FAIL("corpus search error");
            dl_close(db);
            return;
        }
        if (coll_has(&er, ent) != 1) {
            FAIL("entity corpus should find the entity sym");
            dl_close(db);
            return;
        }
        if (coll_has(&er, c1)) {
            FAIL("entity corpus leaked the content sym");
            dl_close(db);
            return;
        }
        if (coll_has(&cr, c1) != 1) {
            FAIL("content corpus should find the content sym");
            dl_close(db);
            return;
        }
        if (coll_has(&cr, ent)) {
            FAIL("content corpus leaked the entity sym");
            dl_close(db);
            return;
        }
    }

    PASS();
    dl_close(db);
}

int main(void)
{
    printf("=== test_vector_search_content (S2 content corpus) ===\n");
    t_content_search();
    t_liveness();
    t_empty();
    t_isolation();
    printf("\n");
    if (tests_failed == 0) printf("All %d tests passed.\n", tests_run);
    else printf("%d/%d tests passed, %d failed.\n",
                tests_run - tests_failed, tests_run, tests_failed);
    return tests_failed != 0;
}
