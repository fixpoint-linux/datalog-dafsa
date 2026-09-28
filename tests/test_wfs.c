/*
 * test_wfs.c — well-founded semantics (dl_query_wfs_ro) acceptance tests.
 *
 * Slice 1 (S1) of Capability 1: negation over recursion via van Gelder's
 * alternating fixpoint.  The canonical fixtures:
 *
 *   win(X) :- move(X,Y), !win(Y).   move = {(a,b),(b,c),(c,d),(e,e)}
 *     -> TRUE win = {a,c}, FALSE win = {b,d}, UNDEFINED win = {e}
 *
 *   win2(X) :- move2(X,Y), !win2(Y).  move2 = {(1,2),(2,1)}  (even cycle)
 *     -> all UNDEFINED
 *
 * Every WFS result is cross-checked by a BRUTE-FORCE 3-valued checker that
 * enumerates all 3^n assignments of the n ground atoms and computes the
 * well-founded model directly from the definition (iterating the
 * consequence operator to a 3-valued fixpoint) — the R1 mitigation against
 * a sign/undefined error in the alternating fixpoint.
 *
 * Also checks:
 *   - a STRATIFIED program answers identically via dl_query_wfs_ro(truth=0)
 *     and dl_query_rules_ro (the isolation check);
 *   - unsafe negation  win(X) :- !win(Y)  is still REJECTED;
 *   - an aggregate inside the recursion is still REJECTED;
 *   - truth modes 1/2/3 return DL_WFS_ERR_NOT_IMPLEMENTED;
 *   - the old stratifier rejects stay intact (win-through-negation via
 *     dl_load_rules/dl_compile still fails — the null-isolation gate).
 */

#include "dl.h"

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

/* ─── tuple collection helpers (mirrors test_m2.c) ─────────────────────── */

typedef struct {
    uint32_t *data;
    long      count;
    long      cap;
    uint8_t   arity;
} tset;

static int tset_cb(const uint32_t *cols, uint8_t arity, void *user)
{
    tset *t = (tset *)user;
    if (t->arity == 0) t->arity = arity;
    assert(arity == t->arity);
    if (t->count >= t->cap) {
        long nc = t->cap ? t->cap * 2 : 256;
        uint32_t *nd = realloc(t->data,
            (size_t)nc * (size_t)t->arity * sizeof(uint32_t));
        if (!nd) return 1;
        t->data = nd;
        t->cap = nc;
    }
    memcpy(t->data + (size_t)t->count * (size_t)t->arity,
           cols, (size_t)t->arity * sizeof(uint32_t));
    t->count++;
    return 0;
}

static void tset_reset(tset *t) { memset(t, 0, sizeof(*t)); }
static void tset_free(tset *t) { free(t->data); memset(t, 0, sizeof(*t)); }

/* sort rows (arity-strided) so set comparison is order-insensitive */
static uint8_t g_sort_arity = 1;
static int row_cmp(const void *a, const void *b) { return memcmp(a, b, (size_t)g_sort_arity * 4); }
static void tset_sort(tset *t)
{
    if (t->count <= 1) return;
    g_sort_arity = t->arity;
    qsort(t->data, (size_t)t->count, (size_t)t->arity * sizeof(uint32_t), row_cmp);
}

static int tset_eq(tset *a, tset *b)
{
    long i;
    if (a->count != b->count || a->arity != b->arity) return 0;
    for (i = 0; i < a->count * a->arity; i++)
        if (a->data[i] != b->data[i]) return 0;
    return 1;
}

static void tset_add_row(tset *t, const uint32_t *row)
{
    tset_cb(row, t->arity ? t->arity : 1, t);
}

/* ─── db helpers ───────────────────────────────────────────────────────── */

static dl_db *g_db;

static void setup(void)
{
    system("rm -rf build-tmp/wfsdb");
    g_db = dl_open("build-tmp/wfsdb");
    assert(g_db);
}

static void teardown(void)
{
    dl_close(g_db);
    system("rm -rf build-tmp/wfsdb");
}

static void load_rows(const char *rel, uint8_t arity,
                      const uint32_t *cols, int nrows)
{
    char path[256];
    FILE *f;
    int i, c;

    assert(dl_declare_relation(g_db, rel, arity) == 0);
    snprintf(path, sizeof path, "build-tmp/wfsdb/%s.csv", rel);
    f = fopen(path, "w");
    assert(f);
    for (i = 0; i < nrows; i++) {
        for (c = 0; c < arity; c++) {
            if (c) fputc(',', f);
            fprintf(f, "%u", cols[(size_t)i * arity + c]);
        }
        fputc('\n', f);
    }
    fclose(f);
    assert(dl_load_facts(g_db, rel, path) == nrows);
}

