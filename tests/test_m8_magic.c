/*
 * test_m8_magic.c — M8 magic-sets (first slice) tests
 *
 * The correctness backstop (non-negotiable): for a leading-prefix-bound goal,
 *   magic_result(Q) == dl_query_bound(Q) over the FULL materialization,
 * byte-for-byte sorted unique tuple-set equality.  This is verified by:
 *
 *   T1-T6   canonical TC graphs + edge cases (chain/star/dense/small/absent/
 *           no-out-edges)
 *   T7      equality in body (Z=W alias) — accepted, still equivalent
 *   T8      multi-rule same head
 *   T9      negation/aggregate now ACCEPTED (negated EDB, aggregate over EDB);
 *           SIPS ACCEPTS the different-adornment (T9c) and non-leading (T9d)
 *           recursive shapes — verified against ground truth
 *   T10     k==0 routes to dl_query
 *   T11     no-mutation: db fields + interner byte-identical before/after
 *   T12     magic works WITHOUT a prior dl_compile (scoped re-eval from EDB)
 *   T13     property test: 200 random graphs x 5 sources, bound vs magic
 *   T29-T33 SIPS body-reordering: suboptimal order, multi-pred reorder,
 *           equality-heavy, TC-FB positive, order-independence property
 *   T34-T38 negation + aggregates: negated non-closure IDB, sum/min/max over
 *           EDB, REJECT negated closure IDB, REJECT aggregate over closure,
 *           negated-EDB property test
 *
 * Regression: full `make test` (224 existing tests) still passes.
 */
#include "dl.h"
#include "dl_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>

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

/* ─── Tuple set (local, mirrors test_m6) ──────────────────────────────── */

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

/* byte-for-byte sorted tuple-set equality */
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

/* ─── Database helpers ────────────────────────────────────────────────── */

static void rm_dir(const char *dir)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    system(cmd);
}

static void setup_db(dl_db **db_out, const char *suffix)
{
    char path[256];
    snprintf(path, sizeof(path), "build-tmp/m8db_%s", suffix);
    rm_dir(path);
    *db_out = dl_open(path);
    assert(*db_out);
}

static void teardown_db(dl_db *db, const char *suffix)
{
    char path[256];
    dl_close(db);
    snprintf(path, sizeof(path), "build-tmp/m8db_%s", suffix);
    rm_dir(path);
}

