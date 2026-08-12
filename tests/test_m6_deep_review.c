/*
 * test_m6_deep_review.c — Deep adversarial review of M6 perm↔binding mapping
 *
 * Focused tests:
 *   D1: arity-3, join on col 2 only (col 1 also shared) — perm correctness
 *   D2: arity-4, join on cols 1 and 2 (multiple non-leading) 
 *   D3: same relation used with TWO different perms in different rules
 *   D4: both leading AND non-leading shared cols (mixed pattern)
 *   D5: project from non-leading join verifies slot mapping
 *   D6: multi-atom join chain with mixed leading/non-leading
 *   D7: negated atom with ALL vars bound (uses rel_exact, not perm)
 *   D8: arity-2 non-leading join at mid-key (col 1 of 2)
 *   D9: randomized arity-3/4 deep perm mapping (30 iterations)
 *   D10: recursive IDB with deeply non-leading join
 */

#include "dl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static int tests_run = 0;
static int tests_failed = 0;

#define TEST(name) do { tests_run++; printf("  %s ... ", name); fflush(stdout); } while(0)
#define PASS() do { printf("OK\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); tests_failed++; fflush(stdout); } while(0)

/* ─── Tuple helpers ────────────────────────────────────────────────────── */
typedef struct {
    uint32_t *data; long count, cap; uint8_t arity;
} tuple_set;

static int tset_cb(const uint32_t *cols, uint8_t arity, void *user) {
    tuple_set *ts = (tuple_set *)user;
    if (ts->arity == 0) ts->arity = arity;
    if (ts->count >= ts->cap) {
        long nc = ts->cap ? ts->cap * 2 : 256;
        uint32_t *nd = realloc(ts->data, (size_t)nc * (size_t)ts->arity * sizeof(uint32_t));
        if (!nd) return 1;
        ts->data = nd; ts->cap = nc;
    }
    memcpy(ts->data + (size_t)ts->count * ts->arity, cols, (size_t)ts->arity * sizeof(uint32_t));
    ts->count++;
    return 0;
}
static void tset_free(tuple_set *ts) { free(ts->data); memset(ts, 0, sizeof(*ts)); }
static int tset_eq(const tuple_set *a, const tuple_set *b) {
    long i, j;
    if (a->count == 0 && b->count == 0) return 1;
    if (a->count != b->count || a->arity != b->arity) return 0;
    for (i = 0; i < a->count; i++) {
        int found = 0;
        const uint32_t *arow = a->data + i * a->arity;
        for (j = 0; j < b->count; j++) {
            if (memcmp(arow, b->data + j * b->arity, a->arity * sizeof(uint32_t)) == 0)
                { found = 1; break; }
        }
        if (!found) return 0;
    }
    return 1;
}
static void tset_print(const char *label, const tuple_set *ts) {
    long i; int c;
    printf("%s (%ld tuples, arity %d):\n", label, ts->count, ts->arity);
    for (i = 0; i < ts->count; i++) {
        printf("  ");
        for (c = 0; c < ts->arity; c++)
            printf("%u ", ts->data[i * ts->arity + c]);
        printf("\n");
    }
}

/* ─── DB helpers ──────────────────────────────────────────────────────── */
static void setup_db(dl_db **db_out, const char *suffix) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf build-tmp/deep_%s", suffix); system(cmd);
    snprintf(cmd, sizeof(cmd), "build-tmp/deep_%s", suffix);
    *db_out = dl_open(cmd); assert(*db_out);
}
static void teardown_db(dl_db *db, const char *suffix) {
    char cmd[512]; dl_close(db);
    snprintf(cmd, sizeof(cmd), "rm -rf build-tmp/deep_%s", suffix); system(cmd);
}
static int load_rows_csv(dl_db *db, const char *rel_name, uint8_t arity,
                         const uint32_t *cols, int nrows) {
    char csv_path[256]; int i, c; FILE *f;
    assert(dl_declare_relation(db, rel_name, arity) == 0);
    snprintf(csv_path, sizeof(csv_path), "build-tmp/csv_%s_%s.csv", rel_name, rel_name);
    f = fopen(csv_path, "w"); assert(f);
    for (i = 0; i < nrows; i++) {
        for (c = 0; c < arity; c++) {
            if (c > 0) fputc(',', f);
            fprintf(f, "%u", cols[i * arity + c]);
        }
        fputc('\n', f);
    }
    fclose(f);
    return dl_load_facts(db, rel_name, csv_path);
}

