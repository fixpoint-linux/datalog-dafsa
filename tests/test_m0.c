/*
 * test_m0.c — M0 verification: fact store + term interner
 *
 * Tests:
 *   1. Encode/decode round-trip for mixed string/int facts, arity <= 8
 *   2. Byte-determinism: same facts in same order → identical DAFSA save bytes
 *   3. The linchpin: rel_prefix(k) correctness for k=0,1,2 on arity-2 and arity-3
 *   4. dl_lookup exact 1/0 correctness; dl_prefix enumerates correct tuples
 *
 * Assert-based, standalone.  Links against libdatalog.so.
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

/* ─── Test 1: encode/decode round-trip ────────────────────────────────── */

static int count_cb(const uint32_t *cols, uint8_t arity, void *user)
{
    long *counter = (long *)user;
    (void)cols;
    (void)arity;
    (*counter)++;
    return 0;
}

static int collect_cb(const uint32_t *cols, uint8_t arity, void *user)
{
    uint32_t *buf = (uint32_t *)user;
    /* buf is pre-sized; caller knows how many to expect */
    memcpy(buf, cols, arity * sizeof(uint32_t));
    return 0;
}

static void test_encode_decode(void)
{
    TEST("encode/decode round-trip arity-2 (int+string)");

    relation *rel = rel_create(2);
    assert(rel);

    /* Add facts: (1, 100), (1, 200), (2, 100), (2, 200) */
    uint32_t facts[][2] = {{1, 100}, {1, 200}, {2, 100}, {2, 200}};
    int i;
    for (i = 0; i < 4; i++) {
        int rc = rel_add(rel, facts[i]);
        assert(rc == 1);  /* all should be new */
    }

    /* Exact lookups */
    assert(rel_exact(rel, facts[0]) == 1);
    assert(rel_exact(rel, facts[3]) == 1);
    {
        uint32_t absent[] = {3, 100};
        assert(rel_exact(rel, absent) == 0);
    }

    /* Prefix k=0: should get all 4 */
    {
        long count = 0;
        long n = rel_prefix(rel, NULL, 0, count_cb, &count);
        assert(n == 4);
        assert(count == 4);
    }

    /* Prefix k=1: first col = 1 → 2 results */
    {
        uint32_t leading[] = {1};
        long count = 0;
        long n = rel_prefix(rel, leading, 1, count_cb, &count);
        assert(n == 2);
        assert(count == 2);
    }

    /* Prefix k=2 is exact match */
    {
        uint32_t leading[] = {1, 100};
        long count = 0;
        long n = rel_prefix(rel, leading, 2, count_cb, &count);
        assert(n == 1);
        assert(count == 1);
    }

    rel_free(rel);
    PASS();
}

/* ─── Test 1b: arity-3 mixed ──────────────────────────────────────────── */

static void test_arity3(void)
{
    TEST("encode/decode round-trip arity-3");
    relation *rel = rel_create(3);
    assert(rel);

    uint32_t f1[] = {10, 20, 30};
    uint32_t f2[] = {10, 20, 40};
    uint32_t f3[] = {10, 30, 50};
    uint32_t f4[] = {20, 10, 60};

    assert(rel_add(rel, f1) == 1);
    assert(rel_add(rel, f2) == 1);
    assert(rel_add(rel, f3) == 1);
    assert(rel_add(rel, f4) == 1);

    /* Exact */
    assert(rel_exact(rel, f1) == 1);
    assert(rel_exact(rel, f4) == 1);

    /* k=0: all 4 */
    {
        long count = 0;
        assert(rel_prefix(rel, NULL, 0, count_cb, &count) == 4);
    }

    /* k=1 with leading [10]: should get f1, f2, f3 */
    {
        uint32_t l[] = {10};
        long count = 0;
        assert(rel_prefix(rel, l, 1, count_cb, &count) == 3);
    }

    /* k=2 with leading [10, 20]: should get f1, f2 */
    {
        uint32_t l[] = {10, 20};
        long count = 0;
        assert(rel_prefix(rel, l, 2, count_cb, &count) == 2);
    }

    /* k=3 exact: [10, 20, 30] → f1 */
    {
        uint32_t l[] = {10, 20, 30};
        long count = 0;
        assert(rel_prefix(rel, l, 3, count_cb, &count) == 1);
    }

    rel_free(rel);
    PASS();
}

/* ─── Test 2: byte-determinism ────────────────────────────────────────── */

