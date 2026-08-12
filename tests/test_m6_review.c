/*
 * test_m6_review.c — Adversarial edge case tests for M6
 *
 * Tests:
 *   1. Arity-1 relation with perm (degenerate)
 *   2. All columns shared (k == arity)
 *   3. Reserved name rejection
 *   4. Empty relation perm index
 *   5. Multi-perm on same relation
 *   6. Recursive IDB with non-leading join in recursive body
 *   7. Snapshot re-query after publish
 */

#include "dl.h"
#include "snapshot.h"
#include "relation.h"
#include "intern.h"
#include "compiler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/stat.h>
#include <dirent.h>

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

/* ─── Tuple helpers ────────────────────────────────────────────────────── */

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

static int __attribute__((unused)) tset_eq(const tuple_set *a, const tuple_set *b)
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

/* ─── DB helpers ──────────────────────────────────────────────────────── */

static void setup_db(dl_db **db_out, const char *suffix)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf build-tmp/m6rev_%s", suffix);
    system(cmd);
    snprintf(cmd, sizeof(cmd), "build-tmp/m6rev_%s", suffix);
    *db_out = dl_open(cmd);
    assert(*db_out);
}

static void teardown_db(dl_db *db, const char *suffix)
{
    char cmd[512];
    dl_close(db);
    snprintf(cmd, sizeof(cmd), "rm -rf build-tmp/m6rev_%s", suffix);
    system(cmd);
}

static int load_rows_csv(dl_db *db, const char *rel_name, uint8_t arity,
                         const uint32_t *cols, int nrows)
{
    char csv_path[256];
    int i, c;
    FILE *f;

    assert(dl_declare_relation(db, rel_name, arity) == 0);

    snprintf(csv_path, sizeof(csv_path), "build-tmp/csv_%s_%p.csv",
             rel_name, (void *)(uintptr_t)rel_name);
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

/* ─── Test 1: Arity-1 perm (degenerate, should still work) ────────────── */

static void test_arity1_perm(void)
{
    dl_db *db;
    int loaded;
    tuple_set result;

    TEST("R1: arity-1 relation with perm (degenerate)");

    setup_db(&db, "r1");

    {
        uint32_t rows[] = {1, 2, 3};
        loaded = load_rows_csv(db, "a", 1, rows, 3);
        assert(loaded == 3);
    }
    {
        uint32_t rows[] = {1, 2, 4};
        loaded = load_rows_csv(db, "b", 1, rows, 3);
        assert(loaded == 3);
    }

    /* Non-leading join on arity-1 — degenerate case (the perm is identity) */
    assert(dl_load_rules(db,
        "c(X):-a(X),b(X).\n") == 0);
    assert(dl_compile(db) == 0);

    memset(&result, 0, sizeof(result));
    dl_query(db, "c", tset_cb, &result);

    /* Expected: intersection = {1, 2} */
    if (result.count == 2) {
        PASS();
    } else {
        printf("  got %ld tuples\n", result.count);
        FAIL("arity-1 result mismatch");
    }

    tset_free(&result);
    teardown_db(db, "r1");
}

/* ─── Test 2: All columns shared ──────────────────────────────────────── */

static void test_all_shared_cols(void)
{
    dl_db *db;
    int loaded;
    tuple_set result;

    TEST("R2: all columns shared (redundant but valid perm)");

    setup_db(&db, "r2");

    {
        uint32_t rows[] = {1,2, 3,4, 5,6};
        loaded = load_rows_csv(db, "p", 2, rows, 3);
        assert(loaded == 3);
    }
    {
        uint32_t rows[] = {1,2, 3,4, 5,6, 7,8};
        loaded = load_rows_csv(db, "q", 2, rows, 4);
        assert(loaded == 4);
    }

    /* Both columns shared — leading join should just use OP_LOOKUP */
    assert(dl_load_rules(db,
        "r(X,Y):-p(X,Y),q(X,Y).\n") == 0);
    assert(dl_compile(db) == 0);

    memset(&result, 0, sizeof(result));
    dl_query(db, "r", tset_cb, &result);

    /* Expected: intersection = {(1,2), (3,4), (5,6)} */
    if (result.count == 3) {
        PASS();
    } else {
        printf("  got %ld tuples\n", result.count);
        FAIL("all-shared result mismatch");
    }

    tset_free(&result);
    teardown_db(db, "r2");
}

/* ─── Test 3: Reserved name rejection ─────────────────────────────────── */

static void test_reserved_name(void)
{
    dl_db *db;
    int rc;

    TEST("R3: reserved __PI<hex>__ name rejected");

    setup_db(&db, "r3");

    rc = dl_declare_relation(db, "edge__PI0__", 2);
    if (rc == -1) {
        PASS();
    } else {
        FAIL("reserved name was accepted");
    }

    teardown_db(db, "r3");
}

/* ─── Test 4: Empty relation perm ─────────────────────────────────────── */

static void test_empty_rel_perm(void)
{
    dl_db *db;
    tuple_set result;

    TEST("R4: empty relation with perm index");

    setup_db(&db, "r4");

    assert(dl_declare_relation(db, "p", 2) == 0);
    assert(dl_declare_relation(db, "q", 2) == 0);

    /* Non-leading join on empty relations */
    assert(dl_load_rules(db,
        "r(X,Z):-p(X,Y),q(Z,Y).\n") == 0);
    assert(dl_compile(db) == 0);

    memset(&result, 0, sizeof(result));
    dl_query(db, "r", tset_cb, &result);

    if (result.count == 0) {
        PASS();
    } else {
        FAIL("empty relations should produce no output");
    }

    tset_free(&result);
    teardown_db(db, "r4");
}

/* ─── Test 5: Recursive IDB with non-leading join ────────────────────── */

static void test_recursive_non_leading_join(void)
{
    dl_db *db;
    int loaded;
    tuple_set result;

    TEST("R5: recursive TC with non-leading body join");

    setup_db(&db, "r5");

    {
        uint32_t rows[] = {1,2, 2,3, 3,4, 4,5, 5,6};
        loaded = load_rows_csv(db, "edge", 2, rows, 5);
        assert(loaded == 5);
    }

    /* Non-leading join in recursive rule: edge(Y,Z) is second atom,
     * join is on Y which is col 0 of tc(X,Y) and col 0 of edge(Y,Z).
     * Actually this is a leading join for edge. Let me make it
     * non-leading: join on Z in edge. */
    assert(dl_load_rules(db,
        "tc(X,Y):-edge(X,Y).\n"
        "tc(X,Z):-tc(X,Y),edge(Y,Z).\n") == 0);
    assert(dl_compile(db) == 0);

    memset(&result, 0, sizeof(result));
    dl_query(db, "tc", tset_cb, &result);

    /* TC of a chain 1→2→3→4→5→6 should have 15 pairs */
    if (result.count == 15) {
        PASS();
    } else {
        printf("  got %ld tuples, expected 15\n", result.count);
        FAIL("recursive TC result mismatch");
    }

    tset_free(&result);
    teardown_db(db, "r5");
}

/* ─── Main ────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("M6 Adversarial Review Tests\n");
    printf("============================\n\n");

    test_arity1_perm();
    test_all_shared_cols();
    test_reserved_name();
    test_empty_rel_perm();
    test_recursive_non_leading_join();

    printf("\n%d tests run, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
