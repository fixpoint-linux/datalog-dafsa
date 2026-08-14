/*
 * test_ivm.c — IVM Slice 0: deletion-correctness for the full re-eval oracle
 *
 * The DAFSA Datalog evaluator is ADD-ONLY by construction: materialize
 * unions pre-existing facts and re-derives on top.  Before the base/view
 * partition, a dl_delete_fact + dl_publish_snapshot left the previously
 * derived tuples in the relation, so the re-evaluation returned stale rows.
 *
 * These tests pin the fix:
 *   T1  recursive transitive closure — deleting edge(2,3) must shrink tc
 *       from 6 rows to 2 (not keep 6).
 *   T2  grouped aggregate — deleting sale(1,10) must drop (1,15), keeping
 *       the recomputed (1,5).
 *   T3  mixed EDB+IDB mutual recursion — deleting the base fact even(0)
 *       must re-derive even/odd to empty (not keep stale evens/odds).
 *   T4  reopen — base persistence: an IDB relation with base facts must
 *       re-derive correctly after dl_close + dl_open.
 *
 * Regression: full `make test` (M0-M9) must stay green + this suite.
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
} while(0)

#define PASS() do { printf("OK\n"); } while(0)
#define FAIL(msg) do { \
    printf("FAIL: %s\n", msg); \
    tests_failed++; \
} while(0)

/* ─── Tuple set (local, mirrors test_m2.c) ────────────────────────────── */

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

static int tset_has(const tuple_set *ts, const uint32_t *row)
{
    long i;
    for (i = 0; i < ts->count; i++) {
        if (memcmp(ts->data + (size_t)i * ts->arity, row,
                   (size_t)ts->arity * sizeof(uint32_t)) == 0)
            return 1;
    }
    return 0;
}

static void query_set(dl_db *db, const char *rel, tuple_set *out)
{
    memset(out, 0, sizeof(*out));
    dl_query(db, rel, tset_cb, out);
}

/* ─── Helpers ─────────────────────────────────────────────────────────── */

static void rm_dir(const char *dir)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    system(cmd);
}

static void setup_db(dl_db **db_out, const char *name)
{
    char dir[256];
    snprintf(dir, sizeof(dir), "build-tmp/ivm-%s", name);
    rm_dir(dir);
    *db_out = dl_open(dir);
    assert(*db_out);
}

static void teardown_db(dl_db *db, const char *name)
{
    char dir[256];
    dl_close(db);
    snprintf(dir, sizeof(dir), "build-tmp/ivm-%s", name);
    rm_dir(dir);
}

/* Load integer rows into a declared relation via CSV. */
static void load_rows(dl_db *db, const char *name, const char *rel,
                      uint8_t arity, const uint32_t *cols, int nrows)
{
    char path[512];
    int i, c;
    FILE *f;

    assert(dl_declare_relation(db, rel, arity) == 0);
    snprintf(path, sizeof(path), "build-tmp/ivm-%s/%s.csv", name, rel);
    f = fopen(path, "w");
    assert(f);
    for (i = 0; i < nrows; i++) {
        for (c = 0; c < arity; c++) {
            if (c > 0) fputc(',', f);
            fprintf(f, "%u", cols[(size_t)i * (size_t)arity + (size_t)c]);
        }
        fputc('\n', f);
    }
    fclose(f);

    int loaded = dl_load_facts(db, rel, path);
    assert(loaded == nrows);
    (void)loaded;
}

/* ─── T1: recursive TC deletion correctness ───────────────────────────── */

static void test_tc_delete(void)
{
    dl_db *db;
    tuple_set ts;

    TEST("T1: recursive tc — delete edge(2,3) shrinks tc 6 -> 2");

    setup_db(&db, "t1");

    /* Chain 1->2->3->4: tc = {(1,2),(2,3),(3,4),(1,3),(1,4),(2,4)} = 6 */
    {
        uint32_t edges[] = {1,2, 2,3, 3,4};
        load_rows(db, "t1", "edge", 2, edges, 3);
    }

    assert(dl_load_rules(db,
        "tc(X,Y):-edge(X,Y).\n"
        "tc(X,Z):-edge(X,Y),tc(Y,Z).\n") == 0);

    assert(dl_compile(db) == 0);
    assert(dl_publish_snapshot(db) == 0);

    query_set(db, "tc", &ts);
    if (ts.count != 6 || ts.arity != 2) {
        printf("  before delete: got %ld rows, expected 6\n", ts.count);
        tset_free(&ts); teardown_db(db, "t1");
        FAIL("T1: wrong tc count before delete");
        return;
    }
    tset_free(&ts);

    /* Delete edge(2,3).  Remaining edges (1,2),(3,4) -> tc = {(1,2),(3,4)}. */
    {
        uint32_t e23[2] = {2, 3};
        assert(dl_delete_fact(db, "edge", e23, 2) == 1);
    }

    assert(dl_publish_snapshot(db) == 0);

    query_set(db, "tc", &ts);
    if (ts.count != 2 || ts.arity != 2) {
        printf("  after delete: got %ld rows, expected 2\n", ts.count);
        long i;
        for (i = 0; i < ts.count; i++)
            printf("    (%u,%u)\n", ts.data[i*2], ts.data[i*2+1]);
        tset_free(&ts); teardown_db(db, "t1");
        FAIL("T1: stale tc rows after delete");
        return;
    }
    {
        uint32_t r12[2] = {1,2}, r34[2] = {3,4};
        if (!tset_has(&ts, r12) || !tset_has(&ts, r34)) {
            tset_free(&ts); teardown_db(db, "t1");
            FAIL("T1: missing expected tc rows after delete");
            return;
        }
    }
    tset_free(&ts);

    teardown_db(db, "t1");
    PASS();
}

