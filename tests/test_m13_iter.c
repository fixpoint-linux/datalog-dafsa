/*
 * test_m13_iter.c — pull-based sorted iterator + merge-join (range-index #5)
 *
 * Verifies the PUBLIC dl_iter_open/seek/next/arity/close cursor (a resumable
 * DFS over the relation DAFSA, emitting tuples in ascending u32BE key order)
 * and dl_merge_join (equi-join of two sorted iterators on a leading prefix,
 * cross-product semantics with duplicates, output sorted).  Iterators route to
 * the mmap snapshot view when a snapshot is current (db->snap_version > 0),
 * else the in-memory relation.
 *
 *   T1  pull == dl_prefix set, sorted:  cross-product facts; dl_iter_open(k=0),
 *                                       collect via next; count==dl_count, set==
 *                                       dl_prefix order, strictly non-decreasing,
 *                                       arity correct.
 *   T2  prefix bound:                   leading k in {0,1,2,arity}; every tuple's
 *                                       first k cols == leading; count ==
 *                                       dl_prefix(rel,leading,k); k==arity -> 0/1.
 *   T3  re-seek:                        open, consume a few, seek a new prefix;
 *                                       seek absent prefix -> next 0; seek(k>arity)
 *                                       -> -1; seek(k>0 && NULL) -> -1.
 *   T4  snapshot:                       publish, open iter, collect == published set
 *                                       sorted == dl_query; dl_add_fact after publish
 *                                       -> still reads the published view; re-publish
 *                                       reflects the new fact; pre-publish reads live.
 *   T5  error matrix:                   open(NULL/unknown/variadic/k>arity/k>0&&
 *                                       !leading) -> NULL; next(NULL,..) -> -1;
 *                                       next(it,NULL) -> -1; seek(NULL,..) -> -1.
 *   T6  merge-join:                     two rels with duplicate join keys, jcols 1
 *                                       and 2, vs brute-force nested-loop equi-join
 *                                       (same sorted output + count incl dups);
 *                                       sorted; jcols=0/>min arity/NULL -> -1; both
 *                                       exhausted after; early-stop cb -> partial.
 *
 * Build: make tests/test_m13_iter (link ALL_OBJS) — see Makefile.
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

/* ─── Tuple buffer ────────────────────────────────────────────────────── */

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

/* Collect an iterator into a tset.  Returns 0 ok, -1 on error. */
static int collect_iter(dl_iter *it, tset *ts)
{
    uint32_t cols[MAXA];
    int rc;
    while ((rc = dl_iter_next(it, cols)) == 1) {
        if (tset_add(ts, cols, dl_iter_arity(it)) != 0) return -1;
    }
    return (rc == 0) ? 0 : -1;
}

/* 1 iff tuples t0 and t1 (arity a) are strictly non-decreasing: each column
 * position compares <=.  Column-major numeric order == key order. */
static int tset_sorted(const uint32_t *data, long n, uint8_t arity)
{
    long i, c;
    for (i = 1; i < n; i++) {
        for (c = 0; c < arity; c++) {
            uint32_t p = data[(size_t)(i - 1) * arity + c];
            uint32_t q = data[(size_t)i * arity + c];
            if (p < q) break;
            if (p > q) return 0;
        }
    }
    return 1;
}

/* ─── DB fixture ──────────────────────────────────────────────────────── */

static const char *BASE = "build-tmp/m13iter";

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

/* ─── T1: pull == dl_prefix set, sorted ──────────────────────────────── */

