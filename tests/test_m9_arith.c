/*
 * test_m9_arith.c — M9 arithmetic + comparison builtin tests
 *
 * Coverage matrix (mirrors the M9 plan):
 *   T1  precedence + parens (end-to-end value checks: A+B*C vs (A+B)*C)
 *   T2  each arithmetic op (X=Y+Z, Y-Z, Y*Z, Y/Z, Y%Z) — exact tuple set
 *   T3  each comparison op (X<Y, <=, >, >=, !=) — exact tuple set
 *   T4  constant operand (X=Y+1) and nested (X=(A+B)*C)
 *   T5  filter path (X pre-bound in a positive atom) + comparison after
 *       arithmetic
 *   T6  parser rejects (unclosed paren, TOK_IDENT in ordering comparison,
 *       TOK_IDENT in arithmetic)
 *   T7  compile rejects (negated builtin, div/mod by literal 0, ungrounded
 *       comparison, ungrounded arithmetic operand, reversed dependency)
 *   T8  edge cases (u32 wrap 0-1, div-by-zero no-crash, arithmetic in a
 *       recursive rule, `X != symbol` via interner)
 *   T9  arithmetic under magic (dl_query_magic_adorn) == full-materialize
 *       filter
 *   T10 property — 200 seeded random cases vs a C reference loop
 *
 * Regression: full `make test` (M0-M8) must stay green.
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

/* ─── Tuple set (local, mirrors test_m8_magic.c) ──────────────────────── */

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

/* build a tuple_set from a flat row array (copies) */
static void tset_from_rows(tuple_set *ts, const uint32_t *rows,
                           long nrows, uint8_t arity)
{
    memset(ts, 0, sizeof(*ts));
    ts->arity = arity;
    ts->count = nrows;
    if (nrows == 0) return;
    ts->data = malloc((size_t)nrows * (size_t)arity * sizeof(uint32_t));
    assert(ts->data);
    memcpy(ts->data, rows, (size_t)nrows * (size_t)arity * sizeof(uint32_t));
}

/* append a unique row (linear scan; fine for small reference sets) */
static void tset_push_unique(tuple_set *ts, const uint32_t *row)
{
    long i;
    for (i = 0; i < ts->count; i++)
        if (memcmp(ts->data + (size_t)i * (size_t)ts->arity, row,
                   (size_t)ts->arity * sizeof(uint32_t)) == 0)
            return;
    if (ts->count >= ts->cap) {
        long nc = ts->cap ? ts->cap * 2 : 256;
        uint32_t *nd = realloc(ts->data,
            (size_t)nc * (size_t)ts->arity * sizeof(uint32_t));
        assert(nd);
        ts->data = nd;
        ts->cap = nc;
    }
    memcpy(ts->data + (size_t)ts->count * (size_t)ts->arity, row,
           (size_t)ts->arity * sizeof(uint32_t));
    ts->count++;
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
    snprintf(path, sizeof(path), "build-tmp/m9db_%s", suffix);
    rm_dir(path);
    *db_out = dl_open(path);
    assert(*db_out);
}

static void teardown_db(dl_db *db, const char *suffix)
{
    char path[256];
    dl_close(db);
    snprintf(path, sizeof(path), "build-tmp/m9db_%s", suffix);
    rm_dir(path);
}