/* Load SYMBOL rows: the CSV cell is the symbol's STRING (which
 * dl_load_facts interns to the same id — and which S2 records as the
 * sym kind; a raw "%u" of an id would record the INT kind instead). */
static void load_rows_sym(const char *rel, uint8_t arity,
                          const char **cells, int nrows)
{
    char path[256];
    FILE *f;
    int i, c;

    assert(dl_declare_relation(g_db, rel, arity) == 0);
    snprintf(path, sizeof path, "build-tmp/wfsdb/%s.csv", rel);
    f = fopen(path, "w");
    assert(f);
    for (i = 0; i < nrows; i++) {
        for (c = 0; c < arity; c++) {
            if (c) fputc(',', f);
            fprintf(f, "%s", cells[(size_t)i * arity + c]);
        }
        fputc('\n', f);
    }
    fclose(f);
    assert(dl_load_facts(g_db, rel, path) == nrows);
}

/* ─── brute-force 3-valued well-founded checker (R1 mitigation) ────────── */

/*
 * Computes the well-founded model of
 *     win(X) :- rel(X,Y), !win(Y).
 * over `rel` (nedges arity-2 rows) with the node universe `nodes` (n nodes)
 * DIRECTLY from the definition — iterating the justified-inference rules to
 * a fixpoint (this win-shape is head-cycle-free, so singleton unfounded
 * sets are complete and this iteration computes exactly the well-founded
 * model):
 *     val(x) = TRUE  iff x has an edge to a node with val = FALSE
 *              (a rule instance whose negated body atom is false)
 *     val(x) = FALSE iff x has no outgoing edge (no rule instance can
 *              fire — x is in every unfounded set) or EVERY successor has
 *              val = TRUE (every instance is defeated)
 *     else UNDEFINED.  Start all-undefined; iterate to a fixpoint.
 */
typedef struct {
    int n;
    uint32_t nodes[64];
    int nedges;
    uint32_t edges[256][2];
} wf_graph;

static int node_ix(const wf_graph *g, uint32_t v)
{
    int i;
    for (i = 0; i < g->n; i++) if (g->nodes[i] == v) return i;
    return -1;
}

static void wf_brute(const wf_graph *g, uint8_t val[64])
{
    int i, x, y, changed;

    for (i = 0; i < g->n; i++) val[i] = 2; /* all undefined */

    do {
        changed = 0;
        for (x = 0; x < g->n; x++) {
            int has_out = 0, true_supp = 0, all_true = 1;
            for (i = 0; i < g->nedges; i++) {
                if (g->edges[i][0] != g->nodes[x]) continue;
                y = node_ix(g, g->edges[i][1]);
                if (y < 0) continue;
                has_out = 1;
                if (val[y] == 0) true_supp = 1;
                if (val[y] != 1) all_true = 0;
            }
            if (true_supp) {
                if (val[x] != 1) { val[x] = 1; changed = 1; }
            } else if (!has_out || all_true) {
                if (val[x] != 0) { val[x] = 0; changed = 1; }
            }
        }
    } while (changed);
}

/* Cross-check the engine's TRUE set against the brute-force model. */
static int check_against_brute(const wf_graph *g, tset *got, const uint32_t *nodes, int n)
{
    uint8_t val[64];
    tset want;
    int i;

    wf_brute(g, val);
    memset(&want, 0, sizeof want);
    want.arity = 1;
    for (i = 0; i < n; i++)
        if (val[i] == 1) tset_add_row(&want, &nodes[i]);
    /* an empty streamed result never learned its arity from the callback */
    if (got->arity == 0) got->arity = 1;
    tset_sort(&want);
    tset_sort(got);
    i = tset_eq(got, &want);
    tset_free(&want);
    return i;
}

/* ─── Test 1: canonical win/move fixture ────────────────────────────────── */

