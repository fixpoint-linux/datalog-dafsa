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
#include "vm.h"   /* vm_ivm_eligible — assert the recursive ruleset is eligible */

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

/* dl_prefix reads the IN-MEMORY view unconditionally (dl_query switches to
 * the mmap'd snapshot once one is published).  The equivalence oracle compares
 * the IVM-maintained in-memory views against the oracle's in-memory views, so
 * it must bypass the snapshot path. */
static void query_set_prefix(dl_db *db, const char *rel, tuple_set *out)
{
    memset(out, 0, sizeof(*out));
    dl_prefix(db, rel, NULL, 0, tset_cb, out);
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

/* ─── IVM Slice 1: insert-only incremental maintenance ─────────────────── */
/*
 * A fixed supported ruleset (non-recursive, negation-free, aggregate-free
 * join/projection DAG):
 *
 *   p(X,Z) :- edge(X,Z).
 *   p(X,Z) :- edge(X,Y), mid(Y,Z).
 *   q(X,W) :- p(X,Y), tail(Y,W).
 *   q(X,W) :- edge(X,Y), end(Y,W).
 *   r(X,V) :- q(X,U), fin(U,V).
 *
 * Every body atom compiles to OP_SCAN or OP_LOOKUP (leading-shared-column
 * join), so the whole program is IVM-insert-eligible: inserts propagate
 * through the DAG instead of triggering a full re-eval.
 */
#define IVM_RULES \
    "p(X,Z):-edge(X,Z).\n" \
    "p(X,Z):-edge(X,Y),mid(Y,Z).\n" \
    "q(X,W):-p(X,Y),tail(Y,W).\n" \
    "q(X,W):-edge(X,Y),end(Y,W).\n" \
    "r(X,V):-q(X,U),fin(U,V).\n"

static const char *IVM_EDB[5] = {"edge", "mid", "tail", "end", "fin"};

/* Deterministic xorshift32 PRNG (fixed seed — reproducible under make test). */
static uint32_t prng_state;
static uint32_t prng_next(void)
{
    uint32_t x = prng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    prng_state = x;
    return x;
}

/* Assert relation `rel` contains EXACTLY the `nrows` arity-2 rows given. */
static int check_rel2(dl_db *db, const char *rel,
                      const uint32_t *rows, int nrows)
{
    tuple_set ts;
    int i;
    query_set_prefix(db, rel, &ts);
    if (ts.count != nrows || ts.arity != 2) {
        printf("  %s: got %ld rows (arity %d), expected %d\n",
               rel, ts.count, ts.arity, nrows);
        long j;
        for (j = 0; j < ts.count; j++)
            printf("    (%u,%u)\n", ts.data[j*2], ts.data[j*2+1]);
        tset_free(&ts);
        return 0;
    }
    for (i = 0; i < nrows; i++) {
        if (!tset_has(&ts, &rows[(size_t)i * 2])) {
            printf("  %s: missing (%u,%u)\n", rel, rows[(size_t)i*2],
                   rows[(size_t)i*2 + 1]);
            tset_free(&ts);
            return 0;
        }
    }
    tset_free(&ts);
    return 1;
}

/* Set equality between two local tuple_sets (order-independent). */
static int sets_equal(const tuple_set *a, const tuple_set *b)
{
    long i;
    if (a->count != b->count || a->arity != b->arity) return 0;
    for (i = 0; i < a->count; i++)
        if (!tset_has(b, a->data + (size_t)i * a->arity)) return 0;
    return 1;
}

/* Compare the in-memory view of every relation across two databases. */
static int compare_all_views(dl_db *ivm, dl_db *oracle)
{
    const char *rels[] = {"edge", "mid", "tail", "end", "fin",
                          "p", "q", "r"};
    int i;
    for (i = 0; i < 8; i++) {
        tuple_set ta, tb;
        query_set_prefix(ivm, rels[i], &ta);
        query_set_prefix(oracle, rels[i], &tb);
        if (!sets_equal(&ta, &tb)) {
            printf("  %s: IVM %ld rows vs oracle %ld rows\n",
                   rels[i], ta.count, tb.count);
            long j;
            for (j = 0; j < ta.count; j++)
                printf("    ivm (%u,%u)\n", ta.data[j*2], ta.data[j*2+1]);
            for (j = 0; j < tb.count; j++)
                printf("    ora (%u,%u)\n", tb.data[j*2], tb.data[j*2+1]);
            tset_free(&ta); tset_free(&tb);
            return 0;
        }
        tset_free(&ta); tset_free(&tb);
    }
    return 1;
}

/* ─── T5: deterministic insert-only IVM (multi-level cascade + fan-out) ── */

static void test_insert_ivm_deterministic(void)
{
    dl_db *db;

    TEST("T5: insert-only IVM — deterministic cascade over the DAG");

    setup_db(&db, "t5");

    {
        int i;
        for (i = 0; i < 5; i++)
            assert(dl_declare_relation(db, IVM_EDB[i], 2) == 0);
    }
    assert(dl_load_rules(db, IVM_RULES) == 0);
    assert(dl_compile(db) == 0);

    /* Milestone 1: build the whole cascade in one publish (batched deltas). */
    {
        uint32_t edge12[]  = {1,2};
        uint32_t edge89[]  = {8,9};
        uint32_t mid23[]   = {2,3};
        uint32_t mid93[]   = {9,3};
        uint32_t tail34[]  = {3,4};
        uint32_t fin45[]   = {4,5};
        uint32_t end27[]   = {2,7};

        assert(dl_add_fact(db, "edge", edge12, 2) == 1);
        assert(dl_add_fact(db, "mid",  mid23, 2) == 1);
        assert(dl_add_fact(db, "tail", tail34, 2) == 1);
        assert(dl_add_fact(db, "fin",  fin45, 2) == 1);
        assert(dl_add_fact(db, "end",  end27, 2) == 1);
        assert(dl_add_fact(db, "edge", edge89, 2) == 1);
        assert(dl_add_fact(db, "mid",  mid93, 2) == 1);
    }
    assert(dl_publish_snapshot(db) == 0);

    {
        /* p = {(1,2),(1,3),(8,9),(8,3)} */
        uint32_t p[] = {1,2, 1,3, 8,9, 8,3};
        /* q = {(1,4),(1,7),(8,4)} */
        uint32_t q[] = {1,4, 1,7, 8,4};
        /* r = {(1,5),(8,5)} */
        uint32_t r[] = {1,5, 8,5};
        if (!check_rel2(db, "p", p, 4) ||
            !check_rel2(db, "q", q, 3) ||
            !check_rel2(db, "r", r, 2)) {
            teardown_db(db, "t5");
            FAIL("T5: milestone-1 cascade wrong");
            return;
        }
    }

    /* Milestone 2: a single incremental insert that triggers a fresh cascade
     * through ALREADY-PRESENT downstream facts (mid/tail/fin are all present,
     * so edge(10,11)+end(11,12) must derive p(10,11), q(10,12) — and NOT
     * disturb the earlier tuples). */
    {
        uint32_t edge10[] = {10,11};
        uint32_t end11[]  = {11,12};
        assert(dl_add_fact(db, "edge", edge10, 2) == 1);
        assert(dl_add_fact(db, "end",  end11, 2) == 1);
    }
    assert(dl_publish_snapshot(db) == 0);

    {
        uint32_t p[] = {1,2, 1,3, 8,9, 8,3, 10,11};
        uint32_t q[] = {1,4, 1,7, 8,4, 10,12};
        uint32_t r[] = {1,5, 8,5};
        if (!check_rel2(db, "p", p, 5) ||
            !check_rel2(db, "q", q, 4) ||
            !check_rel2(db, "r", r, 2)) {
            teardown_db(db, "t5");
            FAIL("T5: milestone-2 incremental cascade wrong");
            return;
        }
    }

    teardown_db(db, "t5");
    PASS();
}

/* ─── T6: equivalence-oracle property test (IVM vs full re-eval) ───────── */

static void test_insert_ivm_property(void)
{
    dl_db *ivm_db, *oracle_db;
    int iter, i;

    TEST("T6: insert-only IVM — seeded random property vs full re-eval oracle");

    setup_db(&ivm_db, "t6ivm");
    setup_db(&oracle_db, "t6ora");

    for (i = 0; i < 5; i++) {
        assert(dl_declare_relation(ivm_db, IVM_EDB[i], 2) == 0);
        assert(dl_declare_relation(oracle_db, IVM_EDB[i], 2) == 0);
    }
    assert(dl_load_rules(ivm_db, IVM_RULES) == 0);
    assert(dl_load_rules(oracle_db, IVM_RULES) == 0);
    assert(dl_compile(ivm_db) == 0);
    assert(dl_compile(oracle_db) == 0);

    prng_state = 0xC0FFEEu;

    for (iter = 0; iter < 120; iter++) {
        int rel = (int)(prng_next() % 5);
        uint32_t cols[2] = { prng_next() % 8, prng_next() % 8 };
        int rc;

        /* Identical random insert into both databases. */
        rc = dl_add_fact(ivm_db, IVM_EDB[rel], cols, 2);
        if (rc < 0) { printf("  IVM dl_add_fact error\n"); goto fail_prop; }
        rc = dl_add_fact(oracle_db, IVM_EDB[rel], cols, 2);
        if (rc < 0) { printf("  oracle dl_add_fact error\n"); goto fail_prop; }

        /* IVM: propagate on publish.  Oracle: full re-eval on compile. */
        if (dl_publish_snapshot(ivm_db) != 0) {
            printf("  publish failed at iter %d\n", iter);
            goto fail_prop;
        }
        if (dl_compile(oracle_db) != 0) {
            printf("  oracle compile failed at iter %d\n", iter);
            goto fail_prop;
        }

        if (!compare_all_views(ivm_db, oracle_db)) {
            printf("  divergence at iter %d (rel %s, +(%u,%u))\n",
                   iter, IVM_EDB[rel], cols[0], cols[1]);
            goto fail_prop;
        }
    }

    teardown_db(ivm_db, "t6ivm");
    teardown_db(oracle_db, "t6ora");
    PASS();
    return;

fail_prop:
    teardown_db(ivm_db, "t6ivm");
    teardown_db(oracle_db, "t6ora");
    FAIL("T6: IVM views diverged from full re-eval oracle");
}

/* ─── T7: mixed EDB+IDB insert falls back to full re-eval ──────────────── */

static void test_insert_mixed_head_fallback(void)
{
    dl_db *db;
    tuple_set ts;

    TEST("T7: base fact into a rule-head relation -> full re-eval (mixed)");

    setup_db(&db, "t7");

    assert(dl_declare_relation(db, "a", 1) == 0);
    {
        uint32_t one = 1, two = 2;
        assert(dl_add_fact(db, "a", &one, 1) == 1);
        assert(dl_add_fact(db, "a", &two, 1) == 1);
    }

    assert(dl_load_rules(db, "r(X):-a(X).\n") == 0);
    assert(dl_compile(db) == 0);

    /* Adding a base fact DIRECTLY to a rule-head relation (r is derived from a)
     * is the mixed EDB+IDB case: the fact must appear in r's view AND count as
     * a new tuple in r.  It is outside the Slice 1 insert class, so it must
     * set full_reeval_pending and re-derive via the full fixpoint — r must
     * become {1,2,3}, NOT stay {1,2} (which a naive delta-only propagate
     * would produce). */
    {
        uint32_t three = 3;
        assert(dl_add_fact(db, "r", &three, 1) == 1);
    }
    assert(dl_publish_snapshot(db) == 0);

    query_set_prefix(db, "r", &ts);
    if (ts.count != 3 || ts.arity != 1) {
        printf("  r got %ld rows, expected 3\n", ts.count);
        long j;
        for (j = 0; j < ts.count; j++) printf("    r(%u)\n", ts.data[j]);
        tset_free(&ts); teardown_db(db, "t7");
        FAIL("T7: mixed-head insert did not full re-eval");
        return;
    }
    {
        uint32_t v;
        for (v = 1; v <= 3; v++) {
            uint32_t row[1] = {v};
            if (!tset_has(&ts, row)) {
                printf("  r missing %u\n", v);
                tset_free(&ts); teardown_db(db, "t7");
                FAIL("T7: r missing value after mixed insert");
                return;
            }
        }
    }
    tset_free(&ts);

    teardown_db(db, "t7");
    PASS();
}

/* ─── IVM Slice 2: recursive insert-only incremental maintenance ──────── */

#define REC_RULES \
    "tc(X,Y):-edge(X,Y).\n" \
    "tc(X,Z):-edge(X,Y),tc(Y,Z).\n" \
    "anc(X,Y):-parent(X,Y).\n" \
    "anc(X,Z):-parent(X,Y),anc(Y,Z).\n"

static const char *REC_EDB[2] = {"edge", "parent"};

/* Compare the in-memory view of every relation across two databases for the
 * recursive ruleset (edge/parent + tc/anc, all arity 2). */
static int compare_rec_views(dl_db *ivm, dl_db *oracle)
{
    const char *rels[] = {"edge", "parent", "tc", "anc"};
    int i;
    for (i = 0; i < 4; i++) {
        tuple_set ta, tb;
        query_set_prefix(ivm, rels[i], &ta);
        query_set_prefix(oracle, rels[i], &tb);
        if (!sets_equal(&ta, &tb)) {
            printf("  %s: IVM %ld rows vs oracle %ld rows\n",
                   rels[i], ta.count, tb.count);
            long j;
            for (j = 0; j < ta.count; j++)
                printf("    ivm (%u,%u)\n", ta.data[j*2], ta.data[j*2+1]);
            for (j = 0; j < tb.count; j++)
                printf("    ora (%u,%u)\n", tb.data[j*2], tb.data[j*2+1]);
            tset_free(&ta); tset_free(&tb);
            return 0;
        }
        tset_free(&ta); tset_free(&tb);
    }
    return 1;
}

/* ─── T8: deterministic recursive insert IVM (incremental tc closure) ──── */

static void test_recursive_insert_ivm_deterministic(void)
{
    dl_db *db;
    tuple_set ts;

    TEST("T8: recursive insert IVM — incremental tc over a path graph");

    setup_db(&db, "t8");

    assert(dl_declare_relation(db, "edge", 2) == 0);
    assert(dl_load_rules(db,
        "tc(X,Y):-edge(X,Y).\n"
        "tc(X,Z):-edge(X,Y),tc(Y,Z).\n") == 0);
    assert(vm_ivm_eligible(db) == 1);  /* recursion is now IVM-eligible */
    assert(dl_compile(db) == 0);

    /* Insert the path 1->2->3->4->5 one edge at a time.  The closure of a
     * chain of k edges is C(k+1,2) pairs, so a one-step-only IVM (that fails
     * to run the fixpoint to convergence) would under-count at k>=3. */
    {
        uint32_t e12[2] = {1,2}, e23[2] = {2,3}, e34[2] = {3,4}, e45[2] = {4,5};
        uint32_t *edges[4] = {e12, e23, e34, e45};
        long expected[4] = {1, 3, 6, 10};
        int i;
        for (i = 0; i < 4; i++) {
            assert(dl_add_fact(db, "edge", edges[i], 2) == 1);
            assert(dl_publish_snapshot(db) == 0);
            query_set_prefix(db, "tc", &ts);
            if (ts.count != expected[i] || ts.arity != 2) {
                printf("  after insert %d: tc got %ld rows, expected %ld\n",
                       i, ts.count, expected[i]);
                tset_free(&ts); teardown_db(db, "t8");
                FAIL("T8: wrong tc closure size");
                return;
            }
            tset_free(&ts);
        }
    }

    /* Batched insert: two edges in one publish must extend the closure to the
     * full 7-node path (C(7,2)=21) — the delta seed must handle multiple new
     * base facts arriving together. */
    {
        uint32_t e56[2] = {5,6}, e67[2] = {6,7};
        assert(dl_add_fact(db, "edge", e56, 2) == 1);
        assert(dl_add_fact(db, "edge", e67, 2) == 1);
    }
    assert(dl_publish_snapshot(db) == 0);

    query_set_prefix(db, "tc", &ts);
    if (ts.count != 21 || ts.arity != 2) {
        printf("  batched: tc got %ld rows, expected 21\n", ts.count);
        tset_free(&ts); teardown_db(db, "t8");
        FAIL("T8: batched insert produced wrong tc closure");
        return;
    }
    {
        uint32_t t14[2] = {1,4}, t17[2] = {1,7}, t26[2] = {2,6};
        if (!tset_has(&ts, t14) || !tset_has(&ts, t17) || !tset_has(&ts, t26)) {
            printf("  missing multi-step closure tuples\n");
            tset_free(&ts); teardown_db(db, "t8");
            FAIL("T8: missing multi-step closure tuples");
            return;
        }
    }
    tset_free(&ts);

    teardown_db(db, "t8");
    PASS();
}

/* ─── T9: recursive insert IVM equivalence-oracle property ─────────────── */

static void test_recursive_ivm_property(void)
{
    dl_db *ivm_db, *oracle_db;
    int iter, i;

    TEST("T9: recursive insert IVM — seeded random property vs full re-eval");

    setup_db(&ivm_db, "t9ivm");
    setup_db(&oracle_db, "t9ora");

    for (i = 0; i < 2; i++) {
        assert(dl_declare_relation(ivm_db, REC_EDB[i], 2) == 0);
        assert(dl_declare_relation(oracle_db, REC_EDB[i], 2) == 0);
    }
    assert(dl_load_rules(ivm_db, REC_RULES) == 0);
    assert(dl_load_rules(oracle_db, REC_RULES) == 0);
    assert(vm_ivm_eligible(ivm_db) == 1);   /* tc + anc are IVM-eligible */
    assert(dl_compile(ivm_db) == 0);
    assert(dl_compile(oracle_db) == 0);

    prng_state = 0xBEEF1234u;

    for (iter = 0; iter < 120; iter++) {
        int rel = (int)(prng_next() % 2);
        uint32_t cols[2] = { prng_next() % 8, prng_next() % 8 };
        int rc;

        rc = dl_add_fact(ivm_db, REC_EDB[rel], cols, 2);
        if (rc < 0) { printf("  IVM dl_add_fact error\n"); goto fail_prop; }
        rc = dl_add_fact(oracle_db, REC_EDB[rel], cols, 2);
        if (rc < 0) { printf("  oracle dl_add_fact error\n"); goto fail_prop; }

        /* IVM: delta-seeded fixpoint on publish.  Oracle: full re-eval. */
        if (dl_publish_snapshot(ivm_db) != 0) {
            printf("  publish failed at iter %d\n", iter);
            goto fail_prop;
        }
        if (dl_compile(oracle_db) != 0) {
            printf("  oracle compile failed at iter %d\n", iter);
            goto fail_prop;
        }

        if (!compare_rec_views(ivm_db, oracle_db)) {
            printf("  divergence at iter %d (rel %s, +(%u,%u))\n",
                   iter, REC_EDB[rel], cols[0], cols[1]);
            goto fail_prop;
        }
    }

    teardown_db(ivm_db, "t9ivm");
    teardown_db(oracle_db, "t9ora");
    PASS();
    return;

fail_prop:
    teardown_db(ivm_db, "t9ivm");
    teardown_db(oracle_db, "t9ora");
    FAIL("T9: recursive IVM views diverged from full re-eval oracle");
}

/* ─── T10: recursive + negation safety net (falls back to full re-eval) ── */

static void test_recursive_negation_fallback(void)
{
    dl_db *db;
    tuple_set ts;

    TEST("T10: recursive + negation is ineligible -> full re-eval");

    setup_db(&db, "t10");

    assert(dl_declare_relation(db, "edge", 2) == 0);
    assert(dl_declare_relation(db, "blocked", 2) == 0);
    assert(dl_load_rules(db,
        "tc(X,Y):-edge(X,Y).\n"
        "tc(X,Z):-edge(X,Y),tc(Y,Z).\n"
        "ok(X,Y):-tc(X,Y),!blocked(X,Y).\n") == 0);

    /* The negated body atom makes the program ineligible for delta
     * propagation — inserts must fall back to the full re-eval fixpoint. */
    assert(vm_ivm_eligible(db) == 0);
    assert(dl_compile(db) == 0);

    {
        uint32_t e12[2] = {1,2}, e23[2] = {2,3}, b13[2] = {1,3};
        assert(dl_add_fact(db, "edge", e12, 2) == 1);
        assert(dl_add_fact(db, "edge", e23, 2) == 1);
        assert(dl_add_fact(db, "blocked", b13, 2) == 1);
    }
    assert(dl_publish_snapshot(db) == 0);

    /* tc = {(1,2),(2,3),(1,3)}; ok = tc \ blocked = {(1,2),(2,3)}. */
    query_set_prefix(db, "tc", &ts);
    if (ts.count != 3 || ts.arity != 2) {
        printf("  tc got %ld rows, expected 3\n", ts.count);
        tset_free(&ts); teardown_db(db, "t10");
        FAIL("T10: wrong tc after full re-eval fallback");
        return;
    }
    tset_free(&ts);

    query_set_prefix(db, "ok", &ts);
    if (ts.count != 2 || ts.arity != 2) {
        printf("  ok got %ld rows, expected 2\n", ts.count);
        tset_free(&ts); teardown_db(db, "t10");
        FAIL("T10: wrong ok after full re-eval fallback");
        return;
    }
    {
        uint32_t b13[2] = {1,3};
        if (tset_has(&ts, b13)) {
            printf("  ok wrongly contains blocked (1,3)\n");
            tset_free(&ts); teardown_db(db, "t10");
            FAIL("T10: negation not applied");
            return;
        }
    }
    tset_free(&ts);

    teardown_db(db, "t10");
    PASS();
}

/* ─── T11: mutual recursion (multi-head SCC) insert IVM ────────────────── */

static void test_mutual_recursion_insert_ivm(void)
{
    dl_db *db;
    tuple_set ts;
    int i;

    TEST("T11: mutual recursion insert IVM — even/odd over succ");

    setup_db(&db, "t11");

    /* EDB-only seed: zero(0) is the exit rule's base; succ chains 0..10. */
    {
        uint32_t zero = 0;
        load_rows(db, "t11", "zero", 1, &zero, 1);
    }
    {
        uint32_t succ[20];
        for (i = 0; i < 10; i++) { succ[i*2] = (uint32_t)i; succ[i*2+1] = (uint32_t)(i+1); }
        load_rows(db, "t11", "succ", 2, succ, 10);
    }

    assert(dl_load_rules(db,
        "even(X):-zero(X).\n"
        "even(X):-succ(Y,X),odd(Y).\n"
        "odd(X):-succ(Y,X),even(Y).\n") == 0);
    assert(vm_ivm_eligible(db) == 1);
    assert(dl_compile(db) == 0);

    /* Baseline: even={0,2,4,6,8,10}, odd={1,3,5,7,9}. */
    query_set_prefix(db, "even", &ts);
    if (ts.count != 6) {
        printf("  baseline even got %ld rows, expected 6\n", ts.count);
        tset_free(&ts); teardown_db(db, "t11");
        FAIL("T11: wrong baseline even");
        return;
    }
    tset_free(&ts);

    /* Insert succ(10,11): only odd(11) is newly derivable (11 is odd). */
    {
        uint32_t s10[2] = {10, 11};
        assert(dl_add_fact(db, "succ", s10, 2) == 1);
    }
    assert(dl_publish_snapshot(db) == 0);

    query_set_prefix(db, "even", &ts);
    if (ts.count != 6) {
        printf("  after insert even got %ld rows, expected 6\n", ts.count);
        tset_free(&ts); teardown_db(db, "t11");
        FAIL("T11: even changed on odd-only insert");
        return;
    }
    tset_free(&ts);

    query_set_prefix(db, "odd", &ts);
    if (ts.count != 6 || !tset_has(&ts, (uint32_t[]){11})) {
        printf("  after insert odd got %ld rows, expected 6 incl. odd(11)\n", ts.count);
        tset_free(&ts); teardown_db(db, "t11");
        FAIL("T11: odd(11) missing after succ(10,11)");
        return;
    }
    tset_free(&ts);

    teardown_db(db, "t11");
    PASS();
}

/* ─── T12: non-recursive head feeding recursion -> full re-eval fallback ── */

static void test_nonrecursive_head_feeds_recursion_fallback(void)
{
    dl_db *db;
    tuple_set ts;

    TEST("T12: non-recursive head feeding recursion is ineligible -> full re-eval");

    setup_db(&db, "t12");

    assert(dl_declare_relation(db, "edge", 2) == 0);
    assert(dl_load_rules(db,
        "helper(X,Y):-edge(X,Y).\n"
        "tc(X,Y):-helper(X,Y).\n"
        "tc(X,Z):-helper(X,Y),tc(Y,Z).\n") == 0);

    /* tc's base atom reads `helper` (a non-recursive rule head) — the delta
     * seed cannot track helper's re-derived tuples (no per-insert delta for a
     * rule head), so this program is ineligible and inserts must fall back to
     * the full re-eval fixpoint (never silently mis-evaluate). */
    assert(vm_ivm_eligible(db) == 0);
    assert(dl_compile(db) == 0);

    {
        uint32_t e12[2] = {1,2}, e23[2] = {2,3};
        assert(dl_add_fact(db, "edge", e12, 2) == 1);
        assert(dl_add_fact(db, "edge", e23, 2) == 1);
    }
    assert(dl_publish_snapshot(db) == 0);

    /* helper = {(1,2),(2,3)}; tc = helper ∪ {(1,3)} = 3 rows. */
    query_set_prefix(db, "tc", &ts);
    if (ts.count != 3 || ts.arity != 2) {
        printf("  tc got %ld rows, expected 3\n", ts.count);
        tset_free(&ts); teardown_db(db, "t12");
        FAIL("T12: wrong tc after full re-eval fallback");
        return;
    }
    {
        uint32_t t13[2] = {1,3};
        if (!tset_has(&ts, t13)) {
            printf("  missing tc(1,3)\n");
            tset_free(&ts); teardown_db(db, "t12");
            FAIL("T12: missing tc(1,3)");
            return;
        }
    }
    tset_free(&ts);

    teardown_db(db, "t12");
    PASS();
}

/* ─── IVM Slice 3: DRed deletion (over-delete + re-derive) ──────────────── */

/* T13: deterministic DRed delete over the monotone DAG (IVM_RULES).
 * Deleting mid(2,3) must retract the multi-level cascade p(1,3) -> q(1,4)
 * -> r(1,5) while keeping every tuple with alternative support, and a
 * re-insert must restore the full cascade through the Slice-1 insert path
 * (DRed-maintained state feeds the insert propagator). */
static void test_dred_delete_deterministic(void)
{
    dl_db *db;

    TEST("T13: DRed delete — deterministic cascade retraction over the DAG");

    setup_db(&db, "t13");

    {
        int i;
        for (i = 0; i < 5; i++)
            assert(dl_declare_relation(db, IVM_EDB[i], 2) == 0);
    }
    assert(dl_load_rules(db, IVM_RULES) == 0);
    assert(vm_dred_eligible(db) == 1);   /* monotone non-recursive: DRed */
    assert(dl_compile(db) == 0);

    /* Same seed as T5 milestone 1. */
    {
        uint32_t edge12[]  = {1,2};
        uint32_t edge89[]  = {8,9};
        uint32_t mid23[]   = {2,3};
        uint32_t mid93[]   = {9,3};
        uint32_t tail34[]  = {3,4};
        uint32_t fin45[]   = {4,5};
        uint32_t end27[]   = {2,7};

        assert(dl_add_fact(db, "edge", edge12, 2) == 1);
        assert(dl_add_fact(db, "mid",  mid23, 2) == 1);
        assert(dl_add_fact(db, "tail", tail34, 2) == 1);
        assert(dl_add_fact(db, "fin",  fin45, 2) == 1);
        assert(dl_add_fact(db, "end",  end27, 2) == 1);
        assert(dl_add_fact(db, "edge", edge89, 2) == 1);
        assert(dl_add_fact(db, "mid",  mid93, 2) == 1);
    }
    assert(dl_publish_snapshot(db) == 0);

    /* Delete mid(2,3):
     *   p loses (1,3)  [p(1,3):-edge(1,2),mid(2,3)]  but keeps (1,2) [edge
     *     alone], (8,9) [edge alone], (8,3) [mid(9,3) survives].
     *   q loses (1,4)  [q(1,4):-p(1,3),tail(3,4)]    but keeps (1,7),(8,4).
     *   r loses (1,5)  [r(1,5):-q(1,4),fin(4,5)]     but keeps (8,5).
     * A single-level retraction (only p) or a one-shot re-derive that fails
     * to cascade would leave (1,4)/(1,5) stale. */
    {
        int runs0 = vm_dred_runs;
        uint32_t m23[2] = {2,3};
        assert(dl_delete_fact(db, "mid", m23, 2) == 1);
        assert(dl_publish_snapshot(db) == 0);
        /* PROVE the DRed path ran (a silent full-re-eval fallback would also
         * produce correct views — the counter distinguishes them). */
        if (vm_dred_runs != runs0 + 1) {
            teardown_db(db, "t13");
            FAIL("T13: delete did not route through DRed");
            return;
        }
    }

    {
        uint32_t p[] = {1,2, 8,9, 8,3};
        uint32_t q[] = {1,7, 8,4};
        uint32_t r[] = {8,5};
        if (!check_rel2(db, "p", p, 3) ||
            !check_rel2(db, "q", q, 2) ||
            !check_rel2(db, "r", r, 1)) {
            teardown_db(db, "t13");
            FAIL("T13: cascade retraction wrong after delete");
            return;
        }
    }

    /* Re-insert mid(2,3): the insert-only path (Slice 1) must restore the
     * whole cascade on top of the DRed-maintained views. */
    {
        int runs0 = vm_dred_runs;
        uint32_t m23[2] = {2,3};
        assert(dl_add_fact(db, "mid", m23, 2) == 1);
        assert(dl_publish_snapshot(db) == 0);
        /* Insert-only on an insert-IVM-eligible program routes to the
         * Slice-1 propagator, NOT DRed. */
        if (vm_dred_runs != runs0) {
            teardown_db(db, "t13");
            FAIL("T13: insert wrongly routed through DRed");
            return;
        }
    }

    {
        uint32_t p[] = {1,2, 1,3, 8,9, 8,3};
        uint32_t q[] = {1,4, 1,7, 8,4};
        uint32_t r[] = {1,5, 8,5};
        if (!check_rel2(db, "p", p, 4) ||
            !check_rel2(db, "q", q, 3) ||
            !check_rel2(db, "r", r, 2)) {
            teardown_db(db, "t13");
            FAIL("T13: cascade not restored after re-insert");
            return;
        }
    }

    /* Delete edge(1,2) too: now (1,2) AND (1,3) leave p, (1,4),(1,7) leave
     * q, (1,5) leaves r — everything from the 1-branch. */
    {
        uint32_t e12[2] = {1,2};
        assert(dl_delete_fact(db, "edge", e12, 2) == 1);
    }
    assert(dl_publish_snapshot(db) == 0);

    {
        uint32_t p[] = {8,9, 8,3};
        uint32_t q[] = {8,4};
        uint32_t r[] = {8,5};
        if (!check_rel2(db, "p", p, 2) ||
            !check_rel2(db, "q", q, 1) ||
            !check_rel2(db, "r", r, 1)) {
            teardown_db(db, "t13");
            FAIL("T13: 1-branch retraction wrong");
            return;
        }
    }

    teardown_db(db, "t13");
    PASS();
}

/* ─── T14: DRed with stratified negation (deterministic) ───────────────── */

#define NEG_RULES \
    "mid(X,Y):-edge(X,Y).\n" \
    "ok(X,Y):-mid(X,Y),!blocked(X,Y).\n" \
    "top(X,Y):-ok(X,Y),!cut(X,Y).\n"

static const char *NEG_EDB[3] = {"edge", "blocked", "cut"};

static void test_dred_negation_deterministic(void)
{
    dl_db *db;

    TEST("T14: DRed + stratified negation — unblock/retract/cascade");

    setup_db(&db, "t14");

    {
        int i;
        for (i = 0; i < 3; i++)
            assert(dl_declare_relation(db, NEG_EDB[i], 2) == 0);
    }
    assert(dl_load_rules(db, NEG_RULES) == 0);
    /* Negation: NOT insert-IVM-eligible, but DRed-eligible (non-recursive). */
    assert(vm_ivm_eligible(db) == 0);
    assert(vm_dred_eligible(db) == 1);
    assert(dl_compile(db) == 0);

    /* Seed: edge={(1,2),(2,3),(3,4)}, blocked={(2,3)}, cut={(1,2)}.
     * mid=edge (3 rows); ok=mid\blocked={(1,2),(3,4)}; top=ok\cut={(3,4)}. */
    {
        uint32_t e12[2] = {1,2}, e23[2] = {2,3}, e34[2] = {3,4};
        uint32_t b23[2] = {2,3}, c12[2] = {1,2};
        assert(dl_add_fact(db, "edge",    e12, 2) == 1);
        assert(dl_add_fact(db, "edge",    e23, 2) == 1);
        assert(dl_add_fact(db, "edge",    e34, 2) == 1);
        assert(dl_add_fact(db, "blocked", b23, 2) == 1);
        assert(dl_add_fact(db, "cut",     c12, 2) == 1);
    }
    assert(dl_publish_snapshot(db) == 0);

    {
        uint32_t mid[] = {1,2, 2,3, 3,4};
        uint32_t ok[]  = {1,2, 3,4};
        uint32_t top[] = {3,4};
        if (!check_rel2(db, "mid", mid, 3) ||
            !check_rel2(db, "ok",  ok,  2) ||
            !check_rel2(db, "top", top, 1)) {
            teardown_db(db, "t14");
            FAIL("T14: seed derivation wrong");
            return;
        }
    }

    /* Step 1: DELETE blocked(2,3) — the negated body atom becomes TRUE, so
     * ok(2,3) becomes newly derivable (an ADD caused by a delete) and
     * top(2,3) follows in the higher stratum. */
    {
        int runs0 = vm_dred_runs;
        uint32_t b23[2] = {2,3};
        assert(dl_delete_fact(db, "blocked", b23, 2) == 1);
        assert(dl_publish_snapshot(db) == 0);
        if (vm_dred_runs != runs0 + 1) {
            teardown_db(db, "t14");
            FAIL("T14: delete did not route through DRed");
            return;
        }
    }

    {
        uint32_t mid[] = {1,2, 2,3, 3,4};
        uint32_t ok[]  = {1,2, 2,3, 3,4};
        uint32_t top[] = {2,3, 3,4};
        if (!check_rel2(db, "mid", mid, 3) ||
            !check_rel2(db, "ok",  ok,  3) ||
            !check_rel2(db, "top", top, 2)) {
            teardown_db(db, "t14");
            FAIL("T14: negation-unblock ADD missing after delete");
            return;
        }
    }

    /* Step 2: INSERT blocked(1,2) — a negated body atom becomes FALSE, so
     * ok(1,2) must be RETRACTED (and top stays without (1,2), which cut
     * already blocked).  This is the insert-retraction direction DRed
     * handles via the conservative full view reset of negation heads.
     * The program is NOT insert-IVM-eligible (negation), so the insert
     * itself routes through DRed here. */
    {
        int runs0 = vm_dred_runs;
        uint32_t b12[2] = {1,2};
        assert(dl_add_fact(db, "blocked", b12, 2) == 1);
        assert(dl_publish_snapshot(db) == 0);
        if (vm_dred_runs != runs0 + 1) {
            teardown_db(db, "t14");
            FAIL("T14: negation insert did not route through DRed");
            return;
        }
    }

    {
        uint32_t mid[] = {1,2, 2,3, 3,4};
        uint32_t ok[]  = {2,3, 3,4};
        uint32_t top[] = {2,3, 3,4};
        if (!check_rel2(db, "mid", mid, 3) ||
            !check_rel2(db, "ok",  ok,  2) ||
            !check_rel2(db, "top", top, 2)) {
            teardown_db(db, "t14");
            FAIL("T14: stale ok(1,2) after insert into negated relation");
            return;
        }
    }

    /* Step 3: DELETE edge(2,3) — a positive-cascade delete WITH negation
     * present: mid(2,3) and ok(2,3) must vanish. */
    {
        uint32_t e23[2] = {2,3};
        assert(dl_delete_fact(db, "edge", e23, 2) == 1);
    }
    assert(dl_publish_snapshot(db) == 0);

    {
        uint32_t mid[] = {1,2, 3,4};
        uint32_t ok[]  = {3,4};
        uint32_t top[] = {3,4};
        if (!check_rel2(db, "mid", mid, 2) ||
            !check_rel2(db, "ok",  ok,  1) ||
            !check_rel2(db, "top", top, 1)) {
            teardown_db(db, "t14");
            FAIL("T14: positive cascade wrong with negation present");
            return;
        }
    }

    teardown_db(db, "t14");
    PASS();
}

/* ─── T15: DRed equivalence-oracle property (monotone DAG, add+delete) ──── */

static void test_dred_property_monotone(void)
{
    dl_db *ivm_db, *oracle_db;
    int iter, i;

    TEST("T15: DRed — seeded random add/delete property vs full re-eval");

    setup_db(&ivm_db, "t15ivm");
    setup_db(&oracle_db, "t15ora");

    for (i = 0; i < 5; i++) {
        assert(dl_declare_relation(ivm_db, IVM_EDB[i], 2) == 0);
        assert(dl_declare_relation(oracle_db, IVM_EDB[i], 2) == 0);
    }
    assert(dl_load_rules(ivm_db, IVM_RULES) == 0);
    assert(dl_load_rules(oracle_db, IVM_RULES) == 0);
    assert(vm_dred_eligible(ivm_db) == 1);
    assert(dl_compile(ivm_db) == 0);
    assert(dl_compile(oracle_db) == 0);

    prng_state = 0xD00DFEEDu;

    {
        int runs0 = vm_dred_runs;
        int expect_dred = 0;   /* exactly the successful-delete iterations */

        for (iter = 0; iter < 150; iter++) {
            int rel = (int)(prng_next() % 5);
            int is_del = (int)(prng_next() & 1u);
            uint32_t cols[2] = { prng_next() % 8, prng_next() % 8 };
            int rc;

            /* Identical random add-or-delete into both databases. */
            if (is_del) {
                rc = dl_delete_fact(ivm_db, IVM_EDB[rel], cols, 2);
                if (rc < 0) { printf("  IVM dl_delete_fact error\n"); goto fail_prop; }
                if (rc == 1) expect_dred++;
                rc = dl_delete_fact(oracle_db, IVM_EDB[rel], cols, 2);
                if (rc < 0) { printf("  oracle dl_delete_fact error\n"); goto fail_prop; }
            } else {
                rc = dl_add_fact(ivm_db, IVM_EDB[rel], cols, 2);
                if (rc < 0) { printf("  IVM dl_add_fact error\n"); goto fail_prop; }
                rc = dl_add_fact(oracle_db, IVM_EDB[rel], cols, 2);
                if (rc < 0) { printf("  oracle dl_add_fact error\n"); goto fail_prop; }
            }

            /* IVM: DRed/propagation on publish.  Oracle: full re-eval. */
            if (dl_publish_snapshot(ivm_db) != 0) {
                printf("  publish failed at iter %d\n", iter);
                goto fail_prop;
            }
            if (dl_compile(oracle_db) != 0) {
                printf("  oracle compile failed at iter %d\n", iter);
                goto fail_prop;
            }

            if (!compare_all_views(ivm_db, oracle_db)) {
                printf("  divergence at iter %d (rel %s, %s (%u,%u))\n",
                       iter, IVM_EDB[rel], is_del ? "DEL" : "ADD",
                       cols[0], cols[1]);
                goto fail_prop;
            }
        }

        /* Every successful delete ran DRed (deletes never fall back on this
         * monotone non-recursive program); every add used the Slice-1
         * propagator instead.  Exact count proves the routing. */
        if (vm_dred_runs != runs0 + expect_dred) {
            printf("  DRed ran %d times, expected %d\n",
                   vm_dred_runs - runs0, expect_dred);
            goto fail_prop;
        }
    }

    teardown_db(ivm_db, "t15ivm");
    teardown_db(oracle_db, "t15ora");
    PASS();
    return;

fail_prop:
    teardown_db(ivm_db, "t15ivm");
    teardown_db(oracle_db, "t15ora");
    FAIL("T15: DRed views diverged from full re-eval oracle");
}

/* ─── T16: DRed equivalence-oracle property (stratified negation) ───────── */

static int compare_neg_views(dl_db *ivm, dl_db *oracle)
{
    const char *rels[] = {"edge", "blocked", "cut", "mid", "ok", "top"};
    int i;
    for (i = 0; i < 6; i++) {
        tuple_set ta, tb;
        query_set_prefix(ivm, rels[i], &ta);
        query_set_prefix(oracle, rels[i], &tb);
        if (!sets_equal(&ta, &tb)) {
            printf("  %s: IVM %ld rows vs oracle %ld rows\n",
                   rels[i], ta.count, tb.count);
            long j;
            for (j = 0; j < ta.count; j++)
                printf("    ivm (%u,%u)\n", ta.data[j*2], ta.data[j*2+1]);
            for (j = 0; j < tb.count; j++)
                printf("    ora (%u,%u)\n", tb.data[j*2], tb.data[j*2+1]);
            tset_free(&ta); tset_free(&tb);
            return 0;
        }
        tset_free(&ta); tset_free(&tb);
    }
    return 1;
}

static void test_dred_property_negation(void)
{
    dl_db *ivm_db, *oracle_db;
    int iter, i;

    TEST("T16: DRed + negation — seeded random add/delete property vs oracle");

    setup_db(&ivm_db, "t16ivm");
    setup_db(&oracle_db, "t16ora");

    for (i = 0; i < 3; i++) {
        assert(dl_declare_relation(ivm_db, NEG_EDB[i], 2) == 0);
        assert(dl_declare_relation(oracle_db, NEG_EDB[i], 2) == 0);
    }
    assert(dl_load_rules(ivm_db, NEG_RULES) == 0);
    assert(dl_load_rules(oracle_db, NEG_RULES) == 0);
    assert(vm_ivm_eligible(ivm_db) == 0);    /* negation: insert path out */
    assert(vm_dred_eligible(ivm_db) == 1);   /* DRed handles it           */
    assert(dl_compile(ivm_db) == 0);
    assert(dl_compile(oracle_db) == 0);

    prng_state = 0xFACEB00Cu;

    {
        int runs0 = vm_dred_runs;
        int expect_dred = 0;   /* every effective change (add OR delete):
                                * inserts also route to DRed here because the
                                * negation program is not insert-IVM-eligible */

        for (iter = 0; iter < 150; iter++) {
            int rel = (int)(prng_next() % 3);
            int is_del = (int)(prng_next() & 1u);
            uint32_t cols[2] = { prng_next() % 6, prng_next() % 6 };
            int rc;

            if (is_del) {
                rc = dl_delete_fact(ivm_db, NEG_EDB[rel], cols, 2);
                if (rc < 0) { printf("  IVM dl_delete_fact error\n"); goto fail_prop; }
                if (rc == 1) expect_dred++;
                rc = dl_delete_fact(oracle_db, NEG_EDB[rel], cols, 2);
                if (rc < 0) { printf("  oracle dl_delete_fact error\n"); goto fail_prop; }
            } else {
                rc = dl_add_fact(ivm_db, NEG_EDB[rel], cols, 2);
                if (rc < 0) { printf("  IVM dl_add_fact error\n"); goto fail_prop; }
                if (rc == 1) expect_dred++;
                rc = dl_add_fact(oracle_db, NEG_EDB[rel], cols, 2);
                if (rc < 0) { printf("  oracle dl_add_fact error\n"); goto fail_prop; }
            }

            if (dl_publish_snapshot(ivm_db) != 0) {
                printf("  publish failed at iter %d\n", iter);
                goto fail_prop;
            }
            if (dl_compile(oracle_db) != 0) {
                printf("  oracle compile failed at iter %d\n", iter);
                goto fail_prop;
            }

            if (!compare_neg_views(ivm_db, oracle_db)) {
                printf("  divergence at iter %d (rel %s, %s (%u,%u))\n",
                       iter, NEG_EDB[rel], is_del ? "DEL" : "ADD",
                       cols[0], cols[1]);
                goto fail_prop;
            }
        }

        if (vm_dred_runs != runs0 + expect_dred) {
            printf("  DRed ran %d times, expected %d\n",
                   vm_dred_runs - runs0, expect_dred);
            goto fail_prop;
        }
    }

    teardown_db(ivm_db, "t16ivm");
    teardown_db(oracle_db, "t16ora");
    PASS();
    return;

fail_prop:
    teardown_db(ivm_db, "t16ivm");
    teardown_db(oracle_db, "t16ora");
    FAIL("T16: DRed+negation views diverged from full re-eval oracle");
}

/* ─── T17: DRed eligibility boundary — recursion/aggregates fall back ───── */

static void test_dred_fallback_eligibility(void)
{
    dl_db *db;

    TEST("T17: recursive / aggregate programs are DRed-ineligible -> fallback");

    setup_db(&db, "t17");

    /* Recursive: the over-delete cascade cannot retract a recursive SCC's
     * mutually-dependent tuples — must fall back to the full fixpoint. */
    assert(dl_declare_relation(db, "edge", 2) == 0);
    assert(dl_load_rules(db,
        "tc(X,Y):-edge(X,Y).\n"
        "tc(X,Z):-edge(X,Y),tc(Y,Z).\n") == 0);
    assert(vm_dred_eligible(db) == 0);

    /* Aggregate: group state (min/max of a deleted extremum) cannot be
     * maintained by over-delete + re-add — must fall back. */
    {
        dl_db *adb;
        setup_db(&adb, "t17a");
        assert(dl_declare_relation(adb, "sale", 2) == 0);
        assert(dl_load_rules(adb, "total(X,S):-sale(X,Y),S=sum(Y).\n") == 0);
        if (vm_dred_eligible(adb) != 0) {
            teardown_db(adb, "t17a");
            teardown_db(db, "t17");
            FAIL("T17: aggregate program wrongly DRed-eligible");
            return;
        }
        teardown_db(adb, "t17a");
    }

    /* The fallback itself stays correct (pinned end-to-end by T1/T2; here a
     * small recursive delete through the full re-eval path). */
    {
        uint32_t e12[2] = {1,2}, e23[2] = {2,3};
        assert(dl_add_fact(db, "edge", e12, 2) == 1);
        assert(dl_add_fact(db, "edge", e23, 2) == 1);
    }
    assert(dl_publish_snapshot(db) == 0);
    {
        uint32_t tc[] = {1,2, 2,3, 1,3};
        if (!check_rel2(db, "tc", tc, 3)) {
            teardown_db(db, "t17");
            FAIL("T17: wrong tc before delete");
            return;
        }
    }
    {
        uint32_t e23[2] = {2,3};
        assert(dl_delete_fact(db, "edge", e23, 2) == 1);
    }
    assert(dl_publish_snapshot(db) == 0);
    {
        uint32_t tc[] = {1,2};
        if (!check_rel2(db, "tc", tc, 1)) {
            teardown_db(db, "t17");
            FAIL("T17: recursive delete did not fall back correctly");
            return;
        }
    }

    teardown_db(db, "t17");
    PASS();
}

/* ─── T18: DRed over-delete with a same-stratum OP_LOOKUP consumer ───────
 * Regression for a confirmed blocker: the over-delete cascade grew over[hid]
 * via ts_add (unsorted) and only sorted it AFTER the in-stratum fixpoint
 * converged.  A same-stratum consumer reading over[hid] as an OP_LOOKUP
 * override (ts_prefix binary search) silently missed tuples -> incomplete
 * over-delete -> stale survivors.  Here q(X):-b(X),p(X) reads p (over-delete
 * set) via OP_LOOKUP while p(X):-a(X). p(X):-c(X). both feed it in the same
 * stratum. */
static void test_dred_overdelete_lookup_same_stratum(void)
{
    dl_db *db;

    TEST("T18: DRed over-delete stays correct when a consumer reads via OP_LOOKUP");

    setup_db(&db, "t18");
    assert(dl_declare_relation(db, "a", 1) == 0);
    assert(dl_declare_relation(db, "b", 1) == 0);
    assert(dl_declare_relation(db, "c", 1) == 0);
    {
        uint32_t v;
        v = 5; assert(dl_add_fact(db, "a", &v, 1) == 1);
        uint32_t bv[3] = {1, 3, 5};
        for (int i = 0; i < 3; i++) { v = bv[i]; assert(dl_add_fact(db, "b", &v, 1) == 1); }
        v = 1; assert(dl_add_fact(db, "c", &v, 1) == 1);
    }
    assert(dl_load_rules(db,
        "p(X):-a(X).\n"
        "p(X):-c(X).\n"
        "q(X):-b(X),p(X).\n") == 0);
    assert(dl_compile(db) == 0);
    assert(dl_publish_snapshot(db) == 0);

    /* q = b ∩ p = {1,3,5} ∩ {1,5} = {1,5} (2 rows). */
    {
        tuple_set ts = {0};
        long n = dl_query(db, "q", tset_cb, &ts);
        if (n < 0 || ts.count != 2) {
            FAIL("T18: wrong q before delete"); tset_free(&ts); teardown_db(db, "t18"); return;
        }
        tset_free(&ts);
    }

    /* Delete a(5) and c(1): p becomes empty, so q must become empty.  A stale
     * over-delete would leave q={1,5} (silent wrong answer). */
    {
        uint32_t v = 5;
        assert(dl_delete_fact(db, "a", &v, 1) == 1);
        v = 1;
        assert(dl_delete_fact(db, "c", &v, 1) == 1);
    }
    assert(dl_publish_snapshot(db) == 0);
    {
        tuple_set ts = {0};
        long n = dl_query(db, "q", tset_cb, &ts);
        if (n < 0 || ts.count != 0) {
            FAIL("T18: stale over-delete survivors (OP_LOOKUP consumer)");
            tset_free(&ts); teardown_db(db, "t18"); return;
        }
        tset_free(&ts);
    }

    teardown_db(db, "t18");
    PASS();
}

/* ─── IVM Slice 4: aggregates under change ──────────────────────────────── */

/* Four grouped aggregates over a single anchor `sale/2`, grouped by the
 * leading column X.  Each head is TERMINAL and pure-derived, so the whole
 * program is aggregate-IVM-eligible (vm_agg_eligible == 1). */
#define AGG_RULES \
    "cnt(X,N):-sale(X,Y),N=count().\n" \
    "tot(X,S):-sale(X,Y),S=sum(Y).\n" \
    "minv(X,M):-sale(X,Y),M=min(Y).\n" \
    "maxv(X,M):-sale(X,Y),M=max(Y).\n"

/* Compare the in-memory views of the aggregate ruleset's relations. */
static int compare_agg_views(dl_db *ivm, dl_db *oracle)
{
    const char *rels[] = {"sale", "cnt", "tot", "minv", "maxv"};
    int i;
    for (i = 0; i < 5; i++) {
        tuple_set ta, tb;
        query_set_prefix(ivm, rels[i], &ta);
        query_set_prefix(oracle, rels[i], &tb);
        if (!sets_equal(&ta, &tb)) {
            printf("  %s: IVM %ld rows vs oracle %ld rows\n",
                   rels[i], ta.count, tb.count);
            tset_free(&ta); tset_free(&tb);
            return 0;
        }
        tset_free(&ta); tset_free(&tb);
    }
    return 1;
}

/* T19: deterministic aggregate IVM — count/sum/min/max under add/delete,
 * including delete-an-extremum (min/max) and the empty-group edge, with a
 * routing-proof assertion that the aggregate path (not full re-eval) ran. */
static void test_aggregate_ivm_deterministic(void)
{
    dl_db *db;
    int runs0;

    TEST("T19: aggregate IVM — count/sum/min/max under add/delete");

    setup_db(&db, "t19");
    assert(dl_declare_relation(db, "sale", 2) == 0);
    assert(dl_load_rules(db, AGG_RULES) == 0);
    if (vm_agg_eligible(db) != 1) {
        teardown_db(db, "t19");
        FAIL("T19: aggregate ruleset should be agg-IVM-eligible");
        return;
    }
    assert(dl_compile(db) == 0);

    /* Seed: sale = {(1,10),(1,5),(2,3),(3,7)}.  Populated via the INCREMENTAL
     * path (dl_add_fact + publish after an empty compile). */
    runs0 = vm_agg_runs;
    {
        uint32_t s[][2] = {{1,10},{1,5},{2,3},{3,7}};
        int i;
        for (i = 0; i < 4; i++)
            assert(dl_add_fact(db, "sale", s[i], 2) == 1);
    }
    assert(dl_publish_snapshot(db) == 0);
    if (vm_agg_runs != runs0 + 1) {
        teardown_db(db, "t19");
        FAIL("T19: seed publish did not route through aggregate IVM");
        return;
    }

    {
        uint32_t cnt[] = {1,2, 2,1, 3,1};
        uint32_t tot[] = {1,15, 2,3, 3,7};
        uint32_t minv[] = {1,5, 2,3, 3,7};
        uint32_t maxv[] = {1,10, 2,3, 3,7};
        if (!check_rel2(db, "cnt", cnt, 3) || !check_rel2(db, "tot", tot, 3) ||
            !check_rel2(db, "minv", minv, 3) || !check_rel2(db, "maxv", maxv, 3)) {
            teardown_db(db, "t19");
            FAIL("T19: seed aggregate derivation wrong");
            return;
        }
    }

    /* add sale(2,9): count/sum/max of group 2 update; min unchanged. */
    runs0 = vm_agg_runs;
    { uint32_t s[2] = {2,9}; assert(dl_add_fact(db, "sale", s, 2) == 1); }
    assert(dl_publish_snapshot(db) == 0);
    if (vm_agg_runs != runs0 + 1) {
        teardown_db(db, "t19");
        FAIL("T19: add did not route through aggregate IVM");
        return;
    }
    {
        uint32_t cnt[] = {1,2, 2,2, 3,1};
        uint32_t tot[] = {1,15, 2,12, 3,7};
        uint32_t minv[] = {1,5, 2,3, 3,7};
        uint32_t maxv[] = {1,10, 2,9, 3,7};
        if (!check_rel2(db, "cnt", cnt, 3) || !check_rel2(db, "tot", tot, 3) ||
            !check_rel2(db, "minv", minv, 3) || !check_rel2(db, "maxv", maxv, 3)) {
            teardown_db(db, "t19");
            FAIL("T19: add(2,9) aggregate update wrong");
            return;
        }
    }

    /* delete sale(1,10): the MAX extremum of group 1 — group re-scan must
     * re-find maxv(1)=5. */
    runs0 = vm_agg_runs;
    { uint32_t s[2] = {1,10}; assert(dl_delete_fact(db, "sale", s, 2) == 1); }
    assert(dl_publish_snapshot(db) == 0);
    if (vm_agg_runs != runs0 + 1) {
        teardown_db(db, "t19");
        FAIL("T19: delete-extremum did not route through aggregate IVM");
        return;
    }
    {
        uint32_t cnt[] = {1,1, 2,2, 3,1};
        uint32_t tot[] = {1,5, 2,12, 3,7};
        uint32_t minv[] = {1,5, 2,3, 3,7};
        uint32_t maxv[] = {1,5, 2,9, 3,7};
        if (!check_rel2(db, "cnt", cnt, 3) || !check_rel2(db, "tot", tot, 3) ||
            !check_rel2(db, "minv", minv, 3) || !check_rel2(db, "maxv", maxv, 3)) {
            teardown_db(db, "t19");
            FAIL("T19: delete-extremum max update wrong");
            return;
        }
    }

    /* delete sale(1,5): group 1 becomes empty — all four heads drop (1,*). */
    runs0 = vm_agg_runs;
    { uint32_t s[2] = {1,5}; assert(dl_delete_fact(db, "sale", s, 2) == 1); }
    assert(dl_publish_snapshot(db) == 0);
    {
        uint32_t cnt[] = {2,2, 3,1};
        uint32_t tot[] = {2,12, 3,7};
        uint32_t minv[] = {2,3, 3,7};
        uint32_t maxv[] = {2,9, 3,7};
        if (!check_rel2(db, "cnt", cnt, 2) || !check_rel2(db, "tot", tot, 2) ||
            !check_rel2(db, "minv", minv, 2) || !check_rel2(db, "maxv", maxv, 2)) {
            teardown_db(db, "t19");
            FAIL("T19: empty-group drop wrong");
            return;
        }
    }

    /* add sale(1,4): group 1 reappears (count/sum/min/max all = 4). */
    { uint32_t s[2] = {1,4}; assert(dl_add_fact(db, "sale", s, 2) == 1); }
    assert(dl_publish_snapshot(db) == 0);
    {
        uint32_t cnt[] = {1,1, 2,2, 3,1};
        uint32_t tot[] = {1,4, 2,12, 3,7};
        uint32_t minv[] = {1,4, 2,3, 3,7};
        uint32_t maxv[] = {1,4, 2,9, 3,7};
        if (!check_rel2(db, "cnt", cnt, 3) || !check_rel2(db, "tot", tot, 3) ||
            !check_rel2(db, "minv", minv, 3) || !check_rel2(db, "maxv", maxv, 3)) {
            teardown_db(db, "t19");
            FAIL("T19: group reappear wrong");
            return;
        }
    }

    teardown_db(db, "t19");
    PASS();
}

/* T20: aggregate equivalence-oracle property test — count/sum/min/max views
 * must equal a full re-eval over random add/delete sequences (multiple
 * groups, empty-group edges, delete-an-extremum), with a routing-proof
 * assertion that the incremental aggregate path ran for every effective
 * change. */
static void test_aggregate_ivm_property(void)
{
    dl_db *ivm_db, *oracle_db;
    int iter;
    int runs0, effective = 0;

    TEST("T20: aggregate IVM — seeded random add/delete property vs oracle");

    setup_db(&ivm_db, "t20ivm");
    setup_db(&oracle_db, "t20ora");

    assert(dl_declare_relation(ivm_db, "sale", 2) == 0);
    assert(dl_declare_relation(oracle_db, "sale", 2) == 0);
    assert(dl_load_rules(ivm_db, AGG_RULES) == 0);
    assert(dl_load_rules(oracle_db, AGG_RULES) == 0);
    assert(vm_agg_eligible(ivm_db) == 1);
    assert(dl_compile(ivm_db) == 0);
    assert(dl_compile(oracle_db) == 0);

    prng_state = 0x5EEDC0DEu;
    runs0 = vm_agg_runs;

    for (iter = 0; iter < 150; iter++) {
        int is_del = (int)(prng_next() & 1u);
        uint32_t cols[2] = { prng_next() % 6, prng_next() % 8 };
        int rc;

        if (is_del) {
            rc = dl_delete_fact(ivm_db, "sale", cols, 2);
            if (rc < 0) { printf("  IVM dl_delete_fact error\n"); goto fail_prop; }
            if (rc == 1) effective++;
            rc = dl_delete_fact(oracle_db, "sale", cols, 2);
            if (rc < 0) { printf("  oracle dl_delete_fact error\n"); goto fail_prop; }
        } else {
            rc = dl_add_fact(ivm_db, "sale", cols, 2);
            if (rc < 0) { printf("  IVM dl_add_fact error\n"); goto fail_prop; }
            if (rc == 1) effective++;
            rc = dl_add_fact(oracle_db, "sale", cols, 2);
            if (rc < 0) { printf("  oracle dl_add_fact error\n"); goto fail_prop; }
        }

        if (dl_publish_snapshot(ivm_db) != 0) {
            printf("  publish failed at iter %d\n", iter);
            goto fail_prop;
        }
        if (dl_compile(oracle_db) != 0) {
            printf("  oracle compile failed at iter %d\n", iter);
            goto fail_prop;
        }

        if (!compare_agg_views(ivm_db, oracle_db)) {
            printf("  divergence at iter %d (%s (%u,%u))\n",
                   iter, is_del ? "DEL" : "ADD", cols[0], cols[1]);
            goto fail_prop;
        }
    }

    if (vm_agg_runs != runs0 + effective) {
        printf("  aggregate IVM ran %d times, expected %d\n",
               vm_agg_runs - runs0, effective);
        goto fail_prop;
    }

    teardown_db(ivm_db, "t20ivm");
    teardown_db(oracle_db, "t20ora");
    PASS();
    return;

fail_prop:
    teardown_db(ivm_db, "t20ivm");
    teardown_db(oracle_db, "t20ora");
    FAIL("T20: aggregate IVM views diverged from full re-eval oracle");
}

/* T21: aggregate-IVM eligibility boundary — tricky aggregate shapes fall
 * back to the full fixpoint (still correct) rather than the incremental
 * path. */
static void test_aggregate_ivm_fallback_eligibility(void)
{
    TEST("T21: non-tractable aggregate shapes fall back (and stay correct)");

    /* result-var-first head: group is not a leading head prefix. */
    {
        dl_db *db;
        setup_db(&db, "t21a");
        assert(dl_declare_relation(db, "sale", 2) == 0);
        assert(dl_load_rules(db, "cnt2(N,X):-sale(X,Y),N=count().\n") == 0);
        if (vm_agg_eligible(db) != 0) {
            teardown_db(db, "t21a");
            FAIL("T21: result-first head wrongly agg-IVM-eligible");
            return;
        }
        /* End-to-end: the fallback (full re-eval) stays correct on delete. */
        {
            uint32_t s[][2] = {{1,10},{1,5},{2,3}};
            int i;
            for (i = 0; i < 3; i++) assert(dl_add_fact(db, "sale", s[i], 2) == 1);
        }
        assert(dl_publish_snapshot(db) == 0);
        { uint32_t s[2] = {1,10}; assert(dl_delete_fact(db, "sale", s, 2) == 1); }
        assert(dl_publish_snapshot(db) == 0);
        {
            tuple_set ts = {0};
            long n = dl_query(db, "cnt2", tset_cb, &ts);
            /* cnt2(N,X): after deleting sale(1,10) from {(1,10),(1,5),(2,3)},
             * group X=1 has 1 row -> (N=1,X=1); group X=2 has 1 row -> (1,2).
             * Head order is (N,X). */
            uint32_t e1[2] = {1,1}, e2[2] = {1,2};
            if (n < 0 || ts.count != 2 || !tset_has(&ts, e1) || !tset_has(&ts, e2)) {
                printf("  cnt2 got %ld rows\n", ts.count);
                long j;
                for (j = 0; j < ts.count; j++)
                    printf("    (%u,%u)\n", ts.data[j*2], ts.data[j*2+1]);
                tset_free(&ts); teardown_db(db, "t21a");
                FAIL("T21: result-first fallback delete wrong");
                return;
            }
            tset_free(&ts);
        }
        teardown_db(db, "t21a");
    }

    /* join body (multi-atom): not the minimal scan-anchor shape. */
    {
        dl_db *db;
        setup_db(&db, "t21b");
        assert(dl_declare_relation(db, "sale", 2) == 0);
        assert(dl_declare_relation(db, "q", 1) == 0);
        assert(dl_load_rules(db, "big(X,N):-sale(X,Y),q(Y),N=count().\n") == 0);
        if (vm_agg_eligible(db) != 0) {
            teardown_db(db, "t21b");
            FAIL("T21: join-body aggregate wrongly agg-IVM-eligible");
            return;
        }
        teardown_db(db, "t21b");
    }

    /* derived anchor (aggregate reads a rule head): no base delta to track. */
    {
        dl_db *db;
        setup_db(&db, "t21c");
        assert(dl_declare_relation(db, "sale", 2) == 0);
        assert(dl_load_rules(db,
            "p(X):-sale(X,Y).\n"
            "cnt3(X,N):-p(X),N=count().\n") == 0);
        if (vm_agg_eligible(db) != 0) {
            teardown_db(db, "t21c");
            FAIL("T21: derived-anchor aggregate wrongly agg-IVM-eligible");
            return;
        }
        teardown_db(db, "t21c");
    }

    /* non-terminal aggregate head (a downstream rule reads it). */
    {
        dl_db *db;
        setup_db(&db, "t21d");
        assert(dl_declare_relation(db, "sale", 2) == 0);
        assert(dl_load_rules(db,
            "cnt4(X,N):-sale(X,Y),N=count().\n"
            "big(X):-cnt4(X,N).\n") == 0);
        if (vm_agg_eligible(db) != 0) {
            teardown_db(db, "t21d");
            FAIL("T21: non-terminal aggregate head wrongly agg-IVM-eligible");
            return;
        }
        teardown_db(db, "t21d");
    }

    /* shared aggregate head: two rules producing the same head would corrupt
     * the in-place re-scan (union semantics).  Must fall back. */
    {
        dl_db *db;
        setup_db(&db, "t21e");
        assert(dl_declare_relation(db, "a", 2) == 0);
        assert(dl_declare_relation(db, "b", 2) == 0);
        assert(dl_load_rules(db,
            "cnt(X,N):-a(X,Y),N=count().\n"
            "cnt(X,N):-b(X,Y),N=count().\n") == 0);
        if (vm_agg_eligible(db) != 0) {
            teardown_db(db, "t21e");
            FAIL("T21: shared aggregate head wrongly agg-IVM-eligible");
            return;
        }
        teardown_db(db, "t21e");
    }

    /* repeated variable in the anchor (intra-anchor equality, e.g. pair(X,X)):
     * rel_prefix re-scan cannot enforce col0==col1.  Must fall back. */
    {
        dl_db *db;
        setup_db(&db, "t21f");
        assert(dl_declare_relation(db, "pair", 2) == 0);
        assert(dl_load_rules(db, "cnt(X,N):-pair(X,X),N=count().\n") == 0);
        if (vm_agg_eligible(db) != 0) {
            teardown_db(db, "t21f");
            FAIL("T21: repeated-variable anchor wrongly agg-IVM-eligible");
            return;
        }
        teardown_db(db, "t21f");
    }

    PASS();
}

/* ─── IVM Slice 5: bulk-load IVM + persistence polish ───────────────────── */

/* Bulk-load a batch of rows into an already-declared relation via CSV (no
 * declare, no strict-count assert — batches may carry duplicates across
 * iterations, which must be deduped against the existing base). */
static void bulk_load_rows(dl_db *db, const char *name, const char *rel,
                           uint8_t arity, const uint32_t *cols, int nrows)
{
    char path[512];
    FILE *f;
    int i, c;
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
    assert(dl_load_facts(db, rel, path) >= 0);
}

/* T22: deterministic bulk-load IVM.  Bulk loading a batch of base facts is a
 * BATCHED DELTA: the whole loaded set propagates through the DAG via the
 * insert propagator (vm_propagate_deltas), NOT a full re-eval.  The
 * vm_propagate_runs counter proves the incremental path ran (a silently
 * fell-back full re-eval would also produce correct views). */
static void test_bulk_load_ivm_deterministic(void)
{
    dl_db *db;

    TEST("T22: bulk-load IVM — batched delta propagates through the DAG");

    setup_db(&db, "t22");

    {
        int i;
        for (i = 0; i < 5; i++)
            assert(dl_declare_relation(db, IVM_EDB[i], 2) == 0);
    }
    assert(dl_load_rules(db, IVM_RULES) == 0);
    assert(vm_ivm_eligible(db) == 1);
    assert(dl_compile(db) == 0);

    /* Bulk-load the whole seed in one batch per relation (no dl_add_fact). */
    {
        uint32_t edge[] = {1,2, 8,9};
        uint32_t mid[]  = {2,3, 9,3};
        uint32_t tail[] = {3,4};
        uint32_t fin[]  = {4,5};
        uint32_t end[]  = {2,7};

        int runs0 = vm_propagate_runs;
        bulk_load_rows(db, "t22", "edge", 2, edge, 2);
        bulk_load_rows(db, "t22", "mid",  2, mid,  2);
        bulk_load_rows(db, "t22", "tail", 2, tail, 1);
        bulk_load_rows(db, "t22", "fin",  2, fin,  1);
        bulk_load_rows(db, "t22", "end",  2, end,  1);
        assert(dl_publish_snapshot(db) == 0);

        /* PROVE the batched-delta path ran (not a silent full re-eval). */
        if (vm_propagate_runs != runs0 + 1) {
            teardown_db(db, "t22");
            FAIL("T22: bulk-load publish did not route through the insert propagator");
            return;
        }
    }

    {
        uint32_t p[] = {1,2, 1,3, 8,9, 8,3};
        uint32_t q[] = {1,4, 1,7, 8,4};
        uint32_t r[] = {1,5, 8,5};
        if (!check_rel2(db, "p", p, 4) ||
            !check_rel2(db, "q", q, 3) ||
            !check_rel2(db, "r", r, 2)) {
            teardown_db(db, "t22");
            FAIL("T22: bulk-load cascade wrong");
            return;
        }
    }

    /* Second bulk batch: two facts in one publish must extend the cascade and
     * leave the earlier tuples undisturbed. */
    {
        uint32_t edge[] = {10,11};
        uint32_t end[]  = {11,12};
        int runs0 = vm_propagate_runs;
        bulk_load_rows(db, "t22", "edge", 2, edge, 1);
        bulk_load_rows(db, "t22", "end",  2, end,  1);
        assert(dl_publish_snapshot(db) == 0);
        if (vm_propagate_runs != runs0 + 1) {
            teardown_db(db, "t22");
            FAIL("T22: second bulk batch did not route through the insert propagator");
            return;
        }
    }

    {
        uint32_t p[] = {1,2, 1,3, 8,9, 8,3, 10,11};
        uint32_t q[] = {1,4, 1,7, 8,4, 10,12};
        uint32_t r[] = {1,5, 8,5};
        if (!check_rel2(db, "p", p, 5) ||
            !check_rel2(db, "q", q, 4) ||
            !check_rel2(db, "r", r, 2)) {
            teardown_db(db, "t22");
            FAIL("T22: incremental bulk cascade wrong");
            return;
        }
    }

    teardown_db(db, "t22");
    PASS();
}

/* T23: bulk-load equivalence-oracle property — random bulk-add sequences.
 * After each bulk load + publish, the IVM-maintained views must equal a full
 * re-eval over the current EDB, byte-for-byte. */
static void test_bulk_load_ivm_property(void)
{
    dl_db *ivm_db, *oracle_db;
    int iter, i;

    TEST("T23: bulk-load IVM — seeded random bulk-add property vs full re-eval");

    setup_db(&ivm_db, "t23ivm");
    setup_db(&oracle_db, "t23ora");

    for (i = 0; i < 5; i++) {
        assert(dl_declare_relation(ivm_db, IVM_EDB[i], 2) == 0);
        assert(dl_declare_relation(oracle_db, IVM_EDB[i], 2) == 0);
    }
    assert(dl_load_rules(ivm_db, IVM_RULES) == 0);
    assert(dl_load_rules(oracle_db, IVM_RULES) == 0);
    assert(vm_ivm_eligible(ivm_db) == 1);
    assert(dl_compile(ivm_db) == 0);
    assert(dl_compile(oracle_db) == 0);

    prng_state = 0xB10C4DEu;

    for (iter = 0; iter < 60; iter++) {
        int rel = (int)(prng_next() % 5);
        int batch = 1 + (int)(prng_next() % 4);   /* 1..4 rows per batch */
        uint32_t rows[4 * 2];
        int b;

        for (b = 0; b < batch; b++) {
            rows[b * 2]     = prng_next() % 8;
            rows[b * 2 + 1] = prng_next() % 8;
        }

        /* Identical bulk load into both databases. */
        bulk_load_rows(ivm_db, "t23ivm", IVM_EDB[rel], 2, rows, batch);
        bulk_load_rows(oracle_db, "t23ora", IVM_EDB[rel], 2, rows, batch);

        /* IVM: batched-delta propagation on publish.  Oracle: full re-eval. */
        if (dl_publish_snapshot(ivm_db) != 0) {
            printf("  publish failed at iter %d\n", iter);
            goto fail_prop;
        }
        if (dl_compile(oracle_db) != 0) {
            printf("  oracle compile failed at iter %d\n", iter);
            goto fail_prop;
        }

        if (!compare_all_views(ivm_db, oracle_db)) {
            printf("  divergence at iter %d (rel %s)\n", iter, IVM_EDB[rel]);
            goto fail_prop;
        }
    }

    teardown_db(ivm_db, "t23ivm");
    teardown_db(oracle_db, "t23ora");
    PASS();
    return;

fail_prop:
    teardown_db(ivm_db, "t23ivm");
    teardown_db(oracle_db, "t23ora");
    FAIL("T23: bulk-load IVM views diverged from full re-eval oracle");
}

/* T24: bulk loading into a RULE-HEAD relation (mixed EDB+IDB) must fall back
 * to the full re-eval — the loaded base fact appears in the view AND the
 * derived tuples are re-derived (not {1,2} from a delta-only view, not {3}
 * from dropping derived tuples). */
static void test_bulk_load_mixed_head_fallback(void)
{
    dl_db *db;
    tuple_set ts;

    TEST("T24: bulk load into a rule-head relation -> full re-eval (mixed)");

    setup_db(&db, "t24");

    assert(dl_declare_relation(db, "a", 1) == 0);
    {
        uint32_t one = 1, two = 2;
        assert(dl_add_fact(db, "a", &one, 1) == 1);
        assert(dl_add_fact(db, "a", &two, 1) == 1);
    }
    assert(dl_load_rules(db, "r(X):-a(X).\n") == 0);
    assert(dl_compile(db) == 0);

    /* Bulk-load r(3) into the rule-head relation r. */
    {
        uint32_t three = 3;
        int runs0 = vm_propagate_runs;
        bulk_load_rows(db, "t24", "r", 1, &three, 1);
        assert(dl_publish_snapshot(db) == 0);
        /* The mixed-head load must NOT route through the insert propagator. */
        if (vm_propagate_runs != runs0) {
            teardown_db(db, "t24");
            FAIL("T24: mixed-head bulk load wrongly routed through the propagator");
            return;
        }
    }

    query_set_prefix(db, "r", &ts);
    if (ts.count != 3 || ts.arity != 1) {
        printf("  r got %ld rows, expected 3\n", ts.count);
        long j;
        for (j = 0; j < ts.count; j++) printf("    r(%u)\n", ts.data[j]);
        tset_free(&ts); teardown_db(db, "t24");
        FAIL("T24: mixed-head bulk load did not full re-eval");
        return;
    }
    {
        uint32_t v;
        for (v = 1; v <= 3; v++) {
            uint32_t row[1] = {v};
            if (!tset_has(&ts, row)) {
                printf("  r missing %u\n", v);
                tset_free(&ts); teardown_db(db, "t24");
                FAIL("T24: r missing value after mixed bulk load");
                return;
            }
        }
    }
    tset_free(&ts);

    teardown_db(db, "t24");
    PASS();
}

/* T25: reopen re-derives an IDB relation's view from its persisted BASE even
 * when the view (.dafsa) file is MISSING — the crash-consistency gap where
 * dl_close wrote base.dafsa but not the view (or the view was lost).  The VM
 * resets view = copy(base) on the next eval, so the view is rebuilt from
 * base + rules, never from a stale/missing view file. */
static void test_reopen_missing_view_rederive(void)
{
    dl_db *db;
    tuple_set ts;
    int i;

    TEST("T25: reopen with a missing view file re-derives from base");

    setup_db(&db, "t25");

    {
        uint32_t succ[20];
        for (i = 0; i < 10; i++) { succ[i*2] = (uint32_t)i; succ[i*2+1] = (uint32_t)(i+1); }
        load_rows(db, "t25", "succ", 2, succ, 10);
    }
    {
        uint32_t ev = 0;
        load_rows(db, "t25", "even", 1, &ev, 1);
    }

    assert(dl_load_rules(db,
        "even(X):-succ(Y,X),odd(Y).\n"
        "odd(X):-succ(Y,X),even(Y).\n") == 0);
    assert(dl_compile(db) == 0);

    /* Persist: writes even.base.dafsa + even.dafsa(view), odd.base.dafsa +
     * odd.dafsa(view). */
    dl_close(db);

    /* Simulate a crash between the base write and the view write: drop the
     * view files for BOTH IDB relations. */
    assert(remove("build-tmp/ivm-t25/even.dafsa") == 0);
    assert(remove("build-tmp/ivm-t25/odd.dafsa") == 0);

    db = dl_open("build-tmp/ivm-t25");
    assert(db != NULL);

    assert(dl_load_rules(db,
        "even(X):-succ(Y,X),odd(Y).\n"
        "odd(X):-succ(Y,X),even(Y).\n") == 0);
    assert(dl_compile(db) == 0);

    query_set(db, "even", &ts);
    if (ts.count != 6 || ts.arity != 1) {
        printf("  even got %ld rows, expected 6\n", ts.count);
        tset_free(&ts); teardown_db(db, "t25");
        FAIL("T25: even not re-derived after missing-view reopen");
        return;
    }
    {
        uint32_t expected[6] = {0,2,4,6,8,10};
        for (i = 0; i < 6; i++) {
            uint32_t row[1] = {expected[i]};
            if (!tset_has(&ts, row)) {
                printf("  missing even(%u)\n", expected[i]);
                tset_free(&ts); teardown_db(db, "t25");
                FAIL("T25: missing even value after reopen");
                return;
            }
        }
    }
    tset_free(&ts);

    query_set(db, "odd", &ts);
    if (ts.count != 5 || ts.arity != 1) {
        printf("  odd got %ld rows, expected 5\n", ts.count);
        tset_free(&ts); teardown_db(db, "t25");
        FAIL("T25: odd not re-derived after missing-view reopen");
        return;
    }
    tset_free(&ts);

    teardown_db(db, "t25");
    PASS();
}

/* ─── Main ────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("IVM Slice 0/1/2/3/4/5 correctness + IVM equivalence-oracle tests\n");
    printf("==========================================================\n\n");

    test_tc_delete();
    test_aggregate_delete();
    test_evenodd_delete();
    test_reopen_rederive();
    test_insert_ivm_deterministic();
    test_insert_ivm_property();
    test_insert_mixed_head_fallback();
    test_recursive_insert_ivm_deterministic();
    test_recursive_ivm_property();
    test_recursive_negation_fallback();
    test_mutual_recursion_insert_ivm();
    test_nonrecursive_head_feeds_recursion_fallback();
    test_dred_delete_deterministic();
    test_dred_negation_deterministic();
    test_dred_property_monotone();
    test_dred_property_negation();
    test_dred_fallback_eligibility();
    test_dred_overdelete_lookup_same_stratum();
    test_aggregate_ivm_deterministic();
    test_aggregate_ivm_property();
    test_aggregate_ivm_fallback_eligibility();
    test_bulk_load_ivm_deterministic();
    test_bulk_load_ivm_property();
    test_bulk_load_mixed_head_fallback();
    test_reopen_missing_view_rederive();

    printf("\n%d tests run, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