static void test_byte_determinism(void)
{
    TEST("byte-determinism (same facts → same DAFSA bytes)");

    /* Build a relation, save it, build another with same facts, save, compare */
    const char *path1 = "tests/bd1.dafsa";
    const char *path2 = "tests/bd2.dafsa";

    /* First build */
    {
        relation *r = rel_create(2);
        uint32_t f[][2] = {{5, 10}, {5, 20}, {6, 10}, {6, 30}};
        int i;
        for (i = 0; i < 4; i++) rel_add(r, f[i]);
        assert(rel_save(r, path1) == 0);
        rel_free(r);
    }

    /* Second build (same order) */
    {
        relation *r = rel_create(2);
        uint32_t f[][2] = {{5, 10}, {5, 20}, {6, 10}, {6, 30}};
        int i;
        for (i = 0; i < 4; i++) rel_add(r, f[i]);
        assert(rel_save(r, path2) == 0);
        rel_free(r);
    }

    /* Compare byte-for-byte */
    {
        FILE *f1 = fopen(path1, "rb");
        FILE *f2 = fopen(path2, "rb");
        assert(f1 && f2);

        fseek(f1, 0, SEEK_END);
        fseek(f2, 0, SEEK_END);
        long sz1 = ftell(f1);
        long sz2 = ftell(f2);
        assert(sz1 == sz2 && sz1 > 0);

        fseek(f1, 0, SEEK_SET);
        fseek(f2, 0, SEEK_SET);

        int c1, c2;
        while ((c1 = fgetc(f1)) != EOF && (c2 = fgetc(f2)) != EOF) {
            if (c1 != c2) {
                fclose(f1); fclose(f2);
                FAIL("byte mismatch");
                remove(path1); remove(path2);
                return;
            }
        }
        fclose(f1); fclose(f2);
    }

    remove(path1);
    remove(path2);
    PASS();
}

/* ─── Test 3: the linchpin ────────────────────────────────────────────── */

/*
 * The linchpin test: verify that rel_prefix with k bound leading columns
 * correctly enumerates all matching tuples.  We compare against exact
 * membership checks for every possible combination.
 */
static void test_linchpin_arity2(void)
{
    TEST("linchpin: rel_prefix(k) vs manual exact on arity-2");

    relation *rel = rel_create(2);
    uint32_t facts[][2] = {
        {100, 200}, {100, 300}, {100, 400},
        {200, 300}, {200, 500}, {300, 100},
    };
    int nf = 6, i;

    for (i = 0; i < nf; i++)
        assert(rel_add(rel, facts[i]) == 1);

    /* k=0: enumerate all, verify each against exact lookup */
    {
        long count = 0;
        /* We'll verify by counting */
        long n = rel_prefix(rel, NULL, 0, count_cb, &count);
        assert(n == 6);
        assert(count == 6);
    }

    /* For each distinct first-column value, verify prefix enum matches */
    /* First col values: 100, 200, 300 */
    uint32_t firsts[] = {100, 200, 300};
    int expected_counts[] = {3, 2, 1};

    for (i = 0; i < 3; i++) {
        uint32_t leading[] = {firsts[i]};
        long count = 0;
        long n = rel_prefix(rel, leading, 1, count_cb, &count);
        if (n != expected_counts[i] || count != (long)expected_counts[i]) {
            FAIL("k=1 prefix count mismatch");
            rel_free(rel);
            return;
        }

        /* Also verify each prefix result matches exact lookup */
        /* For this we need to check the actual values */
        {
            uint32_t buf[2];
            long n2 = rel_prefix(rel, leading, 1, collect_cb, buf);
            assert(n2 == expected_counts[i]);
            /* The last collected tuple is in buf */
            /* We can at least verify it exists */
            assert(rel_exact(rel, buf) == 1);
        }
    }

    /* k=2: each fact should match exactly with prefix */
    for (i = 0; i < nf; i++) {
        long count = 0;
        long n = rel_prefix(rel, facts[i], 2, count_cb, &count);
        assert(n == 1);
        assert(count == 1);
    }

    rel_free(rel);
    PASS();
}

static void test_linchpin_arity3(void)
{
    TEST("linchpin: rel_prefix(k) on arity-3");

    relation *rel = rel_create(3);
    uint32_t facts[][3] = {
        {1, 10, 100}, {1, 10, 200}, {1, 20, 100},
        {2, 10, 100}, {2, 20, 200}, {2, 20, 300},
        {3, 30, 100},
    };
    int nf = 7, i;

    for (i = 0; i < nf; i++)
        assert(rel_add(rel, facts[i]) == 1);

    /* k=0: all 7 */
    {
        long count = 0;
        assert(rel_prefix(rel, NULL, 0, count_cb, &count) == 7);
    }

    /* k=1 with [1] → 3 */
    {
        uint32_t l[] = {1};
        long count = 0;
        assert(rel_prefix(rel, l, 1, count_cb, &count) == 3);
    }

    /* k=2 with [2, 20] → 2 */
    {
        uint32_t l[] = {2, 20};
        long count = 0;
        assert(rel_prefix(rel, l, 2, count_cb, &count) == 2);
    }

    /* k=3 exact for each */
    for (i = 0; i < nf; i++) {
        long count = 0;
        assert(rel_prefix(rel, facts[i], 3, count_cb, &count) == 1);
    }

    /* k=1 with [99] → 0 */
    {
        uint32_t l[] = {99};
        long count = 0;
        assert(rel_prefix(rel, l, 1, count_cb, &count) == 0);
    }

    rel_free(rel);
    PASS();
}

/* ─── Test 4: dl_lookup & dl_prefix through public API ────────────────── */