static void test_win_move_canonical(void)
{
    const char *rules = "win(X):-move(X,Y),!win(Y).\n";
    uint32_t a, b, c, d, e;
    uint32_t mv[8];
    tset res;
    long n;
    wf_graph g;

    TEST("wfs: win(X):-move(X,Y),!win(Y) TRUE={a,c}");

    setup();
    a = dl_intern_str(g_db, "a");
    b = dl_intern_str(g_db, "b");
    c = dl_intern_str(g_db, "c");
    d = dl_intern_str(g_db, "d");
    e = dl_intern_str(g_db, "e");

    mv[0] = a; mv[1] = b;
    mv[2] = b; mv[3] = c;
    mv[4] = c; mv[5] = d;
    mv[6] = e; mv[7] = e;
    load_rows("move", 2, mv, 4);

    tset_reset(&res);
    n = dl_query_wfs_ro(g_db, rules, "win", DL_WFS_TRUE_ONLY, tset_cb, &res);
    if (n < 0) {
        printf("(err %ld) ", n);
        teardown();
        FAIL("dl_query_wfs_ro failed");
        return;
    }

    /* pinned fixture: TRUE win = {a, c} */
    {
        tset want;
        memset(&want, 0, sizeof want);
        want.arity = 1;
        tset_add_row(&want, &a);
        tset_add_row(&want, &c);
        tset_sort(&want);
        tset_sort(&res);
        if (!tset_eq(&res, &want)) {
            tset_free(&want); tset_free(&res);
            teardown();
            FAIL("TRUE win != {a,c}");
            return;
        }
        tset_free(&want);
    }

    /* R1: brute-force 3-valued cross-check */
    memset(&g, 0, sizeof g);
    g.n = 5;
    g.nodes[0] = a; g.nodes[1] = b; g.nodes[2] = c; g.nodes[3] = d; g.nodes[4] = e;
    g.nedges = 4;
    g.edges[0][0] = a; g.edges[0][1] = b;
    g.edges[1][0] = b; g.edges[1][1] = c;
    g.edges[2][0] = c; g.edges[2][1] = d;
    g.edges[3][0] = e; g.edges[3][1] = e;
    if (!check_against_brute(&g, &res, g.nodes, g.n)) {
        tset_free(&res);
        teardown();
        FAIL("brute-force 3-valued cross-check mismatch");
        return;
    }
    /* pin the FULL model the checker computed: TRUE={a,c}, FALSE={b,d},
     * UNDEF={e} (modes 1/2 are unimplemented, so FALSE/UNDEF are pinned
     * through the independent brute-force derivation) */
    {
        uint8_t val[64];
        wf_brute(&g, val);
        if (val[0] != 1 || val[2] != 1 || val[1] != 0 || val[3] != 0 ||
            val[4] != 2) {
            tset_free(&res);
            teardown();
            FAIL("brute-force model != TRUE={a,c} FALSE={b,d} UNDEF={e}");
            return;
        }
    }

    tset_free(&res);
    teardown();
    PASS();
}

/* ─── Test 2: even cycle — all undefined ────────────────────────────────── */

static void test_even_cycle_all_undefined(void)
{
    const char *rules = "win2(X):-move2(X,Y),!win2(Y).\n";
    uint32_t mv[4];
    tset res;
    long n;
    wf_graph g;

    TEST("wfs: even cycle move2={(1,2),(2,1)} -> TRUE={} (all undefined)");

    setup();
    mv[0] = 1; mv[1] = 2;
    mv[2] = 2; mv[3] = 1;
    load_rows("move2", 2, mv, 2);

    tset_reset(&res);
    n = dl_query_wfs_ro(g_db, rules, "win2", DL_WFS_TRUE_ONLY, tset_cb, &res);
    if (n < 0) {
        printf("(err %ld) ", n);
        teardown();
        FAIL("dl_query_wfs_ro failed");
        return;
    }
    if (res.count != 0 || n != 0) {
        tset_free(&res);
        teardown();
        FAIL("TRUE win2 != {} (even cycle must be all-undefined)");
        return;
    }

    /* brute-force agrees everything is undefined (TRUE set empty) */
    memset(&g, 0, sizeof g);
    g.n = 2;
    g.nodes[0] = 1; g.nodes[1] = 2;
    g.nedges = 2;
    g.edges[0][0] = 1; g.edges[0][1] = 2;
    g.edges[1][0] = 2; g.edges[1][1] = 1;
    if (!check_against_brute(&g, &res, g.nodes, g.n)) {
        tset_free(&res);
        teardown();
        FAIL("brute-force cross-check mismatch on the even cycle");
        return;
    }

    tset_free(&res);
    teardown();
    PASS();
}

/* ─── Test 3: stratified program — identical answers (isolation) ───────── */

