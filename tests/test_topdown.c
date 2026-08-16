/*
 * test_topdown.c — top-down / QSQ evaluator tests (STAGE A + B)
 *
 * Correctness backstop (non-negotiable):
 *   dl_query_topdown(...)        == dl_query_bound(...)      (full-materialize
 *                                                             filter), and
 *   dl_query_topdown(...)        == dl_query_magic(...)      (equivalence), and
 *   dl_query_topdown_adorn(...)  == dl_query_magic_adorn(...) (equivalence),
 * byte-for-byte sorted unique tuple-set equality.
 *
 * STAGE A: single-goal leading-prefix self-recursive TC (chain/star/dense/
 *          absent/EDB-goal/equality/multi-rule) + 200-random-graph property.
 * STAGE B: multi-predicate closures, non-leading adornments (fb/fbf),
 *          multi-variant (bf/bb/fb).  STAGE C (negation/aggregates) deferred.
 *
 * Also: chain N=10000 single-source stack-safety check (iterative driver,
 *       returns N-1 tuples), and an HONEST benchmark vs dl_query_magic_adorn.
 */
#include "dl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <time.h>

static int tests_run = 0;
static int tests_failed = 0;

#define TEST(name) do { \
    tests_run++; \
    printf("  %s ... ", name); \
    fflush(stdout); \
} while(0)

#define PASS() do { printf("OK\n"); } while(0)
#define FAIL(msg) do { \
    printf("FAIL: %s\n", msg); \
    tests_failed++; \
} while(0)

/* ─── Tuple set (mirrors test_m8_magic) ────────────────────────────────── */

typedef struct {
    uint32_t *data;
    long      count;
    long      cap;
    uint8_t   arity;
} tuple_set;

static int tset_cb(const uint32_t *cols, uint8_t arity, void *user)
{
    tuple_set *ts = (tuple_set *)user;
    if (ts->arity == 0) ts->arity = arity;
    if (ts->count >= ts->cap) {
        long nc = ts->cap ? ts->cap * 2 : 256;
        uint32_t *nd = realloc(ts->data,
            (size_t)nc * (size_t)ts->arity * sizeof(uint32_t));
        if (!nd) return 1;
        ts->data = nd;
        ts->cap = nc;
    }
    memcpy(ts->data + (size_t)ts->count * (size_t)ts->arity,
           cols, (size_t)ts->arity * sizeof(uint32_t));
    ts->count++;
    return 0;
}

static void tset_free(tuple_set *ts)
{
    free(ts->data);
    memset(ts, 0, sizeof(*ts));
}

static int g_arity;
static int cmp_tup(const void *a, const void *b)
{
    const uint32_t *pa = (const uint32_t *)a;
    const uint32_t *pb = (const uint32_t *)b;
    int i;
    for (i = 0; i < g_arity; i++) {
        if (pa[i] < pb[i]) return -1;
        if (pa[i] > pb[i]) return 1;
    }
    return 0;
}

static void tset_sort(tuple_set *ts)
{
    if (ts->count > 1) {
        g_arity = ts->arity;
        qsort(ts->data, (size_t)ts->count,
              (size_t)ts->arity * sizeof(uint32_t), cmp_tup);
    }
}

static int tset_sorted_eq(tuple_set *a, tuple_set *b)
{
    if (a->count != b->count) return 0;
    if (a->count == 0) return 1;
    if (a->arity != b->arity) return 0;
    tset_sort(a);
    tset_sort(b);
    return memcmp(a->data, b->data,
                  (size_t)a->count * (size_t)a->arity * sizeof(uint32_t)) == 0;
}

/* ─── Database helpers (mirrors test_m8_magic) ─────────────────────────── */

static void rm_dir(const char *dir)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    system(cmd);
}

static void setup_db(dl_db **db_out, const char *suffix)
{
    char path[256];
    snprintf(path, sizeof(path), "build-tmp/tddb_%s", suffix);
    rm_dir(path);
    *db_out = dl_open(path);
    assert(*db_out);
}

static void teardown_db(dl_db *db, const char *suffix)
{
    char path[256];
    dl_close(db);
    snprintf(path, sizeof(path), "build-tmp/tddb_%s", suffix);
    rm_dir(path);
}

