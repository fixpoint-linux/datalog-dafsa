/*
 * test_m14_permsel.c — automatic perm-index selection (M6-permsel)
 *
 * Verifies the cost-gated OP_LOOKUP_PERM vs OP_HASH_JOIN decision in
 * emit_nonleading_join, plus the recursive-IDB carve-out and graceful
 * perm-cap exhaustion:
 *   T1: small rel -> OP_HASH_JOIN, no perm declared, correct result
 *   T2: large rel -> perm declared, correct result
 *   T3: two rules sharing (rel,perm) -> exactly one perm
 *   T4: ORACLE — same ruleset with g_perm_select=1 vs 0 gives byte-identical
 *       query results (strategy never changes semantics)
 *   T5: perm-cap exhaustion degrades to hash join, no hard compile error
 *   T6: recursive IDB non-leading join uses the perm carve-out and matches
 *       its leading-join equivalent
 */

#include "dl.h"
#include "compiler.h"
#include "permindex.h"

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

/* ─── Tuple set helpers ────────────────────────────────────────────────── */

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

/* Build an expected single-tuple set (arity-1). */
static void tset_from_u32(tuple_set *ts, const uint32_t *vals, long n,
                          uint8_t arity)
{
    memset(ts, 0, sizeof(*ts));
    long i;
    for (i = 0; i < n; i++)
        tset_cb(vals + (size_t)i * arity, arity, ts);
}

/* ─── Database helpers ────────────────────────────────────────────────── */

static void setup_db(dl_db **db_out, const char *suffix)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf build-tmp/m14db_%s", suffix);
    system(cmd);
    snprintf(cmd, sizeof(cmd), "build-tmp/m14db_%s", suffix);
    *db_out = dl_open(cmd);
    assert(*db_out);
}

static void teardown_db(dl_db *db, const char *suffix)
{
    char cmd[512];
    dl_close(db);
    snprintf(cmd, sizeof(cmd), "rm -rf build-tmp/m14db_%s", suffix);
    system(cmd);
}