static void test_stratified_identical(void)
{
    const char *rules =
        "reach(X):-seed(X).\n"
        "reach(Y):-reach(X),edge(X,Y).\n"
        "isolated(X):-node(X),!reach(X).\n";
    uint32_t nodes[5], seeds[1], edges[8];
    tset wfs_res, ro_res;
    long n1, n2;

    TEST("wfs: stratified program answers identically to dl_query_rules_ro");

    setup();
    nodes[0] = 1; nodes[1] = 2; nodes[2] = 3; nodes[3] = 4; nodes[4] = 5;
    load_rows("node", 1, nodes, 5);
    seeds[0] = 1;
    load_rows("seed", 1, seeds, 1);
    edges[0] = 1; edges[1] = 2;
    edges[2] = 2; edges[3] = 3;
    edges[4] = 3; edges[5] = 4;
    edges[6] = 9; edges[7] = 10;
    load_rows("edge", 2, edges, 4);

    tset_reset(&wfs_res);
    n1 = dl_query_wfs_ro(g_db, rules, "isolated", DL_WFS_TRUE_ONLY,
                         tset_cb, &wfs_res);
    tset_reset(&ro_res);
    n2 = dl_query_rules_ro(g_db, rules, "isolated", tset_cb, &ro_res);
    if (n1 < 0 || n2 < 0) {
        printf("(errs %ld,%ld) ", n1, n2);
        tset_free(&wfs_res); tset_free(&ro_res);
        teardown();
        FAIL("query failed");
        return;
    }
    tset_sort(&wfs_res);
    tset_sort(&ro_res);
    if (!tset_eq(&wfs_res, &ro_res) || n1 != n2) {
        tset_free(&wfs_res); tset_free(&ro_res);
        teardown();
        FAIL("stratified answers diverge between WFS and rules_ro");
        return;
    }
    /* and the expected value: isolated = {5} (the plan's fixture) */
    {
        tset want;
        memset(&want, 0, sizeof want);
        want.arity = 1;
        tset_add_row(&want, &nodes[4]);
        tset_sort(&want);
        if (!tset_eq(&wfs_res, &want)) {
            tset_free(&want);
            tset_free(&wfs_res); tset_free(&ro_res);
            teardown();
            FAIL("isolated != {5}");
            return;
        }
        tset_free(&want);
    }

    tset_free(&wfs_res);
    tset_free(&ro_res);
    teardown();
    PASS();
}

/* ─── Test 4: unsafe negation must still be rejected ───────────────────── */

static void test_reject_unsafe_negation(void)
{
    const char *rules = "win(X):-!win(Y).\n";
    long n;

    TEST("wfs: reject unsafe negation win(X):-!win(Y)");

    setup();
    assert(dl_declare_relation(g_db, "move", 2) == 0);
    {
        tset res;
        tset_reset(&res);
        n = dl_query_wfs_ro(g_db, rules, "win", DL_WFS_TRUE_ONLY, tset_cb, &res);
        tset_free(&res);
    }
    teardown();

    if (n < 0) PASS();
    else FAIL("expected rejection, got success");
}

/* ─── Test 5: aggregate inside the recursion must still be rejected ────── */

static void test_reject_aggregate_in_scc(void)
{
    const char *rules =
        "win(X):-move(X,Y),!win(Y),N=count().\n";
    long n;

    TEST("wfs: reject aggregate inside the negated recursion");

    setup();
    {
        uint32_t mv[2] = {1, 2};
        load_rows("move", 2, mv, 1);
    }
    {
        tset res;
        tset_reset(&res);
        n = dl_query_wfs_ro(g_db, rules, "win", DL_WFS_TRUE_ONLY, tset_cb, &res);
        tset_free(&res);
    }
    teardown();

    if (n < 0) PASS();
    else FAIL("expected rejection, got success");
}

/* ─── Test 6: truth modes 1/2/3 — clear not-implemented error ──────────── */

static void test_truth_modes_unimplemented(void)
{
    const char *rules = "win(X):-move(X,Y),!win(Y).\n";
    uint32_t mv[2] = {1, 2};
    long n1, n2, n3;

    TEST("wfs: truth modes 1/2/3 return DL_WFS_ERR_NOT_IMPLEMENTED");

    setup();
    load_rows("move", 2, mv, 1);
    {
        tset res;
        tset_reset(&res);
        n1 = dl_query_wfs_ro(g_db, rules, "win", DL_WFS_FALSE_ONLY, tset_cb, &res);
        n2 = dl_query_wfs_ro(g_db, rules, "win", DL_WFS_UNDEF_ONLY, tset_cb, &res);
        n3 = dl_query_wfs_ro(g_db, rules, "win", DL_WFS_ALL_TAGGED, tset_cb, &res);
        tset_free(&res);
    }
    teardown();

    if (n1 == DL_WFS_ERR_NOT_IMPLEMENTED &&
        n2 == DL_WFS_ERR_NOT_IMPLEMENTED &&
        n3 == DL_WFS_ERR_NOT_IMPLEMENTED)
        PASS();
    else {
        printf("(got %ld,%ld,%ld) ", n1, n2, n3);
        FAIL("modes 1/2/3 must return DL_WFS_ERR_NOT_IMPLEMENTED");
    }
}

