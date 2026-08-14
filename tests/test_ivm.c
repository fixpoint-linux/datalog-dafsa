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

/* ─── Main ────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("IVM Slice 0/1/2 correctness + IVM equivalence-oracle tests\n");
    printf("=======================================================\n\n");

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

    printf("\n%d tests run, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