static int load_rows(dl_db *db, const char *rel_name, uint8_t arity,
                     const uint32_t *cols, int nrows, const char *suffix)
{
    char csv_path[256];
    FILE *f;
    int i, c;

    assert(dl_declare_relation(db, rel_name, arity) == 0);
    snprintf(csv_path, sizeof(csv_path), "build-tmp/tdcsv_%s_%s.csv",
             suffix, rel_name);
    f = fopen(csv_path, "w");
    assert(f);
    for (i = 0; i < nrows; i++) {
        for (c = 0; c < arity; c++) {
            if (c > 0) fputc(',', f);
            fprintf(f, "%u", cols[(size_t)i * (size_t)arity + (size_t)c]);
        }
        fputc('\n', f);
    }
    fclose(f);
    return dl_load_facts(db, rel_name, csv_path);
}

static void load_tc_rules(dl_db *db)
{
    assert(dl_load_rules(db,
        "tc(X,Y):-edge(X,Y).\n"
        "tc(X,Y):-edge(X,Z),tc(Z,Y).\n") == 0);
}

static uint32_t rng_state = 0x9E3779B9u;
static uint32_t rng_next(void)
{
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    rng_state = x;
    return x;
}
static uint32_t rng_rand(uint32_t n) { return n ? rng_next() % n : 0; }

/* No-op collector for the benchmark (the query still evaluates fully). */
static int noop_cb(const uint32_t *cols, uint8_t arity, void *user)
{
    (void)cols; (void)arity; (void)user;
    return 0;
}

static int cmp_bound_topdown_rel(dl_db *db, const char *rel, uint32_t src);

/* ─── Comparators ──────────────────────────────────────────────────────── */

/* dl_query_bound vs dl_query_topdown for tc(src, ?). */
static int cmp_bound_topdown(dl_db *db, uint32_t src)
{
    tuple_set rb, rt;
    uint32_t leading[1];
    long nb, nt;
    int eq;

    memset(&rb, 0, sizeof(rb));
    memset(&rt, 0, sizeof(rt));
    leading[0] = src;

    nb = dl_query_bound(db, "tc", leading, 1, tset_cb, &rb);
    nt = dl_query_topdown(db, "tc", leading, 1, tset_cb, &rt);

    if (nb < 0 || nt < 0) { tset_free(&rb); tset_free(&rt); return 0; }
    if (nb != nt) { tset_free(&rb); tset_free(&rt); return 0; }
    eq = tset_sorted_eq(&rb, &rt);
    tset_free(&rb);
    tset_free(&rt);
    return eq;
}

/* dl_query_topdown vs dl_query_magic for a generic arity-2 goal rel(src,?). */
static int cmp_topdown_magic_rel(dl_db *db, const char *rel, uint32_t src)
{
    tuple_set rt, rm;
    uint32_t leading[1];
    long nt, nm;
    int eq;

    memset(&rt, 0, sizeof(rt));
    memset(&rm, 0, sizeof(rm));
    leading[0] = src;

    nt = dl_query_topdown(db, rel, leading, 1, tset_cb, &rt);
    nm = dl_query_magic(db, rel, leading, 1, tset_cb, &rm);

    if (nt < 0 || nm < 0) { tset_free(&rt); tset_free(&rm); return 0; }
    if (nt != nm) { tset_free(&rt); tset_free(&rm); return 0; }
    eq = tset_sorted_eq(&rt, &rm);
    tset_free(&rt);
    tset_free(&rm);
    return eq;
}

/* Filter a materialized tuple_set on the bound positions of `adorn`. */
static int tset_filter_adorn(const tuple_set *src, const char *adorn,
                             const uint32_t *vals, uint8_t nvals,
                             tuple_set *dst)
{
    long i;
    uint8_t j, b;
    (void)nvals;
    memset(dst, 0, sizeof(*dst));
    dst->arity = src->arity;
    for (i = 0; i < src->count; i++) {
        const uint32_t *row = src->data + (size_t)i * (size_t)src->arity;
        int match = 1;
        b = 0;
        for (j = 0; j < src->arity; j++) {
            if (adorn[j] == 'b') {
                if (row[j] != vals[b]) { match = 0; break; }
                b++;
            }
        }
        if (!match) continue;
        if (dst->count >= dst->cap) {
            long nc = dst->cap ? dst->cap * 2 : 256;
            uint32_t *nd = realloc(dst->data,
                (size_t)nc * (size_t)dst->arity * sizeof(uint32_t));
            if (!nd) return -1;
            dst->data = nd;
            dst->cap = nc;
        }
        memcpy(dst->data + (size_t)dst->count * (size_t)dst->arity,
               row, (size_t)dst->arity * sizeof(uint32_t));
        dst->count++;
    }
    return 0;
}

