/*
 * test_m15_vmiter.c — OP_RANGE LAZY resumable pull-iterator generator (VM)
 *
 * The range-index follow-up #5 pull-based iterator is wired into the VM so the
 * `range(X, Rel, Lo, Hi)` GENERATOR (X unbound) no longer EAGERLY materializes
 * every distinct leading-column value into f->tuples via rel_range_each.
 * Instead it opens a LIVE-only cursor (dl_iter_open_live) anchored in the
 * vm_frame, resumes it on each backtrack re-entry (range_resume), and closes
 * it on frame pop / cleanup.  The FILTER branch (X bound) is unchanged.
 *
 *   T1  oracle:        random arity-2 rel x random [lo,hi) — dl_query byte-
 *                      identical to an independent dl_prefix full-scan +
 *                      col0 filter + distinct + sort.
 *   T2  edge cases:    empty [5,5), lo>=hi, single [24,26), below-min,
 *                      beyond-max.
 *   T3  arity-2:       distinct col0 dedup (not every tuple).
 *   T4  filter:        X bound membership FILTER unchanged.
 *   T5  LAZINESS:      N=100000 distinct col0; early-stop consumer via
 *                      vm_exec_rule(dry=1) after 10 tuples; assert
 *                      vm_range_yields delta ~10, NOT ~N.
 *   T6  SNAPSHOT-LIVE: publish -> add fact -> compile -> publish#2 must read
 *                      the LIVE relation (never the stale snapshot view).
 *   T7  gates:         recursive-SCC rejection + db_has_range_builtin keeps
 *                      the program out of IVM (full re-eval, not propagate).
 *
 * Build: make tests/test_m15_vmiter (link ALL_OBJS) — see Makefile.
 */
#include "dl.h"
#include "dl_internal.h"   /* db->crules, db_has_range_builtin */
#include "vm.h"            /* vm_exec_rule, vm_range_yields, vm_propagate_runs */

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
} while (0)
#define PASS() do { printf("OK\n"); } while (0)
#define FAIL(msg) do { \
    printf("FAIL: %s\n", msg); \
    tests_failed++; \
} while (0)

/* ─── Tuple collector (dl_tuple_cb-compatible) ─────────────────────────── */

typedef struct {
    uint32_t *data;
    long      count, cap;
    uint8_t   arity;
} tset;

static int tcb(const uint32_t *cols, uint8_t arity, void *user)
{
    tset *t = (tset *)user;
    if (t->arity == 0) t->arity = arity;
    if (t->count >= t->cap) {
        long nc = t->cap ? t->cap * 2 : 64;
        uint32_t *nd = realloc(t->data,
            (size_t)nc * (size_t)t->arity * sizeof(uint32_t));
        if (!nd) return 1;
        t->data = nd;
        t->cap = nc;
    }
    memcpy(t->data + (size_t)t->count * t->arity, cols,
           (size_t)t->arity * sizeof(uint32_t));
    t->count++;
    return 0;
}

static void tset_free(tset *t) { free(t->data); memset(t, 0, sizeof(*t)); }

/* Lex comparator over a flat u32 array of `cmp_arity`-wide tuples. */
static uint8_t cmp_arity = 0;
static int ucmp(const void *a, const void *b)
{
    const uint32_t *x = (const uint32_t *)a;
    const uint32_t *y = (const uint32_t *)b;
    uint8_t i;
    for (i = 0; i < cmp_arity; i++) {
        if (x[i] < y[i]) return -1;
        if (x[i] > y[i]) return 1;
    }
    return 0;
}

static void tset_sort(tset *t)
{
    cmp_arity = t->arity;
    qsort(t->data, (size_t)t->count,
          (size_t)t->arity * sizeof(uint32_t), ucmp);
}

static int tset_eq(const tset *a, const tset *b)
{
    long i, n;
    if (a->count != b->count || a->arity != b->arity) return 0;
    n = a->count * a->arity;
    for (i = 0; i < n; i++)
        if (a->data[i] != b->data[i]) return 0;
    return 1;
}