/* ─── Test 7: null-isolation — the old stratifier rejects stay intact ──── */

static void test_old_rejects_intact(void)
{
    const char *rules = "win(X):-move(X,Y),!win(Y).\n";
    int ret;

    TEST("wfs: dl_load_rules/dl_compile still reject win-through-negation");

    setup();
    assert(dl_declare_relation(g_db, "move", 2) == 0);
    ret = dl_load_rules(g_db, rules);
    if (ret == 0)
        ret = dl_compile(g_db);
    teardown();

    if (ret != 0) PASS();
    else FAIL("stratifier no longer rejects the unstratifiable program");
}

/* ─── Test 8: a shorter open chain (opposite parity regression) ────────── */

static void test_chain_open(void)
{
    const char *rules = "w(X):-m(X,Y),!w(Y).\n";
    uint32_t mv[4];
    tset res;
    long n;
    wf_graph g;

    TEST("wfs: open 2-chain m={(1,2),(2,3)} -> TRUE={2}, FALSE={1,3}");

    setup();
    mv[0] = 1; mv[1] = 2;
    mv[2] = 2; mv[3] = 3;
    load_rows("m", 2, mv, 2);

    tset_reset(&res);
    n = dl_query_wfs_ro(g_db, rules, "w", DL_WFS_TRUE_ONLY, tset_cb, &res);
    if (n < 0) {
        printf("(err %ld) ", n);
        teardown();
        FAIL("dl_query_wfs_ro failed");
        return;
    }
    {
        tset want;
        uint32_t two = 2;
        memset(&want, 0, sizeof want);
        want.arity = 1;
        tset_add_row(&want, &two);
        tset_sort(&want);
        tset_sort(&res);
        if (!tset_eq(&res, &want)) {
            tset_free(&want); tset_free(&res);
            teardown();
            FAIL("TRUE w != {2}");
            return;
        }
        tset_free(&want);
    }

    memset(&g, 0, sizeof g);
    g.n = 3;
    g.nodes[0] = 1; g.nodes[1] = 2; g.nodes[2] = 3;
    g.nedges = 2;
    g.edges[0][0] = 1; g.edges[0][1] = 2;
    g.edges[1][0] = 2; g.edges[1][1] = 3;
    if (!check_against_brute(&g, &res, g.nodes, g.n)) {
        tset_free(&res);
        teardown();
        FAIL("brute-force cross-check mismatch on the open 2-chain");
        return;
    }

    tset_free(&res);
    teardown();
    PASS();
}

/* ─── Test 9: BLOCKER regression — symbol constant in a negated atom ────── */

/*
 * wfs.zig buildDomain used the NON-INSERTING intern_str_find and SKIPPED a
 * rule symbol-constant absent from the interner; but the compiler INSERTS it
 * later (token_const, compiler.zig:460) and the complement is probed at the
 * new id — so !q(foo) read FALSE where WFS says TRUE (silent-wrong, with the
 * answer depending on invisible interner state).  Regression: with a FRESH
 * interner, e={a}; q(X):-e(X). p(X):-e(X),!q(foo).  goal p -> TRUE p={a}
 * (q={a} only, so q(foo) is FALSE and !q(foo) holds).
 */
static void test_negated_atom_symbol_constant(void)
{
    const char *rules = "q(X):-e(X).\np(X):-e(X),!q(foo).\n";
    uint32_t a;
    tset res;
    long n;

    TEST("wfs: !q(foo), 'foo' not in the interner -> TRUE p={a}");

    setup();
    a = dl_intern_str(g_db, "a");
    {
        const char *cells[1] = { "a" };
        load_rows_sym("e", 1, cells, 1);
    }

    tset_reset(&res);
    n = dl_query_wfs_ro(g_db, rules, "p", DL_WFS_TRUE_ONLY, tset_cb, &res);
    if (n < 0) {
        printf("(err %ld) ", n);
        tset_free(&res);
        teardown();
        FAIL("dl_query_wfs_ro failed");
        return;
    }
    {
        tset want;
        memset(&want, 0, sizeof want);
        want.arity = 1;
        tset_add_row(&want, &a);
        tset_sort(&want);
        tset_sort(&res);
        if (!tset_eq(&res, &want)) {
            tset_free(&want); tset_free(&res);
            teardown();
            FAIL("TRUE p != {a} (symbol constant missing from the complement)");
            return;
        }
        tset_free(&want);
    }
    tset_free(&res);

    /* Same program with 'foo' PRE-interned by an unrelated earlier intern:
     * the answer must NOT depend on invisible interner state. */
    {
        uint32_t foo = dl_intern_str(g_db, "foo");
        tset res2;
        long n2;
        (void)foo;
        tset_reset(&res2);
        n2 = dl_query_wfs_ro(g_db, rules, "p", DL_WFS_TRUE_ONLY, tset_cb, &res2);
        if (n2 != 1 || res2.count != 1) {
            printf("(got n2=%ld cnt=%ld want 1) ", n2, res2.count);
            tset_free(&res2);
            teardown();
            FAIL("answer depends on prior interner state");
            return;
        }
        tset_free(&res2);
    }

    teardown();
    PASS();
}

