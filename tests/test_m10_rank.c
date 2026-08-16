/*
 * test_m10_rank.c — Tier-2 order-statistics (rank / select / range_count)
 *
 * Exhaustive exact-match backstop for the DAFSA subtree-count rank/select/
 * range_count primitives, following the verified oracle pattern:
 *
 *   T1  lex-order rank:    rank(ts[i]) == i for every enumerated tuple.
 *   T2  select:            select(i) == ts[i], round-tripped through rank.
 *   T3  range_count:       range_count(ts[a], ts[b]) == b - a (half-open).
 *   T4  absent-key rank:   a key between/outside present values ranks to its
 *                          exact insertion position.
 *   T5  interleaved add/delete (dirty/rebuild): after every mutation the full
 *          rank/select/range_count invariants still hold.
 *   T6  empty relation:    select(0) == -1, rank(any) == 0, dl_count == 0.
 *   T7  cross-product stress: arity-2 with small col0 x wide col1 =>
 *          n_states << n_tuples; dl_count == N distinct keys (NOT a graph
 *          path count), rank/select/range_count exact over the whole range.
 *   T8  rejection:         NULL/unknown rel/arity-mismatch/variadic all
 *                          return -1 (select) / UINT64_MAX (rank/count).
 *
 * Build: make tests/test_m10_rank (link ALL_OBJS) — see Makefile.
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

#define MAXA 8

/* ─── Tuple buffer: collects dl_prefix output (already in lex/numeric order) ── */

typedef struct {
    uint32_t *data;    /* count*arity u32s */
    long      count;
    uint8_t   arity;
    long      cap;
} tset;

static int tset_add(tset *t, const uint32_t *cols, uint8_t arity)
{
    if (t->count >= t->cap) {
        long nc = t->cap ? t->cap * 2 : 256;
        uint32_t *nd = realloc(t->data, (size_t)nc * arity * sizeof(uint32_t));
        if (!nd) return -1;
        t->data = nd;
        t->cap = nc;
    }
    memcpy(t->data + (size_t)t->count * arity, cols,
           (size_t)arity * sizeof(uint32_t));
    t->count++;
    return 0;
}

static void tset_free(tset *t) { free(t->data); memset(t, 0, sizeof(*t)); }

static int sink_cb(const uint32_t *cols, uint8_t arity, void *user)
{
    return tset_add((tset *)user, cols, arity);
}

/* Enumerate all tuples of `rel` into `ts` (lex/numeric order via u32BE). */
static long collect(dl_db *db, const char *rel, uint8_t arity, tset *ts)
{
    ts->arity = arity;
    return dl_prefix(db, rel, NULL, 0, sink_cb, ts);
}

/* ─── DB fixture ───────────────────────────────────────────────────────── */

static const char *BASE = "build-tmp/rank";

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

/* ─── T1..T4: core invariants over a sorted tuple buffer ──────────────── */
/* Check rank==i, select(i)==ts[i], range_count over the buffer. */
static void check_invariants(dl_db *db, const char *rel, const tset *ts)
{
    long i, a, b;
    uint32_t out[MAXA];
    uint64_t r;

    for (i = 0; i < ts->count; i++) {
        const uint32_t *t = ts->data + (size_t)i * ts->arity;
        r = dl_rank(db, rel, t, ts->arity);
        if (r != (uint64_t)i) { FAIL("rank == index"); return; }

        if (dl_select(db, rel, (uint64_t)i, out, ts->arity) != 0) {
            FAIL("select returned error"); return;
        }
        if (memcmp(out, t, (size_t)ts->arity * sizeof(uint32_t)) != 0) {
            FAIL("select tuple mismatch"); return;
        }
    }

    /* select(i) then rank back must be i (round-trip). */
    for (i = 0; i < ts->count; i++) {
        if (dl_select(db, rel, (uint64_t)i, out, ts->arity) != 0) continue;
        if (dl_rank(db, rel, out, ts->arity) != (uint64_t)i) {
            FAIL("select->rank round-trip"); return;
        }
    }

    /* range_count(ts[a], ts[b]) == b-a for many random [a,b). */
    for (i = 0; i < 200 && ts->count > 1; i++) {
        a = (long)(rand() % (unsigned)(ts->count + 1));
        b = (long)(rand() % (unsigned)(ts->count + 1));
        if (a > b) { long t = a; a = b; b = t; }
        if (a >= ts->count) a = ts->count - 1;
        if (b >= ts->count) b = ts->count;
        if (b == ts->count) {
            /* range [ts[a], +\infty) = count - a */
            r = dl_range_count(db, rel, ts->data + (size_t)a * ts->arity,
                               ts->data + (size_t)(ts->count - 1) * ts->arity,
                               ts->arity);
            if (r != (uint64_t)(ts->count - a - 1)) { FAIL("range_count tail"); return; }
        } else if (a < b) {
            r = dl_range_count(db, rel, ts->data + (size_t)a * ts->arity,
                               ts->data + (size_t)b * ts->arity, ts->arity);
            if (r != (uint64_t)(b - a)) { FAIL("range_count [a,b)"); return; }
        }
    }
}