/* Load rows (u32 values) into a relation via a headerless CSV. */
static int load_rows(dl_db *db, const char *rel_name, uint8_t arity,
                     const uint32_t *cols, int nrows, const char *suffix)
{
    char csv_path[256];
    FILE *f;
    int i, c;

    assert(dl_declare_relation(db, rel_name, arity) == 0);

    snprintf(csv_path, sizeof(csv_path), "build-tmp/m8csv_%s_%s.csv",
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

/* Load the canonical transitive-closure program over `edge` (arity 2). */
static void load_tc_rules(dl_db *db)
{
    int rc = dl_load_rules(db,
        "tc(X,Y):-edge(X,Y).\n"
        "tc(X,Y):-edge(X,Z),tc(Z,Y).\n");
    if (rc != 0) {
        fprintf(stderr, "  (tc rules failed to compile)\n");
    }
    assert(rc == 0);
}

/* Compare dl_query_bound vs dl_query_magic for tc(src, ?). */
static int cmp_bound_magic(dl_db *db, uint32_t src)
{
    tuple_set rb, rm;
    uint32_t leading[1];
    long nb, nm;
    int eq;

    memset(&rb, 0, sizeof(rb));
    memset(&rm, 0, sizeof(rm));
    leading[0] = src;

    nb = dl_query_bound(db, "tc", leading, 1, tset_cb, &rb);
    nm = dl_query_magic(db, "tc", leading, 1, tset_cb, &rm);

    if (nb < 0 || nm < 0) { tset_free(&rb); tset_free(&rm); return 0; }
    if (nb != nm) { tset_free(&rb); tset_free(&rm); return 0; }
    eq = tset_sorted_eq(&rb, &rm);
    tset_free(&rb);
    tset_free(&rm);
    return eq;
}

/* Forward decl: generic arity-2 bound-vs-magic comparator (defined in the
 * T14-T19 section, but used earlier by T9's negation/aggregate cases). */
static int cmp_bound_magic_rel(dl_db *db, const char *rel, uint32_t src);

/* ─── T1: chain ───────────────────────────────────────────────────────── */

static void test_t1_chain(void)
{
    dl_db *db;
    int N = 1000, i;
    uint32_t *edges;
    uint32_t sources[4];

    TEST("T1: chain N=1000 — bound vs magic for several sources");

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

    sources[0] = 1;          /* reaches 2..1000 → 999 tuples */
    sources[1] = 500;        /* reaches 501..1000 → 500 tuples */
    sources[2] = 1000;       /* no out-edges → 0 tuples */
    sources[3] = 9999;       /* absent source → 0 tuples */

    for (i = 0; i < 4; i++) {
        if (!cmp_bound_magic(db, sources[i])) {
            printf("  source %u mismatch\n", sources[i]);
            FAIL("chain bound vs magic mismatch");
            teardown_db(db, "t1");
            return;
        }
    }
    PASS();
    teardown_db(db, "t1");
}

/* ─── T2: star ────────────────────────────────────────────────────────── */

static void test_t2_star(void)
{
    dl_db *db;
    int N = 50, i;
    uint32_t *edges;
    uint32_t sources[3];

    TEST("T2: star center=1 → 49 leaves, plus leaf/absent sources");

    setup_db(&db, "t2");
    edges = malloc((size_t)(N - 1) * 2 * sizeof(uint32_t));
    for (i = 0; i < N - 1; i++) {
        edges[i*2]     = 1;
        edges[i*2 + 1] = (uint32_t)(i + 2);
    }
    load_rows(db, "edge", 2, edges, N - 1, "t2");
    free(edges);
    load_tc_rules(db);
    assert(dl_compile(db) == 0);

    sources[0] = 1;     /* center reaches 2..50 */
    sources[1] = 2;     /* leaf: no out-edges */
    sources[2] = 99;    /* absent */

    for (i = 0; i < 3; i++) {
        if (!cmp_bound_magic(db, sources[i])) {
            printf("  source %u mismatch\n", sources[i]);
            FAIL("star bound vs magic mismatch");
            teardown_db(db, "t2");
            return;
        }
    }
    PASS();
    teardown_db(db, "t2");
}

/* ─── T3: dense / fully-connected ─────────────────────────────────────── */

static void test_t3_dense(void)
{
    dl_db *db;
    int N = 20, i, j, e = 0;
    uint32_t *edges;
    uint32_t sources[3];

    TEST("T3: complete digraph N=20 — bound vs magic");

    setup_db(&db, "t3");
    edges = malloc((size_t)(N * (N - 1)) * 2 * sizeof(uint32_t));
    for (i = 0; i < N; i++) {
        for (j = 0; j < N; j++) {
            if (i == j) continue;
            edges[e*2]     = (uint32_t)(i + 1);
            edges[e*2 + 1] = (uint32_t)(j + 1);
            e++;
        }
    }
    load_rows(db, "edge", 2, edges, e, "t3");
    free(edges);
    load_tc_rules(db);
    assert(dl_compile(db) == 0);

    sources[0] = 1;
    sources[1] = 7;
    sources[2] = 20;

    for (i = 0; i < 3; i++) {
        if (!cmp_bound_magic(db, sources[i])) {
            printf("  source %u mismatch\n", sources[i]);
            FAIL("dense bound vs magic mismatch");
            teardown_db(db, "t3");
            return;
        }
    }
    PASS();
    teardown_db(db, "t3");
}

/* ─── T4: small graphs (single/empty/self-loop/two-cycle/disconnected) ── */

static void test_t4_small_graphs(void)
{
    dl_db *db;

    TEST("T4: single node / empty / self-loop / two-cycle / disconnected");

    /* 4a: single node with self-loop */
    {
        uint32_t e[] = {1, 1};
        setup_db(&db, "t4a");
        load_rows(db, "edge", 2, e, 1, "t4a");
        load_tc_rules(db);
        assert(dl_compile(db) == 0);
        if (!cmp_bound_magic(db, 1) || !cmp_bound_magic(db, 2)) {
            FAIL("self-loop mismatch");
            teardown_db(db, "t4a");
            return;
        }
        teardown_db(db, "t4a");
    }
    /* 4b: empty graph */
    {
        setup_db(&db, "t4b");
        load_rows(db, "edge", 2, NULL, 0, "t4b");
        load_tc_rules(db);
        assert(dl_compile(db) == 0);
        if (!cmp_bound_magic(db, 1)) {
            FAIL("empty graph mismatch");
            teardown_db(db, "t4b");
            return;
        }
        teardown_db(db, "t4b");
    }
    /* 4c: two-cycle 1->2, 2->1 */
    {
        uint32_t e[] = {1, 2, 2, 1};
        setup_db(&db, "t4c");
        load_rows(db, "edge", 2, e, 2, "t4c");
        load_tc_rules(db);
        assert(dl_compile(db) == 0);
        if (!cmp_bound_magic(db, 1) || !cmp_bound_magic(db, 2) ||
            !cmp_bound_magic(db, 3)) {
            FAIL("two-cycle mismatch");
            teardown_db(db, "t4c");
            return;
        }
        teardown_db(db, "t4c");
    }
    /* 4d: two disconnected components */
    {
        uint32_t e[] = {1, 2, 2, 3, 10, 11, 11, 12};
        setup_db(&db, "t4d");
        load_rows(db, "edge", 2, e, 4, "t4d");
        load_tc_rules(db);
        assert(dl_compile(db) == 0);
        if (!cmp_bound_magic(db, 1) || !cmp_bound_magic(db, 10) ||
            !cmp_bound_magic(db, 2) || !cmp_bound_magic(db, 99)) {
            FAIL("disconnected mismatch");
            teardown_db(db, "t4d");
            return;
        }
        teardown_db(db, "t4d");
    }
    PASS();
}

/* ─── T5: bound src absent / no out-edges (explicit counts) ───────────── */

static void test_t5_absent_sources(void)
{
    dl_db *db;
    tuple_set r;

    TEST("T5: absent source and no-out-edge source → empty, both paths");

    {
        uint32_t e[] = {1, 2, 2, 3};
        setup_db(&db, "t5");
        load_rows(db, "edge", 2, e, 2, "t5");
        load_tc_rules(db);
        assert(dl_compile(db) == 0);
    }

    memset(&r, 0, sizeof(r));
    {
        uint32_t leading[1] = { 99 };  /* absent */
        long nb = dl_query_bound(db, "tc", leading, 1, tset_cb, &r);
        assert(nb == 0 && r.count == 0);
        tset_free(&r);
    }
    memset(&r, 0, sizeof(r));
    {
        uint32_t leading[1] = { 99 };
        long nm = dl_query_magic(db, "tc", leading, 1, tset_cb, &r);
        assert(nm == 0 && r.count == 0);
        tset_free(&r);
    }
    PASS();
    teardown_db(db, "t5");
}

/* ─── T6: goal is an EDB relation → degenerates to prefix lookup ──────── */

static void test_t6_edb_goal(void)
{
    dl_db *db;
    uint32_t e[] = {1, 2, 1, 3, 2, 4, 5, 6};

    TEST("T6: EDB goal (edge) → magic degenerates to prefix lookup");

    setup_db(&db, "t6");
    load_rows(db, "edge", 2, e, 4, "t6");
    load_tc_rules(db);            /* edge is EDB here, tc is IDB */
    assert(dl_compile(db) == 0);

    {
        tuple_set rb, rm;
        uint32_t leading[1] = { 1 };
        memset(&rb, 0, sizeof(rb));
        memset(&rm, 0, sizeof(rm));
        long nb = dl_query_bound(db, "edge", leading, 1, tset_cb, &rb);
        long nm = dl_query_magic(db, "edge", leading, 1, tset_cb, &rm);
        if (nb < 0 || nm < 0 || nb != nm || !tset_sorted_eq(&rb, &rm)) {
            FAIL("EDB-goal magic != prefix");
            tset_free(&rb); tset_free(&rm);
            teardown_db(db, "t6");
            return;
        }
        assert(nb == 2);  /* edge(1,*) = {2,3} */
        tset_free(&rb); tset_free(&rm);
    }
    PASS();
    teardown_db(db, "t6");
}

/* ─── T7: equality in body ────────────────────────────────────────────── */

static void test_t7_equality(void)
{
    dl_db *db;
    uint32_t e[] = {1, 2, 2, 3, 3, 4, 4, 5};

    TEST("T7: equality (Z=W alias) accepted and equivalent");

    setup_db(&db, "t7");
    load_rows(db, "edge", 2, e, 4, "t7");

    assert(dl_load_rules(db,
        "tc(X,Y):-edge(X,Y).\n"
        "tc(X,Y):-edge(X,Z),Z=W,tc(W,Y).\n") == 0);
    assert(dl_compile(db) == 0);

    {
        uint32_t sources[3] = {1, 2, 99};
        int i;
        for (i = 0; i < 3; i++) {
            if (!cmp_bound_magic(db, sources[i])) {
                printf("  source %u mismatch\n", sources[i]);
                FAIL("equality rule bound vs magic mismatch");
                teardown_db(db, "t7");
                return;
            }
        }
    }
    PASS();
    teardown_db(db, "t7");
}

/* ─── T8: multi-rule same head (3 rules) ──────────────────────────────── */

static void test_t8_multi_rule(void)
{
    dl_db *db;
    uint32_t e[] = {1, 2, 2, 3, 3, 4};

    TEST("T8: three rules for same head (tc) — equivalent");

    setup_db(&db, "t8");
    load_rows(db, "edge", 2, e, 3, "t8");
    assert(dl_load_rules(db,
        "tc(X,Y):-edge(X,Y).\n"
        "tc(X,Y):-edge(X,Z),edge(Z,Y).\n"
        "tc(X,Y):-edge(X,Z),tc(Z,Y).\n") == 0);
    assert(dl_compile(db) == 0);

    {
        uint32_t sources[3] = {1, 2, 99};
        int i;
        for (i = 0; i < 3; i++) {
            if (!cmp_bound_magic(db, sources[i])) {
                FAIL("multi-rule bound vs magic mismatch");
                teardown_db(db, "t8");
                return;
            }
        }
    }
    PASS();
    teardown_db(db, "t8");
}

/* ─── T9: conservative rejections ─────────────────────────────────────── */

static void test_t9_rejections(void)
{
    dl_db *db;

    TEST("T9: negation/aggregate accepted; different-adornment/non-leading accept");

    /* negation over EDB — now SUPPORTED, verify vs ground truth */
    {
        uint32_t e[] = {1, 2, 2, 3};
        uint32_t b[] = {1, 2};
        uint32_t sources[2] = {1, 2};
        int si;
        setup_db(&db, "t9a");
        load_rows(db, "edge", 2, e, 2, "t9a");
        load_rows(db, "blocked", 2, b, 1, "t9a");
        assert(dl_load_rules(db,
            "ntc(X,Y):-edge(X,Y),!blocked(X,Y).\n") == 0);
        assert(dl_compile(db) == 0);
        for (si = 0; si < 2; si++) {
            if (!cmp_bound_magic_rel(db, "ntc", sources[si])) {
                printf("  source %u mismatch\n", sources[si]);
                FAIL("T9a negated-EDB bound vs magic mismatch");
                teardown_db(db, "t9a");
                return;
            }
        }
        teardown_db(db, "t9a");
    }
    /* aggregate over EDB — now SUPPORTED, verify vs ground truth */
    {
        uint32_t e[] = {1, 2, 1, 3};
        uint32_t sources[2] = {1, 2};
        int si;
        setup_db(&db, "t9b");
        load_rows(db, "edge", 2, e, 2, "t9b");
        assert(dl_load_rules(db,
            "cnt(X,N):-edge(X,Y),N=count().\n") == 0);
        assert(dl_compile(db) == 0);
        for (si = 0; si < 2; si++) {
            if (!cmp_bound_magic_rel(db, "cnt", sources[si])) {
                printf("  source %u mismatch\n", sources[si]);
                FAIL("T9b aggregate-EDB bound vs magic mismatch");
                teardown_db(db, "t9b");
                return;
            }
        }
        teardown_db(db, "t9b");
    }
    /* recursive call that under left-to-right needed a DIFFERENT adornment
     * (bb); SIPS reorders edge(X,Z) before tc(Z,Y) so the recursive call gets
     * bf == head.  ACCEPTED — verify against ground truth. */
    {
        uint32_t e[] = {1, 2, 2, 3, 3, 4};
        uint32_t nd[] = {1, 2, 3, 4};
        uint32_t sources[3] = {1, 2, 99};
        int si;
        setup_db(&db, "t9c");
        load_rows(db, "edge", 2, e, 3, "t9c");
        load_rows(db, "node", 1, nd, 4, "t9c");
        assert(dl_load_rules(db,
            "tc(X,Y):-edge(X,Y).\n"
            "tc(X,Y):-edge(X,Z),node(Y),tc(Z,Y).\n") == 0);
        assert(dl_compile(db) == 0);
        for (si = 0; si < 3; si++) {
            if (!cmp_bound_magic(db, sources[si])) {
                printf("  source %u mismatch\n", sources[si]);
                FAIL("T9c SIPS-accepted bound vs magic mismatch");
                teardown_db(db, "t9c");
                return;
            }
        }
        teardown_db(db, "t9c");
    }
    /* non-leading adornment (fb) on the recursive call — user wrote node(Y),
     * tc(Z,Y), edge(X,Z) in the "bad" order; SIPS reorders edge first so
     * tc(Z,Y) gets bf == head.  ACCEPTED — verify against ground truth. */
    {
        uint32_t e[] = {1, 2, 2, 3, 3, 4};
        uint32_t nd[] = {1, 2, 3, 4};
        uint32_t sources[3] = {1, 2, 99};
        int si;
        setup_db(&db, "t9d");
        load_rows(db, "edge", 2, e, 3, "t9d");
        load_rows(db, "node", 1, nd, 4, "t9d");
        assert(dl_load_rules(db,
            "tc(X,Y):-edge(X,Y).\n"
            "tc(X,Y):-node(Y),tc(Z,Y),edge(X,Z).\n") == 0);
        assert(dl_compile(db) == 0);
        for (si = 0; si < 3; si++) {
            if (!cmp_bound_magic(db, sources[si])) {
                printf("  source %u mismatch\n", sources[si]);
                FAIL("T9d SIPS-accepted bound vs magic mismatch");
                teardown_db(db, "t9d");
                return;
            }
        }
        teardown_db(db, "t9d");
    }
    PASS();
}

/* ─── T10: k==0 routes to dl_query ────────────────────────────────────── */

static void test_t10_k0(void)
{
    dl_db *db;
    uint32_t e[] = {1, 2, 2, 3, 3, 4};

    TEST("T10: k==0 → full materialization (routes to dl_query)");

    setup_db(&db, "t10");
    load_rows(db, "edge", 2, e, 3, "t10");
    load_tc_rules(db);
    assert(dl_compile(db) == 0);

    {
        tuple_set rq, rm;
        memset(&rq, 0, sizeof(rq));
        memset(&rm, 0, sizeof(rm));
        long nq = dl_query(db, "tc", tset_cb, &rq);
        long nm = dl_query_magic(db, "tc", NULL, 0, tset_cb, &rm);
        if (nq < 0 || nm < 0 || nq != nm || !tset_sorted_eq(&rq, &rm)) {
            FAIL("k==0 magic != dl_query");
            tset_free(&rq); tset_free(&rm);
            teardown_db(db, "t10");
            return;
        }
        tset_free(&rq); tset_free(&rm);
    }
    PASS();
    teardown_db(db, "t10");
}

/* ─── T11: no-mutation ────────────────────────────────────────────────── */

static void test_t11_no_mutation(void)
{
    dl_db *db;
    uint32_t e[] = {1, 2, 2, 3, 3, 4, 5, 6};
    size_t nrels_before;
    int n_perms_before;
    int n_crules_before;
    uint32_t snap_before;
    int dirty_before;
    uint32_t id1, id2, id3;

    TEST("T11: dl_query_magic leaves db byte-identical");

    setup_db(&db, "t11");
    load_rows(db, "edge", 2, e, 4, "t11");
    load_tc_rules(db);
    assert(dl_compile(db) == 0);

    /* interner sentinels: id2 should be id1+1, id3 should be id2+1 */
    id1 = dl_intern_str(db, "m8sentinel_alpha");
    id2 = dl_intern_str(db, "m8sentinel_beta");
    assert(id2 == id1 + 1);

    nrels_before  = db->nrels;
    n_perms_before = db->n_perms;
    n_crules_before = db->n_crules;
    snap_before   = db->snap_version;
    dirty_before  = db->fixpoint_dirty;

    {
        tuple_set rm;
        uint32_t leading[1] = { 1 };
        memset(&rm, 0, sizeof(rm));
        long nm = dl_query_magic(db, "tc", leading, 1, tset_cb, &rm);
        (void)nm;
        assert(nm >= 0);
        tset_free(&rm);
    }

    /* interner must not have grown (no new syms during magic eval) */
    id3 = dl_intern_str(db, "m8sentinel_gamma");
    if (id3 != id2 + 1) {
        FAIL("interner mutated during dl_query_magic");
        teardown_db(db, "t11");
        return;
    }

    if (db->nrels != nrels_before ||
        db->n_perms != n_perms_before ||
        db->n_crules != n_crules_before ||
        db->snap_version != snap_before ||
        db->fixpoint_dirty != dirty_before) {
        FAIL("db field mutated during dl_query_magic");
        teardown_db(db, "t11");
        return;
    }

    /* And dl_query_bound still returns correct results afterwards. */
    if (!cmp_bound_magic(db, 1) || !cmp_bound_magic(db, 5)) {
        FAIL("post-magic bound query inconsistent");
        teardown_db(db, "t11");
        return;
    }

    PASS();
    teardown_db(db, "t11");
}

/* ─── T12: magic works without a prior dl_compile ─────────────────────── */

static void test_t12_no_compile(void)
{
    dl_db *db;
    uint32_t e[] = {1, 2, 2, 3, 3, 4, 4, 5};

    TEST("T12: magic re-evaluates scoped fixpoint without dl_compile");

    setup_db(&db, "t12");
    load_rows(db, "edge", 2, e, 4, "t12");
    load_tc_rules(db);
    /* NOTE: no dl_compile — tc is NOT materialized in db */

    {
        tuple_set rm, rb;
        uint32_t leading[1] = { 1 };
        memset(&rm, 0, sizeof(rm));
        memset(&rb, 0, sizeof(rb));

        long nm = dl_query_magic(db, "tc", leading, 1, tset_cb, &rm);
        assert(nm >= 0);

        /* Now materialize + full bound query for ground truth. */
        assert(dl_compile(db) == 0);
        long nb = dl_query_bound(db, "tc", leading, 1, tset_cb, &rb);

        if (nb < 0 || nm != nb || !tset_sorted_eq(&rb, &rm)) {
            FAIL("no-compile magic != bound");
            tset_free(&rb); tset_free(&rm);
            teardown_db(db, "t12");
            return;
        }
        assert(nm == 4);  /* tc(1,*) = {2,3,4,5} */
        tset_free(&rb);
        tset_free(&rm);
    }
    PASS();
    teardown_db(db, "t12");
}

/* ─── T13: property test (backstop) ───────────────────────────────────── */

static uint32_t rng_state = 0x9E3779B9u;
static uint32_t rng_next(void)
{
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    rng_state = x;
    return x;
}
static uint32_t rng_rand(uint32_t n) { return n ? rng_next() % n : 0; }

static void test_t13_property(void)
{
    TEST("T13: property — 200 random graphs x 5 sources, bound == magic");

    int iter;
    for (iter = 0; iter < 200; iter++) {
        dl_db *db;
        char suffix[32];
        int N = 8 + (int)(iter % 12);         /* 8..19 nodes */
        int M = 20 + (int)((iter * 7) % 60);  /* 20..79 edges */
        uint32_t *edges;
        int ei;
        uint32_t sources[5];
        int si;

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
            if (!cmp_bound_magic(db, sources[si])) {
                printf("  iter %d source %u mismatch\n", iter, sources[si]);
                FAIL("property test failed");
                teardown_db(db, suffix);
                return;
            }
        }
        teardown_db(db, suffix);
    }
    PASS();
}

/* ─── T14-T19: multi-predicate dependency closure ─────────────────────── */

/* Compare dl_query_bound vs dl_query_magic for a generic arity-2 goal
 * rel(src, ?) with k=1. */
static int cmp_bound_magic_rel(dl_db *db, const char *rel, uint32_t src)
{
    tuple_set rb, rm;
    uint32_t leading[1];
    long nb, nm;
    int eq;

    memset(&rb, 0, sizeof(rb));
    memset(&rm, 0, sizeof(rm));
    leading[0] = src;

    nb = dl_query_bound(db, rel, leading, 1, tset_cb, &rb);
    nm = dl_query_magic(db, rel, leading, 1, tset_cb, &rm);

    if (nb < 0 || nm < 0) { tset_free(&rb); tset_free(&rm); return 0; }
    if (nb != nm) { tset_free(&rb); tset_free(&rm); return 0; }
    eq = tset_sorted_eq(&rb, &rm);
    tset_free(&rb);
    tset_free(&rm);
    return eq;
}

/* path (non-recursive) depends on tc (self-recursive) over `edge`. */
static void load_path_tc_rules(dl_db *db)
{
    int rc = dl_load_rules(db,
        "path(X,Y):-edge(X,Y).\n"
        "path(X,Y):-edge(X,Z),tc(Z,Y).\n"
        "tc(X,Y):-edge(X,Y).\n"
        "tc(X,Y):-edge(X,Z),tc(Z,Y).\n");
    assert(rc == 0);
}

/* path (SELF-recursive) depends on tc (self-recursive) over `edge` — two
 * distinct recursive SCCs, exercising the chained-recursive-dependent
 * strict-stratification path. */
static void load_rpath_tc_rules(dl_db *db)
{
    int rc = dl_load_rules(db,
        "path(X,Y):-edge(X,Y).\n"
        "path(X,Y):-edge(X,Z),path(Z,Y).\n"
        "path(X,Y):-edge(X,Z),tc(Z,Y).\n"
        "tc(X,Y):-edge(X,Y).\n"
        "tc(X,Y):-edge(X,Z),tc(Z,Y).\n");
    assert(rc == 0);
}

/* ─── T14: 2-pred DAG path+tc ─────────────────────────────────────────── */

static void test_t14_multipred_path_tc(void)
{
    dl_db *db;
    uint32_t sources[4];
    int i;

    TEST("T14: 2-pred DAG path+tc — bound vs magic for several sources");

    /* 14a: non-recursive path depends on recursive tc (stratification
     * case 1: recursive -> non-recursive dependent). */
    {
        uint32_t e[] = {1,2, 2,3, 3,4, 4,5, 2,10, 10,11, 5,6, 6,7,
                        20,21, 21,22, 1,20, 7,1};
        setup_db(&db, "t14a");
        load_rows(db, "edge", 2, e, 12, "t14a");
        load_path_tc_rules(db);
        assert(dl_compile(db) == 0);
        sources[0] = 1; sources[1] = 2; sources[2] = 20; sources[3] = 99;
        for (i = 0; i < 4; i++) {
            if (!cmp_bound_magic_rel(db, "path", sources[i])) {
                printf("  source %u mismatch (non-recursive path)\n",
                       sources[i]);
                FAIL("T14 path+tc bound vs magic mismatch");
                teardown_db(db, "t14a");
                return;
            }
        }
        teardown_db(db, "t14a");
    }

    /* 14b: SELF-recursive path depends on recursive tc (stratification
     * case 2: recursive -> recursive chained dependent, two SCCs). */
    {
        uint32_t e[] = {1,2, 2,3, 3,1, 1,4, 4,5, 5,6, 10,11, 11,12, 6,10};
        setup_db(&db, "t14b");
        load_rows(db, "edge", 2, e, 9, "t14b");
        load_rpath_tc_rules(db);
        assert(dl_compile(db) == 0);
        sources[0] = 1; sources[1] = 4; sources[2] = 10; sources[3] = 99;
        for (i = 0; i < 4; i++) {
            if (!cmp_bound_magic_rel(db, "path", sources[i])) {
                printf("  source %u mismatch (recursive path)\n", sources[i]);
                FAIL("T14 recursive-path+tc bound vs magic mismatch");
                teardown_db(db, "t14b");
                return;
            }
        }
        teardown_db(db, "t14b");
    }
    PASS();
}

/* ─── T15: 3-pred linear chain ────────────────────────────────────────── */

static void test_t15_three_pred_chain(void)
{
    dl_db *db;
    uint32_t e[] = {1,2, 2,3, 3,4, 10,11, 11,12, 4,20, 20,21};
    uint32_t sources[3] = {1, 10, 99};
    int i;

    TEST("T15: 3-pred linear chain r3->r2->r1->edge");

    setup_db(&db, "t15");
    load_rows(db, "edge", 2, e, 7, "t15");
    assert(dl_load_rules(db,
        "r1(X,Y):-edge(X,Y).\n"
        "r2(X,Y):-r1(X,Y).\n"
        "r3(X,Y):-r2(X,Y).\n") == 0);
    assert(dl_compile(db) == 0);

    for (i = 0; i < 3; i++) {
        if (!cmp_bound_magic_rel(db, "r3", sources[i])) {
            printf("  source %u mismatch\n", sources[i]);
            FAIL("T15 3-pred chain bound vs magic mismatch");
            teardown_db(db, "t15");
            return;
        }
    }
    PASS();
    teardown_db(db, "t15");
}

/* ─── T16: non-recursive goal depends on self-recursive pred ──────────── */

static void test_t16_nonrecursive_goal_recursive_dep(void)
{
    dl_db *db;
    uint32_t e[] = {1,2, 2,3, 3,4, 1,5, 5,6};
    uint32_t sources[3] = {1, 2, 99};
    int i;

    TEST("T16: non-recursive goal reach depends on self-recursive tc");

    setup_db(&db, "t16");
    load_rows(db, "edge", 2, e, 5, "t16");
    assert(dl_load_rules(db,
        "reach(X,Y):-edge(X,Y).\n"
        "reach(X,Y):-edge(X,Z),tc(Z,Y).\n"
        "tc(X,Y):-edge(X,Y).\n"
        "tc(X,Y):-edge(X,Z),tc(Z,Y).\n") == 0);
    assert(dl_compile(db) == 0);

    for (i = 0; i < 3; i++) {
        if (!cmp_bound_magic_rel(db, "reach", sources[i])) {
            printf("  source %u mismatch\n", sources[i]);
            FAIL("T16 reach+tc bound vs magic mismatch");
            teardown_db(db, "t16");
            return;
        }
    }
    teardown_db(db, "t16");

    /* 16b: a dependency called with a FULLY-bound (bb) adornment — the
     * non-goal predicate q gets a different (more bound) adornment than the
     * goal p, exercising adornment generality across the predicate
     * boundary (trap: bound-var propagation into Q's call site). */
    {
        uint32_t e2[] = {1,2, 1,3, 2,4};
        setup_db(&db, "t16b");
        load_rows(db, "edge", 2, e2, 3, "t16b");
        assert(dl_load_rules(db,
            "q(X,Y):-edge(X,Y).\n"
            "p(X,Y):-edge(X,Y),q(X,Y).\n") == 0);
        assert(dl_compile(db) == 0);
        if (!cmp_bound_magic_rel(db, "p", 1)) {
            FAIL("T16b bb-adorned dependency mismatch");
            teardown_db(db, "t16b");
            return;
        }
        teardown_db(db, "t16b");
    }
    PASS();
}

/* ─── T17: REJECT cross-predicate mutual recursion ────────────────────── */

static void test_t17_reject_mutual_recursion(void)
{
    dl_db *db;
    uint32_t e[] = {1,2, 2,3};
    tuple_set r;
    uint32_t leading[1] = {1};

    TEST("T17: cross-predicate mutual recursion p:-q,q:-p → -1");

    setup_db(&db, "t17");
    load_rows(db, "edge", 2, e, 2, "t17");
    assert(dl_load_rules(db,
        "p(X,Y):-edge(X,Y).\n"
        "p(X,Y):-q(X,Y).\n"
        "q(X,Y):-p(X,Y).\n") == 0);
    memset(&r, 0, sizeof(r));
    if (dl_query_magic(db, "p", leading, 1, tset_cb, &r) != -1) {
        FAIL("mutual recursion not rejected");
        tset_free(&r);
        teardown_db(db, "t17");
        return;
    }
    tset_free(&r);
    PASS();
    teardown_db(db, "t17");
}

/* ─── T18: REJECT multiple distinct adornments ────────────────────────── */

static void test_t18_reject_multiple_adornments(void)
{
    dl_db *db;
    uint32_t e[] = {1,2, 2,3};
    tuple_set r;
    uint32_t leading[1] = {1};

    TEST("T18: multiple distinct adornments (p bf, q calls p bb) → -1");

    /* 18a: the plan's literal example — note it also contains a p<->q
     * cycle, so it is rejected by the mutual-recursion check first. */
    setup_db(&db, "t18a");
    load_rows(db, "edge", 2, e, 2, "t18a");
    assert(dl_load_rules(db,
        "p(X,Y):-edge(X,Y),q(X).\n"
        "p(X,Y):-edge(X,Z),p(Z,Y).\n"
        "q(X):-edge(X,Y),p(X,Y).\n") == 0);
    memset(&r, 0, sizeof(r));
    if (dl_query_magic(db, "p", leading, 1, tset_cb, &r) != -1) {
        FAIL("multiple adornments not rejected");
        tset_free(&r);
        teardown_db(db, "t18a");
        return;
    }
    tset_free(&r);
    teardown_db(db, "t18a");

    /* 18b: a PURE acyclic multiple-adornment case (no mutual recursion):
     * goal r calls p with adorn bf, and also calls s, whose rule calls p
     * with adorn bb (both args bound via edge).  r->p and r->s->p form a
     * DAG, so the conflict is genuinely the distinct-adornment reject. */
    setup_db(&db, "t18b");
    load_rows(db, "edge", 2, e, 2, "t18b");
    assert(dl_load_rules(db,
        "r(X,Y):-p(X,Y).\n"
        "r(X,Y):-s(X,Y).\n"
        "s(X,Y):-edge(X,Y),p(X,Y).\n"
        "p(X,Y):-edge(X,Y).\n") == 0);
    memset(&r, 0, sizeof(r));
    if (dl_query_magic(db, "r", leading, 1, tset_cb, &r) != -1) {
        FAIL("pure multiple adornments not rejected");
        tset_free(&r);
        teardown_db(db, "t18b");
        return;
    }
    tset_free(&r);
    PASS();
    teardown_db(db, "t18b");
}

/* ─── T19: extended multi-predicate property test ─────────────────────── */

static void test_t19_multipred_property(void)
{
    TEST("T19: property — 120 random graphs x 4 sources, path+tc bound==magic");

    int iter;
    for (iter = 0; iter < 120; iter++) {
        dl_db *db;
        char suffix[32];
        int N = 6 + (int)(iter % 10);         /* 6..15 nodes */
        int M = 15 + (int)((iter * 5) % 45);  /* 15..59 edges */
        uint32_t *edges;
        int ei;
        uint32_t sources[4];
        int si;

        snprintf(suffix, sizeof(suffix), "mp_%d", iter);
        setup_db(&db, suffix);

        edges = malloc((size_t)M * 2 * sizeof(uint32_t));
        for (ei = 0; ei < M; ei++) {
            edges[ei*2]     = rng_rand((uint32_t)N) + 1;
            edges[ei*2 + 1] = rng_rand((uint32_t)N) + 1;
        }
        load_rows(db, "edge", 2, edges, M, suffix);
        free(edges);

        load_rpath_tc_rules(db);   /* recursive path + recursive tc */
        assert(dl_compile(db) == 0);

        for (si = 0; si < 4; si++) {
            sources[si] = rng_rand((uint32_t)(N + 2)) + 1;
            if (!cmp_bound_magic_rel(db, "path", sources[si])) {
                printf("  iter %d source %u mismatch\n", iter, sources[si]);
                FAIL("multipred property test failed");
                teardown_db(db, suffix);
                return;
            }
        }
        teardown_db(db, suffix);
    }
    PASS();
}

/* ─── T20-T28: non-leading adornments (fb/bfb) ────────────────────────── */

/* Filter a materialized tuple_set on the bound positions of `adorn`:
 * for each position i where adorn[i]=='b', keep rows with row[i]==vals[b].
 * Ground-truth oracle for the non-leading magic tests. */
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

/* Compare dl_query_magic_adorn against the ground-truth filter of the full
 * materialization (dl_query + tset_filter_adorn).  Returns 1 iff equal. */
static int cmp_adorn_magic(dl_db *db, const char *rel, const char *adorn,
                           const uint32_t *vals, uint8_t nvals)
{
    tuple_set full, ground, rm;
    long nq, nm;
    int eq;

    memset(&full, 0, sizeof(full));
    memset(&ground, 0, sizeof(ground));
    memset(&rm, 0, sizeof(rm));

    nq = dl_query(db, rel, tset_cb, &full);
    if (nq < 0) { tset_free(&full); tset_free(&ground); tset_free(&rm); return 0; }
    if (tset_filter_adorn(&full, adorn, vals, nvals, &ground) != 0) {
        tset_free(&full); tset_free(&ground); tset_free(&rm); return 0;
    }
    nm = dl_query_magic_adorn(db, rel, adorn, vals, nvals, tset_cb, &rm);
    if (nm < 0) { tset_free(&full); tset_free(&ground); tset_free(&rm); return 0; }
    if ((long)ground.count != nm) {
        tset_free(&full); tset_free(&ground); tset_free(&rm); return 0;
    }
    eq = tset_sorted_eq(&ground, &rm);
    tset_free(&full);
    tset_free(&ground);
    tset_free(&rm);
    return eq;
}

/* ─── T20: fb on 2-arity EDB ───────────────────────────────────────────── */

static void test_t20_fb_edb(void)
{
    dl_db *db;
    uint32_t e[] = {1,42, 2,42, 3,7, 1,7, 9,42};
    uint32_t vals[1] = { 42 };

    TEST("T20: fb on 2-arity EDB (edge) — non-leading filter");

    setup_db(&db, "t20");
    load_rows(db, "edge", 2, e, 5, "t20");

    if (!cmp_adorn_magic(db, "edge", "fb", vals, 1)) {
        FAIL("T20 fb EDB mismatch");
        teardown_db(db, "t20");
        return;
    }
    {
        tuple_set r;
        memset(&r, 0, sizeof(r));
        long n = dl_query_magic_adorn(db, "edge", "fb", vals, 1, tset_cb, &r);
        if (n != 3 || r.count != 3) {  /* edge(*,42) = {1,2,9} */
            FAIL("T20 expected 3 rows (cols[1]==42)");
            tset_free(&r);
            teardown_db(db, "t20");
            return;
        }
        tset_free(&r);
    }
    PASS();
    teardown_db(db, "t20");
}

/* ─── T21: fbf on 3-arity IDB path3 ────────────────────────────────────── */

static void test_t21_fbf_path3(void)
{
    dl_db *db;
    uint32_t e[] = {1,42, 42,3, 42,5, 1,7, 7,3};
    uint32_t vals[1] = { 42 };

    TEST("T21: fbf on 3-arity IDB path3 — middle position bound");

    setup_db(&db, "t21");
    load_rows(db, "edge", 2, e, 5, "t21");
    assert(dl_load_rules(db, "path3(X,Y,Z):-edge(X,Y),edge(Y,Z).\n") == 0);
    assert(dl_compile(db) == 0);

    /* Bind ONLY the middle position (Y=42): adorn "fbf", vals=[42]. */
    if (!cmp_adorn_magic(db, "path3", "fbf", vals, 1)) {
        FAIL("T21 fbf path3 mismatch");
        teardown_db(db, "t21");
        return;
    }
    PASS();
    teardown_db(db, "t21");
}

/* ─── T22: SIPS accepts non-leading self-recursion (fb) ────────────────── */

static void test_t22_fb_selfrec_positive(void)
{
    dl_db *db;
    uint32_t e[] = {1,2, 2,3, 3,4, 4,5, 1,5};
    uint32_t targets[4] = {2, 4, 5, 42};
    int ti;

    TEST("T22: non-leading self-recursive tc(*,k) — SIPS accepts & correct");

    setup_db(&db, "t22");
    load_rows(db, "edge", 2, e, 5, "t22");
    load_tc_rules(db);
    assert(dl_compile(db) == 0);

    for (ti = 0; ti < 4; ti++) {
        uint32_t vals[1] = { targets[ti] };
        if (!cmp_adorn_magic(db, "tc", "fb", vals, 1)) {
            printf("  target %u mismatch\n", targets[ti]);
            FAIL("T22 fb magic != full-filter");
            teardown_db(db, "t22");
            return;
        }
    }
    PASS();
    teardown_db(db, "t22");
}

/* ─── T23: multi-predicate p^fb → q^fb closure ─────────────────────────── */

static void test_t23_multipred_fb(void)
{
    dl_db *db;
    uint32_t e[] = {1,42, 2,42, 3,7, 9,42};
    uint32_t vals[1] = { 42 };

    TEST("T23: p^fb propagates to q^fb (multi-predicate non-leading closure)");

    setup_db(&db, "t23");
    load_rows(db, "edge", 2, e, 4, "t23");
    assert(dl_load_rules(db,
        "p(X,Y):-q(X,Y).\n"
        "q(X,Y):-edge(X,Y).\n") == 0);
    assert(dl_compile(db) == 0);

    if (!cmp_adorn_magic(db, "p", "fb", vals, 1)) {
        FAIL("T23 p^fb/q^fb mismatch");
        teardown_db(db, "t23");
        return;
    }
    PASS();
    teardown_db(db, "t23");
}

/* ─── T24: all-f adorn routes to dl_query ──────────────────────────────── */

static void test_t24_allf_route(void)
{
    dl_db *db;
    uint32_t e[] = {1,2, 2,3, 3,4};

    TEST("T24: all-f adorn (ff, nvals=0) routes to dl_query");

    setup_db(&db, "t24");
    load_rows(db, "edge", 2, e, 3, "t24");
    load_tc_rules(db);
    assert(dl_compile(db) == 0);

    {
        tuple_set rq, rm;
        memset(&rq, 0, sizeof(rq));
        memset(&rm, 0, sizeof(rm));
        long nq = dl_query(db, "tc", tset_cb, &rq);
        long nm = dl_query_magic_adorn(db, "tc", "ff", NULL, 0, tset_cb, &rm);
        if (nq < 0 || nm < 0 || nq != nm || !tset_sorted_eq(&rq, &rm)) {
            FAIL("T24 all-f adorn != dl_query");
            tset_free(&rq); tset_free(&rm);
            teardown_db(db, "t24");
            return;
        }
        tset_free(&rq); tset_free(&rm);
    }
    PASS();
    teardown_db(db, "t24");
}

/* ─── T25: REJECT nvals/count mismatch ─────────────────────────────────── */

static void test_t25_reject_nvals_mismatch(void)
{
    dl_db *db;
    uint32_t e[] = {1,2, 2,3};
    uint32_t vals[2] = { 1, 42 };

    TEST("T25: adorn 'fb' (1 b) with nvals=2 → reject");

    setup_db(&db, "t25");
    load_rows(db, "edge", 2, e, 2, "t25");
    load_tc_rules(db);
    assert(dl_compile(db) == 0);

    {
        tuple_set r;
        memset(&r, 0, sizeof(r));
        if (dl_query_magic_adorn(db, "tc", "fb", vals, 2, tset_cb, &r) != -1) {
            FAIL("T25 nvals/count mismatch not rejected");
            tset_free(&r);
            teardown_db(db, "t25");
            return;
        }
        tset_free(&r);
    }
    PASS();
    teardown_db(db, "t25");
}

/* ─── T26: REJECT length mismatch ──────────────────────────────────────── */

static void test_t26_reject_length_mismatch(void)
{
    dl_db *db;
    uint32_t e[] = {1,2, 2,3};
    uint32_t vals[2] = { 1, 42 };

    TEST("T26: adorn 'fbf' for arity-2 goal → reject");

    setup_db(&db, "t26");
    load_rows(db, "edge", 2, e, 2, "t26");
    load_tc_rules(db);
    assert(dl_compile(db) == 0);

    {
        tuple_set r;
        memset(&r, 0, sizeof(r));
        if (dl_query_magic_adorn(db, "tc", "fbf", vals, 2, tset_cb, &r) != -1) {
            FAIL("T26 length mismatch not rejected");
            tset_free(&r);
            teardown_db(db, "t26");
            return;
        }
        tset_free(&r);
    }
    PASS();
    teardown_db(db, "t26");
}

/* ─── T27: no-mutation under dl_query_magic_adorn ──────────────────────── */

static void test_t27_no_mutation_adorn(void)
{
    dl_db *db;
    uint32_t e[] = {1,2, 2,3, 3,4, 5,6};
    size_t nrels_before;
    int n_perms_before;
    int n_crules_before;
    uint32_t snap_before;
    int dirty_before;
    uint32_t id1, id2, id3;

    TEST("T27: dl_query_magic_adorn leaves db byte-identical");

    setup_db(&db, "t27");
    load_rows(db, "edge", 2, e, 4, "t27");
    load_tc_rules(db);
    assert(dl_compile(db) == 0);

    id1 = dl_intern_str(db, "m8sentinel_alpha");
    id2 = dl_intern_str(db, "m8sentinel_beta");
    assert(id2 == id1 + 1);

    nrels_before  = db->nrels;
    n_perms_before = db->n_perms;
    n_crules_before = db->n_crules;
    snap_before   = db->snap_version;
    dirty_before  = db->fixpoint_dirty;

    {
        tuple_set rm;
        uint32_t vals[1] = { 1 };
        memset(&rm, 0, sizeof(rm));
        long nm = dl_query_magic_adorn(db, "tc", "bf", vals, 1, tset_cb, &rm);
        (void)nm;
        assert(nm >= 0);
        tset_free(&rm);
    }

    id3 = dl_intern_str(db, "m8sentinel_gamma");
    if (id3 != id2 + 1) {
        FAIL("interner mutated during dl_query_magic_adorn");
        teardown_db(db, "t27");
        return;
    }

    if (db->nrels != nrels_before ||
        db->n_perms != n_perms_before ||
        db->n_crules != n_crules_before ||
        db->snap_version != snap_before ||
        db->fixpoint_dirty != dirty_before) {
        FAIL("db field mutated during dl_query_magic_adorn");
        teardown_db(db, "t27");
        return;
    }

    if (!cmp_bound_magic(db, 1) || !cmp_bound_magic(db, 5)) {
        FAIL("post-adorn bound query inconsistent");
        teardown_db(db, "t27");
        return;
    }

    PASS();
    teardown_db(db, "t27");
}

/* ─── T28: property test on non-recursive 2-hop path2 ──────────────────── */

static void test_t28_path2_property(void)
{
    TEST("T28: property — 150 random graphs x 5 targets, path2 fb==full-filter");

    int iter;
    for (iter = 0; iter < 150; iter++) {
        dl_db *db;
        char suffix[32];
        int N = 8 + (int)(iter % 12);         /* 8..19 nodes */
        int M = 20 + (int)((iter * 7) % 60);  /* 20..79 edges */
        uint32_t *edges;
        int ei;
        uint32_t targets[5];
        int ti;

        snprintf(suffix, sizeof(suffix), "p2_%d", iter);
        setup_db(&db, suffix);

        edges = malloc((size_t)M * 2 * sizeof(uint32_t));
        for (ei = 0; ei < M; ei++) {
            edges[ei*2]     = rng_rand((uint32_t)N) + 1;
            edges[ei*2 + 1] = rng_rand((uint32_t)N) + 1;
        }
        load_rows(db, "edge", 2, edges, M, suffix);
        free(edges);

        assert(dl_load_rules(db, "path2(X,Y):-edge(X,Z),edge(Z,Y).\n") == 0);
        assert(dl_compile(db) == 0);

        for (ti = 0; ti < 5; ti++) {
            uint32_t vals[1];
            targets[ti] = rng_rand((uint32_t)(N + 2)) + 1;
            vals[0] = targets[ti];
            if (!cmp_adorn_magic(db, "path2", "fb", vals, 1)) {
                printf("  iter %d target %u mismatch\n", iter, targets[ti]);
                FAIL("T28 path2 property test failed");
                teardown_db(db, suffix);
                return;
            }
        }
        teardown_db(db, suffix);
    }
    PASS();
}

/* ─── T29: suboptimal user order (tc before edge) — SIPS reorders ──────── */

static void test_t29_suboptimal_order(void)
{
    dl_db *db;
    uint32_t e[] = {1,2, 2,3, 3,4, 4,5, 1,5, 10,11, 11,12};
    uint32_t sources[3] = {1, 10, 99};
    int i;

    TEST("T29: suboptimal order path(X,Y):-tc(Z,Y),edge(X,Z) — SIPS correct");

    setup_db(&db, "t29");
    load_rows(db, "edge", 2, e, 7, "t29");
    assert(dl_load_rules(db,
        "tc(X,Y):-edge(X,Y).\n"
        "tc(X,Y):-edge(X,Z),tc(Z,Y).\n"
        "path(X,Y):-tc(Z,Y),edge(X,Z).\n") == 0);
    assert(dl_compile(db) == 0);

    for (i = 0; i < 3; i++) {
        if (!cmp_bound_magic_rel(db, "path", sources[i])) {
            printf("  source %u mismatch\n", sources[i]);
            FAIL("T29 suboptimal order bound vs magic mismatch");
            teardown_db(db, "t29");
            return;
        }
    }
    PASS();
    teardown_db(db, "t29");
}

/* ─── T30: multi-predicate reorder changes the dependent's adornment ───── */

static void test_t30_multipred_reorder(void)
{
    dl_db *db;
    uint32_t e[] = {1,2, 2,3, 3,4, 10,11, 11,12};
    uint32_t sources[3] = {1, 10, 99};
    int i;

    TEST("T30: r:-p; p recursive (bad order) — SIPS reorders, correct");

    setup_db(&db, "t30");
    load_rows(db, "edge", 2, e, 5, "t30");
    assert(dl_load_rules(db,
        "r(X,Y):-p(X,Y).\n"
        "p(X,Y):-edge(X,Y).\n"
        "p(X,Y):-p(Z,Y),edge(X,Z).\n") == 0);
    assert(dl_compile(db) == 0);

    for (i = 0; i < 3; i++) {
        if (!cmp_bound_magic_rel(db, "r", sources[i])) {
            printf("  source %u mismatch\n", sources[i]);
            FAIL("T30 multipred reorder bound vs magic mismatch");
            teardown_db(db, "t30");
            return;
        }
    }
    PASS();
    teardown_db(db, "t30");
}

/* ─── T31: equality-heavy chain (two equalities) ───────────────────────── */

static void test_t31_equality_heavy(void)
{
    dl_db *db;
    uint32_t e[] = {1,2, 2,3, 3,4, 4,5, 10,11};
    uint32_t sources[3] = {1, 2, 99};
    int i;

    TEST("T31: equality-heavy tc(X,Y):-edge(X,Z),Z=W,W=V,tc(V,Y)");

    setup_db(&db, "t31");
    load_rows(db, "edge", 2, e, 5, "t31");
    assert(dl_load_rules(db,
        "tc(X,Y):-edge(X,Y).\n"
        "tc(X,Y):-edge(X,Z),Z=W,W=V,tc(V,Y).\n") == 0);
    assert(dl_compile(db) == 0);

    for (i = 0; i < 3; i++) {
        if (!cmp_bound_magic(db, sources[i])) {
            printf("  source %u mismatch\n", sources[i]);
            FAIL("T31 equality-heavy bound vs magic mismatch");
            teardown_db(db, "t31");
            return;
        }
    }
    PASS();
    teardown_db(db, "t31");
}

/* ─── T32: TC-FB positive (formerly-T22 shape, richer graph) ───────────── */

static void test_t32_tc_fb_positive(void)
{
    dl_db *db;
    uint32_t e[] = {1,2, 2,3, 3,1, 1,4, 4,5, 10,11, 11,10};
    uint32_t targets[3] = {1, 5, 42};
    int ti;

    TEST("T32: TC-FB (fb) for several targets — SIPS accepts & correct");

    setup_db(&db, "t32");
    load_rows(db, "edge", 2, e, 7, "t32");
    load_tc_rules(db);
    assert(dl_compile(db) == 0);

    for (ti = 0; ti < 3; ti++) {
        uint32_t vals[1] = { targets[ti] };
        if (!cmp_adorn_magic(db, "tc", "fb", vals, 1)) {
            printf("  target %u mismatch\n", targets[ti]);
            FAIL("T32 fb magic != full-filter");
            teardown_db(db, "t32");
            return;
        }
    }
    PASS();
    teardown_db(db, "t32");
}

/* ─── T33: body-order independence property test ───────────────────────── */

static void test_t33_order_independence(void)
{
    TEST("T33: property — 100 graphs x 3 sources, body-order invariance");

    int iter;
    for (iter = 0; iter < 100; iter++) {
        dl_db *dba, *dbb;
        char sa[32], sb[32];
        int N = 8 + (int)(iter % 12);         /* 8..19 nodes */
        int M = 20 + (int)((iter * 7) % 60);  /* 20..79 edges */
        uint32_t *edges;
        int ei, si;

        snprintf(sa, sizeof(sa), "oi_a_%d", iter);
        snprintf(sb, sizeof(sb), "oi_b_%d", iter);
        setup_db(&dba, sa);
        setup_db(&dbb, sb);

        edges = malloc((size_t)M * 2 * sizeof(uint32_t));
        for (ei = 0; ei < M; ei++) {
            edges[ei*2]     = rng_rand((uint32_t)N) + 1;
            edges[ei*2 + 1] = rng_rand((uint32_t)N) + 1;
        }
        load_rows(dba, "edge", 2, edges, M, sa);
        load_rows(dbb, "edge", 2, edges, M, sb);
        free(edges);

        assert(dl_load_rules(dba,
            "tc(X,Y):-edge(X,Y).\n"
            "tc(X,Y):-edge(X,Z),tc(Z,Y).\n") == 0);
        assert(dl_load_rules(dbb,
            "tc(X,Y):-edge(X,Y).\n"
            "tc(X,Y):-tc(Z,Y),edge(X,Z).\n") == 0);
        assert(dl_compile(dba) == 0);
        assert(dl_compile(dbb) == 0);

        for (si = 0; si < 3; si++) {
            uint32_t src = rng_rand((uint32_t)(N + 2)) + 1;
            uint32_t leading[1];
            tuple_set ra, rb;
            long na, nb;
            leading[0] = src;
            memset(&ra, 0, sizeof(ra));
            memset(&rb, 0, sizeof(rb));
            na = dl_query_magic(dba, "tc", leading, 1, tset_cb, &ra);
            nb = dl_query_magic(dbb, "tc", leading, 1, tset_cb, &rb);
            if (na < 0 || nb < 0 || na != nb || !tset_sorted_eq(&ra, &rb)) {
                printf("  iter %d src %u mismatch (a=%ld b=%ld)\n",
                       iter, src, na, nb);
                FAIL("T33 order-independence mismatch");
                tset_free(&ra); tset_free(&rb);
                teardown_db(dba, sa);
                teardown_db(dbb, sb);
                return;
            }
            tset_free(&ra); tset_free(&rb);
        }
        teardown_db(dba, sa);
        teardown_db(dbb, sb);
    }
    PASS();
}

/* ─── T34-T38: negation + aggregates in adorned rules ─────────────────── */

static void test_t34_negated_nonclosure_idb(void)
{
    dl_db *db;
    uint32_t e[] = {1,2, 2,3};

    TEST("T34: negated non-closure IDB (!tc) — magic == bound (empty)");

    setup_db(&db, "t34");
    load_rows(db, "edge", 2, e, 2, "t34");
    assert(dl_load_rules(db,
        "tc(X,Y):-edge(X,Y).\n"
        "tc(X,Y):-edge(X,Z),tc(Z,Y).\n"
        "ntc(X,Y):-edge(X,Y),!tc(X,Y).\n") == 0);

    /* No explicit dl_compile: the negation guard in dl_query_magic_adorn
     * must auto-compile so !tc sees the FULL materialization.  Without it,
     * tc would be empty and ntc would wrongly equal edge ({(1,2)}). */
    {
        tuple_set rm;
        uint32_t leading[1] = { 1 };
        memset(&rm, 0, sizeof(rm));
        long nm = dl_query_magic(db, "ntc", leading, 1, tset_cb, &rm);
        if (nm < 0) {
            FAIL("T34 negated non-closure IDB rejected");
            tset_free(&rm);
            teardown_db(db, "t34");
            return;
        }
        if (nm != 0) {
            FAIL("T34 expected empty ntc (tc covers all edges)");
            tset_free(&rm);
            teardown_db(db, "t34");
            return;
        }
        tset_free(&rm);
    }

    /* Materialized now (auto-compile above); bound vs magic must agree. */
    if (!cmp_bound_magic_rel(db, "ntc", 1) ||
        !cmp_bound_magic_rel(db, "ntc", 2) ||
        !cmp_bound_magic_rel(db, "ntc", 99)) {
        FAIL("T34 bound vs magic mismatch");
        teardown_db(db, "t34");
        return;
    }

    PASS();
    teardown_db(db, "t34");
}

static void test_t35_aggregate_edb(void)
{
    dl_db *db;
    uint32_t w[] = {1,5, 1,3, 2,10, 2,20, 2,30};
    uint32_t sources[2] = {1, 2};
    int si;

    TEST("T35: sum/min/max over EDB — magic == bound");

    setup_db(&db, "t35");
    load_rows(db, "edge", 2, w, 5, "t35");
    assert(dl_load_rules(db,
        "sump(X,S):-edge(X,W),S=sum(W).\n"
        "minp(X,M):-edge(X,W),M=min(W).\n"
        "maxp(X,M):-edge(X,W),M=max(W).\n") == 0);
    assert(dl_compile(db) == 0);

    for (si = 0; si < 2; si++) {
        if (!cmp_bound_magic_rel(db, "sump", sources[si]) ||
            !cmp_bound_magic_rel(db, "minp", sources[si]) ||
            !cmp_bound_magic_rel(db, "maxp", sources[si])) {
            printf("  source %u mismatch\n", sources[si]);
            FAIL("T35 aggregate-EDB bound vs magic mismatch");
            teardown_db(db, "t35");
            return;
        }
    }

    PASS();
    teardown_db(db, "t35");
}

static void test_t36_reject_negated_closure_idb(void)
{
    dl_db *db;
    uint32_t e[] = {1,2, 2,3};
    tuple_set r;
    uint32_t leading[1] = {1};

    TEST("T36: negated closure IDB (!p where p in closure) → -1");

    setup_db(&db, "t36");
    load_rows(db, "edge", 2, e, 2, "t36");
    assert(dl_load_rules(db,
        "g(X,Y):-edge(X,Y),!p(X,Y).\n"
        "g(X,Y):-p(X,Y).\n"
        "p(X,Y):-edge(X,Y).\n") == 0);
    memset(&r, 0, sizeof(r));
    if (dl_query_magic(db, "g", leading, 1, tset_cb, &r) != -1) {
        FAIL("negated closure IDB not rejected");
        tset_free(&r);
        teardown_db(db, "t36");
        return;
    }
    tset_free(&r);
    PASS();
    teardown_db(db, "t36");
}

static void test_t37_reject_aggregate_over_closure(void)
{
    dl_db *db;
    uint32_t e[] = {1,2, 2,3};
    tuple_set r;
    uint32_t leading[1] = {1};

    TEST("T37: aggregate over closure IDB (count over tc) → -1");

    setup_db(&db, "t37");
    load_rows(db, "edge", 2, e, 2, "t37");
    assert(dl_load_rules(db,
        "tc(X,Y):-edge(X,Y).\n"
        "tc(X,Y):-edge(X,Z),tc(Z,Y).\n"
        "cnt(X,N):-tc(X,Y),N=count().\n") == 0);
    memset(&r, 0, sizeof(r));
    if (dl_query_magic(db, "cnt", leading, 1, tset_cb, &r) != -1) {
        FAIL("aggregate over closure IDB not rejected");
        tset_free(&r);
        teardown_db(db, "t37");
        return;
    }
    tset_free(&r);
    PASS();
    teardown_db(db, "t37");
}

static void test_t38_negated_edb_property(void)
{
    TEST("T38: property — 100 graphs x 3 sources, negated-EDB bound==magic");

    int iter;
    for (iter = 0; iter < 100; iter++) {
        dl_db *db;
        char suffix[32];
        int N = 8 + (int)(iter % 12);         /* 8..19 nodes */
        int M = 20 + (int)((iter * 7) % 60);  /* 20..79 edges */
        uint32_t *edges, *blocked;
        int ei;
        uint32_t sources[3];
        int si;

        snprintf(suffix, sizeof(suffix), "neg_%d", iter);
        setup_db(&db, suffix);

        edges = malloc((size_t)M * 2 * sizeof(uint32_t));
        for (ei = 0; ei < M; ei++) {
            edges[ei*2]     = rng_rand((uint32_t)N) + 1;
            edges[ei*2 + 1] = rng_rand((uint32_t)N) + 1;
        }
        load_rows(db, "edge", 2, edges, M, suffix);
        free(edges);

        /* Random blocked EDB: a random set of candidate edges. */
        {
            int nb = (int)(rng_rand((uint32_t)M) + 1);
            blocked = malloc((size_t)nb * 2 * sizeof(uint32_t));
            for (ei = 0; ei < nb; ei++) {
                blocked[ei*2]     = rng_rand((uint32_t)N) + 1;
                blocked[ei*2 + 1] = rng_rand((uint32_t)N) + 1;
            }
            load_rows(db, "blocked", 2, blocked, nb, suffix);
            free(blocked);
        }

        assert(dl_load_rules(db,
            "ntc(X,Y):-edge(X,Y),!blocked(X,Y).\n") == 0);
        assert(dl_compile(db) == 0);

        for (si = 0; si < 3; si++) {
            sources[si] = rng_rand((uint32_t)(N + 2)) + 1;
            if (!cmp_bound_magic_rel(db, "ntc", sources[si])) {
                printf("  iter %d source %u mismatch\n", iter, sources[si]);
                FAIL("T38 negated-EDB property test failed");
                teardown_db(db, suffix);
                return;
            }
        }
        teardown_db(db, suffix);
    }
    PASS();
}

/* ─── Main ────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("M8 Magic-Sets Tests (first slice)\n");
    printf("=================================\n\n");

    test_t1_chain();
    test_t2_star();
    test_t3_dense();
    test_t4_small_graphs();
    test_t5_absent_sources();
    test_t6_edb_goal();
    test_t7_equality();
    test_t8_multi_rule();
    test_t9_rejections();
    test_t10_k0();
    test_t11_no_mutation();
    test_t12_no_compile();
    test_t13_property();
    test_t14_multipred_path_tc();
    test_t15_three_pred_chain();
    test_t16_nonrecursive_goal_recursive_dep();
    test_t17_reject_mutual_recursion();
    test_t18_reject_multiple_adornments();
    test_t19_multipred_property();
    test_t20_fb_edb();
    test_t21_fbf_path3();
    test_t22_fb_selfrec_positive();
    test_t23_multipred_fb();
    test_t24_allf_route();
    test_t25_reject_nvals_mismatch();
    test_t26_reject_length_mismatch();
    test_t27_no_mutation_adorn();
    test_t28_path2_property();
    test_t29_suboptimal_order();
    test_t30_multipred_reorder();
    test_t31_equality_heavy();
    test_t32_tc_fb_positive();
    test_t33_order_independence();
    test_t34_negated_nonclosure_idb();
    test_t35_aggregate_edb();
    test_t36_reject_negated_closure_idb();
    test_t37_reject_aggregate_over_closure();
    test_t38_negated_edb_property();

    printf("\n%d tests run, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