/* ─── D1: arity-3 join on col 2 only (non-leading) ────────────────────── */
static void test_d1(void) {
    dl_db *db; tuple_set result;
    TEST("D1: arity-3 join on col 2 (non-leading)");
    setup_db(&db, "d1");
    { uint32_t r[] = {1,10,100, 2,20,200, 3,30,300, 4,40,100};
      assert(load_rows_csv(db, "r", 3, r, 4) == 4); }
    { uint32_t r[] = {11,10,100, 22,20,200, 33,99,100};
      assert(load_rows_csv(db, "s", 3, r, 3) == 3); }
    if (dl_load_rules(db, "q(A,F):-r(A,B,C),s(D,E,F),C=F.\n")) { FAIL("compile"); teardown_db(db,"d1"); return; }
    assert(dl_compile(db) == 0);
    memset(&result, 0, sizeof(result));
    dl_query(db, "q", tset_cb, &result);
    uint32_t exp[] = {1,100, 2,200, 4,100};
    tuple_set es; memset(&es, 0, sizeof(es));
    long ei; for (ei=0;ei<3;ei++) tset_cb(&exp[ei*2], 2, &es);
    if (!tset_eq(&result, &es)) { tset_print("got",&result); FAIL("D1"); }
    else PASS();
    tset_free(&result); tset_free(&es); teardown_db(db,"d1");
}

/* ─── D2: arity-4 join on cols 1+2 (multiple non-leading) ─────────────── */
static void test_d2(void) {
    dl_db *db; tuple_set result;
    TEST("D2: arity-4 join on cols 1+2 (multi non-leading)");
    setup_db(&db, "d2");
    { uint32_t r[] = {1,10,100,1000, 2,20,200,2000};
      assert(load_rows_csv(db, "r", 4, r, 2) == 2); }
    { uint32_t r[] = {11,10,100,1111, 22,20,200,2222, 33,10,100,3333};
      assert(load_rows_csv(db, "s", 4, r, 3) == 3); }
    if (dl_load_rules(db, "q(A,D):-r(A,B,C,D),s(E,B,C,F).\n")) { FAIL("compile"); teardown_db(db,"d2"); return; }
    assert(dl_compile(db) == 0);
    memset(&result, 0, sizeof(result));
    dl_query(db, "q", tset_cb, &result);
    uint32_t exp[] = {1,1000, 2,2000};
    tuple_set es; memset(&es, 0, sizeof(es));
    tset_cb(&exp[0], 2, &es); tset_cb(&exp[2], 2, &es);
    if (!tset_eq(&result, &es)) { tset_print("got",&result); FAIL("D2"); }
    else PASS();
    tset_free(&result); tset_free(&es); teardown_db(db,"d2");
}

/* ─── D3: same rel with 2 different perms ─────────────────────────────── */
static void test_d3(void) {
    dl_db *db; tuple_set result;
    TEST("D3: same relation with two different perms");
    setup_db(&db, "d3");
    { uint32_t r[] = {1,10,100, 2,20,200, 3,10,300, 4,40,100};
      assert(load_rows_csv(db, "r", 3, r, 4) == 4); }
    { uint32_t r[] = {10,1000, 20,2000, 40,4000, 100,9999};
      assert(load_rows_csv(db, "s", 2, r, 4) == 4); }
    if (dl_load_rules(db, "q1(X):-r(X,Y,Z),s(Y,W).\nq2(X):-r(X,Y,Z),s(Z,W).\n")) { FAIL("compile"); teardown_db(db,"d3"); return; }
    assert(dl_compile(db) == 0);
    memset(&result, 0, sizeof(result));
    dl_query(db, "q1", tset_cb, &result);
    if (result.count != 4) { printf(" q1=%ld", result.count); FAIL("D3:q1"); }
    else {
        tset_free(&result); memset(&result, 0, sizeof(result));
        dl_query(db, "q2", tset_cb, &result);
        uint32_t exp[] = {1, 4};
        tuple_set es; memset(&es, 0, sizeof(es));
        tset_cb(&exp[0], 1, &es); tset_cb(&exp[1], 1, &es);
        if (!tset_eq(&result, &es)) { printf(" q2=%ld", result.count); FAIL("D3:q2"); }
        else PASS();
        tset_free(&es);
    }
    tset_free(&result); teardown_db(db,"d3");
}