static void t1_pull(char *dir, size_t cap)
{
    dl_db *db = fresh_db(dir, cap, "t1");
    tset its, pfx;
    uint32_t cols[3];
    dl_iter *it;
    int a, b, c;
    uint64_t expected = 4 * 6 * 8;   /* 4 x 6 x 8 cross-product */

    memset(&its, 0, sizeof(its));
    memset(&pfx, 0, sizeof(pfx));

    TEST("T1 pull == dl_prefix set, sorted, count/arity correct");

    assert(dl_declare_relation(db, "r", 3) == 0);
    for (a = 0; a < 4; a++)
        for (b = 0; b < 6; b++)
            for (c = 0; c < 8; c++) {
                cols[0] = (uint32_t)a; cols[1] = (uint32_t)b; cols[2] = (uint32_t)c;
                dl_add_fact(db, "r", cols, 3);
            }

    if (dl_count(db, "r") != expected) { FAIL("dl_count == cross-product"); goto out; }

    it = dl_iter_open(db, "r", NULL, 0);
    if (!it) { FAIL("open k=0"); goto out; }
    if (dl_iter_arity(it) != 3) { FAIL("iter arity == 3"); goto out; }
    if (collect_iter(it, &its) != 0) { FAIL("collect iter"); dl_iter_close(it); goto out; }
    dl_iter_close(it);

    if (its.count != (long)expected) { FAIL("iter count == cross-product"); goto out; }
    if (dl_prefix(db, "r", NULL, 0, sink_cb, &pfx) != (long)expected) {
        FAIL("dl_prefix count"); goto out;
    }
    if (pfx.count != its.count ||
        memcmp(pfx.data, its.data, (size_t)its.count * 3 * sizeof(uint32_t)) != 0) {
        FAIL("iter set != dl_prefix set"); goto out;
    }
    if (!tset_sorted(its.data, its.count, 3)) { FAIL("iter output not sorted"); goto out; }

    PASS();
out:
    tset_free(&its);
    tset_free(&pfx);
    dl_close(db);
}

/* ─── T2: prefix bound ────────────────────────────────────────────────── */

static void t2_bound(char *dir, size_t cap)
{
    dl_db *db = fresh_db(dir, cap, "t2");
    tset its, pfx;
    dl_iter *it;
    uint32_t cols[3];
    uint32_t lead[3] = { 2, 3, 5 };
    int k, i;

    memset(&its, 0, sizeof(its));
    memset(&pfx, 0, sizeof(pfx));

    TEST("T2 prefix bound: leading k in {0,1,2,arity}");

    assert(dl_declare_relation(db, "r", 3) == 0);
    for (i = 0; i < 100; i++) {
        int c;
        for (c = 0; c < 3; c++) cols[c] = (uint32_t)(rand() % 10);
        dl_add_fact(db, "r", cols, 3);
    }
    /* Ensure the exact leading prefix {2,3,5} exists. */
    cols[0] = 2; cols[1] = 3; cols[2] = 5; dl_add_fact(db, "r", cols, 3);

    for (k = 0; k < 4; k++) {
        long expect;
        long n;
        long j;
        const uint32_t *ld = (k == 0) ? NULL : lead;

        tset_free(&its); memset(&its, 0, sizeof(its));
        tset_free(&pfx); memset(&pfx, 0, sizeof(pfx));

        it = dl_iter_open(db, "r", ld, (uint8_t)k);
        if (!it) { FAIL("open bound"); goto out; }
        if (collect_iter(it, &its) != 0) { FAIL("collect bound"); dl_iter_close(it); goto out; }
        dl_iter_close(it);

        expect = dl_prefix(db, "r", ld, (uint8_t)k, sink_cb, &pfx);
        if (expect < 0) { FAIL("dl_prefix bound"); goto out; }
        n = its.count;
        if (n != expect) { FAIL("bound count == dl_prefix"); goto out; }
        if (pfx.count != n ||
            memcmp(pfx.data, its.data, (size_t)n * 3 * sizeof(uint32_t)) != 0) {
            FAIL("bound set == dl_prefix set"); goto out;
        }
        for (j = 0; j < n; j++) {
            const uint32_t *t = its.data + (size_t)j * 3;
            for (i = 0; i < k; i++)
                if (t[i] != lead[i]) { FAIL("bound first-k cols == leading"); goto out; }
        }
    }

    /* k == arity on a present exact key -> exactly 1 tuple. */
    {
        tset_free(&its); memset(&its, 0, sizeof(its));
        it = dl_iter_open(db, "r", lead, 3);
        if (!it) { FAIL("open k=arity"); goto out; }
        if (collect_iter(it, &its) != 0) { FAIL("collect k=arity"); dl_iter_close(it); goto out; }
        dl_iter_close(it);
        if (its.count != 1) { FAIL("k==arity present -> 1"); goto out; }
    }
    /* k == arity on an ABSENT exact key -> 0. */
    {
        uint32_t absent[3] = { 99, 99, 99 };
        tset_free(&its); memset(&its, 0, sizeof(its));
        it = dl_iter_open(db, "r", absent, 3);
        if (!it) { FAIL("open k=arity absent"); goto out; }
        if (collect_iter(it, &its) != 0) { FAIL("collect k=arity absent"); dl_iter_close(it); goto out; }
        dl_iter_close(it);
        if (its.count != 0) { FAIL("k==arity absent -> 0"); goto out; }
    }

    PASS();
out:
    tset_free(&its);
    tset_free(&pfx);
    dl_close(db);
}