/* ─── T2: grouped aggregate deletion correctness ──────────────────────── */

static void test_aggregate_delete(void)
{
    dl_db *db;
    tuple_set ts;

    TEST("T2: aggregate total — delete sale(1,10) drops stale (1,15)");

    setup_db(&db, "t2");

    /* sale(1,10),(1,5),(2,3) -> total = {(1,15),(2,3)} */
    {
        uint32_t sales[] = {1,10, 1,5, 2,3};
        load_rows(db, "t2", "sale", 2, sales, 3);
    }

    assert(dl_load_rules(db,
        "total(X,S):-sale(X,Y),S=sum(Y).\n") == 0);

    assert(dl_compile(db) == 0);
    assert(dl_publish_snapshot(db) == 0);

    query_set(db, "total", &ts);
    if (ts.count != 2 || ts.arity != 2) {
        printf("  before delete: got %ld rows, expected 2\n", ts.count);
        tset_free(&ts); teardown_db(db, "t2");
        FAIL("T2: wrong total count before delete");
        return;
    }
    tset_free(&ts);

    /* Delete sale(1,10).  Remaining sales (1,5),(2,3) -> total {(1,5),(2,3)}. */
    {
        uint32_t s[2] = {1, 10};
        assert(dl_delete_fact(db, "sale", s, 2) == 1);
    }

    assert(dl_publish_snapshot(db) == 0);

    query_set(db, "total", &ts);
    {
        uint32_t stale[2] = {1, 15};
        uint32_t keep1[2] = {1, 5};
        uint32_t keep2[2] = {2, 3};
        if (ts.count != 2 || ts.arity != 2) {
            printf("  after delete: got %ld rows, expected 2\n", ts.count);
            long i;
            for (i = 0; i < ts.count; i++)
                printf("    (%u,%u)\n", ts.data[i*2], ts.data[i*2+1]);
            tset_free(&ts); teardown_db(db, "t2");
            FAIL("T2: wrong total count after delete");
            return;
        }
        if (tset_has(&ts, stale)) {
            tset_free(&ts); teardown_db(db, "t2");
            FAIL("T2: stale (1,15) survived the delete");
            return;
        }
        if (!tset_has(&ts, keep1) || !tset_has(&ts, keep2)) {
            tset_free(&ts); teardown_db(db, "t2");
            FAIL("T2: recomputed totals missing");
            return;
        }
    }
    tset_free(&ts);

    teardown_db(db, "t2");
    PASS();
}

/* ─── T3: mixed EDB+IDB mutual recursion deletion ─────────────────────── */