/* ─── Tests ────────────────────────────────────────────────────────────── */

static void t1_rank_select_range(char *dir, size_t cap)
{
    dl_db *db = fresh_db(dir, cap, "t1");
    tset ts;
    int i;
    uint32_t cols[2];
    uint32_t out[MAXA];

    memset(&ts, 0, sizeof(ts));
    TEST("T1/T2/T3 rank/select/range over lex-ordered arity-2 relation");

    assert(dl_declare_relation(db, "r", 2) == 0);
    /* Distinct random-ish tuples; dl_prefix returns them in lex order. */
    for (i = 0; i < 500; i++) {
        cols[0] = (uint32_t)(rand() % 100);
        cols[1] = (uint32_t)(rand() % 1000);
        dl_add_fact(db, "r", cols, 2);
    }

    if (collect(db, "r", 2, &ts) < 0) { FAIL("collect"); goto out; }
    check_invariants(db, "r", &ts);

    /* dl_count == number of distinct tuples. */
    if (dl_count(db, "r") != (uint64_t)ts.count) { FAIL("dl_count"); goto out; }

    /* select(0) == smallest, select(count-1) == largest. */
    if (dl_select(db, "r", 0, out, 2) != 0) { FAIL("select(0)"); goto out; }
    if (memcmp(out, ts.data, 2 * sizeof(uint32_t)) != 0) { FAIL("select(0) val"); goto out; }
    if (dl_select(db, "r", (uint64_t)ts.count - 1, out, 2) != 0) { FAIL("select last"); goto out; }
    if (memcmp(out, ts.data + (size_t)(ts.count - 1) * 2, 2 * sizeof(uint32_t)) != 0) {
        FAIL("select last val"); goto out;
    }
    if (dl_select(db, "r", (uint64_t)ts.count, out, 2) != -1) { FAIL("select(count)!= -1"); goto out; }

    PASS();

out:
    tset_free(&ts);
    dl_close(db);
}

static void t4_absent_key_rank(char *dir, size_t cap)
{
    dl_db *db = fresh_db(dir, cap, "t4");
    tset ts;
    int i;
    uint32_t cols[2];
    uint32_t key[2];
    uint64_t r, expected;
    long ins;

    memset(&ts, 0, sizeof(ts));
    TEST("T4 absent-key rank == insertion position");

    assert(dl_declare_relation(db, "r", 2) == 0);
    /* Ascending col0 with small col1: keys are lex = numeric ordered. */
    for (i = 0; i < 100; i++) {
        cols[0] = (uint32_t)(i * 3);        /* 0,3,6,...  gaps for absent keys */
        cols[1] = 7;
        dl_add_fact(db, "r", cols, 2);
    }
    if (collect(db, "r", 2, &ts) < 0 || ts.count != 100) { FAIL("collect"); goto out; }

    /* An absent key between present ones: col0 = 5 (between 3 and 6). */
    key[0] = 5; key[1] = 7;
    expected = 0;
    for (ins = 0; ins < ts.count; ins++) {
        const uint32_t *t = ts.data + (size_t)ins * 2;
        if (t[0] < key[0]) expected++;
        else break;
    }
    r = dl_rank(db, "r", key, 2);
    if (r != expected) { FAIL("absent-key rank (between)"); goto out; }

    /* An absent key BELOW all present values. */
    key[0] = 0; key[1] = 0;
    r = dl_rank(db, "r", key, 2);
    if (r != 0) { FAIL("absent-key rank (below)"); goto out; }

    /* An absent key ABOVE all present values (== count). */
    key[0] = 0xFFFFFFFFu; key[1] = 0xFFFFFFFFu;
    r = dl_rank(db, "r", key, 2);
    if (r != (uint64_t)ts.count) { FAIL("absent-key rank (above)"); goto out; }

    PASS();
out:
    tset_free(&ts);
    dl_close(db);
}