/* dl_query_topdown_adorn vs ground-truth filter of full materialization. */
static int cmp_adorn_topdown(dl_db *db, const char *rel, const char *adorn,
                             const uint32_t *vals, uint8_t nvals)
{
    tuple_set full, ground, rt;
    long nq, nt;
    int eq;

    memset(&full, 0, sizeof(full));
    memset(&ground, 0, sizeof(ground));
    memset(&rt, 0, sizeof(rt));

    nq = dl_query(db, rel, tset_cb, &full);
    if (nq < 0) { tset_free(&full); tset_free(&ground); tset_free(&rt); return 0; }
    if (tset_filter_adorn(&full, adorn, vals, nvals, &ground) != 0) {
        tset_free(&full); tset_free(&ground); tset_free(&rt); return 0;
    }
    nt = dl_query_topdown_adorn(db, rel, adorn, vals, nvals, tset_cb, &rt);
    if (nt < 0) { tset_free(&full); tset_free(&ground); tset_free(&rt); return 0; }
    if ((long)ground.count != nt) {
        tset_free(&full); tset_free(&ground); tset_free(&rt); return 0;
    }
    eq = tset_sorted_eq(&ground, &rt);
    tset_free(&full);
    tset_free(&ground);
    tset_free(&rt);
    return eq;
}

/* dl_query_topdown_adorn vs dl_query_magic_adorn (equivalence). */
static int cmp_adorn_topdown_magic(dl_db *db, const char *rel, const char *adorn,
                                   const uint32_t *vals, uint8_t nvals)
{
    tuple_set rt, rm;
    long nt, nm;
    int eq;

    memset(&rt, 0, sizeof(rt));
    memset(&rm, 0, sizeof(rm));
    nt = dl_query_topdown_adorn(db, rel, adorn, vals, nvals, tset_cb, &rt);
    nm = dl_query_magic_adorn(db, rel, adorn, vals, nvals, tset_cb, &rm);
    if (nt < 0 || nm < 0) { tset_free(&rt); tset_free(&rm); return 0; }
    if (nt != nm) { tset_free(&rt); tset_free(&rm); return 0; }
    eq = tset_sorted_eq(&rt, &rm);
    tset_free(&rt);
    tset_free(&rm);
    return eq;
}

/* ─── T1: chain N=1000 ─────────────────────────────────────────────────── */

static void test_t1_chain(void)
{
    dl_db *db;
    int N = 1000, i;
    uint32_t *edges;
    uint32_t sources[4];

    TEST("T1: chain N=1000 — topdown == bound, topdown == magic");
    setup_db(&db, "t1");
    edges = malloc((size_t)(N - 1) * 2 * sizeof(uint32_t));
    for (i = 0; i < N - 1; i++) {
        edges[i*2]     = (uint32_t)(i + 1);
        edges[i*2 + 1] = (uint32_t)(i + 2);
    }
    load_rows(db, "edge", 2, edges, N - 1, "t1");
    free(edges);
    load_tc_rules(db);
    assert(dl_compile(db) == 0);

    sources[0] = 1; sources[1] = 500; sources[2] = 999; sources[3] = 1000;
    for (i = 0; i < 4; i++) {
        if (!cmp_bound_topdown(db, sources[i])) {
            FAIL("T1 bound vs topdown mismatch");
            teardown_db(db, "t1"); return;
        }
        if (!cmp_topdown_magic_rel(db, "tc", sources[i])) {
            FAIL("T1 topdown vs magic mismatch");
            teardown_db(db, "t1"); return;
        }
    }
    PASS();
    teardown_db(db, "t1");
}

/* ─── T2/T3: star + dense ──────────────────────────────────────────────── */