static int load_rows_csv(dl_db *db, const char *rel_name, uint8_t arity,
                         const uint32_t *cols, int nrows)
{
    char csv_path[256];
    int i, c;
    FILE *f;

    assert(dl_declare_relation(db, rel_name, arity) == 0);

    snprintf(csv_path, sizeof(csv_path), "build-tmp/m14csv_%s.csv", rel_name);
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

/* ─── Oracle: identical ruleset compiled with perm-selection ON vs OFF ── */

/* Compile `nbatches` rule batches (each load_rules+compile) into a fresh db,
 * then query `goal` and compare the tuple sets.  `load_facts` is invoked on
 * each fresh db to (re)load the EDB facts (must be idempotent per db).
 * Returns 1 on equality. */
static int oracle_compare(const char *suffix, void (*load_facts)(dl_db *),
                          const char *const *batches, int nbatches,
                          const char *goal)
{
    dl_db *dbA, *dbB;
    tuple_set rA, rB;
    char sa[64], sb[64];
    int i, ok = 1;

    snprintf(sa, sizeof(sa), "%s_on", suffix);
    snprintf(sb, sizeof(sb), "%s_off", suffix);

    /* select=1 (auto perm selection) */
    g_perm_select = 1;
    setup_db(&dbA, sa);
    if (load_facts) load_facts(dbA);
    for (i = 0; i < nbatches; i++) {
        if (dl_load_rules(dbA, batches[i]) != 0 || dl_compile(dbA) != 0) {
            printf("  select=1 compile error (batch %d)\n", i);
            teardown_db(dbA, sa);
            return 0;
        }
    }
    memset(&rA, 0, sizeof(rA));
    dl_query(dbA, goal, tset_cb, &rA);

    /* select=0 (always hash join) */
    g_perm_select = 0;
    setup_db(&dbB, sb);
    if (load_facts) load_facts(dbB);
    for (i = 0; i < nbatches; i++) {
        if (dl_load_rules(dbB, batches[i]) != 0 || dl_compile(dbB) != 0) {
            printf("  select=0 compile error (batch %d)\n", i);
            tset_free(&rA);
            teardown_db(dbB, sb);
            g_perm_select = 1;
            return 0;
        }
    }
    memset(&rB, 0, sizeof(rB));
    dl_query(dbB, goal, tset_cb, &rB);

    if (!tset_eq(&rA, &rB)) {
        printf("  oracle mismatch: select=1 got %ld, select=0 got %ld\n",
               rA.count, rB.count);
        ok = 0;
    }

    tset_free(&rA);
    tset_free(&rB);
    teardown_db(dbA, sa);
    teardown_db(dbB, sb);
    g_perm_select = 1;
    return ok;
}

/* ─── T1: small rel -> hash join, no perm ──────────────────────────────── */

static void test_t1_small_hash_join(void)
{
    dl_db *db;
    tuple_set result, exp;
    int loaded;

    TEST("T1: small rel uses hash join, declares no perm");

    setup_db(&db, "t1");

    {
        uint32_t rows[] = {1,2, 3,4};
        loaded = load_rows_csv(db, "p", 2, rows, 2);
        assert(loaded == 2);
    }
    {
        uint32_t rows[] = {10,2, 20,6};
        loaded = load_rows_csv(db, "e", 2, rows, 2);
        assert(loaded == 2);
    }

    assert(dl_load_rules(db, "q(X,Z):-p(X,Y),e(Z,Y).\n") == 0);
    assert(dl_compile(db) == 0);

    if (dl_db_perm_count(db) != 0) {
        printf("  perm_count=%d, expected 0\n", dl_db_perm_count(db));
        FAIL("T1 expected no perm for small rels");
        teardown_db(db, "t1");
        return;
    }

    memset(&result, 0, sizeof(result));
    dl_query(db, "q", tset_cb, &result);
    /* p(1,2) joins e(10,2) -> q(1,10); p(3,4) has no e match */
    uint32_t expected[] = {1,10};
    tset_from_u32(&exp, expected, 1, 2);

    if (tset_eq(&result, &exp)) {
        PASS();
    } else {
        printf("  got %ld tuples, expected 1\n", result.count);
        FAIL("T1 result mismatch");
    }

    tset_free(&result);
    tset_free(&exp);
    teardown_db(db, "t1");
}

/* ─── T2: large rel -> perm declared ──────────────────────────────────── */

static void test_t2_large_perm(void)
{
    dl_db *db;
    tuple_set result, exp;
    int loaded;

    TEST("T2: large rel declares a perm index");

    setup_db(&db, "t2");

    {
        uint32_t rows[] = {1,0, 2,5, 3,10};
        loaded = load_rows_csv(db, "p", 2, rows, 3);
        assert(loaded == 3);
    }
    {
        uint32_t rows[2 * 120];
        int i;
        for (i = 0; i < 120; i++) { rows[i*2] = (uint32_t)i; rows[i*2+1] = (uint32_t)i; }
        loaded = load_rows_csv(db, "e", 2, rows, 120);
        assert(loaded == 120);
    }

    assert(dl_load_rules(db, "q(X,Z):-p(X,Y),e(Z,Y).\n") == 0);
    assert(dl_compile(db) == 0);

    if (dl_db_perm_count(db) < 1) {
        printf("  perm_count=%d, expected >=1\n", dl_db_perm_count(db));
        FAIL("T2 expected a perm for large rel");
        teardown_db(db, "t2");
        return;
    }

    memset(&result, 0, sizeof(result));
    dl_query(db, "q", tset_cb, &result);
    /* p(X,Y) joins e(Z,Y): (1,0)->e(0,0); (2,5)->e(5,5); (3,10)->e(10,10) */
    uint32_t expected[] = {1,0, 2,5, 3,10};
    tset_from_u32(&exp, expected, 3, 2);

    if (tset_eq(&result, &exp)) {
        PASS();
    } else {
        printf("  got %ld tuples, expected 3\n", result.count);
        FAIL("T2 result mismatch");
    }

    tset_free(&result);
    tset_free(&exp);
    teardown_db(db, "t2");
}

/* ─── T3: two rules share (rel,perm) -> exactly one perm ──────────────── */

static void test_t3_reuse_dedup(void)
{
    dl_db *db;
    tuple_set result;
    int loaded;

    TEST("T3: two rules sharing (rel,perm) declare exactly one perm");

    setup_db(&db, "t3");

    {
        uint32_t rows[] = {1,2};
        loaded = load_rows_csv(db, "p", 2, rows, 1);
        assert(loaded == 1);
    }
    {
        uint32_t rows[] = {7,2};
        loaded = load_rows_csv(db, "r", 2, rows, 1);
        assert(loaded == 1);
    }
    {
        uint32_t rows[] = {10,2, 11,2, 12,2, 13,2, 14,2};
        loaded = load_rows_csv(db, "e", 2, rows, 5);
        assert(loaded == 5);
    }

    assert(dl_load_rules(db,
        "q1(X,Z):-p(X,Y),e(Z,Y).\n"
        "q2(X,W):-r(X,Y),e(W,Y).\n") == 0);
    assert(dl_compile(db) == 0);

    if (dl_db_perm_count(db) != 1) {
        printf("  perm_count=%d, expected 1\n", dl_db_perm_count(db));
        FAIL("T3 expected exactly one shared perm");
        teardown_db(db, "t3");
        return;
    }

    memset(&result, 0, sizeof(result));
    dl_query(db, "q1", tset_cb, &result);
    if (result.count != 5) {
        printf("  q1 got %ld tuples, expected 5\n", result.count);
        FAIL("T3 q1 result mismatch");
    }
    tset_free(&result);

    memset(&result, 0, sizeof(result));
    dl_query(db, "q2", tset_cb, &result);
    if (result.count != 5) {
        printf("  q2 got %ld tuples, expected 5\n", result.count);
        FAIL("T3 q2 result mismatch");
    } else {
        PASS();
    }
    tset_free(&result);
    teardown_db(db, "t3");
}

/* ─── T4: oracle — select=1 vs select=0 identical results ─────────────── */

static void t4a_facts(dl_db *db)
{
    uint32_t edge[] = {1,2, 2,3, 3,4, 4,5};
    uint32_t label[] = {100,2, 100,4, 200,3, 300,5};
    assert(load_rows_csv(db, "edge", 2, edge, 4) == 4);
    assert(load_rows_csv(db, "label", 2, label, 4) == 4);
}

/* T4a: TC chain + non-leading EDB join */
static void test_t4a_oracle_tc(void)
{
    TEST("T4a: oracle TC chain");
    {
        const char *batches[] = {
            "tc(X,Y):-edge(X,Y).\n"
            "tc(X,Z):-edge(X,Y),tc(Y,Z).\n"
            "q(N):-tc(A,B),label(N,B).\n"
        };
        if (oracle_compare("t4a", t4a_facts, batches, 1, "q"))
            PASS();
        else
            FAIL("T4a oracle mismatch");
    }
}

static void t4b_facts(dl_db *db)
{
    uint32_t f1[] = {1,10,100, 2,20,200, 3,30,300};
    uint32_t f2[] = {1000,10,100, 2000,20,200, 3000,30,300, 4000,40,400};
    assert(load_rows_csv(db, "f1", 3, f1, 3) == 3);
    assert(load_rows_csv(db, "f2", 3, f2, 4) == 4);
}

/* T4b: non-leading 3-column join */
static void test_t4b_oracle_3col(void)
{
    TEST("T4b: oracle non-leading 3-col join");
    {
        const char *batches[] = {
            "q(X,W):-f1(X,Y,Z),f2(W,Y,Z).\n"
        };
        if (oracle_compare("t4b", t4b_facts, batches, 1, "q"))
            PASS();
        else
            FAIL("T4b oracle mismatch");
    }
}

static void t4c_facts(dl_db *db)
{
    uint32_t f1[] = {1,10,100, 2,20,200};
    uint32_t f2[] = {1000,10, 2000,20, 3000,10, 4000,30};
    assert(load_rows_csv(db, "f1", 3, f1, 2) == 2);
    assert(load_rows_csv(db, "f2", 2, f2, 4) == 4);
}

/* T4c: non-recursive IDB non-leading join (two compile batches) */
static void test_t4c_oracle_idb(void)
{
    TEST("T4c: oracle non-recursive IDB non-leading join");
    {
        const char *batches[] = {
            "r1(X,Y,Z):-f1(X,Y,Z).\n"
            "r2(W,V):-f2(W,V).\n",
            "q(X):-r1(X,Y,Z),r2(W,Y).\n"
        };
        if (oracle_compare("t4c", t4c_facts, batches, 2, "q"))
            PASS();
        else
            FAIL("T4c oracle mismatch");
    }
}

static void t4d_facts(dl_db *db)
{
    uint32_t a[] = {1,10, 2,20, 3,30};
    uint32_t b[] = {5,10, 6,20, 7,30, 8,40};
    uint32_t c[] = {2, 9};
    assert(load_rows_csv(db, "a", 2, a, 3) == 3);
    assert(load_rows_csv(db, "b", 2, b, 4) == 4);
    assert(load_rows_csv(db, "c", 1, c, 2) == 2);
}

/* T4d: negation + non-leading join */
static void test_t4d_oracle_negation(void)
{
    TEST("T4d: oracle negation non-leading join");
    {
        const char *batches[] = {
            "out(X):-a(X,Y),b(W,Y),!c(X).\n"
        };
        if (oracle_compare("t4d", t4d_facts, batches, 1, "out"))
            PASS();
        else
            FAIL("T4d oracle mismatch");
    }
}

/* ─── T5: perm-cap exhaustion degrades gracefully ─────────────────────── */

static void test_t5_cap_exhaustion(void)
{
    dl_db *db;
    int loaded;

    TEST("T5: perm-cap exhaustion degrades to hash join, no hard error");

    setup_db(&db, "t5");

    /* 8 arity-8 EDB relations (rel_ids 0..7 in declaration order). */
    {
        int i;
        for (i = 0; i < 8; i++) {
            char rn[16];
            uint32_t rows[8 * 8];  /* 8 rows of arity 8 */
            int c, r;
            snprintf(rn, sizeof(rn), "r%d", i);
            for (r = 0; r < 8; r++)
                for (c = 0; c < 8; c++)
                    rows[r * 8 + c] = (uint32_t)r;
            loaded = load_rows_csv(db, rn, 8, rows, 8);
            assert(loaded == 8);
        }
    }
    /* p (arity 2, driver) and r8 (arity 2, the overflow rel). */
    {
        uint32_t p[] = {1,5};
        uint32_t r8[] = {100,5, 200,5, 300,5, 400,5};
        assert(load_rows_csv(db, "p", 2, p, 1) == 1);
        assert(load_rows_csv(db, "r8", 2, r8, 4) == 4);
    }

    /* Pre-fill the perm table to MAX_PERMS (64): 8 distinct perms on each of
     * the 8 arity-8 rels.  Directly exercising dl_db_declare_perm to fill the
     * global cap so the compiler must fall back to hash join below. */
    {
        static const uint8_t perms[8][8] = {
            {0,1,2,3,4,5,6,7},
            {1,0,2,3,4,5,6,7},
            {2,0,1,3,4,5,6,7},
            {3,0,1,2,4,5,6,7},
            {4,0,1,2,3,5,6,7},
            {5,0,1,2,3,4,6,7},
            {6,0,1,2,3,4,5,7},
            {7,0,1,2,3,4,5,6},
        };
        int i, k;
        for (i = 0; i < 8; i++)
            for (k = 0; k < 8; k++)
                assert(dl_db_declare_perm(db, i, 8, perms[k]) >= 0);
        assert(dl_db_perm_count(db) == 64);
    }

    /* Compile a non-leading join on r8 that would need a 65th perm.  With the
     * cap full, the compiler must fall back to OP_HASH_JOIN — NOT hard-fail. */
    assert(dl_load_rules(db, "q(X):-p(X,Y),r8(Z,Y).\n") == 0);
    assert(dl_compile(db) == 0);
    assert(dl_db_perm_count(db) == 64);

    /* Spot-check the result through the hash-join fallback. */
    {
        tuple_set result, exp;
        long n;
        memset(&result, 0, sizeof(result));
        n = dl_query(db, "q", tset_cb, &result);
        assert(n >= 0);
        {
            uint32_t expected[] = {1};  /* p(1,5) joins r8(*,5) -> q(1) */
            tset_from_u32(&exp, expected, 1, 1);
        }
        if (tset_eq(&result, &exp)) {
            PASS();
        } else {
            printf("  q got %ld tuples, expected 1\n", result.count);
            FAIL("T5 spot-check mismatch");
        }
        tset_free(&result);
        tset_free(&exp);
    }
    teardown_db(db, "t5");
}

/* ─── T6: recursive non-leading join (carve-out) ──────────────────────── */

static void test_t6_recursive_nonleading(void)
{
    dl_db *dbA, *dbB;
    tuple_set rA, rB, exp;
    int loaded;

    TEST("T6: recursive non-leading join works and matches leading equivalent");

    g_perm_select = 1;

    setup_db(&dbA, "t6a");
    setup_db(&dbB, "t6b");

    /* Same edge facts into both */
    {
        uint32_t rows[] = {1,2, 2,3, 3,4};
        loaded = load_rows_csv(dbA, "edge", 2, rows, 3);
        assert(loaded == 3);
        loaded = load_rows_csv(dbB, "edge", 2, rows, 3);
        assert(loaded == 3);
    }

    /* Program A: recursive atom reach in NON-leading join position.
     * reach(W,Y) joins on Y (col1) -> recursive carve-out -> OP_LOOKUP_PERM. */
    assert(dl_load_rules(dbA,
        "reach(X,Y):-edge(X,Y).\n"
        "reach(X,Y):-edge(X,Z),reach(Z,Y).\n"
        "r(X):-edge(X,Y),reach(W,Y).\n") == 0);
    assert(dl_compile(dbA) == 0);

    /* Program B: leading-join equivalent (Y is col0 of reachY). */
    assert(dl_load_rules(dbB,
        "reach(X,Y):-edge(X,Y).\n"
        "reach(X,Y):-edge(X,Z),reach(Z,Y).\n"
        "reachY(Y):-reach(X,Y).\n"
        "r(X):-edge(X,Y),reachY(Y).\n") == 0);
    assert(dl_compile(dbB) == 0);

    memset(&rA, 0, sizeof(rA));
    memset(&rB, 0, sizeof(rB));
    dl_query(dbA, "r", tset_cb, &rA);
    dl_query(dbB, "r", tset_cb, &rB);

    /* r = X where edge(X,Y) and Y reachable from some W.  With edges
     * 1->2->3->4, every of 1,2,3 has an outgoing edge -> {1,2,3}. */
    uint32_t expected[] = {1,2,3};
    tset_from_u32(&exp, expected, 3, 1);

    if (!tset_eq(&rA, &rB)) {
        printf("  non-leading got %ld, leading got %ld\n", rA.count, rB.count);
        FAIL("T6 leading vs non-leading mismatch");
    } else if (!tset_eq(&rA, &exp)) {
        printf("  non-leading got %ld tuples, expected 3\n", rA.count);
        FAIL("T6 result mismatch");
    } else {
        PASS();
    }

    tset_free(&rA);
    tset_free(&rB);
    tset_free(&exp);
    teardown_db(dbA, "t6a");
    teardown_db(dbB, "t6b");
    g_perm_select = 1;
}

/* ─── Main ────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("M14 Perm-Index Selection Tests\n");
    printf("==============================\n\n");

    test_t1_small_hash_join();
    test_t2_large_perm();
    test_t3_reuse_dedup();
    test_t4a_oracle_tc();
    test_t4b_oracle_3col();
    test_t4c_oracle_idb();
    test_t4d_oracle_negation();
    test_t5_cap_exhaustion();
    test_t6_recursive_nonleading();

    printf("\n%d tests run, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