/* ─── D4: mixed leading + non-leading ─────────────────────────────────── */
static void test_d4(void) {
    dl_db *db; tuple_set result;
    TEST("D4: mixed leading/non-leading shared cols");
    setup_db(&db, "d4");
    { uint32_t r[] = {1,10,100,1000, 2,20,200,2000, 3,30,100,3000, 4,40,777,4000};
      assert(load_rows_csv(db, "r", 4, r, 4) == 4); }
    { uint32_t r[] = {99,10,100, 99,20,200, 99,100,777, 99,777,777};
      assert(load_rows_csv(db, "s", 3, r, 4) == 4); }
    if (dl_load_rules(db, "q(A,B,C):-r(A,B,C,D),s(E,B,G),C=G.\n")) { FAIL("compile"); teardown_db(db,"d4"); return; }
    assert(dl_compile(db) == 0);
    memset(&result, 0, sizeof(result));
    dl_query(db, "q", tset_cb, &result);
    uint32_t exp[] = {1,10,100, 2,20,200};
    tuple_set es; memset(&es, 0, sizeof(es));
    tset_cb(&exp[0], 3, &es); tset_cb(&exp[3], 3, &es);
    if (!tset_eq(&result, &es)) { tset_print("got",&result); FAIL("D4"); }
    else PASS();
    tset_free(&result); tset_free(&es); teardown_db(db,"d4");
}

/* ─── D5: project subset from non-leading join ────────────────────────── */
static void test_d5(void) {
    dl_db *db; tuple_set result;
    TEST("D5: project from non-leading join");
    setup_db(&db, "d5");
    { uint32_t r[] = {1,10,100,1000, 2,20,200,2000, 3,30,300,3000};
      assert(load_rows_csv(db, "r", 4, r, 3) == 3); }
    { uint32_t r[] = {99,10,999, 77,20,888, 55,200,777};
      assert(load_rows_csv(db, "s", 3, r, 3) == 3); }
    if (dl_load_rules(db, "q(C,A):-r(A,B,C,D),s(E,B,G).\n")) { FAIL("compile"); teardown_db(db,"d5"); return; }
    assert(dl_compile(db) == 0);
    memset(&result, 0, sizeof(result));
    dl_query(db, "q", tset_cb, &result);
    uint32_t exp[] = {100,1, 200,2};
    tuple_set es; memset(&es, 0, sizeof(es));
    tset_cb(&exp[0], 2, &es); tset_cb(&exp[2], 2, &es);
    if (!tset_eq(&result, &es)) { tset_print("got",&result); FAIL("D5"); }
    else PASS();
    tset_free(&result); tset_free(&es); teardown_db(db,"d5");
}

/* ─── D6: 3-atom chain mixed leading/non-leading ──────────────────────── */
static void test_d6(void) {
    dl_db *db; tuple_set result;
    TEST("D6: 3-atom chain mixed leading/non-leading");
    setup_db(&db, "d6");
    { uint32_t r[] = {1,10, 2,20, 3,30};
      assert(load_rows_csv(db, "a", 2, r, 3) == 3); }
    { uint32_t r[] = {100,10,1000, 200,20,2000, 300,30,3000};
      assert(load_rows_csv(db, "b", 3, r, 3) == 3); }
    { uint32_t r[] = {100, 200};
      assert(load_rows_csv(db, "c", 1, r, 2) == 2); }
    if (dl_load_rules(db, "q(X,W):-a(X,Y),b(Z,Y,W),c(Z).\n")) { FAIL("compile"); teardown_db(db,"d6"); return; }
    assert(dl_compile(db) == 0);
    memset(&result, 0, sizeof(result));
    dl_query(db, "q", tset_cb, &result);
    uint32_t exp[] = {1,1000, 2,2000};
    tuple_set es; memset(&es, 0, sizeof(es));
    tset_cb(&exp[0], 2, &es); tset_cb(&exp[2], 2, &es);
    if (!tset_eq(&result, &es)) { tset_print("got",&result); FAIL("D6"); }
    else PASS();
    tset_free(&result); tset_free(&es); teardown_db(db,"d6");
}

