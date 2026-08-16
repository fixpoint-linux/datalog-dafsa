/*
 * test_m11_range.c — v2 `range(X, Rel, Lo, Hi)` builtin predicate
 *
 * `range(X, Rel, Lo, Hi)` is the reserved RELATION analog of `member(X, L)`:
 *   - X unbound  -> GENERATOR binding X to each DISTINCT leading-column (col0)
 *                   value v of Rel with Lo <= v < Hi, in lex order.
 *   - X bound    -> FILTER: Lo <= X < Hi AND some tuple of Rel has col0 == X.
 * Range is on the LEADING COLUMN (col0).  Rel must be EDB or a NON-recursive
 * IDB — OP_RANGE reads db->rels[rel].rel directly, which the recursive
 * fixpoint never updates, so range over a RECURSIVE relation is rejected at
 * COMPILE time (never silently mis-evaluated).
 *
 *   T1  generator arity-1: [10,20) over {5,10,12,15,20,25} -> {10,12,15}.
 *   T2  filter:            r(X), range(X,r,10,20) -> {10,12,15}.
 *   T3  arity-2 distinct:  range binds DISTINCT col0 values (dedup), not
 *                          every tuple.
 *   T4  edge cases:        empty [5,5), single [24,26), beyond-max [31,40),
 *                          below-min [0,5).
 *   T5  THE ORACLE: random arity-2 relation x many random [lo,hi) — range
 *          bindings == brute-force dl_prefix full-scan + filter + distinct.
 *   T6  reject matrix:     unknown Rel / variadic Rel / negated range /
 *                          ungrounded bound var / rule head named `range` /
 *                          range over a RECURSIVE idb — all loud compile
 *                          errors (dl_load_rules returns non-zero).
 *
 * Build: make tests/test_m11_range (link ALL_OBJS) — see Makefile.
 */
#include "dl.h"

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

static void addf(dl_db *db, const char *rel, const uint32_t *cols, int ar)
{
    assert(dl_add_fact(db, rel, cols, (uint8_t)ar) == 1);
}

/* ─── DB fixture ───────────────────────────────────────────────────────── */

static const char *BASE = "build-tmp/range";

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

/* ─── Test helpers ─────────────────────────────────────────────────────── */

/* Does `ts` (arity-1) contain exactly the sorted values in `exp` (n exp)? */
static int tset_equals_1(const tset *ts, const uint32_t *exp, long n)
{
    long i;
    if (ts->count != n || ts->arity != 1) return 0;
    for (i = 0; i < n; i++)
        if (ts->data[i] != exp[i]) return 0;
    return 1;
}

/* ─── T1: generator, arity-1 ───────────────────────────────────────────── */

static void t1_generator_arity1(char *dir, size_t cap)
{
    dl_db *db = fresh_db(dir, cap, "t1");
    const uint32_t vals[6] = {5, 10, 12, 15, 20, 25};
    const uint32_t exp[3] = {10, 12, 15};
    tset got;
    int i;
    int pass = 1;

    TEST("T1 generator arity-1 [10,20) over {5,10,12,15,20,25} -> {10,12,15}");
    assert(db);
    assert(dl_declare_relation(db, "r", 1) == 0);
    for (i = 0; i < 6; i++) addf(db, "r", &vals[i], 1);

    assert(dl_load_rules(db, "q(X) :- range(X, r, 10, 20).\n") == 0);
    memset(&got, 0, sizeof(got));
    if (dl_query(db, "q", tcb, &got) != 3) pass = 0;
    if (!tset_equals_1(&got, exp, 3)) pass = 0;

    tset_free(&got);
    dl_close(db);
    if (pass) PASS(); else FAIL("generator arity-1 mismatch");
}

/* ─── T2: filter ───────────────────────────────────────────────────────── */

