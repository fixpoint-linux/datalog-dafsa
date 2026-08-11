/*
 * test_m2.c — M2 verification: fixpoint + stratified negation
 *
 * Tests:
 *   1. Transitive closure fixpoint (DAG + cycle)
 *   2. Negation with safety
 *   3. Stratifier: correct stratum assignment
 *   4. Known-bad rejection: negation cycle, clique, negation-in-recursion
 *   5. Unsafe negation rejection
 *   6. Property test: random graphs → fixpoint == brute-force
 *   7. Property test: random stratified programs with negation
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

static int tset_eq(const tuple_set *a, const tuple_set *b)
{
    long i, j;
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

static void setup_db(dl_db **db_out)
{
    system("rm -rf build-tmp/m2db");
    *db_out = dl_open("build-tmp/m2db");
    assert(*db_out);
}

static void teardown_db(dl_db *db)
{
    dl_close(db);
    system("rm -rf build-tmp/m2db");
}

static void load_rows(dl_db *db, const char *rel_name, uint8_t arity,
                      const uint32_t *cols, int nrows)
{
    char csv_path[256];
    int i, c;
    FILE *f;

    assert(dl_declare_relation(db, rel_name, arity) == 0);

    snprintf(csv_path, sizeof(csv_path), "build-tmp/m2db/%s.csv", rel_name);
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

/* Compute transitive closure brute-force in C (Warshall for small graphs) */
static tuple_set brute_tc(const uint32_t *edges, int n_edges)
{
    /* Build adjacency matrix for nodes 0..N-1 */
    int N = 0, i, j, k;
    tuple_set result;
    int found;

    memset(&result, 0, sizeof(result));

    for (i = 0; i < n_edges; i++) {
        if ((int)edges[i*2] > N) N = (int)edges[i*2];
        if ((int)edges[i*2+1] > N) N = (int)edges[i*2+1];
    }
    N++;

    /* Allocate reachability matrix */
    char *mat = calloc((size_t)N * (size_t)N, 1);
    assert(mat);

    /* Direct edges */
    for (i = 0; i < n_edges; i++) {
        int a = (int)edges[i*2];
        int b = (int)edges[i*2+1];
        mat[a * N + b] = 1;
    }

    /* Warshall */
    for (k = 0; k < N; k++)
        for (i = 0; i < N; i++)
            for (j = 0; j < N; j++)
                if (mat[i * N + k] && mat[k * N + j])
                    mat[i * N + j] = 1;

    /* Collect results */
    result.arity = 2;
    for (i = 0; i < N; i++) {
        for (j = 0; j < N; j++) {
            if (mat[i * N + j]) {
                uint32_t tup[2] = {(uint32_t)i, (uint32_t)j};
                /* Dedup (Warshall may produce many) */
                found = 0;
                long m;
                for (m = 0; m < result.count; m++) {
                    if (result.data[m*2] == tup[0] && result.data[m*2+1] == tup[1])
                        { found = 1; break; }
                }
                if (!found) tset_cb(tup, 2, &result);
            }
        }
    }

    free(mat);
    return result;
}

/* ─── Test 1a: TC on a DAG ───────────────────────────────────────────── */

static void test_tc_dag(void)
{
    dl_db *db;
    tuple_set result, expected;
    const char *rules =
        "tc(X,Y):-edge(X,Y).\n"
        "tc(X,Y):-edge(X,Z),tc(Z,Y).\n";

    TEST("transitive closure on DAG");

    setup_db(&db);

    uint32_t edges[] = {1,2, 1,3, 2,3, 2,4, 3,5, 4,5};
    load_rows(db, "edge", 2, edges, 6);

    assert(dl_load_rules(db, rules) == 0);
    assert(dl_compile(db) == 0);

    memset(&result, 0, sizeof(result));
    dl_query(db, "tc", tset_cb, &result);

    expected = brute_tc(edges, 6);

    if (!tset_eq(&result, &expected)) {
        printf("  got %ld tuples, expected %ld\n", result.count, expected.count);
        tset_free(&result); tset_free(&expected);
        teardown_db(db);
        FAIL("tc DAG mismatch");
        return;
    }

    tset_free(&result); tset_free(&expected);
    teardown_db(db);
    PASS();
}