static void addf(dl_db *db, const char *rel, const uint32_t *cols, int ar)
{
    assert(dl_add_fact(db, rel, cols, (uint8_t)ar) == 1);
}

/* ─── DB fixture ───────────────────────────────────────────────────────── */

static const char *BASE = "build-tmp/vmiter";

static dl_db *fresh_db(char *dir_out, size_t cap, const char *name)
{
    char cmd[512];
    snprintf(dir_out, cap, "%s/%s", BASE, name);
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir_out);
    system(cmd);
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", dir_out);
    system(cmd);
    return dl_open(dir_out);
}

/* Does `ts` (arity-1) hold exactly the sorted values in `exp` (n exp)? */
static int tset_equals_1(const tset *ts, const uint32_t *exp, long n)
{
    long i;
    if (ts->count != n || ts->arity != 1) return 0;
    for (i = 0; i < n; i++)
        if (ts->data[i] != exp[i]) return 0;
    return 1;
}

/* ─── T1: oracle vs full-scan + filter + distinct + sort ───────────────── */

static void t1_oracle(char *dir, size_t cap)
{
    dl_db *db = fresh_db(dir, cap, "t1");
    const int NF = 200, NS = 40;
    uint32_t cols[2];
    int i, pass = 1;
    tset facts, bounds, got, exp;
    uint32_t *col0 = NULL;
    long n_col0 = 0;

    TEST("T1 oracle: random arity-2 rel x random [lo,hi) == full-scan+filter+distinct+sort");
    assert(db);
    assert(dl_declare_relation(db, "r", 2) == 0);
    for (i = 0; i < NF; i++) {
        cols[0] = (uint32_t)(rand() % 100);
        cols[1] = (uint32_t)(rand() % 1000);
        dl_add_fact(db, "r", cols, 2);   /* dedups; ignore return */
    }
    assert(dl_declare_relation(db, "bounds", 2) == 0);
    for (i = 0; i < NS; i++) {
        cols[0] = (uint32_t)(rand() % 110);
        cols[1] = (uint32_t)(rand() % 110);
        if (cols[1] <= cols[0]) cols[1] = cols[0] + 1;
        dl_add_fact(db, "bounds", cols, 2);
    }
    assert(dl_load_rules(db,
        "q(Lo, Hi, X) :- bounds(Lo, Hi), range(X, r, Lo, Hi).\n") == 0);

    /* Oracle: distinct col0 of r (full-scan + dedup + sort). */
    memset(&facts, 0, sizeof(facts));
    if (dl_prefix(db, "r", NULL, 0, tcb, &facts) < 0) { FAIL("collect facts"); goto out; }
    col0 = malloc((size_t)facts.count * sizeof(uint32_t));
    if (!col0) { FAIL("OOM col0"); goto out; }
    for (i = 0; i < facts.count; i++) {
        uint32_t v = facts.data[(size_t)i * 2];
        int dup = 0, j;
        for (j = 0; j < n_col0; j++) if (col0[j] == v) { dup = 1; break; }
        if (!dup) col0[n_col0++] = v;
    }
    cmp_arity = 1;
    qsort(col0, (size_t)n_col0, sizeof(uint32_t), ucmp);

    memset(&bounds, 0, sizeof(bounds));
    if (dl_prefix(db, "bounds", NULL, 0, tcb, &bounds) < 0) { FAIL("collect bounds"); goto out; }

    /* Expected = for each (lo,hi) in sorted bounds order, each distinct col0
     * v in [lo,hi): (lo,hi,v). */
    memset(&exp, 0, sizeof(exp));
    exp.arity = 3;
    for (i = 0; i < bounds.count; i++) {
        uint32_t lo = bounds.data[(size_t)i * 2];
        uint32_t hi = bounds.data[(size_t)i * 2 + 1];
        long j;
        for (j = 0; j < n_col0; j++) {
            uint32_t v = col0[j];
            if (v < lo) continue;
            if (v >= hi) break;   /* col0 sorted */
            if (exp.count >= exp.cap) {
                long nc = exp.cap ? exp.cap * 2 : 64;
                uint32_t *nd = realloc(exp.data, (size_t)nc * 3 * sizeof(uint32_t));
                if (!nd) { FAIL("OOM exp"); goto out; }
                exp.data = nd; exp.cap = nc;
            }
            exp.data[(size_t)exp.count * 3]     = lo;
            exp.data[(size_t)exp.count * 3 + 1] = hi;
            exp.data[(size_t)exp.count * 3 + 2] = v;
            exp.count++;
        }
    }

    memset(&got, 0, sizeof(got));
    if (dl_query(db, "q", tcb, &got) < 0) { FAIL("q query error"); goto out; }

    tset_sort(&got);
    tset_sort(&exp);
    if (!tset_eq(&got, &exp)) { FAIL("oracle mismatch"); pass = 0; }

out:
    free(col0);
    tset_free(&facts); tset_free(&bounds); tset_free(&got); tset_free(&exp);
    dl_close(db);
    if (pass) PASS(); else FAIL("oracle");
}