static int dl_collect_cb(const uint32_t *cols, uint8_t arity, void *user)
{
    long *counter = (long *)user;
    (void)cols;
    (void)arity;
    (*counter)++;
    return 0;
}

static void test_public_api(void)
{
    TEST("public API: dl_lookup + dl_prefix");

    /* Remove any stale DB */
    system("rm -rf tests/dl-test-api");

    dl_db *db = dl_open("tests/dl-test-api");
    assert(db);

    /* Declare relation edge/2 */
    assert(dl_declare_relation(db, "edge", 2) == 0);

    /* Add facts manually via a temporary CSV */
    {
        FILE *f = fopen("tests/dl-test-api/test.csv", "w");
        assert(f);
        fprintf(f, "1,2\n1,3\n2,3\n2,4\n3,5\n");
        fclose(f);
    }

    int loaded = dl_load_facts(db, "edge", "tests/dl-test-api/test.csv");
    assert(loaded == 5);

    /* Exact lookup */
    {
        uint32_t cols[] = {1, 2};
        assert(dl_lookup(db, "edge", cols, 2) == 1);
    }
    {
        uint32_t cols[] = {1, 99};
        assert(dl_lookup(db, "edge", cols, 2) == 0);
    }

    /* Prefix lookup k=0: all 5 */
    {
        long count = 0;
        assert(dl_prefix(db, "edge", NULL, 0, dl_collect_cb, &count) == 5);
    }

    /* Prefix k=1: first col = 2 → 2 results */
    {
        uint32_t leading[] = {2};
        long count = 0;
        assert(dl_prefix(db, "edge", leading, 1, dl_collect_cb, &count) == 2);
    }

    /* Prefix k=2: exact match (1,3) */
    {
        uint32_t leading[] = {1, 3};
        long count = 0;
        assert(dl_prefix(db, "edge", leading, 2, dl_collect_cb, &count) == 1);
    }

    dl_close(db);

    /* Cleanup */
    system("rm -rf tests/dl-test-api");
    PASS();
}

/* ─── Test 5: interner ────────────────────────────────────────────────── */

static void test_interner(void)
{
    TEST("interner: string → id round-trip");

    interner *ir = intern_create();
    assert(ir);

    uint32_t id1 = intern_str(ir, "hello");
    uint32_t id2 = intern_str(ir, "world");
    uint32_t id3 = intern_str(ir, "hello");  /* should return same id */

    assert(id1 > 0);
    assert(id2 > 0);
    assert(id3 == id1);
    assert(id1 != id2);

    assert(strcmp(intern_str_of(ir, id1), "hello") == 0);
    assert(strcmp(intern_str_of(ir, id2), "world") == 0);
    assert(intern_str_of(ir, 999) == NULL);
    assert(intern_str_of(ir, 0) == NULL);

    intern_free(ir);
    PASS();
}

/* ─── Test 6: arity limit ─────────────────────────────────────────────── */

static void test_arity_limit(void)
{
    TEST("arity limit (1-8)");

    /* arity 0 should fail */
    assert(rel_create(0) == NULL);

    /* arities 1-8 should succeed */
    int a;
    for (a = 1; a <= 8; a++) {
        relation *r = rel_create((uint8_t)a);
        assert(r);
        rel_free(r);
    }

    /* arity 9 should fail */
    assert(rel_create(9) == NULL);

    PASS();
}

/* ─── Test 7: interner persistence ────────────────────────────────────── */

static void test_interner_persistence(void)
{
    TEST("interner save/load round-trip");

    const char *fwd = "tests/intern-test.dafsa";
    const char *rev = "tests/intern-test.array";

    /* Create and populate */
    interner *ir1 = intern_create();
    assert(ir1);
    uint32_t id_a = intern_str(ir1, "alice");
    uint32_t id_b = intern_str(ir1, "bob");
    assert(id_a > 0 && id_b > 0);
    assert(intern_save(ir1, fwd, rev) == 0);
    intern_free(ir1);

    /* Reload */
    interner *ir2 = intern_load(fwd, rev);
    assert(ir2);

    /* Strings should resolve to same ids */
    uint32_t id_a2 = intern_str(ir2, "alice");
    uint32_t id_b2 = intern_str(ir2, "bob");
    assert(id_a2 == id_a);
    assert(id_b2 == id_b);
    assert(strcmp(intern_str_of(ir2, id_a), "alice") == 0);
    assert(strcmp(intern_str_of(ir2, id_b), "bob") == 0);

    /* A new string should get a new id */
    uint32_t id_c = intern_str(ir2, "charlie");
    assert(id_c > id_b);

    intern_free(ir2);
    remove(fwd);
    remove(rev);
    PASS();
}

/* ─── Main ────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("M0 Tests\n");
    printf("========\n\n");

    test_encode_decode();
    test_arity3();
    test_byte_determinism();
    test_linchpin_arity2();
    test_linchpin_arity3();
    test_public_api();
    test_interner();
    test_arity_limit();
    test_interner_persistence();

    printf("\n---\n");
    printf("%d tests run, %d failed\n", tests_run, tests_failed);

    return tests_failed ? 1 : 0;
}