/* ─── T3: re-seek ─────────────────────────────────────────────────────── */

static void t3_reseek(char *dir, size_t cap)
{
    dl_db *db = fresh_db(dir, cap, "t3");
    uint32_t cols[2];
    uint32_t out[MAXA];
    uint32_t lead_a[1] = { 1 };
    uint32_t lead_b[1] = { 5 };
    uint32_t lead_abs[1] = { 42 };
    dl_iter *it;
    int rc;

    TEST("T3 re-seek: consume, seek new/absent prefix, rejection matrix");

    assert(dl_declare_relation(db, "r", 2) == 0);
    cols[0] = 1; cols[1] = 10; dl_add_fact(db, "r", cols, 2);
    cols[0] = 1; cols[1] = 20; dl_add_fact(db, "r", cols, 2);
    cols[0] = 5; cols[1] = 50; dl_add_fact(db, "r", cols, 2);

    it = dl_iter_open(db, "r", NULL, 0);
    if (!it) { FAIL("open"); goto out; }

    /* Consume the first tuple, then re-seek to prefix {1}. */
    rc = dl_iter_next(it, out);
    if (rc != 1) { FAIL("first next"); goto out; }
    if (dl_iter_seek(it, lead_a, 1) != 0) { FAIL("seek lead_a"); goto out; }
    rc = dl_iter_next(it, out);
    if (rc != 1 || out[0] != 1 || out[1] != 10) { FAIL("post-seek tuple a0"); goto out; }
    rc = dl_iter_next(it, out);
    if (rc != 1 || out[0] != 1 || out[1] != 20) { FAIL("post-seek tuple a1"); goto out; }
    rc = dl_iter_next(it, out);
    if (rc != 0) { FAIL("post-seek end"); goto out; }

    /* Re-seek to a present prefix {5}. */
    if (dl_iter_seek(it, lead_b, 1) != 0) { FAIL("seek lead_b"); goto out; }
    rc = dl_iter_next(it, out);
    if (rc != 1 || out[0] != 5 || out[1] != 50) { FAIL("post-seek tuple b"); goto out; }
    rc = dl_iter_next(it, out);
    if (rc != 0) { FAIL("post-seek b end"); goto out; }

    /* Re-seek to an ABSENT prefix {42} -> valid empty iterator (next==0). */
    if (dl_iter_seek(it, lead_abs, 1) != 0) { FAIL("seek lead_abs"); goto out; }
    if (dl_iter_next(it, out) != 0) { FAIL("absent prefix next==0"); goto out; }

    /* Rejections: k > arity, k>0 && NULL leading. */
    if (dl_iter_seek(it, lead_a, 9) != -1) { FAIL("seek k>arity -> -1"); goto out; }
    if (dl_iter_seek(it, NULL, 1) != -1) { FAIL("seek k>0 NULL -> -1"); goto out; }

    /* After the failing seeks, the iterator must still be usable. */
    if (dl_iter_seek(it, lead_a, 1) != 0) { FAIL("recover seek"); goto out; }
    rc = dl_iter_next(it, out);
    if (rc != 1 || out[0] != 1 || out[1] != 10) { FAIL("recover tuple"); goto out; }

    dl_iter_close(it);
    PASS();
out:
    dl_close(db);
}