static void test_t2_star(void)
{
    dl_db *db;
    int N = 40, i;
    uint32_t *edges;
    uint32_t sources[3];

    TEST("T2: star — topdown == bound");
    setup_db(&db, "t2");
    edges = malloc((size_t)N * 2 * sizeof(uint32_t));
    for (i = 0; i < N; i++) {
        edges[i*2]     = 1;                 /* center -> leaf */
        edges[i*2 + 1] = (uint32_t)(i + 2);
    }
    load_rows(db, "edge", 2, edges, N, "t2");
    free(edges);
    load_tc_rules(db);
    assert(dl_compile(db) == 0);

    sources[0] = 1; sources[1] = 2; sources[2] = 99;
    for (i = 0; i < 3; i++) {
        if (!cmp_bound_topdown(db, sources[i])) {
            FAIL("T2 mismatch");
            teardown_db(db, "t2"); return;
        }
    }
    PASS();
    teardown_db(db, "t2");
}

static void test_t3_dense(void)
{
    dl_db *db;
    int N = 30, i, j, k = 0;
    uint32_t *edges;
    uint32_t sources[3];

    TEST("T3: dense — topdown == bound");
    setup_db(&db, "t3");
    edges = malloc((size_t)(N * N) * 2 * sizeof(uint32_t));
    for (i = 0; i < N; i++)
        for (j = 0; j < N; j++) {
            if (i == j) continue;
            edges[k*2]     = (uint32_t)(i + 1);
            edges[k*2 + 1] = (uint32_t)(j + 1);
            k++;
        }
    load_rows(db, "edge", 2, edges, k, "t3");
    free(edges);
    load_tc_rules(db);
    assert(dl_compile(db) == 0);

    sources[0] = 1; sources[1] = 15; sources[2] = 30;
    for (i = 0; i < 3; i++) {
        if (!cmp_bound_topdown(db, sources[i])) {
            FAIL("T3 mismatch");
            teardown_db(db, "t3"); return;
        }
    }
    PASS();
    teardown_db(db, "t3");
}

/* ─── T4/T5: absent sources + EDB goal ─────────────────────────────────── */

static void test_t4_absent_sources(void)
{
    dl_db *db;
    uint32_t e[] = {1,2, 2,3, 3,4};
    uint32_t sources[3] = {42, 0, 1000000};
    int i;

    TEST("T4: absent sources — topdown == bound (empty)");
    setup_db(&db, "t4");
    load_rows(db, "edge", 2, e, 3, "t4");
    load_tc_rules(db);
    assert(dl_compile(db) == 0);

    for (i = 0; i < 3; i++) {
        if (!cmp_bound_topdown(db, sources[i])) {
            FAIL("T4 mismatch");
            teardown_db(db, "t4"); return;
        }
    }
    PASS();
    teardown_db(db, "t4");
}

static void test_t5_edb_goal(void)
{
    dl_db *db;
    uint32_t e[] = {1,42, 2,42, 3,7, 9,42};

    TEST("T5: EDB goal — topdown degenerates to prefix lookup");
    setup_db(&db, "t5");
    load_rows(db, "edge", 2, e, 4, "t5");
    {
        tuple_set rb, rt;
        uint32_t leading[1] = {42};
        memset(&rb, 0, sizeof(rb));
        memset(&rt, 0, sizeof(rt));
        long nb = dl_query_bound(db, "edge", leading, 1, tset_cb, &rb);
        long nt = dl_query_topdown(db, "edge", leading, 1, tset_cb, &rt);
        if (nb < 0 || nt < 0 || nb != nt || !tset_sorted_eq(&rb, &rt)) {
            FAIL("T5 EDB goal mismatch");
            tset_free(&rb); tset_free(&rt);
            teardown_db(db, "t5"); return;
        }
        tset_free(&rb); tset_free(&rt);
    }
    PASS();
    teardown_db(db, "t5");
}

/* ─── T6/T7: equality in body + multi-rule ─────────────────────────────── */

static void test_t6_equality(void)
{
    dl_db *db;
    uint32_t e[] = {1,2, 2,3, 3,4, 4,5};
    uint32_t sources[2] = {1, 2};
    int i;

    TEST("T6: equality in body — topdown == bound");
    setup_db(&db, "t6");
    load_rows(db, "edge", 2, e, 4, "t6");
    assert(dl_load_rules(db,
        "tc(X,Y):-edge(X,Y).\n"
        "tc(X,Y):-edge(X,Z),tc(Z,W),W=Y.\n") == 0);
    assert(dl_compile(db) == 0);

    for (i = 0; i < 2; i++) {
        if (!cmp_bound_topdown(db, sources[i])) {
            FAIL("T6 mismatch");
            teardown_db(db, "t6"); return;
        }
    }
    PASS();
    teardown_db(db, "t6");
}