/* ─── T2: edge cases ───────────────────────────────────────────────────── */

static void t2_edge_cases(char *dir, size_t cap)
{
    dl_db *db = fresh_db(dir, cap, "t2");
    const uint32_t vals[6] = {5, 10, 12, 15, 20, 25};
    tset got;
    int i, pass = 1;

    TEST("T2 edge cases (empty / lo>=hi / single / below-min / beyond-max)");
    assert(db);
    assert(dl_declare_relation(db, "r", 1) == 0);
    for (i = 0; i < 6; i++) addf(db, "r", &vals[i], 1);

    assert(dl_load_rules(db,
        "e_empty(X)  :- range(X, r, 5, 5).\n"
        "e_inv(X)    :- range(X, r, 20, 10).\n"
        "e_single(X) :- range(X, r, 24, 26).\n"
        "e_below(X)  :- range(X, r, 0, 5).\n"
        "e_beyond(X) :- range(X, r, 31, 40).\n") == 0);

    memset(&got, 0, sizeof(got));
    if (dl_query(db, "e_empty", tcb, &got) != 0) pass = 0;
    tset_free(&got);

    memset(&got, 0, sizeof(got));
    if (dl_query(db, "e_inv", tcb, &got) != 0) pass = 0;
    tset_free(&got);

    memset(&got, 0, sizeof(got));
    if (dl_query(db, "e_single", tcb, &got) != 1) pass = 0;
    if (got.count != 1 || got.data[0] != 25) pass = 0;
    tset_free(&got);

    memset(&got, 0, sizeof(got));
    if (dl_query(db, "e_below", tcb, &got) != 0) pass = 0;
    tset_free(&got);

    memset(&got, 0, sizeof(got));
    if (dl_query(db, "e_beyond", tcb, &got) != 0) pass = 0;
    tset_free(&got);

    dl_close(db);
    if (pass) PASS(); else FAIL("edge-case mismatch");
}

/* ─── T3: arity-2 distinct col0 dedup ──────────────────────────────────── */

static void t3_arity2_distinct(char *dir, size_t cap)
{
    dl_db *db = fresh_db(dir, cap, "t3");
    const uint32_t facts[8][2] = {
        {10, 1}, {10, 2}, {12, 3}, {12, 4},
        {15, 5}, {5, 0}, {20, 0}, {25, 0},
    };
    const uint32_t exp[3] = {10, 12, 15};
    tset got;
    int i, pass = 1;

    TEST("T3 arity-2 distinct col0 dedup");
    assert(db);
    assert(dl_declare_relation(db, "r", 2) == 0);
    for (i = 0; i < 8; i++) addf(db, "r", facts[i], 2);

    assert(dl_load_rules(db, "q(X) :- range(X, r, 10, 20).\n") == 0);
    memset(&got, 0, sizeof(got));
    /* {10,12,15} distinct col0 values — NOT 5 tuples. */
    if (dl_query(db, "q", tcb, &got) != 3) pass = 0;
    if (!tset_equals_1(&got, exp, 3)) pass = 0;

    tset_free(&got);
    dl_close(db);
    if (pass) PASS(); else FAIL("arity-2 distinct mismatch");
}