/* ─── T4: snapshot routing ────────────────────────────────────────────── */

static void t4_snapshot(char *dir, size_t cap)
{
    dl_db *db = fresh_db(dir, cap, "t4");
    tset its;
    uint32_t cols[2];
    uint32_t new_fact[2] = { 4, 40 };
    dl_iter *it;

    memset(&its, 0, sizeof(its));
    TEST("T4 snapshot: publish, read view, mutate live, still published");

    assert(dl_declare_relation(db, "r", 2) == 0);
    cols[0] = 1; cols[1] = 10; dl_add_fact(db, "r", cols, 2);
    cols[0] = 2; cols[1] = 20; dl_add_fact(db, "r", cols, 2);
    cols[0] = 3; cols[1] = 30; dl_add_fact(db, "r", cols, 2);

    /* Pre-publish: reads live. */
    it = dl_iter_open(db, "r", NULL, 0);
    if (!it) { FAIL("pre-publish open"); goto out; }
    if (collect_iter(it, &its) != 0 || its.count != 3) { FAIL("pre-publish collect 3"); dl_iter_close(it); goto out; }
    dl_iter_close(it);

    assert(dl_publish_snapshot(db) == 0);

    /* Post-publish: reads the published view. */
    tset_free(&its); memset(&its, 0, sizeof(its));
    it = dl_iter_open(db, "r", NULL, 0);
    if (!it) { FAIL("post-publish open"); goto out; }
    if (collect_iter(it, &its) != 0 || its.count != 3) { FAIL("post-publish collect 3"); dl_iter_close(it); goto out; }
    if (!tset_sorted(its.data, its.count, 2)) { FAIL("published sorted"); dl_iter_close(it); goto out; }
    dl_iter_close(it);

    /* Mutate the LIVE relation; a fresh iterator still reads the published view. */
    dl_add_fact(db, "r", new_fact, 2);
    tset_free(&its); memset(&its, 0, sizeof(its));
    it = dl_iter_open(db, "r", NULL, 0);
    if (!it) { FAIL("post-add open"); goto out; }
    if (collect_iter(it, &its) != 0 || its.count != 3) {
        FAIL("post-add still published 3"); dl_iter_close(it); goto out;
    }
    dl_iter_close(it);

    /* Re-publish: reflects the new fact. */
    assert(dl_publish_snapshot(db) == 0);
    tset_free(&its); memset(&its, 0, sizeof(its));
    it = dl_iter_open(db, "r", NULL, 0);
    if (!it) { FAIL("re-publish open"); goto out; }
    if (collect_iter(it, &its) != 0 || its.count != 4) {
        FAIL("re-publish collect 4"); dl_iter_close(it); goto out;
    }
    if (!tset_sorted(its.data, its.count, 2)) { FAIL("re-publish sorted"); dl_iter_close(it); goto out; }
    dl_iter_close(it);

    PASS();
out:
    tset_free(&its);
    dl_close(db);
}

/* ─── T5: error matrix ────────────────────────────────────────────────── */