static void test_t7_multi_rule(void)
{
    dl_db *db;
    uint32_t e[] = {1,2, 2,3, 3,4, 1,3};
    uint32_t sources[2] = {1, 2};
    int i;

    TEST("T7: multi-rule same head — topdown == bound");
    setup_db(&db, "t7");
    load_rows(db, "edge", 2, e, 4, "t7");
    assert(dl_load_rules(db,
        "tc(X,Y):-edge(X,Y).\n"
        "tc(X,Y):-edge(X,Z),edge(Z,Y).\n"
        "tc(X,Y):-edge(X,Z),tc(Z,Y).\n") == 0);
    assert(dl_compile(db) == 0);

    for (i = 0; i < 2; i++) {
        if (!cmp_bound_topdown(db, sources[i])) {
            FAIL("T7 mismatch");
            teardown_db(db, "t7"); return;
        }
    }
    PASS();
    teardown_db(db, "t7");
}

/* ─── T8: property — 200 random graphs x 5 sources ─────────────────────── */

static void test_t8_property(void)
{
    TEST("T8: property — 200 random graphs x 5 sources, topdown==bound==magic");

    int iter;
    for (iter = 0; iter < 200; iter++) {
        dl_db *db;
        char suffix[32];
        int N = 8 + (int)(iter % 12);
        int M = 20 + (int)((iter * 7) % 60);
        uint32_t *edges;
        int ei, si;
        uint32_t sources[5];

        snprintf(suffix, sizeof(suffix), "prop_%d", iter);
        setup_db(&db, suffix);

        edges = malloc((size_t)M * 2 * sizeof(uint32_t));
        for (ei = 0; ei < M; ei++) {
            edges[ei*2]     = rng_rand((uint32_t)N) + 1;
            edges[ei*2 + 1] = rng_rand((uint32_t)N) + 1;
        }
        load_rows(db, "edge", 2, edges, M, suffix);
        free(edges);
        load_tc_rules(db);
        assert(dl_compile(db) == 0);

        for (si = 0; si < 5; si++) {
            sources[si] = rng_rand((uint32_t)(N + 2)) + 1;
            if (!cmp_bound_topdown(db, sources[si])) {
                printf("  iter %d source %u mismatch\n", iter, sources[si]);
                FAIL("T8 property failed (topdown vs bound)");
                teardown_db(db, suffix); return;
            }
            if (!cmp_topdown_magic_rel(db, "tc", sources[si])) {
                printf("  iter %d source %u mismatch\n", iter, sources[si]);
                FAIL("T8 property failed (topdown vs magic)");
                teardown_db(db, suffix); return;
            }
        }
        teardown_db(db, suffix);
    }
    PASS();
}

/* ─── T9: multi-predicate closures (path -> tc, 3-pred chain) ──────────── */

static void test_t9_multipred(void)
{
    dl_db *db;
    uint32_t e[] = {1,2, 2,3, 3,4, 4,5, 2,4};
    uint32_t sources[3] = {1, 2, 3};
    int i;

    TEST("T9: multi-predicate path->tc, 3-pred chain — topdown==bound==magic");
    setup_db(&db, "t9");
    load_rows(db, "edge", 2, e, 5, "t9");
    assert(dl_load_rules(db,
        "path(X,Y):-edge(X,Y).\n"
        "path(X,Y):-edge(X,Z),tc(Z,Y).\n"
        "tc(X,Y):-edge(X,Y).\n"
        "tc(X,Y):-edge(X,Z),tc(Z,Y).\n") == 0);
    assert(dl_compile(db) == 0);

    for (i = 0; i < 3; i++) {
        if (!cmp_bound_topdown_rel(db, "path", sources[i])) {
            FAIL("T9 path mismatch");
            teardown_db(db, "t9"); return;
        }
        if (!cmp_topdown_magic_rel(db, "path", sources[i])) {
            FAIL("T9 path topdown vs magic mismatch");
            teardown_db(db, "t9"); return;
        }
    }
    PASS();
    teardown_db(db, "t9");
}