static void t5_interleaved(char *dir, size_t cap)
{
    dl_db *db = fresh_db(dir, cap, "t5");
    tset ts;
    int i;
    uint32_t cols[2];

    memset(&ts, 0, sizeof(ts));
    TEST("T5 interleaved add/delete rebuild path");

    assert(dl_declare_relation(db, "r", 2) == 0);

    /* Seed. */
    for (i = 0; i < 200; i++) {
        cols[0] = (uint32_t)(i % 40);
        cols[1] = (uint32_t)(i * 7 % 1000);
        dl_add_fact(db, "r", cols, 2);
    }

    /* Interleave adds and deletes; after each mutation re-derive and check. */
    for (i = 0; i < 300; i++) {
        tset ts2;
        memset(&ts2, 0, sizeof(ts2));
        if (i % 2 == 0) {
            cols[0] = (uint32_t)(200 + i);
            cols[1] = (uint32_t)(i);
            dl_add_fact(db, "r", cols, 2);
        } else {
            /* Delete a random present tuple. */
            if (collect(db, "r", 2, &ts2) > 0) {
                long idx = (long)(rand() % (unsigned)ts2.count);
                const uint32_t *t = ts2.data + (size_t)idx * 2;
                dl_delete_fact(db, "r", t, 2);
            }
        }
        tset_free(&ts2);

        /* Re-derive + check full invariants each round (dirty/rebuild). */
        ts.count = 0;
        if (collect(db, "r", 2, &ts) < 0) { FAIL("collect"); goto out; }
        check_invariants(db, "r", &ts);
        if (tests_failed) goto out;
    }

    PASS();
out:
    tset_free(&ts);
    dl_close(db);
}

static void t6_empty(char *dir, size_t cap)
{
    dl_db *db = fresh_db(dir, cap, "t6");
    uint32_t cols[2] = {1, 2};
    uint32_t out[MAXA];

    TEST("T6 empty relation");
    assert(dl_declare_relation(db, "r", 2) == 0);

    if (dl_select(db, "r", 0, out, 2) != -1) { FAIL("empty select(0)"); goto out; }
    if (dl_select(db, "r", 5, out, 2) != -1) { FAIL("empty select(5)"); goto out; }
    if (dl_rank(db, "r", cols, 2) != 0) { FAIL("empty rank"); goto out; }
    if (dl_count(db, "r") != 0) { FAIL("empty count"); goto out; }
    if (dl_range_count(db, "r", cols, cols, 2) != 0) { FAIL("empty range_count"); goto out; }

    PASS();
out:
    dl_close(db);
}

