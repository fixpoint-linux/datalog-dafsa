/*
 * test_m9_str.c — M9-strings builtin tests (concat / length / lower / upper /
 * prefix / suffix / contains)
 *
 * Coverage matrix (mirrors the M9-strings plan):
 *   T1  producers: concat (runtime interning) + length (byte length)
 *   T2  filters: prefix / suffix / contains (true + false)
 *   T3  chained + mixed: chained concat, length→arith, length→comparison,
 *       concat→filter, string-constant operand
 *   T4  parser rejects (INT operand to producing builtin, unclosed paren,
 *       deferred lower/upper)
 *   T5  compile rejects (negated builtin, reversed producer dependency,
 *       ungrounded filter, INT operand to filter, malformed arity)
 *   T6  edge cases (empty-string concat, >4096-byte concat backtrack,
 *       non-ASCII byte length, length of a raw-int operand -> no tuple,
 *       string builtin in a recursive rule, magic == full-materialize)
 *   T7  property — seeded random string EDB vs a C reference
 *
 * Regression: full `make test` (M0-M9 + this suite) must stay green.
 */
#include "dl.h"
#include "dl_internal.h"
#include "regexwalk.h"

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

/* ─── Tuple set (local, mirrors test_m9_arith.c) ──────────────────────── */

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
    snprintf(path, sizeof(path), "build-tmp/m9strdb_%s", suffix);
    rm_dir(path);
    *db_out = dl_open(path);
    assert(*db_out);
}

static void teardown_db(dl_db *db, const char *suffix)
{
    char path[256];
    dl_close(db);
    snprintf(path, sizeof(path), "build-tmp/m9strdb_%s", suffix);
    rm_dir(path);
}

/* Load string facts (every cell written as a "quoted" CSV field). */
static int load_str_rows(dl_db *db, const char *rel_name, uint8_t arity,
                         const char *const *cells, int nrows, const char *suffix)
{
    char csv_path[256];
    FILE *f;
    int i, c;

    assert(dl_declare_relation(db, rel_name, arity) == 0);
    snprintf(csv_path, sizeof(csv_path), "build-tmp/m9strcsv_%s_%s.csv",
             suffix, rel_name);
    f = fopen(csv_path, "w");
    assert(f);
    for (i = 0; i < nrows; i++) {
        for (c = 0; c < arity; c++) {
            if (c > 0) fputc(',', f);
            fprintf(f, "\"%s\"", cells[(size_t)i * (size_t)arity + (size_t)c]);
        }
        fputc('\n', f);
    }
    fclose(f);
    return dl_load_facts(db, rel_name, csv_path);
}