static int load_rows(dl_db *db, const char *rel_name, uint8_t arity,
                     const uint32_t *cols, int nrows, const char *suffix)
{
    char csv_path[256];
    FILE *f;
    int i, c;

    assert(dl_declare_relation(db, rel_name, arity) == 0);

    snprintf(csv_path, sizeof(csv_path), "build-tmp/m9csv_%s_%s.csv",
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

/* Query `rel` and compare (byte-for-byte, sorted) against expected rows. */
static int check_query(dl_db *db, const char *rel,
                       const uint32_t *rows, long nrows, uint8_t arity)
{
    tuple_set got = {0}, exp;
    long n = dl_query(db, rel, tset_cb, &got);
    if (n < 0) { tset_free(&got); return 0; }
    tset_from_rows(&exp, rows, nrows, arity);
    if (n != nrows || !tset_sorted_eq(&got, &exp)) {
        tset_free(&got);
        tset_free(&exp);
        return 0;
    }
    tset_free(&got);
    tset_free(&exp);
    return 1;
}

/* ─── T1: precedence + parens ─────────────────────────────────────────── */

static void test_t1_precedence(void)
{
    dl_db *db;

    TEST("T1: precedence (A+B*C) and parens ((A+B)*C)");

    setup_db(&db, "t1");
    {
        uint32_t v[] = {10, 3, 4};
        load_rows(db, "pair", 3, v, 1, "t1");
    }
    /* A+B*C => 10 + (3*4) = 22 ; (A+B)*C => 13*4 = 52 */
    assert(dl_load_rules(db,
        "r1(X):-pair(A,B,C),X=A+B*C.\n"
        "r2(X):-pair(A,B,C),X=(A+B)*C.\n") == 0);
    assert(dl_compile(db) == 0);

    {
        uint32_t e1[] = {22};
        uint32_t e2[] = {52};
        if (!check_query(db, "r1", e1, 1, 1)) {
            FAIL("A+B*C precedence (expected 22)");
            teardown_db(db, "t1"); return;
        }
        if (!check_query(db, "r2", e2, 1, 1)) {
            FAIL("(A+B)*C parens (expected 52)");
            teardown_db(db, "t1"); return;
        }
    }
    PASS();
    teardown_db(db, "t1");
}

/* ─── T2: each arithmetic op ──────────────────────────────────────────── */

static void test_t2_all_arith_ops(void)
{
    dl_db *db;

    TEST("T2: + - * / % (exact tuple sets)");

    setup_db(&db, "t2");
    {
        /* pairs (X,Y) */
        uint32_t v[] = {10, 3, 10, 2, 7, 4, 9, 2, 6, 3, 20, 5};
        load_rows(db, "pair", 2, v, 6, "t2");
    }
    assert(dl_load_rules(db,
        "add(X,Y,S):-pair(X,Y),S=X+Y.\n"
        "sub(X,Y,S):-pair(X,Y),S=X-Y.\n"
        "mul(X,Y,S):-pair(X,Y),S=X*Y.\n"
        "div(X,Y,S):-pair(X,Y),S=X/Y.\n"
        "mod(X,Y,S):-pair(X,Y),S=X%Y.\n") == 0);
    assert(dl_compile(db) == 0);

    /* expected (X,Y,result) triples */
    {
        uint32_t e_add[] = {10,3,13, 10,2,12, 7,4,11, 9,2,11, 6,3,9, 20,5,25};
        uint32_t e_sub[] = {10,3,7, 10,2,8, 7,4,3, 9,2,7, 6,3,3, 20,5,15};
        uint32_t e_mul[] = {10,3,30, 10,2,20, 7,4,28, 9,2,18, 6,3,18, 20,5,100};
        uint32_t e_div[] = {10,3,3, 10,2,5, 7,4,1, 9,2,4, 6,3,2, 20,5,4};
        uint32_t e_mod[] = {10,3,1, 10,2,0, 7,4,3, 9,2,1, 6,3,0, 20,5,0};
        if (!check_query(db, "add", e_add, 6, 3)) {
            FAIL("X+Y mismatch"); teardown_db(db, "t2"); return;
        }
        if (!check_query(db, "sub", e_sub, 6, 3)) {
            FAIL("X-Y mismatch"); teardown_db(db, "t2"); return;
        }
        if (!check_query(db, "mul", e_mul, 6, 3)) {
            FAIL("X*Y mismatch"); teardown_db(db, "t2"); return;
        }
        if (!check_query(db, "div", e_div, 6, 3)) {
            FAIL("X/Y mismatch"); teardown_db(db, "t2"); return;
        }
        if (!check_query(db, "mod", e_mod, 6, 3)) {
            FAIL("X%Y mismatch"); teardown_db(db, "t2"); return;
        }
    }
    PASS();
    teardown_db(db, "t2");
}

/* ─── T3: each comparison op ──────────────────────────────────────────── */

static void test_t3_comparisons(void)
{
    dl_db *db;

    TEST("T3: < <= > >= != (exact tuple sets)");

    setup_db(&db, "t3");
    {
        uint32_t v[] = {3, 5, 5, 5, 7, 4, 1, 9};
        load_rows(db, "pair", 2, v, 4, "t3");
    }
    assert(dl_load_rules(db,
        "lt(X,Y):-pair(X,Y),X<Y.\n"
        "le(X,Y):-pair(X,Y),X<=Y.\n"
        "gt(X,Y):-pair(X,Y),X>Y.\n"
        "ge(X,Y):-pair(X,Y),X>=Y.\n"
        "ne(X,Y):-pair(X,Y),X!=Y.\n") == 0);
    assert(dl_compile(db) == 0);

    {
        /* (3,5): lt,le,ne ; (5,5): le,ge ; (7,4): gt,ge,ne ; (1,9): lt,le,ne */
        uint32_t e_lt[] = {3,5, 1,9};
        uint32_t e_le[] = {3,5, 5,5, 1,9};
        uint32_t e_gt[] = {7,4};
        uint32_t e_ge[] = {5,5, 7,4};
        uint32_t e_ne[] = {3,5, 7,4, 1,9};
        if (!check_query(db, "lt", e_lt, 2, 2)) { FAIL("< mismatch"); teardown_db(db,"t3"); return; }
        if (!check_query(db, "le", e_le, 3, 2)) { FAIL("<= mismatch"); teardown_db(db,"t3"); return; }
        if (!check_query(db, "gt", e_gt, 1, 2)) { FAIL("> mismatch"); teardown_db(db,"t3"); return; }
        if (!check_query(db, "ge", e_ge, 2, 2)) { FAIL(">= mismatch"); teardown_db(db,"t3"); return; }
        if (!check_query(db, "ne", e_ne, 3, 2)) { FAIL("!= mismatch"); teardown_db(db,"t3"); return; }
    }
    PASS();
    teardown_db(db, "t3");
}

/* ─── T4: constant operand + nested ───────────────────────────────────── */

static void test_t4_const_and_nested(void)
{
    dl_db *db;

    TEST("T4: X=Y+1 and X=(A+B)*C");

    setup_db(&db, "t4");
    {
        uint32_t v[] = {5, 2, 3};
        load_rows(db, "pair", 3, v, 1, "t4");
    }
    assert(dl_load_rules(db,
        "inc(X):-pair(A,B,C),X=A+1.\n"
        "nested(X):-pair(A,B,C),X=(A+B)*C.\n") == 0);
    assert(dl_compile(db) == 0);

    {
        uint32_t e_inc[] = {6};
        uint32_t e_nest[] = {21};   /* (5+2)*3 */
        if (!check_query(db, "inc", e_inc, 1, 1)) {
            FAIL("X=Y+1 mismatch"); teardown_db(db, "t4"); return;
        }
        if (!check_query(db, "nested", e_nest, 1, 1)) {
            FAIL("(A+B)*C mismatch"); teardown_db(db, "t4"); return;
        }
    }
    PASS();
    teardown_db(db, "t4");
}

/* ─── T5: filter path + comparison after arithmetic ───────────────────── */

static void test_t5_filter_path(void)
{
    dl_db *db;

    TEST("T5: X pre-bound (filter path) and comparison after arithmetic");

    setup_db(&db, "t5");
    {
        uint32_t v[] = {2,1, 3,1, 5,4, 5,3};
        load_rows(db, "pair", 2, v, 4, "t5");
    }
    /* X is bound by pair(X,Y); X=Y+1 then FILTERS (X == Y+1). */
    assert(dl_load_rules(db,
        "f(X):-pair(X,Y),X=Y+1.\n"
        "g(X):-pair(X,Y),S=Y+1,X>S.\n") == 0);
    assert(dl_compile(db) == 0);

    {
        uint32_t e_f[] = {2, 5};   /* (2,1):2==2 ✓; (3,1):3==2 ✗; (5,4):5==5 ✓; (5,3):5==4 ✗ */
        uint32_t e_g[] = {3, 5};   /* S=Y+1: (2,1)->2 X=2>2 ✗; (3,1)->2 X=3>2 ✓; (5,4)->5 X=5>5 ✗; (5,3)->4 X=5>4 ✓ */
        if (!check_query(db, "f", e_f, 2, 1)) {
            FAIL("filter path mismatch"); teardown_db(db, "t5"); return;
        }
        if (!check_query(db, "g", e_g, 2, 1)) {
            FAIL("comparison-after-arithmetic mismatch"); teardown_db(db, "t5"); return;
        }
    }
    PASS();
    teardown_db(db, "t5");
}

/* ─── T6: parser rejects ──────────────────────────────────────────────── */

static void test_t6_parser_rejects(void)
{
    dl_db *db;

    TEST("T6: parser rejects (unclosed paren, ident in < / arithmetic)");

    setup_db(&db, "t6");
    { uint32_t v[] = {1, 2}; load_rows(db, "pair", 2, v, 1, "t6"); }

    /* unclosed paren */
    if (dl_load_rules(db, "r(X):-pair(X,Y),Z=(A+B*C.\n") == 0) {
        FAIL("unclosed paren not rejected"); teardown_db(db, "t6"); return;
    }
    /* TOK_IDENT in ordering comparison */
    if (dl_load_rules(db, "r(X):-pair(X,Y),X<foo.\n") == 0) {
        FAIL("TOK_IDENT in '<' not rejected"); teardown_db(db, "t6"); return;
    }
    /* TOK_IDENT in arithmetic (symbol constant) */
    if (dl_load_rules(db, "r(X):-pair(X,Y),Z=X+foo.\n") == 0) {
        FAIL("TOK_IDENT in arithmetic not rejected"); teardown_db(db, "t6"); return;
    }
    PASS();
    teardown_db(db, "t6");
}

/* ─── T7: compile rejects ─────────────────────────────────────────────── */

static void test_t7_compile_rejects(void)
{
    dl_db *db;

    TEST("T7: compile rejects (negated builtin, div0, ungrounded, reversed)");

    /* negated builtin */
    {
        setup_db(&db, "t7a");
        { uint32_t v[] = {1, 2}; load_rows(db, "pair", 2, v, 1, "t7a"); }
        if (dl_load_rules(db, "r(X):-pair(X,Y),!X<Y.\n") == 0) {
            FAIL("negated builtin not rejected"); teardown_db(db, "t7a"); return;
        }
        teardown_db(db, "t7a");
    }
    /* div by literal 0 */
    {
        setup_db(&db, "t7b");
        { uint32_t v[] = {1, 2}; load_rows(db, "pair", 2, v, 1, "t7b"); }
        if (dl_load_rules(db, "r(X):-pair(X,Y),Z=X/0.\n") == 0) {
            FAIL("div by literal 0 not rejected"); teardown_db(db, "t7b"); return;
        }
        teardown_db(db, "t7b");
    }
    /* mod by literal 0 */
    {
        setup_db(&db, "t7c");
        { uint32_t v[] = {1, 2}; load_rows(db, "pair", 2, v, 1, "t7c"); }
        if (dl_load_rules(db, "r(X):-pair(X,Y),Z=X%0.\n") == 0) {
            FAIL("mod by literal 0 not rejected"); teardown_db(db, "t7c"); return;
        }
        teardown_db(db, "t7c");
    }
    /* ungrounded comparison (Y not bound by any positive relational atom) */
    {
        setup_db(&db, "t7d");
        { uint32_t v[] = {1}; load_rows(db, "val", 1, v, 1, "t7d"); }
        if (dl_load_rules(db, "r(X):-val(X),X<Y.\n") == 0) {
            FAIL("ungrounded comparison not rejected"); teardown_db(db, "t7d"); return;
        }
        teardown_db(db, "t7d");
    }
    /* ungrounded arithmetic operand */
    {
        setup_db(&db, "t7e");
        { uint32_t v[] = {1}; load_rows(db, "val", 1, v, 1, "t7e"); }
        if (dl_load_rules(db, "r(X):-val(X),Z=Y+1.\n") == 0) {
            FAIL("ungrounded arithmetic operand not rejected"); teardown_db(db, "t7e"); return;
        }
        teardown_db(db, "t7e");
    }
    /* reversed arithmetic dependency (left-to-right required) */
    {
        setup_db(&db, "t7f");
        { uint32_t v[] = {1, 2}; load_rows(db, "pair", 2, v, 1, "t7f"); }
        if (dl_load_rules(db, "r(X):-pair(X,Y),Z=W+1,W=Y+1.\n") == 0) {
            FAIL("reversed arithmetic dependency not rejected"); teardown_db(db, "t7f"); return;
        }
        teardown_db(db, "t7f");
    }
    PASS();
}

/* ─── T8: edge cases ──────────────────────────────────────────────────── */

static void test_t8_edge_cases(void)
{
    dl_db *db;

    TEST("T8: u32 wrap, div-by-zero, recursive arithmetic, != symbol");

    /* 8a: u32 wrap — 0 - 1 == 0xFFFFFFFF */
    {
        setup_db(&db, "t8a");
        { uint32_t v[] = {0}; load_rows(db, "val", 1, v, 1, "t8a"); }
        assert(dl_load_rules(db, "r(X):-val(Y),X=Y-1.\n") == 0);
        assert(dl_compile(db) == 0);
        {
            uint32_t e[] = {0xFFFFFFFFu};
            if (!check_query(db, "r", e, 1, 1)) {
                FAIL("u32 wrap 0-1 != 0xFFFFFFFF"); teardown_db(db, "t8a"); return;
            }
        }
        teardown_db(db, "t8a");
    }
    /* 8b: div-by-zero (variable divisor) — no crash, only Y!=0 rows */
    {
        setup_db(&db, "t8b");
        { uint32_t v[] = {10,0, 10,2, 20,5, 7,0}; load_rows(db, "pair", 2, v, 4, "t8b"); }
        assert(dl_load_rules(db, "r(X,Y,Z):-pair(X,Y),Z=X/Y.\n") == 0);
        assert(dl_compile(db) == 0);
        {
            uint32_t e[] = {10,2,5, 20,5,4};
            if (!check_query(db, "r", e, 2, 3)) {
                FAIL("div-by-zero (var) semantics"); teardown_db(db, "t8b"); return;
            }
        }
        teardown_db(db, "t8b");
    }
    /* 8c: arithmetic in a recursive rule — p = {0..9} */
    {
        setup_db(&db, "t8c");
        { uint32_t v[] = {0}; load_rows(db, "seed", 1, v, 1, "t8c"); }
        assert(dl_load_rules(db,
            "p(X):-seed(X).\n"
            "p(X):-p(Y),X=Y+1,X<10.\n") == 0);
        assert(dl_compile(db) == 0);
        {
            uint32_t e[] = {0,1,2,3,4,5,6,7,8,9};
            if (!check_query(db, "p", e, 10, 1)) {
                FAIL("recursive arithmetic fixpoint"); teardown_db(db, "t8c"); return;
            }
        }
        teardown_db(db, "t8c");
    }
    /* 8d: `X != symbol` interns the symbol and compares sym_ids (int values
     * chosen high so they never collide with the fresh sym_id). */
    {
        setup_db(&db, "t8d");
        { uint32_t v[] = {100, 200, 300}; load_rows(db, "val", 1, v, 3, "t8d"); }
        assert(dl_load_rules(db, "r(X):-val(X),X!=foo.\n") == 0);
        assert(dl_compile(db) == 0);
        {
            uint32_t e[] = {100, 200, 300};
            if (!check_query(db, "r", e, 3, 1)) {
                FAIL("!= symbol operand"); teardown_db(db, "t8d"); return;
            }
        }
        teardown_db(db, "t8d");
    }
    PASS();
}

/* ─── T9: arithmetic under magic ──────────────────────────────────────── */

static void test_t9_magic_arith(void)
{
    dl_db *db;

    TEST("T9: arithmetic under dl_query_magic_adorn == full-materialize filter");

    setup_db(&db, "t9");
    {
        uint32_t e[] = {1,2, 1,3, 2,4, 2,5, 3,10};
        load_rows(db, "edge", 2, e, 5, "t9");
    }
    /* next(X,Y) :- edge(X,Z), Y = Z + 1.  Goal next^bf (X bound). */
    assert(dl_load_rules(db, "next(X,Y):-edge(X,Z),Y=Z+1.\n") == 0);
    assert(dl_compile(db) == 0);

    {
        uint32_t vals[1] = {1};
        tuple_set full = {0}, ground = {0}, rm = {0};
        long nq, nm;

        nq = dl_query(db, "next", tset_cb, &full);
        assert(nq >= 0);
        /* ground truth: filter full materialization on X == 1 */
        ground.arity = 2;
        {
            long i;
            for (i = 0; i < full.count; i++) {
                const uint32_t *row = full.data + (size_t)i * 2;
                if (row[0] == vals[0]) {
                    tset_push_unique(&ground, row);
                }
            }
        }
        nm = dl_query_magic_adorn(db, "next", "bf", vals, 1, tset_cb, &rm);
        if (nm < 0) {
            FAIL("magic transform rejected arithmetic program");
            tset_free(&full); tset_free(&ground); tset_free(&rm);
            teardown_db(db, "t9"); return;
        }
        if ((long)ground.count != nm || !tset_sorted_eq(&ground, &rm)) {
            FAIL("magic arithmetic != full-materialize filter");
            tset_free(&full); tset_free(&ground); tset_free(&rm);
            teardown_db(db, "t9"); return;
        }
        /* next(1,*) = {Z+1 : edge(1,Z)} = {3,4} */
        if (nm != 2) {
            FAIL("expected 2 rows for next(1,*)");
            tset_free(&full); tset_free(&ground); tset_free(&rm);
            teardown_db(db, "t9"); return;
        }
        tset_free(&full); tset_free(&ground); tset_free(&rm);
    }
    PASS();
    teardown_db(db, "t9");
}

/* ─── T10: property test (backstop) ───────────────────────────────────── */

static uint32_t rng_state = 0x9E3779B9u;
static uint32_t rng_next(void)
{
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    rng_state = x;
    return x;
}
static uint32_t rng_rand(uint32_t n) { return n ? rng_next() % n : 0; }

static void test_t10_property(void)
{
    TEST("T10: property — 40 seeded random cases vs C reference");

    int iter;
    for (iter = 0; iter < 40; iter++) {
        dl_db *db;
        char suffix[32];
        int N = 8 + (int)(iter % 20);        /* 8..27 pairs */
        uint32_t *pairs;
        tuple_set ref = {0};
        int i;

        snprintf(suffix, sizeof(suffix), "prop_%d", iter);
        setup_db(&db, suffix);

        pairs = malloc((size_t)N * 2 * sizeof(uint32_t));
        for (i = 0; i < N; i++) {
            pairs[i*2]     = rng_rand(60);     /* A in [0,59] */
            pairs[i*2 + 1] = rng_rand(60) + 1; /* B in [1,60] (no zero) */
        }
        load_rows(db, "pair", 2, pairs, N, suffix);

        /* r(S,P) :- pair(A,B), S = A+B, P = A*B. */
        assert(dl_load_rules(db,
            "r(S,P):-pair(A,B),S=A+B,P=A*B.\n") == 0);
        assert(dl_compile(db) == 0);

        /* C reference: unique set of (A+B, A*B) */
        ref.arity = 2;
        for (i = 0; i < N; i++) {
            uint32_t row[2];
            row[0] = pairs[i*2] + pairs[i*2 + 1];
            row[1] = pairs[i*2] * pairs[i*2 + 1];
            tset_push_unique(&ref, row);
        }
        free(pairs);

        {
            tuple_set got = {0};
            long n = dl_query(db, "r", tset_cb, &got);
            if (n < 0 || n != ref.count || !tset_sorted_eq(&got, &ref)) {
                printf("  iter %d mismatch\n", iter);
                FAIL("property arithmetic mismatch");
                tset_free(&got); tset_free(&ref);
                teardown_db(db, suffix);
                return;
            }
            tset_free(&got);
        }
        tset_free(&ref);
        teardown_db(db, suffix);
    }
    PASS();
}

/* ─── Main ────────────────────────────────────────────────────────────── */

/* ─── T11: reserved temp/const slot names must not alias user variables ──
 * Regression: compiler-generated constant/temp slots are named __kN / __tN.
 * A user variable literally named __t0 or __k0 (legal: vars may start with _)
 * used to alias the generated slot, silently binding the wrong value.  The
 * name generator now skips past any name already taken by a user variable. */
static void test_t11_reserved_names(void)
{
    dl_db *db;

    TEST("T11: __t0/__k0 user vars do not alias generated temp/const slots");

    setup_db(&db, "t11");
    {
        uint32_t v[] = {1, 2};
        load_rows(db, "p", 1, v, 2, "t11");
    }

    /* __t0 is a USER variable (bound from p) AND a would-be generated temp
     * name; __k0 likewise a user var colliding with a would-be generated
     * constant slot.  Under the old name generation these aliased each other,
     * clobbering the user variable.  The rules below are equivalent to
     * r(W,Z):-p(Y),p(Z),W=Y+1. and s(Z):-p(Z),p(Y),Y>1. over p={1,2}. */
    assert(dl_load_rules(db,
        "r(W,__t0):-p(Y),p(__t0),W=Y+1.\n"
        "s(__k0):-p(__k0),p(Y),Y>1.\n") == 0);
    assert(dl_compile(db) == 0);

    {
        /* r = { (2,1),(2,2),(3,1),(3,2) } : W=Y+1, __t0 from p (unrelated) */
        uint32_t e_r[] = {2,1, 2,2, 3,1, 3,2};
        if (!check_query(db, "r", e_r, 4, 2)) {
            FAIL("__t0 clobbered by generated temp slot");
            teardown_db(db, "t11"); return;
        }
        /* s = {1,2} : __k0 projected from p (unrelated to the Y>1 constant) */
        uint32_t e_s[] = {1, 2};
        if (!check_query(db, "s", e_s, 2, 1)) {
            FAIL("__k0 clobbered by generated constant slot");
            teardown_db(db, "t11"); return;
        }
    }
    PASS();
    teardown_db(db, "t11");
}

int main(void)
{
    printf("M9 Arithmetic + Comparison Tests\n");
    printf("================================\n\n");

    test_t1_precedence();
    test_t2_all_arith_ops();
    test_t3_comparisons();
    test_t4_const_and_nested();
    test_t5_filter_path();
    test_t6_parser_rejects();
    test_t7_compile_rejects();
    test_t8_edge_cases();
    test_t9_magic_arith();
    test_t10_property();
    test_t11_reserved_names();

    printf("\n%d tests run, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