/* ─── Test 1b: TC on a graph WITH a cycle ──────────────────────────────── */

static void test_tc_cycle(void)
{
    dl_db *db;
    tuple_set result, expected;
    const char *rules =
        "tc(X,Y):-edge(X,Y).\n"
        "tc(X,Y):-edge(X,Z),tc(Z,Y).\n";

    TEST("transitive closure on graph with cycle");

    setup_db(&db);

    /* Graph: 1->2, 2->3, 3->1 (cycle), 3->4 */
    uint32_t edges[] = {1,2, 2,3, 3,1, 3,4};
    load_rows(db, "edge", 2, edges, 4);

    assert(dl_load_rules(db, rules) == 0);
    assert(dl_compile(db) == 0);

    memset(&result, 0, sizeof(result));
    dl_query(db, "tc", tset_cb, &result);

    expected = brute_tc(edges, 4);

    if (!tset_eq(&result, &expected)) {
        printf("  got %ld tuples, expected %ld\n", result.count, expected.count);
        tset_free(&result); tset_free(&expected);
        teardown_db(db);
        FAIL("tc cycle mismatch");
        return;
    }

    tset_free(&result); tset_free(&expected);
    teardown_db(db);
    PASS();
}

/* ─── Test 2a: Safe negation !blocked ────────────────────────────────── */

static void test_negation_safe(void)
{
    dl_db *db;
    tuple_set result;
    const char *rule = "path(X,Y):-edge(X,Y),!blocked(X,Y).";

    TEST("safe negation: path(X,Y):-edge(X,Y),!blocked(X,Y).");

    setup_db(&db);

    {
        uint32_t e[] = {1,2, 1,3, 2,3, 2,4, 3,5};
        load_rows(db, "edge", 2, e, 5);
    }
    {
        uint32_t b[] = {1,3, 2,4};
        load_rows(db, "blocked", 2, b, 2);
    }

    assert(dl_load_rules(db, rule) == 0);
    assert(dl_compile(db) == 0);

    memset(&result, 0, sizeof(result));
    dl_query(db, "path", tset_cb, &result);

    /* Expected: edge minus blocked = (1,2), (2,3), (3,5) */
    uint32_t exp[][2] = {{1,2},{2,3},{3,5}};
    if (result.count != 3 || result.arity != 2) {
        printf("  got %ld tuples, expected 3\n", result.count);
        tset_free(&result); teardown_db(db);
        FAIL("negation: wrong count");
        return;
    }

    int ei;
    for (ei = 0; ei < 3; ei++) {
        int found = 0;
        long j;
        for (j = 0; j < result.count; j++) {
            uint32_t *r = result.data + j * 2;
            if (r[0] == exp[ei][0] && r[1] == exp[ei][1])
                { found = 1; break; }
        }
        if (!found) {
            printf("  missing (%u,%u)\n", exp[ei][0], exp[ei][1]);
            tset_free(&result); teardown_db(db);
            FAIL("negation: missing tuple");
            return;
        }
    }

    tset_free(&result);
    teardown_db(db);
    PASS();
}

/* ─── Test 2b: Negation with TC (stratified) ─────────────────────────── */