static void test_evenodd_delete(void)
{
    dl_db *db;
    tuple_set ts;
    int i;

    TEST("T3: mixed even/odd — delete even(0) empties both");

    setup_db(&db, "t3");

    /* succ(0,1)..succ(9,10) */
    {
        uint32_t succ[20];
        for (i = 0; i < 10; i++) { succ[i*2] = (uint32_t)i; succ[i*2+1] = (uint32_t)(i+1); }
        load_rows(db, "t3", "succ", 2, succ, 10);
    }
    /* even(0) base fact */
    {
        uint32_t ev = 0;
        load_rows(db, "t3", "even", 1, &ev, 1);
    }

    assert(dl_load_rules(db,
        "even(X):-succ(Y,X),odd(Y).\n"
        "odd(X):-succ(Y,X),even(Y).\n") == 0);

    assert(dl_compile(db) == 0);

    query_set(db, "even", &ts);
    if (ts.count != 6) {
        printf("  before delete: even got %ld rows, expected 6\n", ts.count);
        tset_free(&ts); teardown_db(db, "t3");
        FAIL("T3: wrong even count before delete");
        return;
    }
    tset_free(&ts);

    /* Delete the base fact even(0).  Without it the mutual recursion has no
     * seed, so even and odd must both re-derive to empty. */
    {
        uint32_t e0[1] = {0};
        assert(dl_delete_fact(db, "even", e0, 1) == 1);
    }

    assert(dl_publish_snapshot(db) == 0);

    query_set(db, "even", &ts);
    if (ts.count != 0) {
        printf("  after delete: even got %ld rows, expected 0\n", ts.count);
        long j;
        for (j = 0; j < ts.count; j++) printf("    even(%u)\n", ts.data[j]);
        tset_free(&ts); teardown_db(db, "t3");
        FAIL("T3: stale even rows after delete");
        return;
    }
    tset_free(&ts);

    query_set(db, "odd", &ts);
    if (ts.count != 0) {
        printf("  after delete: odd got %ld rows, expected 0\n", ts.count);
        long j;
        for (j = 0; j < ts.count; j++) printf("    odd(%u)\n", ts.data[j]);
        tset_free(&ts); teardown_db(db, "t3");
        FAIL("T3: stale odd rows after delete");
        return;
    }
    tset_free(&ts);

    teardown_db(db, "t3");
    PASS();
}

/* ─── T4: IDB base persistence across reopen ──────────────────────────── */

static void test_reopen_rederive(void)
{
    dl_db *db;
    tuple_set ts;
    int i;

    TEST("T4: reopen re-derives IDB from persisted base");

    setup_db(&db, "t4");

    {
        uint32_t succ[20];
        for (i = 0; i < 10; i++) { succ[i*2] = (uint32_t)i; succ[i*2+1] = (uint32_t)(i+1); }
        load_rows(db, "t4", "succ", 2, succ, 10);
    }
    {
        uint32_t ev = 0;
        load_rows(db, "t4", "even", 1, &ev, 1);
    }

    assert(dl_load_rules(db,
        "even(X):-succ(Y,X),odd(Y).\n"
        "odd(X):-succ(Y,X),even(Y).\n") == 0);
    assert(dl_compile(db) == 0);

    /* Close (persists even.base.dafsa + rels.txt idb flag), reopen, reload
     * rules, re-derive.  even must be {0,2,4,6,8,10} again. */
    dl_close(db);
    db = dl_open("build-tmp/ivm-t4");
    assert(db != NULL);

    assert(dl_load_rules(db,
        "even(X):-succ(Y,X),odd(Y).\n"
        "odd(X):-succ(Y,X),even(Y).\n") == 0);
    assert(dl_compile(db) == 0);

    query_set(db, "even", &ts);
    if (ts.count != 6) {
        printf("  reopen: even got %ld rows, expected 6\n", ts.count);
        tset_free(&ts); teardown_db(db, "t4");
        FAIL("T4: wrong even count after reopen");
        return;
    }
    {
        uint32_t expected[6] = {0,2,4,6,8,10};
        for (i = 0; i < 6; i++) {
            uint32_t row[1] = {expected[i]};
            if (!tset_has(&ts, row)) {
                printf("  reopen: missing even(%u)\n", expected[i]);
                tset_free(&ts); teardown_db(db, "t4");
                FAIL("T4: missing even value after reopen");
                return;
            }
        }
    }
    tset_free(&ts);

    query_set(db, "odd", &ts);
    if (ts.count != 5) {
        printf("  reopen: odd got %ld rows, expected 5\n", ts.count);
        tset_free(&ts); teardown_db(db, "t4");
        FAIL("T4: wrong odd count after reopen");
        return;
    }
    tset_free(&ts);

    /* Now delete the base fact even(0) AFTER reopen.  If base had been
     * persisted as the full view {0,2,4,6,8,10} instead of just {0}, this
     * would leave stale evens behind.  Correctly-persisted base re-derives
     * even/odd to empty. */
    {
        uint32_t e0[1] = {0};
        assert(dl_delete_fact(db, "even", e0, 1) == 1);
    }
    assert(dl_publish_snapshot(db) == 0);

    query_set(db, "even", &ts);
    if (ts.count != 0) {
        printf("  reopen+delete: even got %ld rows, expected 0\n", ts.count);
        tset_free(&ts); teardown_db(db, "t4");
        FAIL("T4: base not persisted as base-only (stale evens after reopen+delete)");
        return;
    }
    tset_free(&ts);

    teardown_db(db, "t4");
    PASS();
}

/* ─── Main ────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("IVM Slice 0 deletion-correctness tests\n");
    printf("=======================================\n\n");

    test_tc_delete();
    test_aggregate_delete();
    test_evenodd_delete();
    test_reopen_rederive();

    printf("\n%d tests run, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