/* ─── D7: negated atom with all vars bound ────────────────────────────── */
/*
 * a(x,y)={(1,10),(2,20),(3,30)}  c(z)={100,200}  b(z,y)={(100,10),(200,30)}
 * q1(X):-a(X,Y),c(Z),!b(Z,Y).  negated uses rel_exact — all cols bound.
 * Expected: (1,10)+c(100):!b(100,10)→fail; +c(200):!b(200,10)→OK→q(1)
 * (2,20)+c(100):OK→q(2); +c(200):OK→q(2); (3,30)+c(100):OK→q(3); +c(200):fail
 * Dedup: {1,2,3}
 */
static void test_d7(void) {
    dl_db *db; tuple_set result;
    TEST("D7: negated atom with all vars bound");
    setup_db(&db, "d7");
    { uint32_t r[] = {1,10, 2,20, 3,30};
      assert(load_rows_csv(db, "a", 2, r, 3) == 3); }
    { uint32_t r[] = {100, 200};
      assert(load_rows_csv(db, "c", 1, r, 2) == 2); }
    { uint32_t r[] = {100,10, 200,30};
      assert(load_rows_csv(db, "b", 2, r, 2) == 2); }
    if (dl_load_rules(db, "q1(X):-a(X,Y),c(Z),!b(Z,Y).\n")) { FAIL("compile"); teardown_db(db,"d7"); return; }
    assert(dl_compile(db) == 0);
    memset(&result, 0, sizeof(result));
    dl_query(db, "q1", tset_cb, &result);
    uint32_t exp[] = {1, 2, 3};
    tuple_set es; memset(&es, 0, sizeof(es));
    tset_cb(&exp[0], 1, &es); tset_cb(&exp[1], 1, &es); tset_cb(&exp[2], 1, &es);
    if (!tset_eq(&result, &es)) { tset_print("got",&result); FAIL("D7"); }
    else PASS();
    tset_free(&result); tset_free(&es); teardown_db(db,"d7");
}

/* ─── D8: non-leading join at col 1 of 2 ──────────────────────────────── */
/*
 * r(a,b,c)={(1,10,100),(2,20,200),(3,10,300),(4,40,100)}
 * s(d,e)={(1000,10),(2000,20),(3000,10)}
 * q(A,B,C,D):-r(A,B,C),s(D,B).  s shares B at col1 (non-leading).
 * perm={1,0}. r(1,10,100)+s(1000/3000,10)→2; r(2,20,200)+s(2000,20)→1;
 * r(3,10,300)+s(1000/3000,10)→2; r(4,40,100)→0. Total: 5.
 */
static void test_d8(void) {
    dl_db *db; tuple_set result;
    TEST("D8: non-leading join at mid-key col 1 of 2");
    setup_db(&db, "d8");
    { uint32_t r[] = {1,10,100, 2,20,200, 3,10,300, 4,40,100};
      assert(load_rows_csv(db, "r", 3, r, 4) == 4); }
    { uint32_t r[] = {1000,10, 2000,20, 3000,10};
      assert(load_rows_csv(db, "s", 2, r, 3) == 3); }
    if (dl_load_rules(db, "q(A,B,C,D):-r(A,B,C),s(D,B).\n")) { FAIL("compile"); teardown_db(db,"d8"); return; }
    assert(dl_compile(db) == 0);
    memset(&result, 0, sizeof(result));
    dl_query(db, "q", tset_cb, &result);
    if (result.count == 5) PASS();
    else { printf(" got %ld exp 5\n", result.count); tset_print("got",&result); FAIL("D8"); }
    tset_free(&result); teardown_db(db,"d8");
}