/* ─── T4: filter (X bound) unchanged ───────────────────────────────────── */

static void t4_filter(char *dir, size_t cap)
{
    dl_db *db = fresh_db(dir, cap, "t4");
    const uint32_t vals[6] = {5, 10, 12, 15, 20, 25};
    const uint32_t exp[3] = {10, 12, 15};
    tset got;
    int i, pass = 1;

    TEST("T4 filter: r(X), range(X,r,10,20) -> {10,12,15}");
    assert(db);
    assert(dl_declare_relation(db, "r", 1) == 0);
    for (i = 0; i < 6; i++) addf(db, "r", &vals[i], 1);

    assert(dl_load_rules(db, "p(X) :- r(X), range(X, r, 10, 20).\n") == 0);
    memset(&got, 0, sizeof(got));
    if (dl_query(db, "p", tcb, &got) != 3) pass = 0;
    if (!tset_equals_1(&got, exp, 3)) pass = 0;

    tset_free(&got);
    dl_close(db);
    if (pass) PASS(); else FAIL("filter mismatch");
}

/* ─── T5: LAZINESS — early stop short-circuits the range ───────────────── */

static int early_stop_cb(const uint32_t *cols, uint8_t arity, void *user)
{
    long *n = (long *)user;
    (void)cols; (void)arity;
    (*n)++;
    return (*n >= 10) ? 1 : 0;
}

static void t5_laziness(char *dir, size_t cap)
{
    dl_db *db = fresh_db(dir, cap, "t5");
    const int N = 100000;
    char csv[600];
    FILE *f;
    int i, pass = 1;
    long before, delta, emitted = 0, n_seen = 0;

    TEST("T5 laziness: early stop after 10 of N=100000 yields ~10, not N");
    assert(db);
    assert(dl_declare_relation(db, "r", 1) == 0);

    /* Bulk-load N distinct col0 values (dl_load_facts bypasses the per-fact
     * WAL fsync so this stays fast). */
    snprintf(csv, sizeof(csv), "%s/data.csv", dir);
    f = fopen(csv, "w");
    assert(f);
    for (i = 0; i < N; i++) fprintf(f, "%u\n", (unsigned)i);
    fclose(f);
    assert(dl_load_facts(db, "r", csv) == N);

    assert(dl_load_rules(db, "q(X) :- range(X, r, 0, 1000000).\n") == 0);

    before = vm_range_yields;
    emitted = vm_exec_rule(db, db->crules[0], NULL, 0, 1, early_stop_cb, &n_seen);
    delta = vm_range_yields - before;

    if (emitted != 10) { FAIL("early-stop emitted != 10"); pass = 0; }
    if (n_seen != 10)  { FAIL("early-stop cb saw != 10"); pass = 0; }
    /* The crux: the lazy generator advanced ~10 distinct values, NOT all N.
     * (An eager materialization would have visited all N.) */
    if (delta < 10 || delta >= 20) { FAIL("vm_range_yields delta not ~10 (not lazy)"); pass = 0; }

    dl_close(db);
    if (pass) PASS(); else FAIL("laziness");
}

/* ─── T6: SNAPSHOT-LIVE — re-publish reads live, not stale snapshot ────── */

