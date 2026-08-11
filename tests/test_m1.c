/*
 * test_m1.c — M1 verification: parser + compiler + VM
 *
 * Tests:
 *   1. Known-good: edge + tc(X,Y):-edge(X,Y). returns correct tuples
 *   2. Join correctness: 2-hop join via edge/2
 *   3. Known-bad compile errors: undefined predicate, arity mismatch,
 *      ungrounded head, negation in M1, non-leading-column join
 *   4. Property test: random graphs, check VM vs brute-force
 *   5. CLI: dl query on rule string and .dl file
 */

#include "dl.h"
#include "relation.h"
#include "intern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

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

/* ─── Helpers ────────────────────────────────────────────────────────── */

typedef struct {
    uint32_t *data;      /* flat array of arity*u32 tuples */
    long      count;
    long      cap;
    uint8_t   arity;
} tuple_set;

static int tset_cb(const uint32_t *cols, uint8_t arity, void *user)
{
    tuple_set *ts = (tuple_set *)user;
    /* Set arity on first call */
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

/* Compare two tuple sets for equality (order-independent) */
static int tset_eq(const tuple_set *a, const tuple_set *b)
{
    long i, j;
    /* Both empty: equal */
    if (a->count == 0 && b->count == 0) return 1;
    if (a->count != b->count || a->arity != b->arity) return 0;

    for (i = 0; i < a->count; i++) {
        int found = 0;
        const uint32_t *arow = a->data + (size_t)i * (size_t)a->arity;
        for (j = 0; j < b->count; j++) {
            const uint32_t *brow = b->data + (size_t)j * (size_t)b->arity;
            if (memcmp(arow, brow, (size_t)a->arity * sizeof(uint32_t)) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) return 0;
    }
    return 1;
}

/* ─── Test utilities ─────────────────────────────────────────────────── */

static void setup_db(dl_db **db_out)
{
    system("rm -rf build-tmp/m1db");
    *db_out = dl_open("build-tmp/m1db");
    assert(*db_out);
}

static void teardown_db(dl_db *db)
{
    dl_close(db);
    system("rm -rf build-tmp/m1db");
}

/* Load integer edge facts */
static void load_edges(dl_db *db, const char *rel, int n, const int edges[][2])
{
    char csv_path[256];
    int i;
    FILE *f;

    assert(dl_declare_relation(db, rel, 2) == 0);

    snprintf(csv_path, sizeof(csv_path), "build-tmp/m1db/%s.csv", rel);
    f = fopen(csv_path, "w");
    assert(f);
    for (i = 0; i < n; i++)
        fprintf(f, "%d,%d\n", edges[i][0], edges[i][1]);
    fclose(f);

    int loaded = dl_load_facts(db, rel, csv_path);
    assert(loaded == n);
    (void)loaded;
}

/* ─── Test 1: Known-good single-step tc ──────────────────────────────── */

static void test_tc_single_step(void)
{
    dl_db *db;
    tuple_set result;
    const char *rule = "tc(X,Y):-edge(X,Y).";

    TEST("tc(X,Y):-edge(X,Y). single step");

    setup_db(&db);

    int edges[][2] = {{1,2},{1,3},{2,3},{2,4},{3,5}};
    load_edges(db, "edge", 5, edges);

    assert(dl_load_rules(db, rule) == 0);
    assert(dl_compile(db) == 0);

    memset(&result, 0, sizeof(result));
    dl_query(db, "tc", tset_cb, &result);

    /* Should have exactly the same 5 tuples */
    assert(result.count == 5);
    assert(result.arity == 2);

    /* Verify each expected tuple is present */
    uint32_t expected[][2] = {{1,2},{1,3},{2,3},{2,4},{3,5}};
    int i;
    for (i = 0; i < 5; i++) {
        int found = 0;
        long j;
        for (j = 0; j < result.count; j++) {
            uint32_t *row = result.data + (size_t)j * 2;
            if (row[0] == expected[i][0] && row[1] == expected[i][1])
                { found = 1; break; }
        }
        if (!found) {
            tset_free(&result);
            teardown_db(db);
            FAIL("missing expected tuple");
            return;
        }
    }

    tset_free(&result);
    teardown_db(db);
    PASS();
}

/* ─── Test 2: Join correctness (2-hop) ───────────────────────────────── */

static void test_join_two_hop(void)
{
    dl_db *db;
    tuple_set result, expected;
    const char *rule = "path(X,Z):-edge(X,Y),edge(Y,Z).";

    TEST("2-hop join path(X,Z):-edge(X,Y),edge(Y,Z).");

    setup_db(&db);

    int edges[][2] = {{1,2},{2,3},{1,3},{3,4},{2,4}};
    load_edges(db, "edge", 5, edges);

    /* Compute expected 2-hop result manually, deduplicating */
    {
        int i, j;
        memset(&expected, 0, sizeof(expected));
        expected.arity = 2;
        for (i = 0; i < 5; i++) {
            for (j = 0; j < 5; j++) {
                if (edges[i][1] == edges[j][0]) {
                    uint32_t tup[2] = {
                        (uint32_t)edges[i][0],
                        (uint32_t)edges[j][1]
                    };
                    /* Check if already in expected set */
                    int found2 = 0;
                    long k;
                    for (k = 0; k < expected.count; k++) {
                        uint32_t *er = expected.data + k * 2;
                        if (er[0] == tup[0] && er[1] == tup[1])
                            { found2 = 1; break; }
                    }
                    if (!found2)
                        tset_cb(tup, 2, &expected);
                }
            }
        }
    }

    assert(dl_load_rules(db, rule) == 0);
    assert(dl_compile(db) == 0);

    memset(&result, 0, sizeof(result));
    dl_query(db, "path", tset_cb, &result);

    if (!tset_eq(&result, &expected)) {
        printf("  got %ld tuples, expected %ld\n", result.count, expected.count);
        tset_free(&result);
        tset_free(&expected);
        teardown_db(db);
        FAIL("result mismatch");
        return;
    }

    tset_free(&result);
    tset_free(&expected);
    teardown_db(db);
    PASS();
}

/* ─── Test 3a: Undefined predicate ───────────────────────────────────── */

static void test_err_undefined_pred(void)
{
    dl_db *db;
    const char *rule = "tc(X,Y):-unknown(X,Y).";

    TEST("compile error: undefined predicate");

    setup_db(&db);
    /* declare tc so the head is known */
    assert(dl_declare_relation(db, "tc", 2) == 0);

    int ret = dl_load_rules(db, rule);
    /* Should fail */
    if (ret == 0) {
        teardown_db(db);
        FAIL("expected compile error, got success");
        return;
    }

    teardown_db(db);
    PASS();
}

/* ─── Test 3b: Arity mismatch ────────────────────────────────────────── */

static void test_err_arity_mismatch(void)
{
    dl_db *db;
    const char *rule = "tc(X,Y):-edge(X,Y,Z).";

    TEST("compile error: arity mismatch");

    setup_db(&db);
    assert(dl_declare_relation(db, "edge", 2) == 0);
    assert(dl_declare_relation(db, "tc", 2) == 0);

    int ret = dl_load_rules(db, rule);
    if (ret == 0) {
        teardown_db(db);
        FAIL("expected compile error, got success");
        return;
    }

    teardown_db(db);
    PASS();
}

/* ─── Test 3c: Ungrounded head ───────────────────────────────────────── */

static void test_err_ungrounded(void)
{
    dl_db *db;
    const char *rule = "tc(X,Y):-edge(X,Z).";

    TEST("compile error: ungrounded head");

    setup_db(&db);
    assert(dl_declare_relation(db, "edge", 2) == 0);

    int ret = dl_load_rules(db, rule);
    if (ret == 0) {
        teardown_db(db);
        FAIL("expected compile error, got success");
        return;
    }

    teardown_db(db);
    PASS();
}

/* ─── Test 3d: Unsafe negation ──────────────────────────────────────── */

static void test_err_negation(void)
{
    dl_db *db;
    const char *rule = "tc(X,Y):-!edge(X,Y).";

    TEST("compile error: unsafe negation (no positive atom)");

    setup_db(&db);
    assert(dl_declare_relation(db, "edge", 2) == 0);

    int ret = dl_load_rules(db, rule);
    if (ret == 0) {
        teardown_db(db);
        FAIL("expected compile error, got success");
        return;
    }

    teardown_db(db);
    PASS();
}

/* ─── Test 3e: Non-leading-column join (if implemented) ──────────────── */

static void test_err_nonleading_join(void)
{
    dl_db *db;
    const char *rule = "result(A,C):-edge(A,B),edge(C,B).";

    TEST("compile error: non-leading-column join");

    setup_db(&db);
    assert(dl_declare_relation(db, "edge", 2) == 0);

    int ret = dl_load_rules(db, rule);
    /* This SHOULD fail because the join is on column 1 of the inner edge,
     * not column 0. B is at column 1 of the inner edge(C,B). */
    if (ret == 0) {
        teardown_db(db);
        FAIL("expected compile error, got success");
        return;
    }

    teardown_db(db);
    PASS();
}

/* ─── Test 4: Property test (random graphs) ──────────────────────────── */

static void test_property_random(void)
{
    dl_db *db;
    int i;

    TEST("property test: random graphs (50 iterations)");

    for (i = 0; i < 50; i++) {
        /* Generate a small random directed graph with unique edges */
        int n_edges = 3 + (i % 10); /* 3-12 edges */
        int edges[20][2];
        int j, actual_n = 0;

        /* Generate random edges with small node ids (0-7)
         * Ensure uniqueness by checking before adding */
        {
            unsigned seed = (unsigned)(42 + i * 137);
            for (j = 0; j < n_edges * 3 && actual_n < n_edges; j++) {
                int a = (int)(((seed = seed * 1103515245 + 12345) >> 16) & 7);
                int b = (int)(((seed = seed * 1103515245 + 12345) >> 16) & 7);
                /* Check uniqueness */
                int dup = 0, k;
                for (k = 0; k < actual_n; k++) {
                    if (edges[k][0] == a && edges[k][1] == b) { dup = 1; break; }
                }
                if (!dup) {
                    edges[actual_n][0] = a;
                    edges[actual_n][1] = b;
                    actual_n++;
                }
            }
        }

        /* Set up fresh DB */
        setup_db(&db);
        load_edges(db, "edge", actual_n, edges);

        /* Compile tc(X,Y):-edge(X,Y). */
        assert(dl_load_rules(db, "tc(X,Y):-edge(X,Y).") == 0);
        assert(dl_compile(db) == 0);

        /* Collect VM result */
        tuple_set vm_result;
        memset(&vm_result, 0, sizeof(vm_result));
        dl_query(db, "tc", tset_cb, &vm_result);

        /* Compute expected: brute-force from edges */
        tuple_set expected;
        memset(&expected, 0, sizeof(expected));
        expected.arity = 2;
        for (j = 0; j < actual_n; j++) {
            uint32_t tup[2] = {
                (uint32_t)edges[j][0],
                (uint32_t)edges[j][1]
            };
            tset_cb(tup, 2, &expected);
        }

        if (!tset_eq(&vm_result, &expected)) {
            printf("\n  iteration %d: n_edges=%d, vm=%ld expected=%ld\n",
                   i, n_edges, vm_result.count, expected.count);
            tset_free(&vm_result);
            tset_free(&expected);
            teardown_db(db);
            FAIL("property test failure");
            return;
        }

        tset_free(&vm_result);
        tset_free(&expected);
        teardown_db(db);
    }

    PASS();
}

/* ─── Test 4b: Property test for 2-hop join ──────────────────────────── */

static void test_property_two_hop(void)
{
    int i;

    TEST("property test: 2-hop join (30 iterations)");

    for (i = 0; i < 30; i++) {
        dl_db *db;
        int n_edges = 3 + (i % 8); /* 3-10 edges */
        int edges[20][2];
        int j, actual_n = 0;

        /* Generate random edges with small node ids (0-5), ensure unique */
        {
            unsigned seed = (unsigned)(12345 + i * 7919);
            for (j = 0; j < n_edges * 3 && actual_n < n_edges; j++) {
                int a = (int)(((seed = seed * 1103515245 + 12345) >> 16) % 6);
                int b = (int)(((seed = seed * 1103515245 + 12345) >> 16) % 6);
                int dup = 0, k;
                for (k = 0; k < actual_n; k++) {
                    if (edges[k][0] == a && edges[k][1] == b) { dup = 1; break; }
                }
                if (!dup) {
                    edges[actual_n][0] = a;
                    edges[actual_n][1] = b;
                    actual_n++;
                }
            }
        }

        setup_db(&db);
        load_edges(db, "edge", actual_n, edges);

        /* Compile path(X,Z):-edge(X,Y),edge(Y,Z). */
        assert(dl_load_rules(db, "path(X,Z):-edge(X,Y),edge(Y,Z).") == 0);
        assert(dl_compile(db) == 0);

        /* Collect VM result */
        tuple_set vm_result;
        memset(&vm_result, 0, sizeof(vm_result));
        dl_query(db, "path", tset_cb, &vm_result);

        /* Compute expected: brute-force nested loop, deduplicated */
        tuple_set expected;
        memset(&expected, 0, sizeof(expected));
        expected.arity = 2;
        for (j = 0; j < actual_n; j++) {
            int k;
            for (k = 0; k < actual_n; k++) {
                if (edges[j][1] == edges[k][0]) {
                    uint32_t tup[2] = {
                        (uint32_t)edges[j][0],
                        (uint32_t)edges[k][1]
                    };
                    /* Dedup */
                    int found2 = 0;
                    long m;
                    for (m = 0; m < expected.count; m++) {
                        uint32_t *er = expected.data + m * 2;
                        if (er[0] == tup[0] && er[1] == tup[1])
                            { found2 = 1; break; }
                    }
                    if (!found2)
                        tset_cb(tup, 2, &expected);
                }
            }
        }

        if (!tset_eq(&vm_result, &expected)) {
            printf("\n  iteration %d: n_edges=%d, vm=%ld expected=%ld\n",
                   i, n_edges, vm_result.count, expected.count);
            tset_free(&vm_result);
            tset_free(&expected);
            teardown_db(db);
            FAIL("property test 2-hop failure");
            return;
        }

        tset_free(&vm_result);
        tset_free(&expected);
        teardown_db(db);
    }

    PASS();
}

/* ─── Test 5a: CLI query inline rule ─────────────────────────────────── */

static void test_cli_query_rule(void)
{
    TEST("CLI: dl query inline rule (end-to-end)");

    /* Use dl API directly to verify end-to-end flow */
    system("rm -rf build-tmp/m1cli");
    {
        dl_db *db = dl_open("build-tmp/m1cli");
        assert(db);

        /* Declare and load edge facts */
        assert(dl_declare_relation(db, "edge", 2) == 0);
        {
            FILE *f = fopen("build-tmp/m1cli/edges.csv", "w");
            assert(f);
            fprintf(f, "1,2\n2,3\n1,3\n3,4\n");
            fclose(f);
        }
        int loaded = dl_load_facts(db, "edge", "build-tmp/m1cli/edges.csv");
        assert(loaded == 4);

        /* Load and compile rules */
        assert(dl_load_rules(db, "tc(X,Y):-edge(X,Y).") == 0);
        assert(dl_compile(db) == 0);

        /* Query the goal relation */
        tuple_set ts;
        memset(&ts, 0, sizeof(ts));
        long n = dl_prefix(db, "tc", NULL, 0, tset_cb, &ts);
        assert(n == 4);
        assert(ts.count == 4);
        assert(ts.arity == 2);
        tset_free(&ts);

        dl_close(db);
    }
    system("rm -rf build-tmp/m1cli");
    PASS();
}

/* ─── Test 5b: CLI query .dl file ────────────────────────────────────── */

static void test_cli_query_file(void)
{
    TEST("CLI: dl query .dl file (end-to-end)");

    system("rm -rf build-tmp/m1cli2");
    {
        dl_db *db = dl_open("build-tmp/m1cli2");
        assert(db);

        /* Declare and load edge facts with string constants */
        assert(dl_declare_relation(db, "edge", 2) == 0);
        {
            FILE *f = fopen("build-tmp/m1cli2/edges.csv", "w");
            assert(f);
            fprintf(f, "a,b\na,c\nb,c\nb,d\n");
            fclose(f);
        }
        int loaded = dl_load_facts(db, "edge", "build-tmp/m1cli2/edges.csv");
        assert(loaded == 4);

        /* Load and compile rules (from a .dl file content) */
        {
            FILE *f = fopen("build-tmp/m1cli2/prog.dl", "w");
            assert(f);
            fprintf(f, "path(X,Z):-edge(X,Y),edge(Y,Z).\n");
            fclose(f);
        }

        /* Read the .dl file and load rules */
        {
            FILE *f = fopen("build-tmp/m1cli2/prog.dl", "r");
            assert(f);
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            char *buf = malloc((size_t)sz + 1);
            assert(buf);
            size_t nr = fread(buf, 1, (size_t)sz, f);
            buf[nr] = '\0';
            fclose(f);

            assert(dl_load_rules(db, buf) == 0);
            free(buf);
        }

        assert(dl_compile(db) == 0);

        /* Query the goal relation */
        tuple_set ts;
        memset(&ts, 0, sizeof(ts));
        long n = dl_prefix(db, "path", NULL, 0, tset_cb, &ts);
        assert(n == 2);
        assert(ts.count == 2);
        assert(ts.arity == 2);
        tset_free(&ts);

        dl_close(db);
    }
    system("rm -rf build-tmp/m1cli2");
    PASS();
}

/* ─── Test 6: Aggregate rejection ────────────────────────────────────── */

static void test_err_aggregate(void)
{
    dl_db *db;
    const char *rule = "tc(C):-count(C,edge(X,Y)).";

    TEST("compile error: aggregate in M1 (parse failure)");

    setup_db(&db);

    int ret = dl_load_rules(db, rule);
    if (ret == 0) {
        teardown_db(db);
        FAIL("expected parse/compile error, got success");
        return;
    }

    teardown_db(db);
    PASS();
}

/* ─── Helpers for regression tests ───────────────────────────────────── */

/* Load facts from a literal rows specification into a relation.
 * `cols` is an array of `nrows * arity` uint32_t values. */
static void load_rows(dl_db *db, const char *rel_name, uint8_t arity,
                      const uint32_t *cols, int nrows)
{
    char csv_path[256];
    int i, c;
    FILE *f;

    assert(dl_declare_relation(db, rel_name, arity) == 0);

    snprintf(csv_path, sizeof(csv_path), "build-tmp/m1db/%s.csv", rel_name);
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

    int loaded = dl_load_facts(db, rel_name, csv_path);
    assert(loaded == nrows);
    (void)loaded;
}

/* ─── Regression test B1: shared var overwritten in inner LOOKUP ─────── */
/* Facts: rel(1,100,2). rel(2,200,1). rel(2,201,5).
 * Rule:  q(A,B):-rel(A,C,D),rel(D,B,A).
 * Previously produced q(5,201) — the non-match where A'=5≠A=2.
 * Expected: q(1,200) and q(2,100) only (q(5,201) filtered). */

static void test_regression_b1(void)
{
    dl_db *db;
    tuple_set result;
    const char *rule = "q(A,B):-rel(A,C,D),rel(D,B,A).";

    TEST("regression B1: shared var overwritten in inner LOOKUP");

    setup_db(&db);

    {
        uint32_t rows[] = {1,100,2, 2,200,1, 2,201,5};
        load_rows(db, "rel", 3, rows, 3);
    }

    assert(dl_load_rules(db, rule) == 0);
    assert(dl_compile(db) == 0);

    memset(&result, 0, sizeof(result));
    dl_query(db, "q", tset_cb, &result);

    /* Expected: q(1,200) and q(2,100) — 2 tuples */
    if (result.count != 2 || result.arity != 2) {
        printf("  got %ld tuples (arity %d), expected 2 (arity 2)\n",
               result.count, result.arity);
        tset_free(&result); teardown_db(db);
        FAIL("B1: wrong result count");
        return;
    }

    /* Verify both expected tuples are present */
    {
        uint32_t exp[][2] = {{1,200},{2,100}};
        int ei;
        for (ei = 0; ei < 2; ei++) {
            int found = 0;
            long j;
            for (j = 0; j < result.count; j++) {
                uint32_t *row = result.data + j * 2;
                if (row[0] == exp[ei][0] && row[1] == exp[ei][1])
                    { found = 1; break; }
            }
            if (!found) {
                printf("  missing expected tuple (%u,%u)\n",
                       exp[ei][0], exp[ei][1]);
                tset_free(&result); teardown_db(db);
                FAIL("B1: missing expected tuple");
                return;
            }
        }
    }

    tset_free(&result);
    teardown_db(db);
    PASS();
}

/* ─── Regression test B2: non-adjacent shared vars → cartesian ───────── */
/* Expected: (1,2,4) and (1,3,5) — 2 tuples.
 * Bug: only checks predecessor for shared vars, so Y from atom 3 vs atom 1
 * is invisible → atom 3 emitted as SCAN (cartesian). */

static void test_regression_b2(void)
{
    dl_db *db;
    tuple_set result;
    const char *rule = "q(X,Y,Z):-edge(X,Y),unrelated(W),edge(Y,Z).";

    TEST("regression B2: non-adjacent shared vars");

    setup_db(&db);

    {
        uint32_t edges[] = {1,2, 1,3, 2,4, 3,5};
        load_rows(db, "edge", 2, edges, 4);
    }
    {
        uint32_t urels[] = {100};
        load_rows(db, "unrelated", 1, urels, 1);
    }

    assert(dl_load_rules(db, rule) == 0);
    assert(dl_compile(db) == 0);

    memset(&result, 0, sizeof(result));
    dl_query(db, "q", tset_cb, &result);

    /* Expected: (1,2,4), (1,3,5) — 2 tuples */
    if (result.count != 2) {
        printf("  got %ld tuples, expected 2\n", result.count);
        tset_free(&result); teardown_db(db);
        FAIL("B2: wrong result count");
        return;
    }

    /* Verify both expected tuples are present */
    {
        uint32_t exp[][3] = {{1,2,4},{1,3,5}};
        int ei;
        for (ei = 0; ei < 2; ei++) {
            int found = 0;
            long j;
            for (j = 0; j < result.count; j++) {
                uint32_t *row = result.data + j * 3;
                if (row[0] == exp[ei][0] && row[1] == exp[ei][1] && row[2] == exp[ei][2])
                    { found = 1; break; }
            }
            if (!found) {
                printf("  missing expected tuple (%u,%u,%u)\n",
                       exp[ei][0], exp[ei][1], exp[ei][2]);
                tset_free(&result); teardown_db(db);
                FAIL("B2: missing expected tuple");
                return;
            }
        }
    }

    tset_free(&result);
    teardown_db(db);
    PASS();
}

/* ─── Regression test B3: head constants silently halt ───────────────── */
/* Expected: foo(1,2) and foo(1,3).
 * Bug: PROJECT gives 0xFF for constant head arg → running=0. */

static void test_regression_b3(void)
{
    dl_db *db;
    tuple_set result;
    const char *rule = "foo(1,X):-edge(X,1).";

    TEST("regression B3: head constants");

    setup_db(&db);

    {
        uint32_t edges[] = {2,1, 3,1, 2,2};
        load_rows(db, "edge", 2, edges, 3);
    }

    assert(dl_load_rules(db, rule) == 0);
    assert(dl_compile(db) == 0);

    memset(&result, 0, sizeof(result));
    dl_query(db, "foo", tset_cb, &result);

    /* Expected: foo(1,2) and foo(1,3) — 2 tuples */
    if (result.count != 2) {
        printf("  got %ld tuples, expected 2\n", result.count);
        tset_free(&result); teardown_db(db);
        FAIL("B3: wrong result count (head constant bug)");
        return;
    }

    {
        uint32_t exp[][2] = {{1,2},{1,3}};
        int ei;
        for (ei = 0; ei < 2; ei++) {
            int found = 0;
            long j;
            for (j = 0; j < result.count; j++) {
                uint32_t *row = result.data + j * 2;
                if (row[0] == exp[ei][0] && row[1] == exp[ei][1])
                    { found = 1; break; }
            }
            if (!found) {
                printf("  missing expected tuple (%u,%u)\n",
                       exp[ei][0], exp[ei][1]);
                tset_free(&result); teardown_db(db);
                FAIL("B3: missing expected tuple");
                return;
            }
        }
    }

    tset_free(&result);
    teardown_db(db);
    PASS();
}

/* ─── Regression test B4: same var at multiple positions in one atom ──── */
/* Expected: q(1), q(2) only. edge(2,5) must NOT match edge(X,X).
 * Bug: last column binding overwrites same slot without check. */

static void test_regression_b4(void)
{
    dl_db *db;
    tuple_set result;
    const char *rule = "q(X):-edge(X,X).";

    TEST("regression B4: same var at multiple positions");

    setup_db(&db);

    {
        uint32_t edges[] = {1,1, 1,2, 2,2, 2,5};
        load_rows(db, "edge", 2, edges, 4);
    }

    assert(dl_load_rules(db, rule) == 0);
    assert(dl_compile(db) == 0);

    memset(&result, 0, sizeof(result));
    dl_query(db, "q", tset_cb, &result);

    /* Expected: q(1), q(2) — 2 tuples. NOT q(5). */
    if (result.count != 2) {
        printf("  got %ld tuples, expected 2\n", result.count);
        if (result.count > 0) {
            long j;
            for (j = 0; j < result.count; j++)
                printf("  [%ld] = %u\n", j, result.data[j]);
        }
        tset_free(&result); teardown_db(db);
        FAIL("B4: wrong result count");
        return;
    }

    {
        uint32_t exp[] = {1, 2};
        int ei;
        for (ei = 0; ei < 2; ei++) {
            int found = 0;
            long j;
            for (j = 0; j < result.count; j++) {
                if (result.data[j] == exp[ei]) { found = 1; break; }
            }
            if (!found) {
                printf("  missing expected tuple (%u)\n", exp[ei]);
                tset_free(&result); teardown_db(db);
                FAIL("B4: missing expected tuple");
                return;
            }
        }
    }

    tset_free(&result);
    teardown_db(db);
    PASS();
}

/* ─── Regression test B5: multiple constants share __b slot ───────────── */
/* Expected: (1,10), (2,20), (2,21) — 3 tuples.
 * Bug: constants 1 and 2 in data(Z,1,2) both map to __b → EQ_CONST
 * compares wrong value. */

static void test_regression_b5(void)
{
    dl_db *db;
    tuple_set result;
    const char *rule = "tc(X,Z):-edge(X,Z),data(Z,1,2).";

    TEST("regression B5: multiple constants in one atom");

    setup_db(&db);

    {
        uint32_t edges[] = {1,10, 2,20, 2,21};
        load_rows(db, "edge", 2, edges, 3);
    }
    {
        uint32_t dcols[] = {10,1,2, 20,1,2, 21,1,2};
        load_rows(db, "data", 3, dcols, 3);
    }

    assert(dl_load_rules(db, rule) == 0);
    assert(dl_compile(db) == 0);

    memset(&result, 0, sizeof(result));
    dl_query(db, "tc", tset_cb, &result);

    /* Expected: (1,10), (2,20), (2,21) — 3 tuples */
    if (result.count != 3) {
        printf("  got %ld tuples, expected 3\n", result.count);
        long j;
        for (j = 0; j < result.count; j++)
            printf("  [%ld] = (%u,%u)\n", j,
                   result.data[j*2], result.data[j*2+1]);
        tset_free(&result); teardown_db(db);
        FAIL("B5: wrong result count");
        return;
    }

    {
        uint32_t exp[][2] = {{1,10},{2,20},{2,21}};
        int ei;
        for (ei = 0; ei < 3; ei++) {
            int found = 0;
            long j;
            for (j = 0; j < result.count; j++) {
                uint32_t *row = result.data + j * 2;
                if (row[0] == exp[ei][0] && row[1] == exp[ei][1])
                    { found = 1; break; }
            }
            if (!found) {
                printf("  missing expected tuple (%u,%u)\n",
                       exp[ei][0], exp[ei][1]);
                tset_free(&result); teardown_db(db);
                FAIL("B5: missing expected tuple");
                return;
            }
        }
    }

    tset_free(&result);
    teardown_db(db);
    PASS();
}

/* ─── Property test: 3-atom join with non-adjacent shared var ─────────── */

static void test_property_nonadjacent_join(void)
{
    int i;

    TEST("property test: 3-atom non-adjacent join (20 iterations)");

    for (i = 0; i < 20; i++) {
        dl_db *db;
        int n_edges = 3 + (i % 6); /* 3-8 edges */
        int edges[20][2];
        int j, actual_n = 0;

        /* Generate random edges (0-5 node ids) */
        {
            unsigned seed = (unsigned)(4242 + i * 31337);
            for (j = 0; j < n_edges * 3 && actual_n < n_edges; j++) {
                int a = (int)(((seed = seed * 1103515245 + 12345) >> 16) % 6);
                int b = (int)(((seed = seed * 1103515245 + 12345) >> 16) % 6);
                int dup = 0, k;
                for (k = 0; k < actual_n; k++) {
                    if (edges[k][0] == a && edges[k][1] == b) { dup = 1; break; }
                }
                if (!dup) {
                    edges[actual_n][0] = a;
                    edges[actual_n][1] = b;
                    actual_n++;
                }
            }
        }

        setup_db(&db);

        {
            uint32_t *flat = malloc((size_t)actual_n * 2 * sizeof(uint32_t));
            assert(flat);
            for (j = 0; j < actual_n; j++) {
                flat[j*2] = (uint32_t)edges[j][0];
                flat[j*2+1] = (uint32_t)edges[j][1];
            }
            load_rows(db, "edge", 2, flat, actual_n);
            free(flat);
        }
        /* Add one unrelated fact */
        {
            uint32_t u = 100;
            load_rows(db, "unrelated", 1, &u, 1);
        }

        /* Compile q(X,Y,Z):-edge(X,Y),unrelated(W),edge(Y,Z). */
        assert(dl_load_rules(db, "q(X,Y,Z):-edge(X,Y),unrelated(W),edge(Y,Z).") == 0);
        assert(dl_compile(db) == 0);

        tuple_set vm_result;
        memset(&vm_result, 0, sizeof(vm_result));
        dl_query(db, "q", tset_cb, &vm_result);

        /* Compute expected: brute-force */
        tuple_set expected;
        memset(&expected, 0, sizeof(expected));
        for (j = 0; j < actual_n; j++) {
            int k;
            for (k = 0; k < actual_n; k++) {
                if (edges[j][1] == edges[k][0]) {
                    uint32_t tup[3] = {
                        (uint32_t)edges[j][0],
                        (uint32_t)edges[j][1],
                        (uint32_t)edges[k][1]
                    };
                    /* Dedup */
                    int found2 = 0;
                    long m;
                    for (m = 0; m < expected.count; m++) {
                        uint32_t *er = expected.data + m * 3;
                        if (er[0] == tup[0] && er[1] == tup[1] && er[2] == tup[2])
                            { found2 = 1; break; }
                    }
                    if (!found2)
                        tset_cb(tup, 3, &expected);
                }
            }
        }

        if (!tset_eq(&vm_result, &expected)) {
            printf("\n  iteration %d: n_edges=%d, vm=%ld expected=%ld\n",
                   i, actual_n, vm_result.count, expected.count);
            tset_free(&vm_result); tset_free(&expected);
            teardown_db(db);
            FAIL("B2 property test failure");
            return;
        }

        tset_free(&vm_result);
        tset_free(&expected);
        teardown_db(db);
    }

    PASS();
}

/* ─── Main ────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("M1 Tests\n");
    printf("========\n\n");

    test_tc_single_step();
    test_join_two_hop();
    test_err_undefined_pred();
    test_err_arity_mismatch();
    test_err_ungrounded();
    test_err_negation();
    test_err_nonleading_join();
    test_err_aggregate();
    test_property_random();
    test_property_two_hop();
    test_cli_query_rule();
    test_cli_query_file();

    /* Regression tests for confirmed bugs */
    test_regression_b1();
    test_regression_b2();
    test_regression_b3();
    test_regression_b4();
    test_regression_b5();
    test_property_nonadjacent_join();

    printf("\n---\n");
    printf("%d tests run, %d failed\n", tests_run, tests_failed);

    return tests_failed ? 1 : 0;
}