static void t7_cross_product(char *dir, size_t cap)
{
    dl_db *db = fresh_db(dir, cap, "t7");
    tset ts;
    long a, b;
    uint32_t cols[2];
    uint64_t expected_count = 2000;   /* 20 x 100 cross-product */

    memset(&ts, 0, sizeof(ts));
    TEST("T7 cross-product stress (distinct-key count, n_states << n_tuples)");

    assert(dl_declare_relation(db, "r", 2) == 0);
    for (a = 0; a < 20; a++) {
        for (b = 0; b < 100; b++) {
            cols[0] = (uint32_t)a;
            cols[1] = (uint32_t)b;
            dl_add_fact(db, "r", cols, 2);
        }
    }

    /* All A*B tuples are distinct keys; dl_count must be exactly A*B.  This is
     * precisely what a naive graph-PATH count would get wrong (the minimized
     * DAG shares one suffix sub-DAFSA across all 20 rows: ~20+100+2 states vs
     * 2000 keys). */
    if (dl_count(db, "r") != expected_count) {
        FAIL("cross-product dl_count == A*B"); goto out;
    }

    if (collect(db, "r", 2, &ts) < 0) { FAIL("collect"); goto out; }
    if (ts.count != (long)expected_count) { FAIL("cross-product collect count"); goto out; }
    check_invariants(db, "r", &ts);

    /* range_count over a wide slice. */
    {
        uint32_t lo[2] = {0, 0};
        uint32_t hi[2] = {20, 100};   /* exclusive upper bound (all tuples) */
        uint64_t r = dl_range_count(db, "r", lo, hi, 2);
        if (r != expected_count) { FAIL("cross-product full range"); goto out; }
    }
    {
        uint32_t lo[2] = {5, 0};
        uint32_t hi[2] = {6, 0};      /* [5,0) .. [6,0) => exactly 100 tuples */
        uint64_t r = dl_range_count(db, "r", lo, hi, 2);
        if (r != 100) { FAIL("cross-product one-row range"); goto out; }
    }

    /* rank of the largest tuple is count-1 (its index), proving distinct-key
     * counting not path counting across the shared suffix. */
    {
        uint32_t last[2] = {19, 99};
        uint64_t r = dl_rank(db, "r", last, 2);
        if (r != expected_count - 1) { FAIL("cross-product max rank"); goto out; }
    }

    PASS();
out:
    tset_free(&ts);
    dl_close(db);
}

static void t8_rejection(char *dir, size_t cap)
{
    dl_db *db = fresh_db(dir, cap, "t8");
    uint32_t cols[2] = {1, 2};
    uint32_t out[MAXA];

    TEST("T8 rejection: NULL/unknown/arity-mismatch/variadic");

    assert(dl_declare_relation(db, "r", 2) == 0);
    assert(dl_declare_relation_variadic(db, "v") == 0);
    dl_add_fact(db, "r", cols, 2);

    /* NULL rel name. */
    if (dl_rank(db, NULL, cols, 2) != UINT64_MAX) { FAIL("rank NULL rel"); goto out; }
    if (dl_select(db, NULL, 0, out, 2) != -1) { FAIL("select NULL rel"); goto out; }
    if (dl_count(db, NULL) != UINT64_MAX) { FAIL("count NULL rel"); goto out; }

    /* NULL cols / out. */
    if (dl_rank(db, "r", NULL, 2) != UINT64_MAX) { FAIL("rank NULL cols"); goto out; }
    if (dl_select(db, "r", 0, NULL, 2) != -1) { FAIL("select NULL out"); goto out; }

    /* Unknown rel. */
    if (dl_rank(db, "nope", cols, 2) != UINT64_MAX) { FAIL("rank unknown rel"); goto out; }
    if (dl_select(db, "nope", 0, out, 2) != -1) { FAIL("select unknown rel"); goto out; }
    if (dl_count(db, "nope") != UINT64_MAX) { FAIL("count unknown rel"); goto out; }

    /* Arity mismatch. */
    if (dl_rank(db, "r", cols, 1) != UINT64_MAX) { FAIL("rank arity mismatch"); goto out; }
    if (dl_select(db, "r", 0, out, 1) != -1) { FAIL("select arity mismatch"); goto out; }
    if (dl_range_count(db, "r", cols, cols, 3) != UINT64_MAX) { FAIL("range arity mismatch"); goto out; }

    /* Variadic rejected. */
    if (dl_rank(db, "v", cols, 2) != UINT64_MAX) { FAIL("rank variadic"); goto out; }
    if (dl_select(db, "v", 0, out, 2) != -1) { FAIL("select variadic"); goto out; }
    if (dl_count(db, "v") != UINT64_MAX) { FAIL("count variadic"); goto out; }

    PASS();
out:
    dl_close(db);
}