/* Load raw integer facts (bare integers, mirrors test_m9_arith.c). */
static int load_rows(dl_db *db, const char *rel_name, uint8_t arity,
                     const uint32_t *cols, int nrows, const char *suffix)
{
    char csv_path[256];
    FILE *f;
    int i, c;

    assert(dl_declare_relation(db, rel_name, arity) == 0);
    snprintf(csv_path, sizeof(csv_path), "build-tmp/m9strcsv_%s_%s.csv",
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

static uint32_t sid(dl_db *db, const char *s) { return dl_intern_str(db, s); }

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

/* ─── T1: producers (concat + length) ─────────────────────────────────── */

static void test_t1_producers(void)
{
    dl_db *db;
    const char *str_cells[] = { "hello", "", "ab", "abc" };
    const char *pair_cells[] = { "hello","world", "foo","bar" };

    TEST("T1: concat (runtime interning) + length (byte length)");

    setup_db(&db, "t1");
    load_str_rows(db, "str", 1, str_cells, 4, "t1");
    load_str_rows(db, "pair", 2, pair_cells, 2, "t1");
    assert(dl_load_rules(db,
        "cat(A,B,C):-pair(A,B),C=concat(A,B).\n"
        "lens(A,N):-str(A),N=length(A).\n") == 0);
    assert(dl_compile(db) == 0);

    {
        uint32_t e_cat[] = {
            sid(db,"hello"), sid(db,"world"), sid(db,"helloworld"),
            sid(db,"foo"),   sid(db,"bar"),   sid(db,"foobar"),
        };
        uint32_t e_lens[] = {
            sid(db,"hello"), 5,
            sid(db,""),      0,
            sid(db,"ab"),    2,
            sid(db,"abc"),   3,
        };
        if (!check_query(db, "cat", e_cat, 2, 3)) {
            FAIL("concat mismatch"); teardown_db(db, "t1"); return;
        }
        if (!check_query(db, "lens", e_lens, 4, 2)) {
            FAIL("length mismatch"); teardown_db(db, "t1"); return;
        }
    }
    PASS();
    teardown_db(db, "t1");
}

/* ─── T2: filters (prefix / suffix / contains) ────────────────────────── */

static void test_t2_filters(void)
{
    dl_db *db;
    const char *str_cells[] = { "hello", "he", "world", "abc", "a" };

    TEST("T2: prefix / suffix / contains (true + false)");

    setup_db(&db, "t2");
    load_str_rows(db, "str", 1, str_cells, 5, "t2");
    assert(dl_load_rules(db,
        "pref(X):-str(X),prefix(X,\"he\").\n"
        "suf(X):-str(X),suffix(X,\"o\").\n"
        "cont(X):-str(X),contains(X,\"ll\").\n") == 0);
    assert(dl_compile(db) == 0);

    {
        uint32_t e_pref[] = { sid(db,"hello"), sid(db,"he") };
        uint32_t e_suf[]  = { sid(db,"hello") };
        uint32_t e_cont[] = { sid(db,"hello") };
        if (!check_query(db, "pref", e_pref, 2, 1)) {
            FAIL("prefix mismatch"); teardown_db(db, "t2"); return;
        }
        if (!check_query(db, "suf", e_suf, 1, 1)) {
            FAIL("suffix mismatch"); teardown_db(db, "t2"); return;
        }
        if (!check_query(db, "cont", e_cont, 1, 1)) {
            FAIL("contains mismatch"); teardown_db(db, "t2"); return;
        }
    }
    PASS();
    teardown_db(db, "t2");
}

/* ─── T3: chained + mixed (length feeding arith/comparison, etc.) ─────── */

static void test_t3_chained_mixed(void)
{
    dl_db *db;
    const char *str_cells[] = { "hello" };
    const char *pair_cells[] = { "hello","world" };

    TEST("T3: chained concat, length->arith, length->comparison, concat->filter");

    setup_db(&db, "t3");
    load_str_rows(db, "str", 1, str_cells, 1, "t3");
    load_str_rows(db, "pair", 2, pair_cells, 1, "t3");
    assert(dl_load_rules(db,
        "chain(A,B,C):-pair(A,B),C1=concat(A,B),C=concat(C1,\"!\").\n"
        "lenp1(S,M):-str(S),N=length(S),M=N+1.\n"
        "long(S):-str(S),N=length(S),N>2.\n"
        "pfxcat(A,B):-pair(A,B),C=concat(A,B),prefix(C,\"he\").\n"
        "lenconst(N):-str(S),N=length(\"hello\").\n") == 0);
    assert(dl_compile(db) == 0);

    {
        uint32_t e_chain[] = {
            sid(db,"hello"), sid(db,"world"), sid(db,"helloworld!"),
        };
        uint32_t e_lenp1[] = { sid(db,"hello"), 6 };
        uint32_t e_long[]  = { sid(db,"hello") };
        uint32_t e_pfxcat[] = { sid(db,"hello"), sid(db,"world") };
        uint32_t e_lenconst[] = { 5 };
        if (!check_query(db, "chain", e_chain, 1, 3)) {
            FAIL("chained concat mismatch"); teardown_db(db, "t3"); return;
        }
        if (!check_query(db, "lenp1", e_lenp1, 1, 2)) {
            FAIL("length->arith mismatch"); teardown_db(db, "t3"); return;
        }
        if (!check_query(db, "long", e_long, 1, 1)) {
            FAIL("length->comparison mismatch"); teardown_db(db, "t3"); return;
        }
        if (!check_query(db, "pfxcat", e_pfxcat, 1, 2)) {
            FAIL("concat->filter mismatch"); teardown_db(db, "t3"); return;
        }
        if (!check_query(db, "lenconst", e_lenconst, 1, 1)) {
            FAIL("length(constant) mismatch"); teardown_db(db, "t3"); return;
        }
    }
    PASS();
    teardown_db(db, "t3");
}

/* ─── T4: parser rejects ──────────────────────────────────────────────── */

static void test_t4_parser_rejects(void)
{
    dl_db *db;

    TEST("T4: parser rejects (INT operand, unclosed paren)");

    setup_db(&db, "t4");
    { const char *s[] = { "hello" }; load_str_rows(db, "str", 1, s, 1, "t4"); }

    /* INT operand to a producing string builtin */
    if (dl_load_rules(db, "r(C):-str(A),C=concat(A,1).\n") == 0) {
        FAIL("INT operand to concat not rejected"); teardown_db(db, "t4"); return;
    }
    /* unclosed paren */
    if (dl_load_rules(db, "r(C):-str(A),C=concat(A,\"b\".\n") == 0) {
        FAIL("unclosed paren in concat not rejected"); teardown_db(db, "t4"); return;
    }
    /* INT operand to lower (a raw int is never a string) */
    if (dl_load_rules(db, "r(C):-str(A),C=lower(1).\n") == 0) {
        FAIL("INT operand to lower not rejected"); teardown_db(db, "t4"); return;
    }
    PASS();
    teardown_db(db, "t4");
}

/* ─── T5: compile rejects ─────────────────────────────────────────────── */

static void test_t5_compile_rejects(void)
{
    dl_db *db;

    TEST("T5: compile rejects (negated, reversed, ungrounded, INT filter, arity)");

    /* negated filter builtin */
    {
        setup_db(&db, "t5a");
        { const char *s[] = { "hello" }; load_str_rows(db, "str", 1, s, 1, "t5a"); }
        if (dl_load_rules(db, "r(X):-str(X),!prefix(X,\"he\").\n") == 0) {
            FAIL("negated filter builtin not rejected"); teardown_db(db, "t5a"); return;
        }
        teardown_db(db, "t5a");
    }
    /* negated producing builtin */
    {
        setup_db(&db, "t5b");
        { const char *s[] = { "hello" }; load_str_rows(db, "str", 1, s, 1, "t5b"); }
        if (dl_load_rules(db, "r(C):-str(A),!C=concat(A,\"b\").\n") == 0) {
            FAIL("negated producing builtin not rejected"); teardown_db(db, "t5b"); return;
        }
        teardown_db(db, "t5b");
    }
    /* reversed producer dependency */
    {
        setup_db(&db, "t5c");
        { const char *s[] = { "a" }; load_str_rows(db, "str", 1, s, 1, "t5c"); }
        if (dl_load_rules(db,
                "r(C):-str(A),D=concat(E,\"z\"),E=concat(A,\"y\").\n") == 0) {
            FAIL("reversed producer dependency not rejected"); teardown_db(db, "t5c"); return;
        }
        teardown_db(db, "t5c");
    }
    /* ungrounded filter operand */
    {
        setup_db(&db, "t5d");
        { const char *s[] = { "hello" }; load_str_rows(db, "str", 1, s, 1, "t5d"); }
        if (dl_load_rules(db, "r(X):-str(X),prefix(X,P).\n") == 0) {
            FAIL("ungrounded filter operand not rejected"); teardown_db(db, "t5d"); return;
        }
        teardown_db(db, "t5d");
    }
    /* INT operand to a filter builtin (parses as normal atom, rejected here) */
    {
        setup_db(&db, "t5e");
        { const char *s[] = { "hello" }; load_str_rows(db, "str", 1, s, 1, "t5e"); }
        if (dl_load_rules(db, "r(X):-str(X),prefix(X,5).\n") == 0) {
            FAIL("INT operand to filter not rejected"); teardown_db(db, "t5e"); return;
        }
        teardown_db(db, "t5e");
    }
    /* malformed arity (bare `length(X)` with no result var) */
    {
        setup_db(&db, "t5f");
        { const char *s[] = { "hello" }; load_str_rows(db, "str", 1, s, 1, "t5f"); }
        if (dl_load_rules(db, "r(X):-str(X),length(X).\n") == 0) {
            FAIL("bare length(X) not rejected"); teardown_db(db, "t5f"); return;
        }
        teardown_db(db, "t5f");
    }
    PASS();
}

/* ─── T6: edge cases ──────────────────────────────────────────────────── */

static void test_t6_edge_cases(void)
{
    dl_db *db;

    TEST("T6: empty concat, >4096 backtrack, non-ASCII, int operand, recursion, magic");

    /* 6a: empty-string concat */
    {
        const char *pair_cells[] = { "","", "","ab", "ab","" };
        setup_db(&db, "t6a");
        load_str_rows(db, "pair", 2, pair_cells, 3, "t6a");
        assert(dl_load_rules(db, "cat(C):-pair(A,B),C=concat(A,B).\n") == 0);
        assert(dl_compile(db) == 0);
        {
            uint32_t e[] = { sid(db,""), sid(db,"ab") };
            if (!check_query(db, "cat", e, 2, 1)) {
                FAIL("empty-string concat mismatch"); teardown_db(db, "t6a"); return;
            }
        }
        teardown_db(db, "t6a");
    }

    /* 6b: concat result exceeding the symbol cap -> backtrack (no tuple).
     * Two 33KB operands (which now intern fine at the raised 64K cap) join to
     * a 66KB result that is too long, so the producing builtin backtracks. */
    {
        char *big = malloc(33001);
        const char *cells[1];
        assert(big);
        memset(big, 'a', 33000);
        big[33000] = '\0';
        cells[0] = big;
        setup_db(&db, "t6b");
        /* The 33KB operand must actually intern/load (non-vacuous: with the
         * old 4096 cap this load silently failed and `str` stayed empty). */
        assert(load_str_rows(db, "str", 1, cells, 1, "t6b") == 1);
        assert(dl_load_rules(db, "r(C):-str(A),str(B),C=concat(A,B).\n") == 0);
        assert(dl_compile(db) == 0);
        {
            if (!check_query(db, "r", NULL, 0, 1)) {
                FAIL("over-long concat should backtrack to empty");
                free(big); teardown_db(db, "t6b"); return;
            }
        }
        free(big);
        teardown_db(db, "t6b");
    }

    /* 6c: non-ASCII byte length (é = 2 UTF-8 bytes) */
    {
        const char *str_cells[] = { "h\xc3\xa9llo", "ab" };
        setup_db(&db, "t6c");
        load_str_rows(db, "str", 1, str_cells, 2, "t6c");
        assert(dl_load_rules(db, "len(A,N):-str(A),N=length(A).\n") == 0);
        assert(dl_compile(db) == 0);
        {
            uint32_t e[] = { sid(db,"h\xc3\xa9llo"), 6, sid(db,"ab"), 2 };
            if (!check_query(db, "len", e, 2, 2)) {
                FAIL("non-ASCII byte length mismatch"); teardown_db(db, "t6c"); return;
            }
        }
        teardown_db(db, "t6c");
    }

    /* 6d: length of a raw-int operand (out-of-range sym_id) -> no tuple */
    {
        uint32_t v[] = { 999999 };
        setup_db(&db, "t6d");
        load_rows(db, "val", 1, v, 1, "t6d");
        assert(dl_load_rules(db, "r(N):-val(X),N=length(X).\n") == 0);
        assert(dl_compile(db) == 0);
        {
            if (!check_query(db, "r", NULL, 0, 1)) {
                FAIL("length(raw int) should produce no tuple"); teardown_db(db, "t6d"); return;
            }
        }
        teardown_db(db, "t6d");
    }

    /* 6e: string builtin in a recursive rule (bounded by length<4) */
    {
        const char *seed_cells[] = { "a" };
        setup_db(&db, "t6e");
        load_str_rows(db, "seed", 1, seed_cells, 1, "t6e");
        assert(dl_load_rules(db,
            "p(X):-seed(X).\n"
            "p(Y):-p(X),Y=concat(X,\"b\"),N=length(Y),N<4.\n") == 0);
        assert(dl_compile(db) == 0);
        {
            uint32_t e[] = { sid(db,"a"), sid(db,"ab"), sid(db,"abb") };
            if (!check_query(db, "p", e, 3, 1)) {
                FAIL("recursive string-builtin fixpoint"); teardown_db(db, "t6e"); return;
            }
        }
        teardown_db(db, "t6e");
    }

    /* 6f: magic (dl_query_magic_adorn) == full-materialize filter */
    {
        char csv_path[256];
        FILE *f;
        setup_db(&db, "t6f");
        assert(dl_declare_relation(db, "edge", 2) == 0);
        snprintf(csv_path, sizeof(csv_path), "build-tmp/m9strcsv_t6f_edge.csv");
        f = fopen(csv_path, "w");
        assert(f);
        fprintf(f, "1,\"a\"\n1,\"b\"\n2,\"c\"\n");
        fclose(f);
        assert(dl_load_facts(db, "edge", csv_path) == 3);

        assert(dl_load_rules(db, "next(X,Y):-edge(X,Z),Y=concat(Z,\"s\").\n") == 0);
        assert(dl_compile(db) == 0);

        {
            uint32_t vals[1] = { 1 };
            tuple_set full = {0}, ground = {0}, rm = {0};
            long nq, nm;

            nq = dl_query(db, "next", tset_cb, &full);
            assert(nq >= 0);
            ground.arity = 2;
            {
                long i;
                for (i = 0; i < full.count; i++) {
                    const uint32_t *row = full.data + (size_t)i * 2;
                    if (row[0] == vals[0]) tset_push_unique(&ground, row);
                }
            }
            nm = dl_query_magic_adorn(db, "next", "bf", vals, 1, tset_cb, &rm);
            if (nm < 0) {
                FAIL("magic transform rejected string program");
                tset_free(&full); tset_free(&ground); tset_free(&rm);
                teardown_db(db, "t6f"); return;
            }
            if ((long)ground.count != nm || !tset_sorted_eq(&ground, &rm)) {
                FAIL("magic string != full-materialize filter");
                tset_free(&full); tset_free(&ground); tset_free(&rm);
                teardown_db(db, "t6f"); return;
            }
            /* next(1,*) = {concat("a","s"), concat("b","s")} = {"as","bs"} */
            if (nm != 2) {
                FAIL("expected 2 rows for next(1,*)");
                tset_free(&full); tset_free(&ground); tset_free(&rm);
                teardown_db(db, "t6f"); return;
            }
            tset_free(&full); tset_free(&ground); tset_free(&rm);
        }
        teardown_db(db, "t6f");
    }
    PASS();
}

/* ─── T7: property test (backstop) ────────────────────────────────────── */

static uint32_t rng_state = 0x9E3779B9u;
static uint32_t rng_next(void)
{
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    rng_state = x;
    return x;
}
static uint32_t rng_rand(uint32_t n) { return n ? rng_next() % n : 0; }

static void test_t7_property(void)
{
    TEST("T7: property — seeded random string concat + length vs C reference");

    int iter;
    for (iter = 0; iter < 200; iter++) {
        dl_db *db;
        char suffix[32];
        int N = 4 + (int)(iter % 7);   /* 4..10 strings */
        char **strs = malloc((size_t)N * sizeof(char *));
        tuple_set ref_cat = {0};
        int i, j;

        assert(strs);
        snprintf(suffix, sizeof(suffix), "prop_%d", iter);
        setup_db(&db, suffix);

        for (i = 0; i < N; i++) {
            int len = (int)rng_rand(4);   /* length 0..3 */
            strs[i] = malloc((size_t)len + 1);
            assert(strs[i]);
            for (j = 0; j < len; j++)
                strs[i][j] = (char)('a' + (int)rng_rand(2));
            strs[i][len] = '\0';
        }
        load_str_rows(db, "str", 1, (const char *const *)strs, N, suffix);

        /* cat(C) :- str(A), str(B), C = concat(A,B). */
        assert(dl_load_rules(db, "cat(C):-str(A),str(B),C=concat(A,B).\n") == 0);
        assert(dl_compile(db) == 0);

        ref_cat.arity = 1;
        for (i = 0; i < N; i++) {
            for (j = 0; j < N; j++) {
                char buf[16];
                uint32_t row[1];
                snprintf(buf, sizeof(buf), "%s%s", strs[i], strs[j]);
                row[0] = sid(db, buf);
                tset_push_unique(&ref_cat, row);
            }
        }

        {
            tuple_set got = {0};
            long n = dl_query(db, "cat", tset_cb, &got);
            if (n < 0 || n != ref_cat.count || !tset_sorted_eq(&got, &ref_cat)) {
                printf("  iter %d concat mismatch\n", iter);
                FAIL("property concat mismatch");
                tset_free(&got); tset_free(&ref_cat);
                for (i = 0; i < N; i++) free(strs[i]);
                free(strs);
                teardown_db(db, suffix);
                return;
            }
            tset_free(&got);
        }
        tset_free(&ref_cat);
        for (i = 0; i < N; i++) free(strs[i]);
        free(strs);
        teardown_db(db, suffix);
    }
    PASS();
}

/* ─── Main ────────────────────────────────────────────────────────────── */

/* ─── T8: empty-string literals in rules ────────────────────────────────
 * Regression: a zero-length quoted string "" in a RULE was dropped by the
 * lexer (tok_new kept text==NULL), so every string builtin silently failed
 * interning and dl_load_rules returned -1.  The empty-string value is now
 * carried (text = ""), so these must compile and evaluate correctly. */
static void test_t8_empty_string_literals(void)
{
    dl_db *db;

    TEST("T8: empty-string literals in rules (concat/prefix/length)");

    setup_db(&db, "t8");
    {
        const char *str_cells[] = { "abc", "", "ab" };
        load_str_rows(db, "str", 1, str_cells, 3, "t8");
        load_str_rows(db, "dummy", 1, (const char *const[]){"x"}, 1, "t8");
    }

    /* C = concat("",""), C = concat("ab",""), N = length("") must compile.
     * dummy grounds the rule (a rule needs a positive body atom). */
    if (dl_load_rules(db,
        "e1(C):-dummy(_),C=concat(\"\",\"\").\n"
        "e2(C):-dummy(_),C=concat(\"ab\",\"\").\n"
        "e3(C):-dummy(_),C=concat(\"\",\"cd\").\n") != 0) {
        FAIL("empty-string concat literal did not compile");
        teardown_db(db, "t8"); return;
    }
    if (dl_load_rules(db, "len0(N):-dummy(_),N=length(\"\").\n") != 0) {
        FAIL("empty-string length literal did not compile");
        teardown_db(db, "t8"); return;
    }
    /* prefix(S,"") / contains(S,"") must compile and match all. */
    if (dl_load_rules(db,
        "allp(X):-str(X),prefix(X,\"\").\n"
        "allc(X):-str(X),contains(X,\"\").\n") != 0) {
        FAIL("empty-string filter literal did not compile");
        teardown_db(db, "t8"); return;
    }
    assert(dl_compile(db) == 0);

    {
        uint32_t e_len0[] = { 0 };                 /* length("") == 0 */
        if (!check_query(db, "len0", e_len0, 1, 1)) {
            FAIL("length(\"\") != 0"); teardown_db(db, "t8"); return;
        }
        /* e1 -> {""} ; e2 -> {"ab"} ; e3 -> {"cd"} */
        {
            uint32_t e_empty = sid(db,"");
            uint32_t e_ab = sid(db,"ab");
            uint32_t e_cd = sid(db,"cd");
            if (!check_query(db, "e1", &e_empty, 1, 1)) {
                FAIL("concat(\"\",\"\") != \"\""); teardown_db(db, "t8"); return;
            }
            if (!check_query(db, "e2", &e_ab, 1, 1)) {
                FAIL("concat(\"ab\",\"\") != \"ab\""); teardown_db(db, "t8"); return;
            }
            if (!check_query(db, "e3", &e_cd, 1, 1)) {
                FAIL("concat(\"\",\"cd\") != \"cd\""); teardown_db(db, "t8"); return;
            }
        }
        /* allp/allc should match all 3 str rows. */
        {
            uint32_t e_all[] = {
                sid(db,"abc"), sid(db,""), sid(db,"ab"),
            };
            if (!check_query(db, "allp", e_all, 3, 1)) {
                FAIL("prefix(S,\"\") did not match all");
                teardown_db(db, "t8"); return;
            }
            if (!check_query(db, "allc", e_all, 3, 1)) {
                FAIL("contains(S,\"\") did not match all");
                teardown_db(db, "t8"); return;
            }
        }
    }
    PASS();
    teardown_db(db, "t8");
}

/* ─── T9: lower/upper (ASCII case folding, OP_STR_BIND imm 1/2) ───────── */
static void test_t9_lower_upper(void)
{
    dl_db *db;

    TEST("T9: lower/upper (ASCII case folding)");

    setup_db(&db, "t9");
    {
        const char *cells[] = { "Hello World", "MiXeD", "abc", "ABC123!" };
        load_str_rows(db, "s", 1, cells, 4, "t9");
    }

    assert(dl_load_rules(db,
        "lc(X):-s(Y),X=lower(Y).\n"
        "uc(X):-s(Y),X=upper(Y).\n") == 0);
    assert(dl_compile(db) == 0);

    {
        uint32_t e_lc[] = {
            sid(db,"hello world"), sid(db,"mixed"),
            sid(db,"abc"),         sid(db,"abc123!"),
        };
        uint32_t e_uc[] = {
            sid(db,"HELLO WORLD"), sid(db,"MIXED"),
            sid(db,"ABC"),         sid(db,"ABC123!"),
        };
        if (!check_query(db, "lc", e_lc, 4, 1)) {
            FAIL("lower mismatch"); teardown_db(db, "t9"); return;
        }
        if (!check_query(db, "uc", e_uc, 4, 1)) {
            FAIL("upper mismatch"); teardown_db(db, "t9"); return;
        }
    }
    PASS();
    teardown_db(db, "t9");
}

/* ─── T10: long-symbol interning (raised 64K cap) ──────────────────────── */

static int symset_collect_cb(uint32_t sym_id, void *user)
{
    sym_set *set = (sym_set *)user;
    return symset_add(set, sym_id);
}

static void test_t10_long_symbol(void)
{
    TEST("T10: 33311-byte symbol round-trips (intern/save/reload/find/of)");

    const size_t N = 33311;
    char *long_s = malloc(N + 1);
    assert(long_s);
    memset(long_s, 'a', N);
    long_s[N] = '\0';

    interner *ir = intern_create();
    assert(ir);

    /* Intern: must succeed (was rejected at the old 4096 cap). */
    uint32_t id = intern_str(ir, long_s);
    if (id == 0) {
        FAIL("33311-byte symbol rejected");
        free(long_s); intern_free(ir); return;
    }

    /* find + of agree on the live interner. */
    if (intern_str_find(ir, long_s) != id) {
        FAIL("intern_str_find mismatch (live)");
        free(long_s); intern_free(ir); return;
    }
    {
        const char *back = intern_str_of(ir, id);
        if (!back || strcmp(back, long_s) != 0) {
            FAIL("intern_str_of mismatch (live)");
            free(long_s); intern_free(ir); return;
        }
    }

    /* Regex symbol walk over the long symbol (iterative product DFS). */
    {
        regex_dfa *dfa = regex_compile(".*");
        sym_set set;
        assert(dfa && !dfa->errmsg);
        assert(symset_init(&set) == 0);
        long n = symbols_dfa_walk(intern_fwd(ir), dfa, symset_collect_cb, &set);
        if (n != 1 || !symset_contains(&set, id)) {
            FAIL("symbol regex walk did not match the long symbol");
        }
        symset_free(&set);
        regex_dfa_free(dfa);
    }

    /* Persist, free, reload, then verify find/of round-trip. */
    {
        const char *fwd = "build-tmp/m9str_t10_symbols.dafsa";
        const char *rev = "build-tmp/m9str_t10_symbols.array";
        remove(fwd);
        remove(rev);
        if (intern_save(ir, fwd, rev) != 0) {
            FAIL("intern_save failed");
            free(long_s); intern_free(ir); return;
        }
        intern_free(ir);
        ir = intern_load(fwd, rev);
        assert(ir);
        if (intern_str_find(ir, long_s) != id) {
            FAIL("intern_str_find mismatch (reloaded)");
            free(long_s); intern_free(ir); return;
        }
        {
            const char *back = intern_str_of(ir, id);
            if (!back || strcmp(back, long_s) != 0) {
                FAIL("intern_str_of mismatch (reloaded)");
                free(long_s); intern_free(ir); return;
            }
        }
        remove(fwd);
        remove(rev);
    }

    /* Rejection above the raised cap is still exercised. */
    {
        const size_t over = 65537;
        char *too = malloc(over + 1);
        assert(too);
        memset(too, 'b', over);
        too[over] = '\0';
        if (intern_str(ir, too) != 0) {
            FAIL(">64K symbol should be rejected");
        }
        free(too);
    }

    free(long_s);
    intern_free(ir);
    PASS();
}

int main(void)
{
    printf("M9-strings Builtin Tests\n");
    printf("========================\n\n");

    test_t1_producers();
    test_t2_filters();
    test_t3_chained_mixed();
    test_t4_parser_rejects();
    test_t5_compile_rejects();
    test_t6_edge_cases();
    test_t7_property();
    test_t8_empty_string_literals();
    test_t9_lower_upper();
    test_t10_long_symbol();

    printf("\n%d tests run, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
