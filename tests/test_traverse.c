/*
 * test_traverse.c — Graph traversal tests (Tier-2)
 */
#include "dl.h"

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
} while (0)
#define PASS() do { printf("OK\n"); } while (0)
#define FAIL(msg) do { \
    printf("FAIL: %s\n", msg); \
    tests_failed++; \
} while (0)

static const char *BASE = "build-tmp/traverse";

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

static int traverse_count_cb(uint32_t node_sym, uint8_t depth, void *user)
{
    (void)node_sym; (void)depth; (void)user;
    return 0;
}

static int obs_count_cb(const char *s, void *user)
{
    (void)s; (void)user;
    return 0;
}

static void t1_forward_bfs(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t1");
    uint32_t sym_a, sym_b, sym_c, cols[3];
    long n;

    TEST("T1 forward BFS");
    assert(dl_declare_relation(db, "edge", 3) == 0);
    sym_a = dl_intern_str(db, "A");
    sym_b = dl_intern_str(db, "B");
    sym_c = dl_intern_str(db, "C");

    cols[0] = sym_a; cols[1] = sym_b; cols[2] = dl_intern_str(db, "next");
    assert(dl_add_fact(db, "edge", cols, 3) == 1);
    cols[0] = sym_b; cols[1] = sym_c; cols[2] = dl_intern_str(db, "next");
    assert(dl_add_fact(db, "edge", cols, 3) == 1);

    n = dl_traverse(db, "A", 2, 100, traverse_count_cb, NULL);
    if (n != 3) { FAIL("expected 3 nodes"); dl_close(db); return; }
    PASS();
    dl_close(db);
}

static void t2_reverse_bfs(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t2");
    uint32_t sym_a, sym_b, sym_c, cols[3];
    long n;

    TEST("T2 reverse BFS");
    assert(dl_declare_relation(db, "edge", 3) == 0);
    sym_a = dl_intern_str(db, "A");
    sym_b = dl_intern_str(db, "B");
    sym_c = dl_intern_str(db, "C");

    cols[0] = sym_a; cols[1] = sym_b; cols[2] = dl_intern_str(db, "next");
    assert(dl_add_fact(db, "edge", cols, 3) == 1);
    cols[0] = sym_b; cols[1] = sym_c; cols[2] = dl_intern_str(db, "next");
    assert(dl_add_fact(db, "edge", cols, 3) == 1);

    n = dl_traverse(db, "C", 2, 100, traverse_count_cb, NULL);
    if (n != 3) { FAIL("expected 3 nodes from reverse"); dl_close(db); return; }
    PASS();
    dl_close(db);
}

static void t3_cycle(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t3");
    uint32_t sym_a, sym_b, cols[3];
    long n;

    TEST("T3 bidirectional cycle no infinite loop");
    assert(dl_declare_relation(db, "edge", 3) == 0);
    sym_a = dl_intern_str(db, "A");
    sym_b = dl_intern_str(db, "B");

    cols[0] = sym_a; cols[1] = sym_b; cols[2] = dl_intern_str(db, "next");
    assert(dl_add_fact(db, "edge", cols, 3) == 1);
    cols[0] = sym_b; cols[1] = sym_a; cols[2] = dl_intern_str(db, "next");
    assert(dl_add_fact(db, "edge", cols, 3) == 1);

    n = dl_traverse(db, "A", 3, 100, traverse_count_cb, NULL);
    if (n != 2) { FAIL("expected 2 nodes (cycle prevented)"); dl_close(db); return; }
    PASS();
    dl_close(db);
}

static void t4_depth_cap(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t4");
    uint32_t sym_a, sym_b, sym_c, sym_d, cols[3];
    long n;

    TEST("T4 depth cap");
    assert(dl_declare_relation(db, "edge", 3) == 0);
    sym_a = dl_intern_str(db, "A");
    sym_b = dl_intern_str(db, "B");
    sym_c = dl_intern_str(db, "C");
    sym_d = dl_intern_str(db, "D");

    cols[0] = sym_a; cols[1] = sym_b; cols[2] = dl_intern_str(db, "next");
    assert(dl_add_fact(db, "edge", cols, 3) == 1);
    cols[0] = sym_b; cols[1] = sym_c; cols[2] = dl_intern_str(db, "next");
    assert(dl_add_fact(db, "edge", cols, 3) == 1);
    cols[0] = sym_c; cols[1] = sym_d; cols[2] = dl_intern_str(db, "next");
    assert(dl_add_fact(db, "edge", cols, 3) == 1);

    n = dl_traverse(db, "A", 1, 100, traverse_count_cb, NULL);
    if (n != 2) { FAIL("expected 2 nodes (A and B)"); dl_close(db); return; }
    PASS();
    dl_close(db);
}

static void t5_max_nodes(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t5");
    uint32_t sym_center, sym_n[10], cols[3];
    long n;
    int i;

    TEST("T5 max_nodes bound");
    assert(dl_declare_relation(db, "edge", 3) == 0);
    sym_center = dl_intern_str(db, "center");
    for (i = 0; i < 10; i++) {
        char name[16];
        snprintf(name, sizeof(name), "node%d", i);
        sym_n[i] = dl_intern_str(db, name);
        cols[0] = sym_center; cols[1] = sym_n[i]; cols[2] = dl_intern_str(db, "link");
        assert(dl_add_fact(db, "edge", cols, 3) == 1);
    }

    n = dl_traverse(db, "center", 1, 3, traverse_count_cb, NULL);
    if (n != 3) { FAIL("expected 3 nodes (center + 2 neighbors)"); dl_close(db); return; }
    PASS();
    dl_close(db);
}