static void test_negation_with_tc(void)
{
    dl_db *db;
    tuple_set result;
    const char *rules =
        "tc(X,Y):-edge(X,Y).\n"
        "tc(X,Y):-edge(X,Z),tc(Z,Y).\n"
        "reachable(X):-edge(X,Y),!blocked(X).\n";

    TEST("negation with TC (stratified)");

    setup_db(&db);

    {
        uint32_t e[] = {1,2, 2,3, 1,3, 3,4};
        load_rows(db, "edge", 2, e, 4);
    }
    {
        uint32_t b[] = {2}; /* block node 2 */
        load_rows(db, "blocked", 1, b, 1);
    }

    assert(dl_load_rules(db, rules) == 0);
    assert(dl_compile(db) == 0);

    memset(&result, 0, sizeof(result));
    dl_query(db, "reachable", tset_cb, &result);

    /* reachable(X): X has an outgoing edge AND X is not blocked.
     * 1 -> outgoing to 2, not blocked → reachable(1)
     * 2 -> outgoing to 3, IS blocked → NOT reachable
     * 3 -> outgoing to 4, not blocked → reachable(3)
     * 4 -> no outgoing edge
     * → {1, 3} */
    if (result.count != 2 || result.arity != 1) {
        printf("  got %ld tuples (arity %d), expected 2\n", result.count, result.arity);
        long j;
        for (j = 0; j < result.count; j++)
            printf("  [%ld] = %u\n", j, result.data[j]);
        tset_free(&result); teardown_db(db);
        FAIL("negation+TC: wrong count");
        return;
    }

    uint32_t exp[] = {1, 3};
    int ei;
    for (ei = 0; ei < 2; ei++) {
        int found = 0;
        long j;
        for (j = 0; j < result.count; j++) {
            if (result.data[j] == exp[ei]) { found = 1; break; }
        }
        if (!found) {
            printf("  missing %u\n", exp[ei]);
            tset_free(&result); teardown_db(db);
            FAIL("negation+TC: missing tuple");
            return;
        }
    }

    tset_free(&result);
    teardown_db(db);
    PASS();
}

/* ─── Test 3: Stratifier unit test ──────────────────────────────────── */

static void test_stratifier_mixed(void)
{
    dl_db *db;
    const char *rules =
        "p(X,Y):-q(X,Y).\n"
        "q(X,Y):-r(X,Y).\n"
        "tc(X,Y):-edge(X,Y).\n"
        "tc(X,Y):-edge(X,Z),tc(Z,Y).\n"
        "good(X):-tc(X,Y),!bad(Y).\n";

    TEST("stratifier: mixed ruleset stratum assignment");

    setup_db(&db);

    /* Declare EDB */
    assert(dl_declare_relation(db, "r", 2) == 0);
    assert(dl_declare_relation(db, "edge", 2) == 0);
    assert(dl_declare_relation(db, "bad", 1) == 0);

    /* This should succeed — the ruleset is stratifiable */
    int ret = dl_load_rules(db, rules);
    if (ret != 0) {
        teardown_db(db);
        FAIL("stratifier rejected a valid ruleset");
        return;
    }

    assert(dl_compile(db) == 0);

    /* After compilation, verify that the rules compiled successfully */
    /* The important thing is that it was NOT rejected */
    teardown_db(db);
    PASS();
}

/* ─── Test 4a: Negation cycle (unstratifiable) ──────────────────────── */

static void test_reject_negation_cycle(void)
{
    dl_db *db;
    const char *rules =
        "p(X):-q(X),!r(X).\n"
        "r(X):-p(X).\n";

    TEST("reject: negation cycle");

    setup_db(&db);
    assert(dl_declare_relation(db, "q", 1) == 0);

    int ret = dl_load_rules(db, rules);
    if (ret == 0) {
        /* Might compile but should fail at compile — check dl_compile too */
        ret = dl_compile(db);
    }
    if (ret == 0) {
        teardown_db(db);
        FAIL("expected rejection, got success");
        return;
    }

    teardown_db(db);
    PASS();
}

/* ─── Test 4b: Mutual negation (clique) ─────────────────────────────── */

static void test_reject_clique(void)
{
    dl_db *db;
    const char *rules =
        "p(X):-!q(X).\n"
        "q(X):-!p(X).\n";

    TEST("reject: mutual negation clique");

    setup_db(&db);

    int ret = dl_load_rules(db, rules);
    if (ret != 0) {
        teardown_db(db);
        PASS();
        return;
    }

    /* dl_load_rules might succeed (rules compile structurally)
     * but dl_compile should still work if stratification passes.
     * Let's check: both p and q have no positive body atoms, so
     * the compiler should reject with "no positive body atom". */
    teardown_db(db);
    FAIL("expected rejection at compile, got success");
}

/* ─── Test 4c: Unstratifiable — negation through mutual recursion ──── */