static void t5_errors(char *dir, size_t cap)
{
    dl_db *db = fresh_db(dir, cap, "t5");
    uint32_t cols[2] = { 1, 2 };
    uint32_t out[MAXA];
    dl_iter *it;

    TEST("T5 error matrix: open/next/seek rejections");

    assert(dl_declare_relation(db, "r", 2) == 0);
    assert(dl_declare_relation_variadic(db, "v") == 0);
    dl_add_fact(db, "r", cols, 2);

    /* open rejections -> NULL. */
    if (dl_iter_open(NULL, "r", NULL, 0) != NULL) { FAIL("open NULL db"); goto out; }
    if (dl_iter_open(db, "nope", NULL, 0) != NULL) { FAIL("open unknown rel"); goto out; }
    if (dl_iter_open(db, "v", NULL, 0) != NULL) { FAIL("open variadic"); goto out; }
    if (dl_iter_open(db, "r", NULL, 9) != NULL) { FAIL("open k>arity"); goto out; }
    if (dl_iter_open(db, "r", NULL, 1) != NULL) { FAIL("open k>0 NULL leading"); goto out; }

    /* Valid open for the next/seek rejections. */
    it = dl_iter_open(db, "r", NULL, 0);
    if (!it) { FAIL("open valid"); goto out; }

    /* next rejections -> -1. */
    if (dl_iter_next(NULL, out) != -1) { FAIL("next NULL it"); goto out; }
    if (dl_iter_next(it, NULL) != -1) { FAIL("next NULL cols_out"); goto out; }
    /* seek rejections -> -1. */
    if (dl_iter_seek(NULL, cols, 1) != -1) { FAIL("seek NULL it"); goto out; }

    /* The iterator is still usable after the failed calls. */
    if (dl_iter_next(it, out) != 1) { FAIL("still usable after errors"); goto out; }

    dl_iter_close(it);
    PASS();
out:
    dl_close(db);
}

/* ─── T6: merge-join ──────────────────────────────────────────────────── */

typedef struct {
    uint32_t l[MAXA];
    uint32_t r[MAXA];
} jpair;

typedef struct {
    jpair *p;
    long   count;
    long   cap;
    uint8_t la, ra;
} jlist;

static int join_cb(const uint32_t *l, uint8_t la,
                   const uint32_t *r, uint8_t ra, void *user)
{
    jlist *jl = (jlist *)user;
    if (jl->count >= jl->cap) {
        long nc = jl->cap ? jl->cap * 2 : 64;
        jpair *np = realloc(jl->p, (size_t)nc * sizeof(jpair));
        if (!np) return -1;
        jl->p = np;
        jl->cap = nc;
    }
    memcpy(jl->p[jl->count].l, l, la * sizeof(uint32_t));
    memcpy(jl->p[jl->count].r, r, ra * sizeof(uint32_t));
    jl->count++;
    return 0;
}

static void jlist_free(jlist *jl) { free(jl->p); memset(jl, 0, sizeof(*jl)); }

/* Collect the tuples of a relation into a tset via the iterator (sorted). */
static int rel_to_tset(dl_db *db, const char *rel, tset *ts, uint8_t arity)
{
    dl_iter *it = dl_iter_open(db, rel, NULL, 0);
    if (!it) return -1;
    ts->arity = arity;
    if (collect_iter(it, ts) != 0) { dl_iter_close(it); return -1; }
    dl_iter_close(it);
    return 0;
}

/* Brute-force nested-loop equi-join on the first jcols columns.  `a` and `b`
 * are sorted; the nested loop (a outer, b inner, both ascending) yields the
 * same sorted output as the merge-join (equal-key groups, l ascending x r
 * ascending). */
static void brute_join(const tset *a, const tset *b, uint8_t jcols, jlist *out)
{
    long i, j, k;
    for (i = 0; i < a->count; i++) {
        for (j = 0; j < b->count; j++) {
            const uint32_t *la = a->data + (size_t)i * a->arity;
            const uint32_t *lb = b->data + (size_t)j * b->arity;
            int eq = 1;
            for (k = 0; k < jcols; k++)
                if (la[k] != lb[k]) { eq = 0; break; }
            if (eq) {
                if (out->count >= out->cap) {
                    long nc = out->cap ? out->cap * 2 : 64;
                    jpair *np = realloc(out->p, (size_t)nc * sizeof(jpair));
                    if (!np) return;
                    out->p = np;
                    out->cap = nc;
                }
                memcpy(out->p[out->count].l, la, a->arity * sizeof(uint32_t));
                memcpy(out->p[out->count].r, lb, b->arity * sizeof(uint32_t));
                out->count++;
            }
        }
    }
}