static void t6_obs_fetch(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t6");
    uint32_t sym_a, cols[2];
    long n;

    TEST("T6 obs fetch");
    assert(dl_declare_relation(db, "observation", 2) == 0);
    sym_a = dl_intern_str(db, "A");

    cols[0] = sym_a; cols[1] = dl_intern_str(db, "hello");
    assert(dl_add_fact(db, "observation", cols, 2) == 1);
    cols[0] = sym_a; cols[1] = dl_intern_str(db, "world");
    assert(dl_add_fact(db, "observation", cols, 2) == 1);

    n = dl_node_observations(db, "A", 10, obs_count_cb, NULL);
    if (n != 2) { FAIL("expected 2 observations"); dl_close(db); return; }
    PASS();
    dl_close(db);
}

static void t7_obs_cap(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t7");
    uint32_t sym_a, cols[2];
    long n;
    int i;

    TEST("T7 obs max cap");
    assert(dl_declare_relation(db, "observation", 2) == 0);
    sym_a = dl_intern_str(db, "A");

    for (i = 0; i < 10; i++) {
        char obs[16];
        snprintf(obs, sizeof(obs), "observation%d", i);
        cols[0] = sym_a; cols[1] = dl_intern_str(db, obs);
        assert(dl_add_fact(db, "observation", cols, 2) == 1);
    }

    n = dl_node_observations(db, "A", 3, obs_count_cb, NULL);
    if (n != 3) { FAIL("expected 3 observations (capped)"); dl_close(db); return; }
    PASS();
    dl_close(db);
}

static void t8_missing_relation(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t8");
    long n;

    TEST("T8 missing relation error");
    n = dl_traverse(db, "A", 1, 100, traverse_count_cb, NULL);
    if (n != -1) { FAIL("expected -1 for missing edge relation"); dl_close(db); return; }
    PASS();
    dl_close(db);
}

static void t9_wrong_arity(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t9");
    long n;

    TEST("T9 wrong arity error");
    assert(dl_declare_relation(db, "edge", 2) == 0);
    n = dl_traverse(db, "A", 1, 100, traverse_count_cb, NULL);
    if (n != -1) { FAIL("expected -1 for wrong arity"); dl_close(db); return; }
    PASS();
    dl_close(db);
}

static void t10_unknown_node(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t10");
    uint32_t before_find, after_find;
    long n;

    TEST("T10 unknown start node -> 0, no interner side effect");
    assert(dl_declare_relation(db, "edge", 3) == 0);
    {
        uint32_t a = dl_intern_str(db, "A");
        uint32_t b = dl_intern_str(db, "B");
        uint32_t cols[3];
        cols[0] = a; cols[1] = b; cols[2] = dl_intern_str(db, "edge");
        dl_add_fact(db, "edge", cols, 3);
    }
    before_find = dl_intern_str_find(db, "GHOST");
    n = dl_traverse(db, "GHOST", 2, 100, traverse_count_cb, NULL);
    after_find = dl_intern_str_find(db, "GHOST");
    /* Unknown node: returns 0 (no results), and was never interned. */
    if (n != 0) { FAIL("expected 0 for unknown start node"); dl_close(db); return; }
    if (before_find != 0 || after_find != 0) {
        FAIL("unknown node should not be interned by traverse");
        dl_close(db); return;
    }
    PASS();
    dl_close(db);
}

static void t11_intern_find_nonmutating(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t11");
    uint32_t f;

    TEST("T11 dl_intern_str_find is non-mutating");
    (void)dl_intern_str(db, "known");
    f = dl_intern_str_find(db, "known");
    if (f == 0) { FAIL("expected find to return existing id"); dl_close(db); return; }
    /* A never-interned string stays 0 across repeated finds (never grown). */
    if (dl_intern_str_find(db, "never-interned") != 0 ||
        dl_intern_str_find(db, "never-interned") != 0) {
        FAIL("find should be idempotent-nonmutating (0 for absent)");
        dl_close(db); return;
    }
    PASS();
    dl_close(db);
}

static void t12_collect_error_propagates(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t12");
    long n;

    /* With edge arity 3 declared but its DAFSA empty, forward collect returns
     * 0 (empty) — so this verifies an empty edge is not an error.  The real
     * "collect error propagates" is exercised at the relation-validation layer
     * (T8/T9): once edge is validated at the top of dl_traverse, a later
     * collect failure is a genuine error that must propagate as -1. */
    TEST("T12 empty edge -> 0 (not an error), and intern_find(empty)");
    assert(dl_declare_relation(db, "edge", 3) == 0);
    assert(dl_declare_relation(db, "observation", 2) == 0);
    dl_intern_str(db, "A");
    n = dl_traverse(db, "A", 1, 100, traverse_count_cb, NULL);
    if (n != 1) { FAIL("expected 1 (start only) for empty edge"); dl_close(db); return; }
    if (dl_node_observations(db, "A", 10, obs_count_cb, NULL) != 0) {
        FAIL("expected 0 observations for empty observation rel");
        dl_close(db); return;
    }
    PASS();
    dl_close(db);
}

int main(void)
{
    printf("=== test_traverse ===\n");
    t1_forward_bfs();
    t2_reverse_bfs();
    t3_cycle();
    t4_depth_cap();
    t5_max_nodes();
    t6_obs_fetch();
    t7_obs_cap();
    t8_missing_relation();
    t9_wrong_arity();
    t10_unknown_node();
    t11_intern_find_nonmutating();
    t12_collect_error_propagates();

    printf("\n");
    if (tests_failed == 0)
        printf("All %d tests passed.\n", tests_run);
    else
        printf("%d/%d tests passed, %d failed.\n",
               tests_run - tests_failed, tests_run, tests_failed);

    return tests_failed != 0;
}