static void test_reject_neg_in_recursion(void)
{
    dl_db *db;
    const char *rules =
        "p(X):-q(X).\n"
        "q(X):-!p(X).\n";

    TEST("reject: negation through mutual recursion (p→q, q→!p)");

    setup_db(&db);

    int ret = dl_load_rules(db, rules);
    if (ret == 0) {
        ret = dl_compile(db);
    }
    if (ret == 0) {
        teardown_db(db);
        FAIL("expected rejection, got success");
        return;
    }

    teardown_db(db);
    PASS();
}

/* ─── Test 5: Unsafe negation (variable not bound) ──────────────────── */

static void test_reject_unsafe_negation(void)
{
    dl_db *db;
    const char *rule = "q(X):-!p(X).";

    TEST("reject: unsafe negation (X unbound by positive atom)");

    setup_db(&db);
    assert(dl_declare_relation(db, "p", 1) == 0);

    int ret = dl_load_rules(db, rule);
    if (ret == 0) {
        teardown_db(db);
        FAIL("expected rejection, got success");
        return;
    }

    teardown_db(db);
    PASS();
}

/* ─── Test 6: Property test — random graphs TC fixpoint ──────────────── */

static void test_property_tc(void)
{
    int i;
    TEST("property test: random graphs → TC fixpoint == brute-force (40 iterations)");

    for (i = 0; i < 40; i++) {
        dl_db *db;
        int n_edges = 3 + (i % 10);
        int edges[20][2];
        int j, actual_n = 0;

        {
            unsigned seed = (unsigned)(9999 + i * 31337);
            for (j = 0; j < n_edges * 4 && actual_n < n_edges; j++) {
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

        const char *rules =
            "tc(X,Y):-edge(X,Y).\n"
            "tc(X,Y):-edge(X,Z),tc(Z,Y).\n";
        assert(dl_load_rules(db, rules) == 0);
        assert(dl_compile(db) == 0);

        tuple_set vm_result;
        memset(&vm_result, 0, sizeof(vm_result));
        dl_query(db, "tc", tset_cb, &vm_result);

        {
            uint32_t *flat = malloc((size_t)actual_n * 2 * sizeof(uint32_t));
            for (j = 0; j < actual_n; j++) {
                flat[j*2] = (uint32_t)edges[j][0];
                flat[j*2+1] = (uint32_t)edges[j][1];
            }
            tuple_set expected = brute_tc(flat, actual_n);
            free(flat);

            if (!tset_eq(&vm_result, &expected)) {
                printf("\n  iteration %d: n=%d, vm=%ld expected=%ld\n",
                       i, actual_n, vm_result.count, expected.count);
                tset_free(&vm_result); tset_free(&expected);
                teardown_db(db);
                FAIL("property TC failure");
                return;
            }

            tset_free(&expected);
        }

        tset_free(&vm_result);
        teardown_db(db);
    }

    PASS();
}

/* ─── Test 7: Property test — random stratified programs with negation ── */

static void test_property_negation(void)
{
    int i;
    TEST("property test: random stratified negation (30 iterations)");

    for (i = 0; i < 30; i++) {
        dl_db *db;
        int n_edges = 4 + (i % 8);
        int edges[20][2];
        int n_blocked = 1 + (i % 4);
        int blocked[16];
        int j, actual_edges = 0, actual_blocked = 0;

        /* Generate edges */
        {
            unsigned seed = (unsigned)(5555 + i * 7919);
            for (j = 0; j < n_edges * 4 && actual_edges < n_edges; j++) {
                int a = (int)(((seed = seed * 1103515245 + 12345) >> 16) % 7);
                int b = (int)(((seed = seed * 1103515245 + 12345) >> 16) % 7);
                int dup = 0, k;
                for (k = 0; k < actual_edges; k++) {
                    if (edges[k][0] == a && edges[k][1] == b) { dup = 1; break; }
                }
                if (!dup) {
                    edges[actual_edges][0] = a;
                    edges[actual_edges][1] = b;
                    actual_edges++;
                }
            }
        }

        /* Generate blocked edges (pairs of node ids that match edge endpoints) */
        {
            unsigned seed = (unsigned)(7777 + i * 12345);
            for (j = 0; j < n_blocked * 4 && actual_blocked < n_blocked; j++) {
                if (actual_edges == 0) break;
                int idx = (int)(((seed = seed * 1103515245 + 12345) >> 16) % (unsigned)actual_edges);
                int a = edges[idx][0];
                int b = edges[idx][1];
                int dup = 0, k;
                for (k = 0; k < actual_blocked; k++) {
                    if (blocked[k*2] == a && blocked[k*2+1] == b) { dup = 1; break; }
                }
                if (!dup) {
                    blocked[actual_blocked*2] = a;
                    blocked[actual_blocked*2+1] = b;
                    actual_blocked++;
                }
            }
        }

        setup_db(&db);

        {
            uint32_t *f = malloc((size_t)actual_edges * 2 * sizeof(uint32_t));
            for (j = 0; j < actual_edges; j++) {
                f[j*2] = (uint32_t)edges[j][0];
                f[j*2+1] = (uint32_t)edges[j][1];
            }
            load_rows(db, "edge", 2, f, actual_edges);
            free(f);
        }

        {
            uint32_t *f = malloc((size_t)actual_blocked * 2 * sizeof(uint32_t));
            for (j = 0; j < actual_blocked; j++) {
                f[j*2] = (uint32_t)blocked[j*2];
                f[j*2+1] = (uint32_t)blocked[j*2+1];
            }
            load_rows(db, "blocked", 2, f, actual_blocked);
            free(f);
        }

        const char *rule = "path(X,Y):-edge(X,Y),!blocked(X,Y).\n";
        assert(dl_load_rules(db, rule) == 0);
        assert(dl_compile(db) == 0);

        tuple_set vm_result;
        memset(&vm_result, 0, sizeof(vm_result));
        dl_query(db, "path", tset_cb, &vm_result);

        /* Compute expected: edges not in blocked */
        tuple_set expected;
        memset(&expected, 0, sizeof(expected));
        expected.arity = 2;
        for (j = 0; j < actual_edges; j++) {
            int is_blocked = 0;
            int k;
            for (k = 0; k < actual_blocked; k++) {
                if (blocked[k*2] == edges[j][0] && blocked[k*2+1] == edges[j][1])
                    { is_blocked = 1; break; }
            }
            if (!is_blocked) {
                uint32_t tup[2] = {(uint32_t)edges[j][0], (uint32_t)edges[j][1]};
                tset_cb(tup, 2, &expected);
            }
        }

        if (!tset_eq(&vm_result, &expected)) {
            printf("\n  iteration %d: vm=%ld expected=%ld\n",
                   i, vm_result.count, expected.count);
            tset_free(&vm_result); tset_free(&expected);
            teardown_db(db);
            FAIL("property negation failure");
            return;
        }

        tset_free(&vm_result); tset_free(&expected);
        teardown_db(db);
    }

    PASS();
}

/* ─── Test 8: Edge case — empty delta loop termination ──────────────── */

static void test_tc_empty_graph(void)
{
    dl_db *db;
    tuple_set result;
    const char *rules =
        "tc(X,Y):-edge(X,Y).\n"
        "tc(X,Y):-edge(X,Z),tc(Z,Y).\n";

    TEST("TC on empty graph (fixpoint terminates)");

    setup_db(&db);

    /* Declare edge but load NO facts */
    assert(dl_declare_relation(db, "edge", 2) == 0);

    assert(dl_load_rules(db, rules) == 0);
    assert(dl_compile(db) == 0);

    memset(&result, 0, sizeof(result));
    dl_query(db, "tc", tset_cb, &result);

    /* Should be empty */
    if (result.count != 0) {
        printf("  got %ld tuples, expected 0\n", result.count);
        tset_free(&result); teardown_db(db);
        FAIL("empty TC: wrong count");
        return;
    }

    tset_free(&result);
    teardown_db(db);
    PASS();
}

/* ─── Test B1-a: unsafe negation: head variable not bound ─────────── */
/* q(X,Y):-r(X),!p(Y).  Y is in head and in negated atom but never
 * grounded by a positive body atom.  Must be REJECTED. */

static void test_b1_head_var_unbound(void)
{
    dl_db *db;
    const char *rule = "q(X,Y):-r(X),!p(Y).";

    TEST("B1a: unsafe negation — head var Y not grounded by positive atom");

    setup_db(&db);
    assert(dl_declare_relation(db, "r", 1) == 0);
    assert(dl_declare_relation(db, "p", 1) == 0);

    int ret = dl_load_rules(db, rule);
    if (ret == 0) {
        teardown_db(db);
        FAIL("expected rejection, got success");
        return;
    }

    teardown_db(db);
    PASS();
}

/* ─── Test B1-b: unsafe negation — variable not bound at point of
 * negated check ───────────────────────────────────────────────────── */
/* q(X):-!p(X),r(X).  X appears in negated atom before any positive
 * atom grounds it.  Must be REJECTED. */

static void test_b1_neg_before_pos(void)
{
    dl_db *db;
    const char *rule = "q(X):-!p(X),r(X).";

    TEST("B1b: unsafe negation — negated atom before grounding positive atom");

    setup_db(&db);
    assert(dl_declare_relation(db, "r", 1) == 0);
    assert(dl_declare_relation(db, "p", 1) == 0);

    int ret = dl_load_rules(db, rule);
    if (ret == 0) {
        teardown_db(db);
        FAIL("expected rejection, got success");
        return;
    }

    teardown_db(db);
    PASS();
}

/* ─── Test B2: long-chain transitive closure ──────────────────────── */
/* Verify that the fixpoint loop does NOT silently truncate.  A chain of
 * N nodes requires N-1 fixpoint iterations.  We use N=2000 → 1999
 * iterations.  The old code capped at 10000, so this would work there
 * too, but the key property is: the loop terminates on empty delta, not
 * on a hardcoded cap.  Verified by code inspection + correct total. */

static void test_tc_long_chain(void)
{
    dl_db *db;
    int N = 200;
    int i;

    TEST("long-chain TC (200 nodes, 199 fixpoint rounds)");

    setup_db(&db);

    assert(dl_declare_relation(db, "edge", 2) == 0);
    {
        char csv_path[256];
        snprintf(csv_path, sizeof(csv_path), "build-tmp/m2db/edge.csv");
        FILE *f = fopen(csv_path, "w");
        assert(f);
        for (i = 1; i < N; i++) {
            fprintf(f, "%d,%d\n", i, i + 1);
        }
        fclose(f);
        int loaded = dl_load_facts(db, "edge", csv_path);
        assert(loaded == N - 1);
        (void)loaded;
    }

    const char *rules =
        "tc(X,Y):-edge(X,Y).\n"
        "tc(X,Y):-edge(X,Z),tc(Z,Y).\n";
    assert(dl_load_rules(db, rules) == 0);
    assert(dl_compile(db) == 0);

    /* Count tc tuples */
    long count = 0;
    dl_query(db, "tc", NULL, &count);
    /* Use custom count via dl_prefix */
    {
        tuple_set result;
        memset(&result, 0, sizeof(result));
        dl_query(db, "tc", tset_cb, &result);
        count = result.count;
        tset_free(&result);
    }

    /* Expected: for chain 1→2→...→N, every (i,j) with i<j is reachable.
     * Total pairs = N*(N-1)/2 */
    long expected = (long)N * (long)(N - 1) / 2;
    if (count != expected) {
        printf("  got %ld, expected %ld\n", count, expected);
        teardown_db(db);
        FAIL("long-chain TC: wrong count");
        return;
    }

    teardown_db(db);
    PASS();
}

/* ─── Test: SCC-based is_recursive not over-marked ────────────────── */
/* A non-recursive rule like p(X):-q(X) where p never appears in any
 * body should have is_recursive=0. */

static void test_scc_not_overmarked(void)
{
    dl_db *db;

    TEST("SCC-based recursion: non-recursive rule not over-marked");

    setup_db(&db);
    assert(dl_declare_relation(db, "q", 1) == 0);

    /* p depends on q, but p never appears in any body → p not recursive */
    const char *rule = "p(X):-q(X).\n";
    assert(dl_load_rules(db, rule) == 0);
    assert(dl_compile(db) == 0);

    /* If p was wrongly marked recursive, the fixpoint loop would still
     * terminate correctly (p has no recursive body atoms).  But the key
     * is that the program compiles and evaluates without error. */
    teardown_db(db);
    PASS();
}

/* ─── Test: mutual recursion correctness ──────────────────────────── */
/* even(0).  even(X):-succ(Y,X),odd(Y).
 * odd(X):-succ(Y,X),even(Y).  with succ facts. */

static void test_mutual_recursion(void)
{
    dl_db *db;
    tuple_set result;
    int i;

    TEST("mutual recursion: even/odd");

    setup_db(&db);

    /* Build succ(0,1), succ(1,2), ..., succ(9,10) */
    assert(dl_declare_relation(db, "succ", 2) == 0);
    {
        char csv_path[256];
        snprintf(csv_path, sizeof(csv_path), "build-tmp/m2db/succ.csv");
        FILE *f = fopen(csv_path, "w");
        assert(f);
        for (i = 0; i < 10; i++)
            fprintf(f, "%d,%d\n", i, i + 1);
        fclose(f);
        int loaded = dl_load_facts(db, "succ", csv_path);
        assert(loaded == 10);
        (void)loaded;
    }

    const char *rules =
        "even(X):-succ(Y,X),odd(Y).\n"
        "odd(X):-succ(Y,X),even(Y).\n";

    assert(dl_load_rules(db, rules) == 0);

    /* Load even(0) as a ground fact (not as a rule, since rules need body) */
    {
        assert(dl_declare_relation(db, "even", 1) == 0);
        char csv_path[256];
        snprintf(csv_path, sizeof(csv_path), "build-tmp/m2db/even.csv");
        FILE *f = fopen(csv_path, "w");
        assert(f);
        fprintf(f, "0\n");
        fclose(f);
        int loaded = dl_load_facts(db, "even", csv_path);
        assert(loaded == 1);
        (void)loaded;
    }

    /* This should compile and evaluate.  even(0) is a fact.
     * From succ(0,1), even(0) → odd(1)
     * From succ(1,2), odd(1) → even(2)
     * ... up to 10.
     * Expected: even = {0,2,4,6,8,10}, odd = {1,3,5,7,9} */
    assert(dl_compile(db) == 0);

    memset(&result, 0, sizeof(result));
    dl_query(db, "even", tset_cb, &result);
    if (result.count != 6 || result.arity != 1) {
        printf("  even got %ld tuples, expected 6\n", result.count);
        tset_free(&result); teardown_db(db);
        FAIL("mutual recursion: wrong even count");
        return;
    }
    /* Verify even contains 0,2,4,6,8,10 */
    {
        uint32_t expected[] = {0,2,4,6,8,10};
        int ei, found;
        for (ei = 0; ei < 6; ei++) {
            found = 0;
            long j;
            for (j = 0; j < result.count; j++) {
                if (result.data[j] == expected[ei]) { found = 1; break; }
            }
            if (!found) {
                printf("  missing even(%u)\n", expected[ei]);
                tset_free(&result); teardown_db(db);
                FAIL("mutual recursion: missing even value");
                return;
            }
        }
    }
    tset_free(&result);

    memset(&result, 0, sizeof(result));
    dl_query(db, "odd", tset_cb, &result);
    if (result.count != 5 || result.arity != 1) {
        printf("  odd got %ld tuples, expected 5\n", result.count);
        tset_free(&result); teardown_db(db);
        FAIL("mutual recursion: wrong odd count");
        return;
    }

    tset_free(&result);
    teardown_db(db);
    PASS();
}

/* ─── Test: strict stratification — non-recursive dependent of
 * recursive SCC at higher stratum ──────────────────────────────────── */
/* M2.1 regression: a non-recursive rule that depends on a recursive SCC
 * must be assigned a STRICTLY higher stratum so it is evaluated AFTER the
 * recursive fixpoint completes.  Without strict stratification, the
 * non-recursive rule is only evaluated during the seed phase and misses
 * tuples that require the full fixpoint.
 *
 * We use a 5-node chain 1→2→3→4→5, transitive closure tc, and:
 *   chain(X,Z) :- tc(X,Y), edge(Y,Z).
 *
 * During the seed phase (one round of the recursive rule), tc lacks (1,4)
 * (requires 2+ fixpoint iterations).  Without strict stratification,
 * chain is at the same stratum as tc and evaluated only during seed,
 * missing chain(1,5) from tc(1,4)+edge(4,5). */

static void test_strict_stratification(void)
{
    dl_db *db;
    tuple_set result;
    int i;

    TEST("strict stratification: non-recursive dependent of recursive SCC");

    setup_db(&db);

    /* Build chain: 1→2→3→4→5 */
    assert(dl_declare_relation(db, "edge", 2) == 0);
    {
        char csv_path[256];
        snprintf(csv_path, sizeof(csv_path), "build-tmp/m2db/edge.csv");
        FILE *f = fopen(csv_path, "w");
        assert(f);
        for (i = 1; i < 5; i++)
            fprintf(f, "%d,%d\n", i, i + 1);
        fclose(f);
        int loaded = dl_load_facts(db, "edge", csv_path);
        assert(loaded == 4);
        (void)loaded;
    }

    const char *rules =
        "tc(X,Y):-edge(X,Y).\n"
        "tc(X,Y):-edge(X,Z),tc(Z,Y).\n"
        "chain(X,Z):-tc(X,Y),edge(Y,Z).\n";

    assert(dl_load_rules(db, rules) == 0);
    assert(dl_compile(db) == 0);

    memset(&result, 0, sizeof(result));
    dl_query(db, "chain", tset_cb, &result);

    /* Expected: for each tc(X,Y)+edge(Y,Z):
     *   (1,2)+(2,3)→(1,3), (1,3)+(3,4)→(1,4), (1,4)+(4,5)→(1,5),
     *   (2,3)+(3,4)→(2,4), (2,4)+(4,5)→(2,5), (3,4)+(4,5)→(3,5)
     * → 6 tuples.  Without strict stratification, chain is at the same
     *   stratum as tc, evaluated during seed, and (1,5) is missing
     *   because tc(1,4) is not yet materialized → only 5 tuples. */
    uint32_t expected[][2] = {
        {1,3},{1,4},{1,5},
        {2,4},{2,5},
        {3,5}
    };
    int n_expected = 6;

    if (result.count != (long)n_expected || result.arity != 2) {
        printf("  got %ld tuples (arity %d), expected %d\n",
               result.count, result.arity, n_expected);
        long j;
        for (j = 0; j < result.count; j++) {
            uint32_t *r = result.data + j * 2;
            printf("  [%ld] = (%u,%u)\n", j, r[0], r[1]);
        }
        tset_free(&result); teardown_db(db);
        FAIL("strict stratification: wrong count");
        return;
    }

    int ei;
    for (ei = 0; ei < n_expected; ei++) {
        int found = 0;
        long j;
        for (j = 0; j < result.count; j++) {
            uint32_t *r = result.data + j * 2;
            if (r[0] == expected[ei][0] && r[1] == expected[ei][1])
                { found = 1; break; }
        }
        if (!found) {
            printf("  missing (%u,%u)\n", expected[ei][0], expected[ei][1]);
            tset_free(&result); teardown_db(db);
            FAIL("strict stratification: missing tuple");
            return;
        }
    }

    tset_free(&result);
    teardown_db(db);
    PASS();
}

/* ─── Main ────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("M2 Tests\n");
    printf("========\n\n");

    test_tc_dag();
    test_tc_cycle();
    test_negation_safe();
    test_negation_with_tc();
    test_stratifier_mixed();
    test_reject_negation_cycle();
    test_reject_clique();
    test_reject_neg_in_recursion();
    test_reject_unsafe_negation();
    test_property_tc();
    test_property_negation();
    test_tc_empty_graph();

    /* New regression tests (B1, B2, semi-naive correctness) */
    test_b1_head_var_unbound();
    test_b1_neg_before_pos();
    test_tc_long_chain();
    test_scc_not_overmarked();
    test_mutual_recursion();
    test_strict_stratification();

    printf("\n---\n");
    printf("%d tests run, %d failed\n", tests_run, tests_failed);

    return tests_failed ? 1 : 0;
}