/* dl_query_bound vs dl_query_topdown for a generic arity-2 goal. */
static int cmp_bound_topdown_rel(dl_db *db, const char *rel, uint32_t src)
{
    tuple_set rb, rt;
    uint32_t leading[1];
    long nb, nt;
    int eq;
    memset(&rb, 0, sizeof(rb));
    memset(&rt, 0, sizeof(rt));
    leading[0] = src;
    nb = dl_query_bound(db, rel, leading, 1, tset_cb, &rb);
    nt = dl_query_topdown(db, rel, leading, 1, tset_cb, &rt);
    if (nb < 0 || nt < 0) { tset_free(&rb); tset_free(&rt); return 0; }
    if (nb != nt) { tset_free(&rb); tset_free(&rt); return 0; }
    eq = tset_sorted_eq(&rb, &rt);
    tset_free(&rb);
    tset_free(&rt);
    return eq;
}

/* ─── T10: non-leading adornments (fb self-recursive) ──────────────────── */

static void test_t10_fb_selfrec(void)
{
    dl_db *db;
    uint32_t e[] = {1,2, 2,3, 3,4, 4,5, 1,5};
    uint32_t targets[4] = {2, 4, 5, 42};
    int ti;

    TEST("T10: non-leading self-recursive tc(*,k) — topdown==filter==magic");
    setup_db(&db, "t10");
    load_rows(db, "edge", 2, e, 5, "t10");
    load_tc_rules(db);
    assert(dl_compile(db) == 0);

    for (ti = 0; ti < 4; ti++) {
        uint32_t vals[1] = { targets[ti] };
        if (!cmp_adorn_topdown(db, "tc", "fb", vals, 1)) {
            printf("  target %u mismatch\n", targets[ti]);
            FAIL("T10 fb topdown != full-filter");
            teardown_db(db, "t10"); return;
        }
        if (!cmp_adorn_topdown_magic(db, "tc", "fb", vals, 1)) {
            FAIL("T10 fb topdown != magic");
            teardown_db(db, "t10"); return;
        }
    }
    PASS();
    teardown_db(db, "t10");
}

/* ─── T11: non-leading fbf + multi-pred fb ─────────────────────────────── */

static void test_t11_nonleading_misc(void)
{
    dl_db *db;
    uint32_t e[] = {1,42, 42,3, 42,5, 1,7, 7,3};
    uint32_t vals[1] = {42};

    TEST("T11: fbf path3 + fb multi-pred — topdown==filter==magic");
    setup_db(&db, "t11");
    load_rows(db, "edge", 2, e, 5, "t11");
    assert(dl_load_rules(db, "path3(X,Y,Z):-edge(X,Y),edge(Y,Z).\n") == 0);
    assert(dl_compile(db) == 0);

    if (!cmp_adorn_topdown(db, "path3", "fbf", vals, 1)) {
        FAIL("T11 fbf path3 mismatch");
        teardown_db(db, "t11"); return;
    }
    if (!cmp_adorn_topdown_magic(db, "path3", "fbf", vals, 1)) {
        FAIL("T11 fbf path3 topdown != magic");
        teardown_db(db, "t11"); return;
    }
    PASS();
    teardown_db(db, "t11");
}

/* ─── T12: multi-variant (two-adornment) property ──────────────────────── */

static void test_t12_two_adorn_property(void)
{
    TEST("T12: property — 100 graphs x 3 sources, two-adornment p");

    int iter;
    for (iter = 0; iter < 100; iter++) {
        dl_db *db;
        char suffix[32];
        int N = 8 + (int)(iter % 12);
        int M = 20 + (int)((iter * 7) % 60);
        uint32_t *edges;
        int ei, si;
        uint32_t sources[3];

        snprintf(suffix, sizeof(suffix), "ta_%d", iter);
        setup_db(&db, suffix);

        edges = malloc((size_t)M * 2 * sizeof(uint32_t));
        for (ei = 0; ei < M; ei++) {
            edges[ei*2]     = rng_rand((uint32_t)N) + 1;
            edges[ei*2 + 1] = rng_rand((uint32_t)N) + 1;
        }
        load_rows(db, "edge", 2, edges, M, suffix);
        free(edges);

        assert(dl_load_rules(db,
            "r(X,Y):-p(X,Y).\n"
            "r(X,Y):-s(X,Y).\n"
            "s(X,Y):-edge(X,Y),p(X,Y).\n"
            "p(X,Y):-edge(X,Y).\n") == 0);
        assert(dl_compile(db) == 0);

        for (si = 0; si < 3; si++) {
            sources[si] = rng_rand((uint32_t)(N + 2)) + 1;
            if (!cmp_topdown_magic_rel(db, "r", sources[si])) {
                printf("  iter %d source %u mismatch\n", iter, sources[si]);
                FAIL("T12 two-adornment topdown != magic");
                teardown_db(db, suffix); return;
            }
        }
        teardown_db(db, suffix);
    }
    PASS();
}