static void t9_prefix_bound(char *dir, size_t cap)
{
    dl_db *db = fresh_db(dir, cap, "t9");
    uint32_t leading[1];
    uint32_t cols[2];
    uint32_t out[MAXA];
    long a, b;
    uint64_t r;

    TEST("T9 prefix-bound rank/select/range_count (leading col0)");

    assert(dl_declare_relation(db, "r", 2) == 0);
    /* Cross-product arity-2: 20 col0 x 100 col1 (heavy suffix sharing). */
    for (a = 0; a < 20; a++) {
        for (b = 0; b < 100; b++) {
            cols[0] = (uint32_t)a;
            cols[1] = (uint32_t)b;
            dl_add_fact(db, "r", cols, 2);
        }
    }

    /* rank_bound(leading={a}, k=1, (a,b)) == b: within the bound the suffixes
     * are the complete set 0..99, so the suffix rank is exactly b. */
    for (a = 0; a < 20; a++) {
        for (b = 0; b < 100; b++) {
            leading[0] = (uint32_t)a;
            cols[0] = (uint32_t)a;
            cols[1] = (uint32_t)b;
            r = dl_rank_bound(db, "r", leading, 1, cols, 2);
            if (r != (uint64_t)b) { FAIL("rank_bound == b"); goto out; }
        }
    }

    /* Round-trip: select_bound(idx) then rank_bound of that tuple == idx. */
    for (a = 0; a < 20; a++) {
        leading[0] = (uint32_t)a;
        for (b = 0; b < 100; b++) {
            if (dl_select_bound(db, "r", leading, 1, (uint64_t)b, out, 2) != 0) {
                FAIL("select_bound error"); goto out;
            }
            if (dl_rank_bound(db, "r", leading, 1, out, 2) != (uint64_t)b) {
                FAIL("select_bound->rank_bound round-trip"); goto out;
            }
        }
    }

    /* select_bound within bound: idx-th tuple == (a, idx), full tuple out. */
    for (a = 0; a < 20; a++) {
        leading[0] = (uint32_t)a;
        for (b = 0; b < 100; b++) {
            if (dl_select_bound(db, "r", leading, 1, (uint64_t)b, out, 2) != 0) {
                FAIL("select_bound error"); goto out;
            }
            if (out[0] != (uint32_t)a || out[1] != (uint32_t)b) {
                FAIL("select_bound tuple"); goto out;
            }
        }
        /* idx == bound count -> -1 (out of range). */
        if (dl_select_bound(db, "r", leading, 1, 100, out, 2) != -1) {
            FAIL("select_bound(count) != -1"); goto out;
        }
    }

    /* range_count_bound within bound: tuples (a, 5..10) == 5. */
    for (a = 0; a < 20; a++) {
        leading[0] = (uint32_t)a;
        cols[0] = (uint32_t)a;
        cols[1] = 5;
        out[0] = (uint32_t)a;
        out[1] = 10;
        r = dl_range_count_bound(db, "r", leading, 1, cols, out, 2);
        if (r != 5) { FAIL("range_count_bound == 5"); goto out; }
    }

    /* Unmatched prefix: leading value not present as a col0 (>=20) -> 0 / -1. */
    leading[0] = 20;
    cols[0] = 20; cols[1] = 0;
    if (dl_rank_bound(db, "r", leading, 1, cols, 2) != 0) {
        FAIL("unmatched rank_bound != 0"); goto out;
    }
    if (dl_select_bound(db, "r", leading, 1, 0, out, 2) != -1) {
        FAIL("unmatched select_bound != -1"); goto out;
    }
    if (dl_range_count_bound(db, "r", leading, 1, cols, cols, 2) != 0) {
        FAIL("unmatched range_count_bound != 0"); goto out;
    }

    PASS();
out:
    dl_close(db);
}