static void t2_filter(char *dir, size_t cap)
{
    dl_db *db = fresh_db(dir, cap, "t2");
    const uint32_t vals[6] = {5, 10, 12, 15, 20, 25};
    const uint32_t exp[3] = {10, 12, 15};
    tset got;
    int i;
    int pass = 1;

    TEST("T2 filter: r(X), range(X,r,10,20) -> {10,12,15}");
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
    int i;
    int pass = 1;

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

/* ─── T4: edge cases ───────────────────────────────────────────────────── */

static void t4_edge_cases(char *dir, size_t cap)
{
    dl_db *db = fresh_db(dir, cap, "t4");
    const uint32_t vals[6] = {5, 10, 12, 15, 20, 25};
    tset got;
    int i;
    int pass = 1;

    TEST("T4 edge cases (empty / single / beyond-max / below-min)");
    assert(db);
    assert(dl_declare_relation(db, "r", 1) == 0);
    for (i = 0; i < 6; i++) addf(db, "r", &vals[i], 1);

    /* All four edge-case rules share relation r. */
    assert(dl_load_rules(db,
        "e_empty(X)  :- range(X, r, 5, 5).\n"
        "e_single(X) :- range(X, r, 24, 26).\n"
        "e_beyond(X) :- range(X, r, 31, 40).\n"
        "e_below(X)  :- range(X, r, 0, 5).\n") == 0);

    /* [5,5) empty. */
    memset(&got, 0, sizeof(got));
    if (dl_query(db, "e_empty", tcb, &got) != 0) pass = 0;
    tset_free(&got);

    /* [24,26) -> {25} single. */
    memset(&got, 0, sizeof(got));
    if (dl_query(db, "e_single", tcb, &got) != 1) pass = 0;
    if (got.count != 1 || got.data[0] != 25) pass = 0;
    tset_free(&got);

    /* [31,40) beyond max (values end at 25) -> empty. */
    memset(&got, 0, sizeof(got));
    if (dl_query(db, "e_beyond", tcb, &got) != 0) pass = 0;
    tset_free(&got);

    /* [0,5) below min (5 is NOT < 5) -> empty. */
    memset(&got, 0, sizeof(got));
    if (dl_query(db, "e_below", tcb, &got) != 0) pass = 0;
    tset_free(&got);

    dl_close(db);
    if (pass) PASS(); else FAIL("edge-case mismatch");
}

/* ─── T5: oracle vs brute-force full-scan + filter + distinct ─────────── */

static int uint_cmp(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static void t5_oracle(char *dir, size_t cap)
{
    dl_db *db = fresh_db(dir, cap, "t5");
    const int NF = 200, NS = 40;
    uint32_t cols[2];
    int i, pass = 1;
    tset facts;      /* distinct arity-2 facts (brute-force source) */
    uint32_t *col0;  /* distinct sorted col0 values of r */
    long n_col0 = 0;
    tset got;

    TEST("T5 oracle: random relation x random [lo,hi) == full-scan+filter+distinct");
    assert(db);
    assert(dl_declare_relation(db, "r", 2) == 0);
    memset(&facts, 0, sizeof(facts));
    for (i = 0; i < NF; i++) {
        cols[0] = (uint32_t)(rand() % 100);
        cols[1] = (uint32_t)(rand() % 1000);
        dl_add_fact(db, "r", cols, 2);   /* dedups; ignore return */
    }
    /* Brute-force distinct col0 values: full-scan + dedup + sort. */
    if (dl_prefix(db, "r", NULL, 0, tcb, &facts) < 0) { FAIL("collect facts"); goto out; }
    col0 = malloc((size_t)facts.count * sizeof(uint32_t));
    if (!col0) { FAIL("OOM col0"); goto out; }
    for (i = 0; i < facts.count; i++) {
        uint32_t v = facts.data[(size_t)i * 2];
        int dup = 0, j;
        for (j = 0; j < n_col0; j++) if (col0[j] == v) { dup = 1; break; }
        if (!dup) col0[n_col0++] = v;
    }
    qsort(col0, (size_t)n_col0, sizeof(uint32_t), uint_cmp);

    /* Bounds relation: NS random half-open [lo,hi). */
    assert(dl_declare_relation(db, "bounds", 2) == 0);
    {
        uint32_t b[2];
        for (i = 0; i < NS; i++) {
            b[0] = (uint32_t)(rand() % 110);
            b[1] = (uint32_t)(rand() % 110);
            if (b[1] <= b[0]) b[1] = b[0] + 1;
            dl_add_fact(db, "bounds", b, 2);   /* dup bounds are harmless */
        }
    }

    /* q(X, Lo, Hi) :- bounds(Lo, Hi), range(X, r, Lo, Hi). */
    assert(dl_load_rules(db,
        "q(X, Lo, Hi) :- bounds(Lo, Hi), range(X, r, Lo, Hi).\n") == 0);

    memset(&got, 0, sizeof(got));
    long total = dl_query(db, "q", tcb, &got);
    if (total < 0) { FAIL("oracle query error"); goto out; }

    /* For each bounds row (Lo,Hi): expected = distinct col0 in [lo,hi). */
    {
        uint32_t *bo = NULL;
        long nbo = 0, k;
        tset bounds;
        memset(&bounds, 0, sizeof(bounds));
        if (dl_prefix(db, "bounds", NULL, 0, tcb, &bounds) < 0) { FAIL("collect bounds"); goto out; }
        nbo = bounds.count;
        bo = malloc((size_t)nbo * 2 * sizeof(uint32_t));
        if (!bo) { FAIL("OOM bo"); tset_free(&bounds); goto out; }
        for (i = 0; i < nbo; i++) {
            bo[i*2]   = bounds.data[(size_t)i*2];
            bo[i*2+1] = bounds.data[(size_t)i*2+1];
        }
        tset_free(&bounds);

        /* Check each query row (X, Lo, Hi) is a valid distinct-col0 match. */
        for (i = 0; i < got.count; i++) {
            uint32_t x = got.data[(size_t)i*3];
            uint32_t lo = got.data[(size_t)i*3+1];
            uint32_t hi = got.data[(size_t)i*3+2];
            int seen = 0;
            for (k = 0; k < nbo; k++) {
                if (bo[k*2]==lo && bo[k*2+1]==hi) { seen = 1; break; }
            }
            if (!seen) { FAIL("q row has bogus bounds"); pass = 0; goto oracle_done; }
            /* x must be a distinct col0 value of r in [lo,hi). */
            if (x < lo || x >= hi) { FAIL("q row outside [lo,hi)"); pass = 0; goto oracle_done; }
            {
                int present = 0;
                for (k = 0; k < n_col0; k++) if (col0[k] == x) { present = 1; break; }
                if (!present) { FAIL("q row not a col0 value"); pass = 0; goto oracle_done; }
            }
        }

        /* Completeness: for each (lo,hi), the set of X rows must equal the
         * expected distinct col0 values, with no duplicates. */
        for (k = 0; k < nbo; k++) {
            uint32_t lo = bo[k*2], hi = bo[k*2+1];
            uint32_t exp[256];
            long ne = 0, j, m;
            long matched = 0;
            for (j = 0; j < n_col0; j++)
                if (col0[j] >= lo && col0[j] < hi) exp[ne++] = col0[j];
            if (ne > 256) { FAIL("ne>256"); pass = 0; goto oracle_done; }
            for (j = 0; j < ne; j++) {
                int found = 0, m;
                for (m = 0; m < got.count; m++) {
                    if (got.data[(size_t)m*3+1]==lo && got.data[(size_t)m*3+2]==hi &&
                        got.data[(size_t)m*3]==exp[j]) { found = 1; break; }
                }
                if (!found) { FAIL("missing range value"); pass = 0; goto oracle_done; }
            }
            /* count of q rows for this (lo,hi) == ne (no dup / no extra). */
            for (m = 0, matched = 0; m < got.count; m++)
                if (got.data[(size_t)m*3+1]==lo && got.data[(size_t)m*3+2]==hi)
                    matched++;
            if (matched != ne) { FAIL("row-count mismatch for a bounds pair"); pass = 0; goto oracle_done; }
        }

oracle_done:
        free(bo);
    }

out:
    tset_free(&facts);
    tset_free(&got);
    dl_close(db);
    if (pass) PASS(); else FAIL("oracle mismatch");
}

/* ─── T6: reject matrix ────────────────────────────────────────────────── */

static void t6_reject(char *dir, size_t cap)
{
    dl_db *db = fresh_db(dir, cap, "t6");
    const uint32_t c1[1] = {5};
    int pass = 1;

    TEST("T6 reject matrix (unknown/variadic/negated/ungrounded/head-name/recursive)");
    assert(db);
    assert(dl_declare_relation(db, "r", 1) == 0);
    assert(dl_declare_relation(db, "p", 1) == 0);
    assert(dl_declare_relation_variadic(db, "v") == 0);
    addf(db, "r", c1, 1);

    /* Unknown Rel. */
    if (dl_load_rules(db, "q(X) :- range(X, nosuch, 1, 10).\n") == 0) { FAIL("unknown Rel"); pass = 0; }
    /* Variadic Rel. */
    if (dl_load_rules(db, "q(X) :- range(X, v, 1, 10).\n") == 0) { FAIL("variadic Rel"); pass = 0; }
    /* Negated range. */
    if (dl_load_rules(db, "q(X) :- p(X), !(range(X, r, 1, 10)).\n") == 0) { FAIL("negated range"); pass = 0; }
    /* Ungrounded bound var Y. */
    if (dl_load_rules(db, "q(X) :- range(X, r, Y, 10).\n") == 0) { FAIL("ungrounded bound"); pass = 0; }
    /* Rule head named `range`. */
    if (dl_load_rules(db, "range(X) :- p(X).\n") == 0) { FAIL("head named range"); pass = 0; }

    dl_close(db);

    /* Range over a RECURSIVE idb — separate db (program must actually
     * compile a recursive relation, which the gate then rejects). */
    {
        dl_db *db2 = fresh_db(dir, cap, "t6rec");
        uint32_t e[2];
        assert(db2);
        assert(dl_declare_relation(db2, "edge", 2) == 0);
        e[0] = 1; e[1] = 2; addf(db2, "edge", e, 2);
        e[0] = 2; e[1] = 3; addf(db2, "edge", e, 2);
        /* reach is self-recursive; range over it must be rejected at compile. */
        if (dl_load_rules(db2,
                "reach(X) :- edge(X, Y), reach(Y).\n"
                "reach(X) :- edge(X, _).\n"
                "q(X) :- range(X, reach, 1, 10).\n") == 0) {
            FAIL("range over recursive idb");
            pass = 0;
        }
        dl_close(db2);
    }

    if (pass) PASS(); else FAIL("reject matrix leak");
}

/* ─── main ─────────────────────────────────────────────────────────────── */

int main(void)
{
    char dir[512];

    printf("=== M11 range predicate tests ===\n");
    srand(424242);

    t1_generator_arity1(dir, sizeof(dir));
    t2_filter(dir, sizeof(dir));
    t3_arity2_distinct(dir, sizeof(dir));
    t4_edge_cases(dir, sizeof(dir));
    t5_oracle(dir, sizeof(dir));
    t6_reject(dir, sizeof(dir));

    printf("\n%d tests run, %d failed.\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