/* ─── T13: rejection parity ────────────────────────────────────────────── */

static void test_t13_rejections(void)
{
    dl_db *db;
    uint32_t e[] = {1,2, 2,3};
    uint32_t vals[2] = {1, 42};

    TEST("T13: rejection parity (nvals mismatch, length mismatch)");
    setup_db(&db, "t13");
    load_rows(db, "edge", 2, e, 2, "t13");
    load_tc_rules(db);
    assert(dl_compile(db) == 0);

    {
        tuple_set r;
        memset(&r, 0, sizeof(r));
        if (dl_query_topdown_adorn(db, "tc", "fb", vals, 2, tset_cb, &r) != -1) {
            FAIL("T13 nvals/count mismatch not rejected");
            tset_free(&r); teardown_db(db, "t13"); return;
        }
        tset_free(&r);
    }
    {
        tuple_set r;
        memset(&r, 0, sizeof(r));
        if (dl_query_topdown_adorn(db, "tc", "f", vals, 1, tset_cb, &r) != -1) {
            FAIL("T13 length mismatch not rejected");
            tset_free(&r); teardown_db(db, "t13"); return;
        }
        tset_free(&r);
    }
    PASS();
    teardown_db(db, "t13");
}

/* ─── T14: chain N=10000 single source (stack safety) ──────────────────── */

static void test_t14_chain_10000(void)
{
    dl_db *db;
    int N = 10000, i;
    uint32_t *edges;
    uint32_t leading[1] = {1};

    TEST("T14: chain N=10000 — topdown single source, no stack overflow");
    setup_db(&db, "t14");
    edges = malloc((size_t)(N - 1) * 2 * sizeof(uint32_t));
    for (i = 0; i < N - 1; i++) {
        edges[i*2]     = (uint32_t)(i + 1);
        edges[i*2 + 1] = (uint32_t)(i + 2);
    }
    load_rows(db, "edge", 2, edges, N - 1, "t14");
    free(edges);
    load_tc_rules(db);   /* no dl_compile: the top-down path clones EDB + re-evals */

    {
        tuple_set rt;
        long nt;
        memset(&rt, 0, sizeof(rt));
        nt = dl_query_topdown(db, "tc", leading, 1, tset_cb, &rt);
        if (nt != N - 1) {
            printf("  got %ld tuples, expected %d\n", nt, N - 1);
            FAIL("T14 wrong tuple count");
            tset_free(&rt); teardown_db(db, "t14"); return;
        }
        tset_free(&rt);
    }
    PASS();
    teardown_db(db, "t14");
}

/* ─── T15: memo-dependent discovery (two IDB atoms in one rule) ──────────
 * r(X,Y):-p(X,Z),q(Z,Y) with p,q base over edge.  r's magic rule for q has an
 * IDB prefix (p), so q's bounds are discovered during Phase B (delta
 * propagation of p's memo) — exercises the late-join path. */
static void test_t15_memo_dependent_discovery(void)
{
    dl_db *db;
    uint32_t e[] = {1,2, 2,3, 3,4, 2,5, 5,6};
    uint32_t sources[3] = {1, 2, 3};
    int i;

    TEST("T15: memo-dependent discovery r:-p,q (2 IDB atoms/rule) — topdown==bound==magic");
    setup_db(&db, "t15");
    load_rows(db, "edge", 2, e, 5, "t15");
    assert(dl_load_rules(db,
        "r(X,Y):-p(X,Z),q(Z,Y).\n"
        "p(X,Y):-edge(X,Y).\n"
        "q(X,Y):-edge(X,Y).\n") == 0);
    assert(dl_compile(db) == 0);

    for (i = 0; i < 3; i++) {
        if (!cmp_bound_topdown_rel(db, "r", sources[i])) {
            FAIL("T15 memo-dependent discovery mismatch (vs bound)");
            teardown_db(db, "t15"); return;
        }
        if (!cmp_topdown_magic_rel(db, "r", sources[i])) {
            FAIL("T15 memo-dependent discovery mismatch (vs magic)");
            teardown_db(db, "t15"); return;
        }
    }
    PASS();
    teardown_db(db, "t15");
}