static int jlist_equal(const jlist *x, const jlist *y)
{
    long i;
    if (x->count != y->count) return 0;
    for (i = 0; i < x->count; i++) {
        if (memcmp(x->p[i].l, y->p[i].l, x->la * sizeof(uint32_t)) != 0) return 0;
        if (memcmp(x->p[i].r, y->p[i].r, x->ra * sizeof(uint32_t)) != 0) return 0;
    }
    return 1;
}

/* Merge-join output must be sorted by (full l tuple, then full r tuple). */
static int jlist_sorted(const jlist *x)
{
    long i, c;
    for (i = 1; i < x->count; i++) {
        const jpair *prev = &x->p[i - 1];
        const jpair *cur = &x->p[i];
        int cmp = 0;
        for (c = 0; c < x->la; c++) {
            if (prev->l[c] < cur->l[c]) { cmp = -1; break; }
            if (prev->l[c] > cur->l[c]) { cmp = 1; break; }
        }
        if (cmp == 0) {
            for (c = 0; c < x->ra; c++) {
                if (prev->r[c] < cur->r[c]) { cmp = -1; break; }
                if (prev->r[c] > cur->r[c]) { cmp = 1; break; }
            }
        }
        if (cmp > 0) return 0;
    }
    return 1;
}

static int early_stop_cb_calls;
static int early_stop_cb(const uint32_t *l, uint8_t la,
                         const uint32_t *r, uint8_t ra, void *user)
{
    (void)l; (void)r; (void)la; (void)ra; (void)user;
    early_stop_cb_calls++;
    return (early_stop_cb_calls >= 3) ? 1 : 0;   /* stop on the 3rd pair */
}