static void t10_bound_empty_reject(char *dir, size_t cap)
{
    dl_db *db = fresh_db(dir, cap, "t10");
    uint32_t leading[1] = {7};
    uint32_t cols[2] = {7, 0};
    uint32_t out[MAXA];

    TEST("T10 prefix-bound empty + reject matrix");

    assert(dl_declare_relation(db, "e", 2) == 0);     /* empty arity-2 */
    assert(dl_declare_relation(db, "r", 2) == 0);
    assert(dl_declare_relation_variadic(db, "v") == 0);
    dl_add_fact(db, "r", cols, 2);

    /* Empty relation: rank_bound 0, select_bound -1, range_count_bound 0. */
    if (dl_rank_bound(db, "e", leading, 1, cols, 2) != 0) { FAIL("empty rank_bound"); goto out; }
    if (dl_select_bound(db, "e", leading, 1, 0, out, 2) != -1) { FAIL("empty select_bound"); goto out; }
    if (dl_range_count_bound(db, "e", leading, 1, cols, cols, 2) != 0) { FAIL("empty range_count_bound"); goto out; }

    /* NULL rel name. */
    if (dl_rank_bound(db, NULL, leading, 1, cols, 2) != UINT64_MAX) { FAIL("rank_bound NULL rel"); goto out; }
    if (dl_select_bound(db, NULL, leading, 1, 0, out, 2) != -1) { FAIL("select_bound NULL rel"); goto out; }

    /* NULL cols / out / lo-hi. */
    if (dl_rank_bound(db, "r", leading, 1, NULL, 2) != UINT64_MAX) { FAIL("rank_bound NULL cols"); goto out; }
    if (dl_select_bound(db, "r", leading, 1, 0, NULL, 2) != -1) { FAIL("select_bound NULL out"); goto out; }
    if (dl_range_count_bound(db, "r", leading, 1, NULL, cols, 2) != UINT64_MAX) { FAIL("range_count_bound NULL lo"); goto out; }
    if (dl_range_count_bound(db, "r", leading, 1, cols, NULL, 2) != UINT64_MAX) { FAIL("range_count_bound NULL hi"); goto out; }

    /* k > 0 but leading NULL -> rejected. */
    if (dl_rank_bound(db, "r", NULL, 1, cols, 2) != UINT64_MAX) { FAIL("rank_bound NULL leading"); goto out; }
    if (dl_select_bound(db, "r", NULL, 1, 0, out, 2) != -1) { FAIL("select_bound NULL leading"); goto out; }
    if (dl_range_count_bound(db, "r", NULL, 1, cols, cols, 2) != UINT64_MAX) { FAIL("range_count_bound NULL leading"); goto out; }

    /* Unknown rel. */
    if (dl_rank_bound(db, "nope", leading, 1, cols, 2) != UINT64_MAX) { FAIL("rank_bound unknown rel"); goto out; }
    if (dl_select_bound(db, "nope", leading, 1, 0, out, 2) != -1) { FAIL("select_bound unknown rel"); goto out; }

    /* Arity mismatch. */
    if (dl_rank_bound(db, "r", leading, 1, cols, 3) != UINT64_MAX) { FAIL("rank_bound arity mismatch"); goto out; }
    if (dl_select_bound(db, "r", leading, 1, 0, out, 3) != -1) { FAIL("select_bound arity mismatch"); goto out; }
    if (dl_range_count_bound(db, "r", leading, 1, cols, cols, 3) != UINT64_MAX) { FAIL("range_count_bound arity mismatch"); goto out; }

    /* k > arity rejected at the relation layer. */
    if (dl_rank_bound(db, "r", leading, 3, cols, 2) != UINT64_MAX) { FAIL("rank_bound k>arity"); goto out; }
    if (dl_select_bound(db, "r", leading, 3, 0, out, 2) != -1) { FAIL("select_bound k>arity"); goto out; }
    if (dl_range_count_bound(db, "r", leading, 3, cols, cols, 2) != UINT64_MAX) { FAIL("range_count_bound k>arity"); goto out; }

    /* Variadic rejected. */
    if (dl_rank_bound(db, "v", leading, 1, cols, 2) != UINT64_MAX) { FAIL("rank_bound variadic"); goto out; }
    if (dl_select_bound(db, "v", leading, 1, 0, out, 2) != -1) { FAIL("select_bound variadic"); goto out; }
    if (dl_range_count_bound(db, "v", leading, 1, cols, cols, 2) != UINT64_MAX) { FAIL("range_count_bound variadic"); goto out; }

    PASS();
out:
    dl_close(db);
}

/* ─── main ─────────────────────────────────────────────────────────────── */

int main(void)
{
    char dir[512];

    printf("=== M10 order-statistics (rank/select/range_count) tests ===\n");
    srand(12345);

    t1_rank_select_range(dir, sizeof(dir));
    t4_absent_key_rank(dir, sizeof(dir));
    t5_interleaved(dir, sizeof(dir));
    t6_empty(dir, sizeof(dir));
    t7_cross_product(dir, sizeof(dir));
    t8_rejection(dir, sizeof(dir));
    t9_prefix_bound(dir, sizeof(dir));
    t10_bound_empty_reject(dir, sizeof(dir));

    printf("\n%d tests run, %d failed.\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