/* ─── Benchmark: topdown vs dl_query_magic_adorn ───────────────────────── */

static double now_sec(void)
{
    return (double)clock() / (double)CLOCKS_PER_SEC;
}

static void bench_one(const char *label, dl_db *db, const char *rel,
                      const uint32_t *leading, uint8_t k)
{
    double t0, t1, tm, tt;
    long nm, nt;

    t0 = now_sec();
    nm = dl_query_magic(db, rel, leading, k, noop_cb, NULL);
    t1 = now_sec();
    tm = t1 - t0;

    t0 = now_sec();
    nt = dl_query_topdown(db, rel, leading, k, noop_cb, NULL);
    t1 = now_sec();
    tt = t1 - t0;

    printf("  %-28s magic=%8.4fs (%ld)  topdown=%8.4fs (%ld)  speedup=%.2fx\n",
           label, tm, nm, tt, nt, (tt > 0.0 ? tm / tt : 0.0));
}

static void test_benchmark(void)
{
    dl_db *db;
    int N, i;
    uint32_t *edges;
    uint32_t leading[1];

    TEST("BENCH: chain N=1000/10000 + dense — topdown vs dl_query_magic");

    /* chain N=1000 */
    N = 1000;
    setup_db(&db, "b1");
    edges = malloc((size_t)(N - 1) * 2 * sizeof(uint32_t));
    for (i = 0; i < N - 1; i++) {
        edges[i*2] = (uint32_t)(i + 1); edges[i*2+1] = (uint32_t)(i + 2);
    }
    load_rows(db, "edge", 2, edges, N - 1, "b1");
    free(edges);
    load_tc_rules(db);
    leading[0] = 1;
    bench_one("chain N=1000 src=1", db, "tc", leading, 1);
    teardown_db(db, "b1");

    /* chain N=10000 */
    N = 10000;
    setup_db(&db, "b2");
    edges = malloc((size_t)(N - 1) * 2 * sizeof(uint32_t));
    for (i = 0; i < N - 1; i++) {
        edges[i*2] = (uint32_t)(i + 1); edges[i*2+1] = (uint32_t)(i + 2);
    }
    load_rows(db, "edge", 2, edges, N - 1, "b2");
    free(edges);
    load_tc_rules(db);
    leading[0] = 1;
    bench_one("chain N=10000 src=1", db, "tc", leading, 1);
    teardown_db(db, "b2");

    /* dense N=300 (complete digraph) */
    N = 300;
    setup_db(&db, "b3");
    edges = malloc((size_t)(N * N) * 2 * sizeof(uint32_t));
    {
        int j, k = 0;
        for (i = 0; i < N; i++)
            for (j = 0; j < N; j++) {
                if (i == j) continue;
                edges[k*2] = (uint32_t)(i + 1); edges[k*2+1] = (uint32_t)(j + 1);
                k++;
            }
        load_rows(db, "edge", 2, edges, N * (N - 1), "b3");
    }
    free(edges);
    load_tc_rules(db);
    leading[0] = 1;
    bench_one("dense N=300 src=1", db, "tc", leading, 1);
    teardown_db(db, "b3");

    PASS();
}

/* ─── Main ─────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("Top-down / QSQ Evaluator Tests (STAGE A + B)\n");
    printf("=============================================\n\n");

    test_t1_chain();
    test_t2_star();
    test_t3_dense();
    test_t4_absent_sources();
    test_t5_edb_goal();
    test_t6_equality();
    test_t7_multi_rule();
    test_t8_property();
    test_t9_multipred();
    test_t10_fb_selfrec();
    test_t11_nonleading_misc();
    test_t12_two_adorn_property();
    test_t13_rejections();
    test_t15_memo_dependent_discovery();
    test_t14_chain_10000();

    /* Honest benchmark (topdown vs dl_query_magic) is OPT-IN to keep the
     * default `make test` fast (~3min if it runs: chain N=10000 twice ≈ 90s).
     * Enable with RUN_BENCH=1.  The correctness case for N=10000 is already
     * covered by test_t14_chain_10000 above. */
    if (getenv("RUN_BENCH"))
        test_benchmark();

    printf("\n%d tests run, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