static void t6_snapshot_live(char *dir, size_t cap)
{
    dl_db *db = fresh_db(dir, cap, "t6");
    const uint32_t five = 5, seven = 7;
    tset got;
    int pass = 1;

    TEST("T6 snapshot-LIVE: re-publish reads live rel->d, not stale snapshot");
    assert(db);
    assert(dl_declare_relation(db, "r", 1) == 0);
    assert(dl_add_fact(db, "r", &five, 1) == 1);
    assert(dl_load_rules(db, "q(X) :- range(X, r, 0, 10).\n") == 0);
    assert(dl_compile(db) == 0);
    assert(dl_publish_snapshot(db) == 0);   /* snap_version = 1, q = {5} */

    /* New fact while a snapshot is current: snap_version stays 1. */
    assert(dl_add_fact(db, "r", &seven, 1) == 1);

    /* Re-compile runs vm_execute with snap_version == 1.  OP_RANGE MUST read
     * the LIVE r = {5,7}, NOT the stale v1 snapshot {5}. */
    assert(dl_compile(db) == 0);

    /* Publish again -> snapshot v2 reflects the live q = {5,7}. */
    assert(dl_publish_snapshot(db) == 0);

    memset(&got, 0, sizeof(got));
    if (dl_query(db, "q", tcb, &got) != 2) pass = 0;
    if (got.count != 2 || got.data[0] != 5 || got.data[1] != 7) pass = 0;

    tset_free(&got);
    dl_close(db);
    if (pass) PASS(); else FAIL("snapshot-LIVE mismatch");
}

/* ─── T7: gates — recursive-SCC rejection + IVM exclusion ──────────────── */

static void t7_gates(char *dir, size_t cap)
{
    int pass = 1;

    TEST("T7 gates: recursive-SCC rejection + range excluded from IVM");

    /* (a) range over a recursive idb -> loud compile error. */
    {
        dl_db *db2 = fresh_db(dir, cap, "t7rec");
        uint32_t e[2];
        assert(db2);
        assert(dl_declare_relation(db2, "edge", 2) == 0);
        e[0] = 1; e[1] = 2; addf(db2, "edge", e, 2);
        e[0] = 2; e[1] = 3; addf(db2, "edge", e, 2);
        if (dl_load_rules(db2,
                "reach(X) :- edge(X, Y), reach(Y).\n"
                "reach(X) :- edge(X, _).\n"
                "q(X) :- range(X, reach, 1, 10).\n") == 0) {
            FAIL("range over recursive idb accepted"); pass = 0;
        }
        dl_close(db2);
    }

    /* (b) range program is excluded from IVM: a pending insert delta routes
     * through the FULL re-eval (vm_propagate_runs unchanged), never the
     * incremental propagator. */
    {
        dl_db *db = fresh_db(dir, cap, "t7ivm");
        uint32_t v = 5;
        tset got;
        int before;
        assert(db);
        assert(dl_declare_relation(db, "r", 1) == 0);
        assert(dl_add_fact(db, "r", &v, 1) == 1);
        assert(dl_load_rules(db, "q(X) :- range(X, r, 0, 10).\n") == 0);
        assert(dl_compile(db) == 0);
        assert(db_has_range_builtin(db) == 1);

        before = vm_propagate_runs;
        v = 7;
        assert(dl_add_fact(db, "r", &v, 1) == 1);
        assert(dl_publish_snapshot(db) == 0);

        if (vm_propagate_runs != before) {
            FAIL("range program wrongly routed through IVM"); pass = 0;
        }

        /* Full re-eval produced the correct view: q = {5,7}. */
        memset(&got, 0, sizeof(got));
        if (dl_query(db, "q", tcb, &got) != 2) pass = 0;
        if (got.count != 2 || got.data[0] != 5 || got.data[1] != 7) pass = 0;
        tset_free(&got);
        dl_close(db);
    }

    if (pass) PASS(); else FAIL("gates leak");
}

/* ─── main ─────────────────────────────────────────────────────────────── */

int main(void)
{
    char dir[512];

    printf("=== M15 OP_RANGE lazy pull-iterator tests ===\n");
    srand(424243);

    t1_oracle(dir, sizeof(dir));
    t2_edge_cases(dir, sizeof(dir));
    t3_arity2_distinct(dir, sizeof(dir));
    t4_filter(dir, sizeof(dir));
    t5_laziness(dir, sizeof(dir));
    t6_snapshot_live(dir, sizeof(dir));
    t7_gates(dir, sizeof(dir));

    printf("\n%d tests run, %d failed.\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