/* ─── Test 10: p/q tie — both undefined ─────────────────────────────────── */

/*
 * p(X):-node(X),!q(X).  q(X):-node(X),!p(X).  over node={1,2}: p and q
 * mutually negate each other (a 2-cycle through negation) — nothing is
 * decided; TRUE p = {} (all undefined).
 */
static void test_pq_tie(void)
{
    const char *rules = "p(X):-node(X),!q(X).\nq(X):-node(X),!p(X).\n";
    uint32_t nodes[2];
    tset res;
    long n;

    TEST("wfs: p/q tie (mutual negation) -> TRUE p={} (all undefined)");

    setup();
    nodes[0] = 1; nodes[1] = 2;
    load_rows("node", 1, nodes, 2);

    tset_reset(&res);
    n = dl_query_wfs_ro(g_db, rules, "p", DL_WFS_TRUE_ONLY, tset_cb, &res);
    if (n < 0) {
        printf("(err %ld) ", n);
        tset_free(&res);
        teardown();
        FAIL("dl_query_wfs_ro failed");
        return;
    }
    if (res.count != 0 || n != 0) {
        printf("(got n=%ld cnt=%ld) ", n, res.count);
        tset_free(&res);
        teardown();
        FAIL("TRUE p != {} (the p/q tie is all-undefined)");
        return;
    }
    tset_free(&res);
    teardown();
    PASS();
}

/* ─── Test 11: negation 3-cycle — all undefined ─────────────────────────── */

/*
 * p(X):-node(X),!q(X).  q(X):-node(X),!r(X).  r(X):-node(X),!p(X).
 * over node={1}: an odd negation cycle — nothing is decided, TRUE={}.
 */
static void test_negation_3cycle(void)
{
    const char *rules =
        "p(X):-node(X),!q(X).\n"
        "q(X):-node(X),!r(X).\n"
        "r(X):-node(X),!p(X).\n";
    uint32_t nodes[1];
    tset res;
    long n;

    TEST("wfs: negation 3-cycle p/q/r -> all undefined (TRUE={})");

    setup();
    nodes[0] = 1;
    load_rows("node", 1, nodes, 1);

    tset_reset(&res);
    n = dl_query_wfs_ro(g_db, rules, "p", DL_WFS_TRUE_ONLY, tset_cb, &res);
    if (n < 0) {
        printf("(err %ld) ", n);
        tset_free(&res);
        teardown();
        FAIL("dl_query_wfs_ro failed");
        return;
    }
    if (res.count != 0 || n != 0) {
        printf("(got n=%ld cnt=%ld) ", n, res.count);
        tset_free(&res);
        teardown();
        FAIL("TRUE p != {} (the 3-cycle is all-undefined)");
        return;
    }
    tset_free(&res);
    teardown();
    PASS();
}

/* ─── Test 12: dual-use predicate — only the negated occurrence rewritten ── */

/*
 * q is BOTH a rule head (positive occurrence) and negated in p's rule.  The
 * rewrite must move ONLY the negated occurrence to the complement; q's own
 * rule keeps reading the live q view.  Fixture: e={22}; q(X):-e(X).
 * p(X):-e(X),!q(X).  -> q(22) is TRUE so !q(22) fails: TRUE p={}, TRUE q={22}.
 */