/* ─── D9: randomized 30 iterations ────────────────────────────────────── */
static void test_d9(void) {
    TEST("D9: randomized arity-3/4 (30 iter)");
    int ok = 1, iter;
    for (iter = 0; iter < 30; iter++) {
        dl_db *db1, *db2; char s1[32], s2[32];
        snprintf(s1,sizeof(s1),"d9a_%d",iter); snprintf(s2,sizeof(s2),"d9b_%d",iter);
        setup_db(&db1, s1); setup_db(&db2, s2);
        int nr = 15 + iter % 10, ri;
        uint32_t *rr = malloc((size_t)nr * 3 * sizeof(uint32_t));
        for (ri=0; ri<nr; ri++) {
            rr[ri*3+0] = (uint32_t)((ri*3+iter)%10+1);
            rr[ri*3+1] = (uint32_t)((ri*7+iter*2)%8+1);
            rr[ri*3+2] = (uint32_t)((ri*5+iter*3)%12+1);
        }
        load_rows_csv(db1, "r", 3, rr, nr); load_rows_csv(db2, "r", 3, rr, nr);
        int ns = 12 + iter % 8, si;
        uint32_t *sr = malloc((size_t)ns * 4 * sizeof(uint32_t));
        for (si=0; si<ns; si++) {
            sr[si*4+0] = (uint32_t)((si*11+iter*5)%15+1);
            sr[si*4+1] = (uint32_t)((si*7+iter)%8+1);
            sr[si*4+2] = (uint32_t)((si*13+iter*7)%6+1);
            sr[si*4+3] = (uint32_t)((si*3+iter*11)%9+1);
        }
        load_rows_csv(db1, "s", 4, sr, ns); load_rows_csv(db2, "s", 4, sr, ns);
        free(rr); free(sr);
        int rc1 = dl_load_rules(db1, "q1(C,G):-r(A,B,C),s(D,E,F,G),B=E.\n");
        int rc2 = dl_load_rules(db2, "q2(C,G):-s(D,E,F,G),r(A,B,C),B=E.\n");
        if (rc1||rc2) { ok=0; printf(" iter %d: compile err\n",iter); teardown_db(db1,s1); teardown_db(db2,s2); break; }
        if (dl_compile(db1)||dl_compile(db2)) { ok=0; printf(" iter %d: compile err\n",iter); teardown_db(db1,s1); teardown_db(db2,s2); break; }
        tuple_set r1, r2;
        memset(&r1,0,sizeof(r1)); memset(&r2,0,sizeof(r2));
        dl_query(db1, "q1", tset_cb, &r1); dl_query(db2, "q2", tset_cb, &r2);
        if (!tset_eq(&r1, &r2)) {
            ok=0; printf(" iter %d: MISMATCH %ld vs %ld\n",iter,r1.count,r2.count);
            tset_print("nl",&r1); tset_print("lead",&r2);
            tset_free(&r1); tset_free(&r2);
            teardown_db(db1,s1); teardown_db(db2,s2); break;
        }
        tset_free(&r1); tset_free(&r2);
        teardown_db(db1,s1); teardown_db(db2,s2);
    }
    if (ok) PASS(); else FAIL("randomized");
}

/* ─── D10: recursive IDB with non-leading join ────────────────────────── */
/*
 * reach(X):-edge(X,Y).  reach(Z):-reach(Y),edge(Z,Y).
 * edge: (1,2),(2,3),(3,4),(4,5),(1,5). Expected reach = {1,2,3,4}
 */
static void test_d10(void) {
    dl_db *db; tuple_set result;
    TEST("D10: recursive IDB with non-leading join");
    setup_db(&db, "d10");
    { uint32_t r[] = {1,2, 2,3, 3,4, 4,5, 1,5};
      assert(load_rows_csv(db, "edge", 2, r, 5) == 5); }
    if (dl_load_rules(db, "reach(X):-edge(X,Y).\nreach(Z):-reach(Y),edge(Z,Y).\n")) { FAIL("compile"); teardown_db(db,"d10"); return; }
    assert(dl_compile(db) == 0);
    memset(&result, 0, sizeof(result));
    dl_query(db, "reach", tset_cb, &result);
    if (result.count == 4) PASS();
    else { printf(" got %ld exp 4\n", result.count); FAIL("D10"); }
    tset_free(&result); teardown_db(db,"d10");
}

int main(void) {
    printf("M6 Deep Adversarial Review\n==========================\n\n");
    test_d1(); test_d2(); test_d3(); test_d4(); test_d5();
    test_d6(); test_d7(); test_d8(); test_d9(); test_d10();
    printf("\n%d tests run, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