static void t6_join(char *dir, size_t cap)
{
    dl_db *db = fresh_db(dir, cap, "t6");
    tset A, B;
    jlist mj, bf;
    dl_iter *la, *rb;
    uint32_t cols[2];
    uint32_t dummy[MAXA];
    long emitted;
    int i;

    memset(&A, 0, sizeof(A));
    memset(&B, 0, sizeof(B));
    memset(&mj, 0, sizeof(mj));
    memset(&bf, 0, sizeof(bf));

    TEST("T6 merge-join vs brute-force, duplicates, sorted, rejections");

    /* A: (1,10),(1,11),(1,12),(2,20),(3,30),(3,31)  */
    assert(dl_declare_relation(db, "A", 2) == 0);
    { uint32_t f[6][2] = { {1,10},{1,11},{1,12},{2,20},{3,30},{3,31} };
      for (i = 0; i < 6; i++) { cols[0]=f[i][0]; cols[1]=f[i][1]; dl_add_fact(db, "A", cols, 2); } }
    /* B: (1,100),(1,200),(2,300),(4,400),(4,500)  */
    assert(dl_declare_relation(db, "B", 2) == 0);
    { uint32_t f[5][2] = { {1,100},{1,200},{2,300},{4,400},{4,500} };
      for (i = 0; i < 5; i++) { cols[0]=f[i][0]; cols[1]=f[i][1]; dl_add_fact(db, "B", cols, 2); } }

    if (rel_to_tset(db, "A", &A, 2) != 0) { FAIL("collect A"); goto out; }
    if (rel_to_tset(db, "B", &B, 2) != 0) { FAIL("collect B"); goto out; }

    /* jcols = 1: join on col0. */
    la = dl_iter_open(db, "A", NULL, 0);
    rb = dl_iter_open(db, "B", NULL, 0);
    if (!la || !rb) { FAIL("open join iters"); goto out; }
    mj.la = 2; mj.ra = 2;
    emitted = dl_merge_join(la, rb, 1, join_cb, &mj);
    if (emitted != mj.count || emitted < 0) { FAIL("merge-join count"); dl_iter_close(la); dl_iter_close(rb); goto out; }
    /* Both iterators exhausted. */
    if (dl_iter_next(la, dummy) != 0 || dl_iter_next(rb, dummy) != 0) {
        FAIL("both exhausted after join"); dl_iter_close(la); dl_iter_close(rb); goto out;
    }
    dl_iter_close(la);
    dl_iter_close(rb);

    /* Expected col0 matches: A.col0 {1,1,1,2,3,3} x B.col0 {1,1,2}:
     *   1: 3*2=6 ; 2: 1*1=1 ; 3: 2*0=0  -> 7 pairs. */
    brute_join(&A, &B, 1, &bf);
    if (mj.count != 7) { FAIL("jcols1 count == 7"); goto out; }
    if (!jlist_equal(&mj, &bf)) { FAIL("jcols1 == brute force"); goto out; }
    if (!jlist_sorted(&mj)) { FAIL("jcols1 sorted"); goto out; }

    /* jcols = 2: full-tuple join (exact match). */
    jlist_free(&mj); memset(&mj, 0, sizeof(mj));
    jlist_free(&bf); memset(&bf, 0, sizeof(bf));
    la = dl_iter_open(db, "A", NULL, 0);
    rb = dl_iter_open(db, "B", NULL, 0);
    mj.la = 2; mj.ra = 2;
    emitted = dl_merge_join(la, rb, 2, join_cb, &mj);
    if (emitted != mj.count || emitted < 0) { FAIL("merge-join jcols2"); dl_iter_close(la); dl_iter_close(rb); goto out; }
    dl_iter_close(la);
    dl_iter_close(rb);
    /* No A tuple equals a B tuple exactly (A.second<100 except... no exact): 0. */
    brute_join(&A, &B, 2, &bf);
    if (!jlist_equal(&mj, &bf)) { FAIL("jcols2 == brute force"); goto out; }
    if (!jlist_sorted(&mj)) { FAIL("jcols2 sorted"); goto out; }

    /* Rejections -> -1. */
    la = dl_iter_open(db, "A", NULL, 0);
    rb = dl_iter_open(db, "B", NULL, 0);
    if (dl_merge_join(NULL, rb, 1, join_cb, &mj) != -1) { FAIL("join NULL l"); goto out; }
    if (dl_merge_join(la, NULL, 1, join_cb, &mj) != -1) { FAIL("join NULL r"); goto out; }
    if (dl_merge_join(la, rb, 1, NULL, &mj) != -1) { FAIL("join NULL cb"); goto out; }
    if (dl_merge_join(la, rb, 0, join_cb, &mj) != -1) { FAIL("join jcols0"); goto out; }
    if (dl_merge_join(la, rb, 3, join_cb, &mj) != -1) { FAIL("join jcols>arity"); goto out; }
    /* jcols == la+1 but <= ra is impossible here (both arity 2); test the
     * >min-arity rejection via jcols==3 (already done). */
    dl_iter_close(la);
    dl_iter_close(rb);

    /* Early stop: cb returns non-zero on the 3rd pair -> partial count. */
    early_stop_cb_calls = 0;
    jlist_free(&mj); memset(&mj, 0, sizeof(mj));
    la = dl_iter_open(db, "A", NULL, 0);
    rb = dl_iter_open(db, "B", NULL, 0);
    emitted = dl_merge_join(la, rb, 1, early_stop_cb, NULL);
    if (emitted != 3) { FAIL("early-stop partial == 3"); dl_iter_close(la); dl_iter_close(rb); goto out; }
    if (emitted >= 7) { FAIL("early-stop is partial"); dl_iter_close(la); dl_iter_close(rb); goto out; }
    dl_iter_close(la);
    dl_iter_close(rb);

    PASS();
out:
    tset_free(&A);
    tset_free(&B);
    jlist_free(&mj);
    jlist_free(&bf);
    dl_close(db);
}

/* ─── main ─────────────────────────────────────────────────────────────── */

int main(void)
{
    char dir[512];

    printf("=== M13 pull-iterator / merge-join tests ===\n");
    srand(1234567);

    t1_pull(dir, sizeof(dir));
    t2_bound(dir, sizeof(dir));
    t3_reseek(dir, sizeof(dir));
    t4_snapshot(dir, sizeof(dir));
    t5_errors(dir, sizeof(dir));
    t6_join(dir, sizeof(dir));

    printf("\n%d tests run, %d failed.\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