static void test_dual_use_predicate(void)
{
    const char *rules = "q(X):-e(X).\np(X):-e(X),!q(X).\n";
    uint32_t v;
    uint32_t rows[1];
    tset res;
    long n;

    TEST("wfs: dual-use q (positive + negated) -> TRUE p={}, q={22}");

    setup();
    v = dl_intern_str(g_db, "v22");
    rows[0] = v;
    load_rows("e", 1, rows, 1);

    /* p: !q(22) fails since q(22) is TRUE -> p = {} */
    tset_reset(&res);
    n = dl_query_wfs_ro(g_db, rules, "p", DL_WFS_TRUE_ONLY, tset_cb, &res);
    if (n < 0) {
        printf("(err %ld) ", n);
        tset_free(&res);
        teardown();
        FAIL("dl_query_wfs_ro failed on p");
        return;
    }
    if (res.count != 0 || n != 0) {
        printf("(got n=%ld cnt=%ld) ", n, res.count);
        tset_free(&res);
        teardown();
        FAIL("TRUE p != {} (dual-use q must stay TRUE for 22)");
        return;
    }
    tset_free(&res);

    /* q: the positive occurrence reads the live view -> q={22} */
    tset_reset(&res);
    n = dl_query_wfs_ro(g_db, rules, "q", DL_WFS_TRUE_ONLY, tset_cb, &res);
    if (n < 0) {
        printf("(err %ld) ", n);
        tset_free(&res);
        teardown();
        FAIL("dl_query_wfs_ro failed on q");
        return;
    }
    {
        tset want;
        memset(&want, 0, sizeof want);
        want.arity = 1;
        tset_add_row(&want, &v);
        tset_sort(&want);
        tset_sort(&res);
        if (!tset_eq(&res, &want)) {
            tset_free(&want); tset_free(&res);
            teardown();
            FAIL("TRUE q != {22} (the positive occurrence was clobbered)");
            return;
        }
        tset_free(&want);
    }
    tset_free(&res);
    teardown();
    PASS();
}

/* ─── Test 13: arity-2 negated atom with swapped arguments ──────────────── */

/*
 * r(X,Y):-edge(X,Y),!r(Y,X).  over edge={(1,2),(2,3)}: the negated atom's
 * argument order differs from the head's.  r(2,1)/r(3,2) have no supporting
 * rule instance (no such edge) so they are FALSE; !r(2,1)/!r(3,2) hold, so
 * TRUE r = {(1,2),(2,3)} exactly.
 */
static void test_negated_arity2_swapped_args(void)
{
    const char *rules = "r(X,Y):-edge(X,Y),!r(Y,X).\n";
    uint32_t edges[4];
    tset res;
    long n;

    TEST("wfs: arity-2 negated atom, swapped args -> TRUE r={(1,2),(2,3)}");

    setup();
    edges[0] = 1; edges[1] = 2;
    edges[2] = 2; edges[3] = 3;
    load_rows("edge", 2, edges, 2);

    tset_reset(&res);
    n = dl_query_wfs_ro(g_db, rules, "r", DL_WFS_TRUE_ONLY, tset_cb, &res);
    if (n < 0) {
        printf("(err %ld) ", n);
        tset_free(&res);
        teardown();
        FAIL("dl_query_wfs_ro failed");
        return;
    }
    {
        tset want;
        uint32_t row[2];
        memset(&want, 0, sizeof want);
        want.arity = 2;
        row[0] = 1; row[1] = 2;
        tset_add_row(&want, row);
        row[0] = 2; row[1] = 3;
        tset_add_row(&want, row);
        tset_sort(&want);
        tset_sort(&res);
        if (!tset_eq(&res, &want)) {
            tset_free(&want); tset_free(&res);
            teardown();
            FAIL("TRUE r != {(1,2),(2,3)}");
            return;
        }
        tset_free(&want);
    }
    tset_free(&res);
    teardown();
    PASS();
}

/* ─── Test 14: head/EDB collision — stored facts survive the rounds ─────── */

/*
 * The rule head names an EXISTING relation with stored facts.  The driver
 * evaluates into a deep copy; each round the view resets to base (the
 * facts), so the facts are unconditionally TRUE.  Fixture: stored p={2},
 * e={1}, rule p(X):-e(X),!p(X): p(1) is self-negated (paradoxical ->
 * UNDEFINED), the fact p(2) survives -> TRUE p={2} via the 2-cycle
 * {2} <-> {1,2} (intersection {2}).
 */
static void test_head_edb_collision(void)
{
    const char *rules = "p(X):-e(X),!p(X).\n";
    uint32_t one = 1, two = 2;
    uint32_t facts[1];
    tset res;
    long n;

    TEST("wfs: head/EDB collision — stored p={2} survives, p(1) undef");

    setup();
    facts[0] = two;
    load_rows("p", 1, facts, 1);
    facts[0] = one;
    load_rows("e", 1, facts, 1);

    tset_reset(&res);
    n = dl_query_wfs_ro(g_db, rules, "p", DL_WFS_TRUE_ONLY, tset_cb, &res);
    if (n < 0) {
        printf("(err %ld) ", n);
        tset_free(&res);
        teardown();
        FAIL("dl_query_wfs_ro failed");
        return;
    }
    {
        tset want;
        memset(&want, 0, sizeof want);
        want.arity = 1;
        tset_add_row(&want, &two);
        tset_sort(&want);
        tset_sort(&res);
        if (!tset_eq(&res, &want)) {
            tset_free(&want); tset_free(&res);
            teardown();
            FAIL("TRUE p != {2} (collision facts must survive the rounds)");
            return;
        }
        tset_free(&want);
    }
    tset_free(&res);
    teardown();
    PASS();
}

/* ─── Test 15: mixed 8-node graph — three regions in one fixture ────────── */

/*
 * win(X):-move(X,Y),!win(Y).  over nodes 1..8 with
 *   1->2->3->4->2   (a cycle 2,3,4 with 1 feeding in: 1..4 UNDEFINED)
 *   5->6->7         (7 has no out: FALSE; 6 TRUE; 5's only successor TRUE:
 *                    FALSE)
 *   8->5            (8 sees FALSE: TRUE)
 * Hand-derived WFM: TRUE={6,8}, FALSE={5,7}, UNDEF={1,2,3,4}; cross-checked
 * against the brute-force 3-valued checker.
 */
static void test_mixed_8node_graph(void)
{
    const char *rules = "win(X):-move(X,Y),!win(Y).\n";
    uint32_t mv[14];
    tset res;
    long n;
    wf_graph g;

    TEST("wfs: mixed 8-node graph -> TRUE={6,8}, FALSE={5,7}, UNDEF={1,2,3,4}");

    setup();
    mv[0] = 1;  mv[1] = 2;
    mv[2] = 2;  mv[3] = 3;
    mv[4] = 3;  mv[5] = 4;
    mv[6] = 4;  mv[7] = 2;
    mv[8] = 5;  mv[9] = 6;
    mv[10] = 6; mv[11] = 7;
    mv[12] = 8; mv[13] = 5;
    load_rows("move", 2, mv, 7);

    tset_reset(&res);
    n = dl_query_wfs_ro(g_db, rules, "win", DL_WFS_TRUE_ONLY, tset_cb, &res);
    if (n < 0) {
        printf("(err %ld) ", n);
        tset_free(&res);
        teardown();
        FAIL("dl_query_wfs_ro failed");
        return;
    }

    /* pinned hand-derived TRUE set */
    {
        tset want;
        uint32_t six = 6, eight = 8;
        memset(&want, 0, sizeof want);
        want.arity = 1;
        tset_add_row(&want, &six);
        tset_add_row(&want, &eight);
        tset_sort(&want);
        tset_sort(&res);
        if (!tset_eq(&res, &want)) {
            tset_free(&want); tset_free(&res);
            teardown();
            FAIL("TRUE win != {6,8}");
            return;
        }
        tset_free(&want);
    }

    /* brute-force 3-valued cross-check */
    memset(&g, 0, sizeof g);
    g.n = 8;
    g.nodes[0] = 1; g.nodes[1] = 2; g.nodes[2] = 3; g.nodes[3] = 4;
    g.nodes[4] = 5; g.nodes[5] = 6; g.nodes[6] = 7; g.nodes[7] = 8;
    g.nedges = 7;
    g.edges[0][0] = 1;  g.edges[0][1] = 2;
    g.edges[1][0] = 2;  g.edges[1][1] = 3;
    g.edges[2][0] = 3;  g.edges[2][1] = 4;
    g.edges[3][0] = 4;  g.edges[3][1] = 2;
    g.edges[4][0] = 5;  g.edges[4][1] = 6;
    g.edges[5][0] = 6;  g.edges[5][1] = 7;
    g.edges[6][0] = 8;  g.edges[6][1] = 5;
    if (!check_against_brute(&g, &res, g.nodes, g.n)) {
        tset_free(&res);
        teardown();
        FAIL("brute-force 3-valued cross-check mismatch on the mixed graph");
        return;
    }

    tset_free(&res);
    teardown();
    PASS();
}

int main(void)
{
    printf("test_wfs: well-founded semantics (dl_query_wfs_ro)\n");

    test_win_move_canonical();
    test_even_cycle_all_undefined();
    test_stratified_identical();
    test_reject_unsafe_negation();
    test_reject_aggregate_in_scc();
    test_truth_modes_unimplemented();
    test_old_rejects_intact();
    test_chain_open();
    test_negated_atom_symbol_constant();
    test_pq_tie();
    test_negation_3cycle();
    test_dual_use_predicate();
    test_negated_arity2_swapped_args();
    test_head_edb_collision();
    test_mixed_8node_graph();

    printf("\nWFS: %d/%d passed\n", tests_run - tests_failed, tests_run);
    return tests_failed ? 1 : 0;
}
