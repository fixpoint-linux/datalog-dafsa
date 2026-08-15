/*
 * test_vararity.c — v2 VARIABLE-ARITY relation tests
 *
 * Covers:
 *   T1  single-variant equivalence: a variadic relation holding only
 *       arity-a facts enumerates byte-identically to a fixed relation of
 *       arity a (the correctness backstop), including prefix queries and
 *       a close/reopen round-trip
 *   T2  variable-tail prefix: dl_prefix(R,[c],1) spans arities with the
 *       correct per-tuple arity in the callback; k=2 spans only variants
 *       with arity >= 2
 *   T3  fixed-arity-equivalent oracle: mixed-arity programs — including a
 *       rule that joins ACROSS two arities of one variadic relation —
 *       satisfy union(variadic) == union(fixed equivalents)
 *   T4  property tests: random mixed-arity fact sets + random prefixes +
 *       random deletes vs a brute-force in-memory model (count, per-tuple
 *       arity, membership, lookup)
 *   T5  gating: variadic programs never route to IVM / DRed / aggregate
 *       IVM (vm_*_runs counters stay put; eligibility = 0) and still
 *       evaluate correctly via the full fixpoint after add AND delete
 *   T6  compile errors: aggregate-over-variadic; recursive variadic head
 *   T7  persistence round-trip: rels.txt '*' marker; per-variant WAL /
 *       base / view files; EDB facts survive reopen; a variadic rule head
 *       re-derives after reopen + new facts
 *   T8  snapshot publish: per-variant manifest lines + mmap query fan-out
 *       equals the in-memory result (full scan and bound prefix)
 *   T9  magic-sets rejection: dl_query_magic returns -1 on a db that
 *       contains a variadic relation
 *   T10 backward-compat: fixed relations coexisting with variadic ones
 *       keep their exact v1 files and metadata lines
 */
#include "dl.h"
#include "vm.h"           /* vm_*_runs counters, eligibility fns */
#include "dl_internal.h"  /* rel_entry kinds */
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
} while (0)

#define PASS() do { printf("OK\n"); } while (0)
#define FAIL(msg) do { \
    printf("FAIL: %s\n", msg); \
    tests_failed++; \
} while (0)

/* ─── Mixed-arity tuple collector ──────────────────────────────────────── */

#define VSET_MAX 1024

typedef struct {
    uint32_t data[VSET_MAX][8];
    uint8_t  ar[VSET_MAX];
    long     count;
} vset;

static int vset_has(const vset *v, const uint32_t *cols, uint8_t arity)
{
    long i;
    int c;
    for (i = 0; i < v->count; i++) {
        if (v->ar[i] != arity) continue;
        for (c = 0; c < arity; c++)
            if (v->data[i][c] != cols[c]) break;
        if (c == arity) return 1;
    }
    return 0;
}

static int vset_add(vset *v, const uint32_t *cols, uint8_t arity)
{
    if (vset_has(v, cols, arity)) return 0;
    if (v->count >= VSET_MAX) return -1;
    memcpy(v->data[v->count], cols, (size_t)arity * sizeof(uint32_t));
    v->ar[v->count] = arity;
    v->count++;
    return 1;
}

static void vset_del(vset *v, const uint32_t *cols, uint8_t arity)
{
    long i;
    int c;
    for (i = 0; i < v->count; i++) {
        if (v->ar[i] != arity) continue;
        for (c = 0; c < arity; c++)
            if (v->data[i][c] != cols[c]) break;
        if (c == arity) {
            memmove(&v->data[i], &v->data[i + 1],
                    (size_t)(v->count - i - 1) * sizeof(v->data[0]));
            memmove(&v->ar[i], &v->ar[i + 1],
                    (size_t)(v->count - i - 1) * sizeof(v->ar[0]));
            v->count--;
            return;
        }
    }
}

/* Brute-force model filter: tuples with the k leading cols equal. */
static void vset_filter(const vset *v, const uint32_t *lead, int k, vset *out)
{
    long i;
    memset(out, 0, sizeof(*out));
    for (i = 0; i < v->count; i++) {
        int c, match = 1;
        if ((int)v->ar[i] < k) continue;
        for (c = 0; c < k; c++)
            if (v->data[i][c] != lead[c]) { match = 0; break; }
        if (match) vset_add(out, v->data[i], v->ar[i]);
    }
}

static int vset_eq(const vset *a, const vset *b)
{
    long i;
    if (a->count != b->count) return 0;
    for (i = 0; i < a->count; i++)
        if (!vset_has(b, a->data[i], a->ar[i])) return 0;
    return 1;
}

/* Single-arity view of a vset (all tuples of one arity), for comparing a
 * variadic relation against a fixed one. */
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

/* vset callback: record (cols, arity). */
static int vcb(const uint32_t *cols, uint8_t arity, void *user)
{
    vset *v = (vset *)user;
    if (vset_add(v, cols, arity) < 0) return 1;
    return 0;
}

static int tset_eq(const tset *a, const tset *b)
{
    long i;
    if (a->count != b->count) return 0;
    for (i = 0; i < a->count; i++)
        if (memcmp(a->data + i * a->arity, b->data + i * b->arity,
                   a->arity * sizeof(uint32_t)) != 0)
            return 0;
    return 1;
}

/* ─── Helpers ──────────────────────────────────────────────────────────── */

static void rm_dir(const char *dir)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    system(cmd);
}

static const char *BASE = "build-tmp/vararity";

static dl_db *fresh_db(char *dir_out, size_t cap, const char *name)
{
    char cmd[512];
    snprintf(dir_out, cap, "%s/%s", BASE, name);
    /* mkdir -p: dl_open's mkdir is single-level, so the parent must
     * already exist. */
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir_out);
    system(cmd);
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", dir_out);
    system(cmd);
    return dl_open(dir_out);
}

static void addf(dl_db *db, const char *rel, const uint32_t *cols, uint8_t ar)
{
    assert(dl_add_fact(db, rel, cols, ar) == 1);
}

/* ─── T1: single-variant equivalence ──────────────────────────────────── */

static void t1_single_variant(void)
{
    char dir[256];
    dl_db *db;
    tset fixed_t = {0}, var_t = {0};
    const uint32_t facts[5][3] = {
        {1,2,3},{1,2,9},{2,1,3},{5,5,5},{7,0,7}
    };
    int i, pass = 1;

    TEST("T1 single-variant == fixed (enum + prefixes + reopen)");

    db = fresh_db(dir, sizeof(dir), "t1");
    assert(db);
    assert(dl_declare_relation_variadic(db, "v") == 0);
    assert(dl_declare_relation(db, "f", 3) == 0);
    for (i = 0; i < 5; i++) {
        addf(db, "v", facts[i], 3);
        addf(db, "f", facts[i], 3);
    }

    /* full enumeration */
    dl_prefix(db, "v", NULL, 0, tcb, &var_t);
    dl_prefix(db, "f", NULL, 0, tcb, &fixed_t);
    if (var_t.count != 5 || !tset_eq(&var_t, &fixed_t)) pass = 0;

    /* prefix queries k=1,2 */
    {
        uint32_t lead1[1] = {1}, lead2[2] = {1,2};
        tset a = {0}, b = {0};
        dl_prefix(db, "v", lead1, 1, tcb, &a);
        dl_prefix(db, "f", lead1, 1, tcb, &b);
        if (a.count != 2 || !tset_eq(&a, &b)) pass = 0;
        tset_free(&a); tset_free(&b);
        a.arity = b.arity = 0; a.count = b.count = 0;
        dl_prefix(db, "v", lead2, 2, tcb, &a);
        dl_prefix(db, "f", lead2, 2, tcb, &b);
        if (a.count != 2 || !tset_eq(&a, &b)) pass = 0;
        tset_free(&a); tset_free(&b);
    }

    /* exact lookup */
    {
        int r1 = dl_lookup(db, "v", facts[3], 3);
        uint32_t nope[3] = {1,2,4};
        int r2 = dl_lookup(db, "v", nope, 3);
        if (r1 != 1 || r2 != 0) pass = 0;
    }

    /* reopen round-trip */
    dl_close(db);
    db = dl_open(dir);
    assert(db);
    tset_free(&var_t); tset_free(&fixed_t);
    dl_prefix(db, "v", NULL, 0, tcb, &var_t);
    dl_prefix(db, "f", NULL, 0, tcb, &fixed_t);
    if (var_t.count != 5 || !tset_eq(&var_t, &fixed_t)) pass = 0;

    dl_close(db);
    tset_free(&var_t); tset_free(&fixed_t);

    if (pass) PASS(); else FAIL("single-variant divergence");
}

/* ─── T2: variable-tail prefix ────────────────────────────────────────── */

static void t2_variable_tail(void)
{
    char dir[256];
    dl_db *db;
    vset got;
    int pass = 1;

    TEST("T2 variable-tail prefix (k=1 spans arities, cb arity correct)");

    db = fresh_db(dir, sizeof(dir), "t2");
    assert(db);
    assert(dl_declare_relation_variadic(db, "v") == 0);
    {
        uint32_t a1[1] = {7};
        uint32_t a2a[2] = {7,8}, a2b[2] = {8,7};
        uint32_t a3a[3] = {7,8,9}, a3b[3] = {7,9,8};
        uint32_t a4[4] = {1,2,3,4};
        addf(db, "v", a1, 1);
        addf(db, "v", a2a, 2);
        addf(db, "v", a2b, 2);
        addf(db, "v", a3a, 3);
        addf(db, "v", a3b, 3);
        addf(db, "v", a4, 4);
    }

    /* k=0: everything */
    memset(&got, 0, sizeof(got));
    if (dl_prefix(db, "v", NULL, 0, vcb, &got) != 6 || got.count != 6)
        pass = 0;

    /* k=1 with leading 7: arities 1,2,3,3 — NOT the arity-4 tuple. */
    {
        uint32_t lead[1] = {7};
        vset exp;
        memset(&got, 0, sizeof(got));
        if (dl_prefix(db, "v", lead, 1, vcb, &got) != 4) pass = 0;
        memset(&exp, 0, sizeof(exp));
        {
            uint32_t a1[1] = {7};
            uint32_t a2[2] = {7,8};
            uint32_t a3a[3] = {7,8,9}, a3b[3] = {7,9,8};
            vset_add(&exp, a1, 1);
            vset_add(&exp, a2, 2);
            vset_add(&exp, a3a, 3);
            vset_add(&exp, a3b, 3);
        }
        if (!vset_eq(&got, &exp)) pass = 0;
    }

    /* k=2 with leading (7,8): arities 2,3 only. */
    {
        uint32_t lead[2] = {7,8};
        vset exp;
        memset(&got, 0, sizeof(got));
        if (dl_prefix(db, "v", lead, 2, vcb, &got) != 2) pass = 0;
        memset(&exp, 0, sizeof(exp));
        {
            uint32_t a2[2] = {7,8};
            uint32_t a3[3] = {7,8,9};
            vset_add(&exp, a2, 2);
            vset_add(&exp, a3, 3);
        }
        if (!vset_eq(&got, &exp)) pass = 0;
    }

    /* k=8 (no variant can match a full-width prefix of absent arity 8):
     * legal query, zero tuples. */
    {
        uint32_t lead[8] = {1,1,1,1,1,1,1,1};
        memset(&got, 0, sizeof(got));
        if (dl_prefix(db, "v", lead, 8, vcb, &got) != 0) pass = 0;
    }

    dl_close(db);
    if (pass) PASS(); else FAIL("variable-tail prefix mismatch");
}

/* ─── T3: fixed-arity-equivalent oracle (rules across arities) ────────── */

static void t3_rules_oracle(void)
{
    char vdir[256], fdir[256];
    dl_db *vdb, *fdb;
    vset qv, qf_union, rv, rf;
    tset q2f = {0}, q3f = {0}, rfix = {0};
    int pass = 1;

    TEST("T3 union(variadic head) == union(fixed equivalents)");

    /* variadic program:
     *   v : (1,2) (2,3) (5,6) | (1,2,3) (5,6,7)
     *   q(X,Y)   :- v(X,Y).
     *   q(X,Y,Z) :- v(X,Y,Z).
     *   r(X)     :- v(X,Y), v(X,Y,Z).     <-- joins ACROSS arities
     */
    vdb = fresh_db(vdir, sizeof(vdir), "t3v");
    assert(vdb);
    assert(dl_declare_relation_variadic(vdb, "v") == 0);
    assert(dl_declare_relation_variadic(vdb, "q") == 0);
    assert(dl_declare_relation(vdb, "r", 1) == 0);
    {
        uint32_t v2[3][2] = {{1,2},{2,3},{5,6}};
        uint32_t v3[2][3] = {{1,2,3},{5,6,7}};
        int i;
        for (i = 0; i < 3; i++) addf(vdb, "v", v2[i], 2);
        for (i = 0; i < 2; i++) addf(vdb, "v", v3[i], 3);
    }
    assert(dl_load_rules(vdb,
        "q(X,Y) :- v(X,Y).\n"
        "q(X,Y,Z) :- v(X,Y,Z).\n"
        "r(X) :- v(X,Y), v(X,Y,Z).\n") == 0);
    assert(dl_compile(vdb) == 0);

    memset(&qv, 0, sizeof(qv));
    dl_prefix(vdb, "q", NULL, 0, vcb, &qv);
    memset(&rv, 0, sizeof(rv));
    dl_prefix(vdb, "r", NULL, 0, vcb, &rv);
    dl_close(vdb);

    /* fixed mirror: v2/v3 fixed relations, q2/q3 fixed heads */
    fdb = fresh_db(fdir, sizeof(fdir), "t3f");
    assert(fdb);
    assert(dl_declare_relation(fdb, "v2", 2) == 0);
    assert(dl_declare_relation(fdb, "v3", 3) == 0);
    assert(dl_declare_relation(fdb, "q2", 2) == 0);
    assert(dl_declare_relation(fdb, "q3", 3) == 0);
    assert(dl_declare_relation(fdb, "rf", 1) == 0);
    {
        uint32_t v2[3][2] = {{1,2},{2,3},{5,6}};
        uint32_t v3[2][3] = {{1,2,3},{5,6,7}};
        int i;
        for (i = 0; i < 3; i++) addf(fdb, "v2", v2[i], 2);
        for (i = 0; i < 2; i++) addf(fdb, "v3", v3[i], 3);
    }
    assert(dl_load_rules(fdb,
        "q2(X,Y) :- v2(X,Y).\n"
        "q3(X,Y,Z) :- v3(X,Y,Z).\n"
        "rf(X) :- v2(X,Y), v3(X,Y,Z).\n") == 0);
    assert(dl_compile(fdb) == 0);

    dl_prefix(fdb, "q2", NULL, 0, tcb, &q2f);
    dl_prefix(fdb, "q3", NULL, 0, tcb, &q3f);
    dl_prefix(fdb, "rf", NULL, 0, tcb, &rfix);
    dl_close(fdb);

    /* union(fixed equivalents) built into a vset */
    memset(&qf_union, 0, sizeof(qf_union));
    {
        long i;
        for (i = 0; i < q2f.count; i++)
            vset_add(&qf_union, q2f.data + i * 2, 2);
        for (i = 0; i < q3f.count; i++)
            vset_add(&qf_union, q3f.data + i * 3, 3);
    }
    if (qv.count != 5 || !vset_eq(&qv, &qf_union)) pass = 0;

    /* r: X must appear as col0 of BOTH an arity-2 and an arity-3 v fact:
     * X=1 and X=5. */
    memset(&rf, 0, sizeof(rf));
    {
        long i;
        for (i = 0; i < rfix.count; i++) vset_add(&rf, rfix.data + i, 1);
    }
    if (rv.count != 2 || !vset_eq(&rv, &rf)) pass = 0;

    tset_free(&q2f); tset_free(&q3f); tset_free(&rfix);
    if (pass) PASS(); else FAIL("variadic/fixed oracle mismatch");
}

/* ─── T4: property tests vs brute-force model ─────────────────────────── */

static uint32_t rng_state = 0x12345678u;
static uint32_t rng(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

static void t4_property(void)
{
    char dir[256];
    int trial, pass = 1;

    TEST("T4 property: random mixed-arity adds/deletes/prefixes vs model");

    for (trial = 0; trial < 12 && pass; trial++) {
        dl_db *db = fresh_db(dir, sizeof(dir), "t4");
        vset model;
        int nops = 30 + (int)(rng() % 20);
        int op;

        assert(db);
        assert(dl_declare_relation_variadic(db, "v") == 0);
        memset(&model, 0, sizeof(model));

        for (op = 0; op < nops; op++) {
            uint32_t cols[8];
            uint8_t ar = (uint8_t)(1 + rng() % 4);
            int c;
            for (c = 0; c < ar; c++) cols[c] = rng() % 5;

            if (rng() % 8 == 0 && model.count > 0) {
                /* delete a random model tuple */
                long idx = (long)(rng() % (uint32_t)model.count);
                uint32_t t[8];
                uint8_t ta = model.ar[idx];
                memcpy(t, model.data[idx], sizeof(t));
                assert(dl_delete_fact(db, "v", t, ta) == 1);
                vset_del(&model, t, ta);
            } else {
                int rc = dl_add_fact(db, "v", cols, ar);
                int added = vset_add(&model, cols, ar);
                /* rc: 1 added, 0 duplicate; model_add: 1 new, 0 dup */
                if (rc != added) pass = 0;
            }
        }

        /* full scan vs model */
        {
            vset got;
            memset(&got, 0, sizeof(got));
            if (dl_prefix(db, "v", NULL, 0, vcb, &got) != model.count ||
                !vset_eq(&got, &model))
                pass = 0;
        }

        /* random prefixes vs model filter */
        {
            int q;
            for (q = 0; q < 12; q++) {
                uint32_t lead[2];
                int k = (int)(rng() % 3);   /* 0,1,2 */
                vset got, exp;
                lead[0] = rng() % 5;
                lead[1] = rng() % 5;
                memset(&got, 0, sizeof(got));
                if (dl_prefix(db, "v", k ? lead : NULL, (uint8_t)k,
                              vcb, &got) < 0)
                    { pass = 0; break; }
                vset_filter(&model, lead, k, &exp);
                if (!vset_eq(&got, &exp)) { pass = 0; break; }
            }
        }

        /* random lookups vs model membership */
        {
            int q;
            for (q = 0; q < 12; q++) {
                uint32_t t[8];
                uint8_t ar = (uint8_t)(1 + rng() % 4);
                int c, want, gotrc;
                for (c = 0; c < ar; c++) t[c] = rng() % 5;
                want = vset_has(&model, t, ar);
                gotrc = dl_lookup(db, "v", t, ar);
                if (want != gotrc) { pass = 0; break; }
            }
        }

        dl_close(db);
    }
    rm_dir(dir);

    if (pass) PASS(); else FAIL("property divergence vs brute model");
}

/* ─── T5: gating (never incremental; always correct) ──────────────────── */

static int g_t5_count;
static int t5_cb(const uint32_t *cols, uint8_t arity, void *user)
{
    (void)cols; (void)arity; (void)user;
    g_t5_count++;
    return 0;
}

static void t5_gating(void)
{
    char dir[256];
    dl_db *db;
    uint32_t e1[2] = {1,2}, e2[2] = {3,4}, e3[2] = {5,6};
    int pass = 1;

    TEST("T5 gating: IVM/DRed/agg counters stay 0, results correct");

    db = fresh_db(dir, sizeof(dir), "t5");
    assert(db);
    assert(dl_declare_relation_variadic(db, "v") == 0);
    assert(dl_declare_relation(db, "p", 2) == 0);
    addf(db, "v", e1, 2);
    addf(db, "v", e2, 2);
    assert(dl_load_rules(db, "p(X,Y) :- v(X,Y).\n") == 0);

    /* eligibility must be 0 for the whole db. */
    if (vm_ivm_eligible(db) != 0 || vm_dred_eligible(db) != 0 ||
        vm_agg_eligible(db) != 0)
        pass = 0;

    vm_propagate_runs = 0;
    vm_dred_runs = 0;
    vm_agg_runs = 0;

    /* insert -> publish: full re-eval, no incremental path taken */
    addf(db, "v", e3, 2);
    if (dl_publish_snapshot(db) != 0) pass = 0;
    g_t5_count = 0;
    dl_query_bound(db, "p", NULL, 0, t5_cb, NULL);
    if (g_t5_count != 3) pass = 0;
    if (vm_propagate_runs != 0 || vm_dred_runs != 0 || vm_agg_runs != 0)
        pass = 0;

    /* delete -> publish: full re-eval again */
    assert(dl_delete_fact(db, "v", e2, 2) == 1);
    if (dl_publish_snapshot(db) != 0) pass = 0;
    g_t5_count = 0;
    dl_query_bound(db, "p", NULL, 0, t5_cb, NULL);
    if (g_t5_count != 2) pass = 0;
    if (vm_propagate_runs != 0 || vm_dred_runs != 0 || vm_agg_runs != 0)
        pass = 0;

    dl_close(db);
    if (pass) PASS(); else FAIL("incremental path taken or wrong result");
}

/* ─── T6: compile errors ──────────────────────────────────────────────── */

static void t6_compile_errors(void)
{
    char dir[256];
    dl_db *db;
    uint32_t f2[2] = {1,2};
    int pass = 1;

    TEST("T6 compile errors: aggregate-over-variadic, recursive variadic head");

    db = fresh_db(dir, sizeof(dir), "t6");
    assert(db);
    assert(dl_declare_relation_variadic(db, "v") == 0);
    addf(db, "v", f2, 2);

    /* aggregate reading a variadic relation: rejected. */
    if (dl_load_rules(db, "cnt(X,N) :- v(X,Y), N = count().\n") == 0) {
        pass = 0;
    }
    assert(dl_declare_relation(db, "e", 2) == 0);
    addf(db, "e", f2, 2);

    /* aggregate with a VARIADIC HEAD (pre-declared): rejected. */
    {
        dl_db *db2 = fresh_db(dir, sizeof(dir), "t6b");
        assert(db2);
        assert(dl_declare_relation_variadic(db2, "aggv") == 0);
        assert(dl_declare_relation(db2, "e", 2) == 0);
        addf(db2, "e", f2, 2);
        if (dl_load_rules(db2, "aggv(X,Y,N) :- e(X,Y), N = count().\n") == 0)
            pass = 0;
        dl_close(db2);
    }

    /* recursive variadic head: rejected. */
    {
        dl_db *db3 = fresh_db(dir, sizeof(dir), "t6c");
        assert(db3);
        assert(dl_declare_relation(db3, "edge", 2) == 0);
        assert(dl_declare_relation_variadic(db3, "tc") == 0);
        addf(db3, "edge", f2, 2);
        if (dl_load_rules(db3,
                "tc(X,Y) :- edge(X,Y).\n"
                "tc(X,Y) :- tc(X,Z), edge(Z,Y).\n") == 0)
            pass = 0;
        dl_close(db3);
    }

    /* control: a recursive rule whose body only READS variadic EDB
     * variants is fine. */
    {
        dl_db *db4 = fresh_db(dir, sizeof(dir), "t6d");
        assert(db4);
        assert(dl_declare_relation_variadic(db4, "v") == 0);
        assert(dl_declare_relation(db4, "edge", 2) == 0);
        assert(dl_declare_relation(db4, "tc", 2) == 0);
        addf(db4, "v", f2, 2);
        addf(db4, "edge", f2, 2);
        if (dl_load_rules(db4,
                "tc(X,Y) :- edge(X,Y), v(X,Y).\n"
                "tc(X,Y) :- tc(X,Z), edge(Z,Y).\n") != 0)
            pass = 0;
        if (dl_compile(db4) != 0) pass = 0;
        dl_close(db4);
    }

    dl_close(db);
    if (pass) PASS(); else FAIL("expected compile error not raised");
}

/* ─── T7: persistence round-trip ──────────────────────────────────────── */

static int file_contains(const char *path, const char *needle)
{
    FILE *f = fopen(path, "r");
    char buf[512];
    if (!f) return 0;
    while (fgets(buf, sizeof(buf), f))
        if (strstr(buf, needle)) { fclose(f); return 1; }
    fclose(f);
    return 0;
}

static int file_exists(const char *path)
{
    FILE *f = fopen(path, "r");
    if (f) { fclose(f); return 1; }
    return 0;
}

static void t7_persistence(void)
{
    char dir[256], path[512];
    dl_db *db;
    vset got;
    uint32_t a1[1] = {9};
    uint32_t a2[2] = {1,2};
    uint32_t a3[3] = {3,4,5};
    int pass = 1;

    TEST("T7 persistence: '*' marker, per-variant files, reopen, re-derive");

    db = fresh_db(dir, sizeof(dir), "t7");
    assert(db);
    assert(dl_declare_relation_variadic(db, "v") == 0);
    assert(dl_declare_relation(db, "e", 2) == 0);
    assert(dl_declare_relation_variadic(db, "q") == 0);
    addf(db, "v", a1, 1);
    addf(db, "v", a2, 2);
    addf(db, "v", a3, 3);
    addf(db, "e", a2, 2);
    assert(dl_load_rules(db, "q(X,Y) :- e(X,Y).\n") == 0);
    assert(dl_compile(db) == 0);
    dl_close(db);

    /* rels.txt: marker + fixed lines coexist. */
    snprintf(path, sizeof(path), "%s/rels.txt", dir);
    if (!file_contains(path, "v:*:edb")) pass = 0;
    if (!file_contains(path, "e:2:edb")) pass = 0;
    if (!file_contains(path, "q:*:idb")) pass = 0;

    /* per-variant files exist. */
    snprintf(path, sizeof(path), "%s/v.1.dafsa", dir); if (!file_exists(path)) pass = 0;
    snprintf(path, sizeof(path), "%s/v.2.dafsa", dir); if (!file_exists(path)) pass = 0;
    snprintf(path, sizeof(path), "%s/v.3.dafsa", dir); if (!file_exists(path)) pass = 0;
    snprintf(path, sizeof(path), "%s/q.2.base.dafsa", dir); if (!file_exists(path)) pass = 0;
    snprintf(path, sizeof(path), "%s/q.2.dafsa", dir); if (!file_exists(path)) pass = 0;

    /* reopen: EDB facts survive; kinds preserved. */
    db = dl_open(dir);
    assert(db);
    memset(&got, 0, sizeof(got));
    if (dl_prefix(db, "v", NULL, 0, vcb, &got) != 3) pass = 0;
    {
        vset exp;
        memset(&exp, 0, sizeof(exp));
        vset_add(&exp, a1, 1);
        vset_add(&exp, a2, 2);
        vset_add(&exp, a3, 3);
        if (!vset_eq(&got, &exp)) pass = 0;
    }

    /* delete after reopen works (WAL path). */
    assert(dl_delete_fact(db, "v", a3, 3) == 1);
    memset(&got, 0, sizeof(got));
    if (dl_prefix(db, "v", NULL, 0, vcb, &got) != 2) pass = 0;

    /* variadic head re-derives after reopen + new base facts. */
    {
        uint32_t e2b[2] = {6,7};
        addf(db, "e", e2b, 2);
        assert(dl_load_rules(db, "q(X,Y) :- e(X,Y).\n") == 0);
        assert(dl_compile(db) == 0);
        memset(&got, 0, sizeof(got));
        if (dl_prefix(db, "q", NULL, 0, vcb, &got) != 2) pass = 0; /* (1,2),(6,7) */
    }

    dl_close(db);
    /* second reopen: q still materialized in the saved view. */
    db = dl_open(dir);
    assert(db);
    memset(&got, 0, sizeof(got));
    if (dl_prefix(db, "q", NULL, 0, vcb, &got) != 2) pass = 0;
    dl_close(db);

    if (pass) PASS(); else FAIL("persistence round-trip mismatch");
}

/* ─── T8: snapshot publish + mmap fan-out ─────────────────────────────── */

static void t8_snapshot(void)
{
    char dir[256], path[512];
    dl_db *db;
    vset before, after, bound, bound_exp;
    uint32_t a1[1] = {4};
    uint32_t a2a[2] = {4,5}, a2b[2] = {6,7};
    uint32_t a3[3] = {4,5,6};
    int pass = 1;

    TEST("T8 snapshot: per-variant manifest + mmap query fan-out");

    db = fresh_db(dir, sizeof(dir), "t8");
    assert(db);
    assert(dl_declare_relation_variadic(db, "v") == 0);
    addf(db, "v", a1, 1);
    addf(db, "v", a2a, 2);
    addf(db, "v", a2b, 2);
    addf(db, "v", a3, 3);

    memset(&before, 0, sizeof(before));
    dl_prefix(db, "v", NULL, 0, vcb, &before);

    assert(dl_publish_snapshot(db) == 0);

    /* manifest: marker + per-variant lines. */
    snprintf(path, sizeof(path), "%s/snapshots/1/manifest.txt", dir);
    if (!file_contains(path, "v:*:edb")) pass = 0;
    if (!file_contains(path, "v.1:1:edb")) pass = 0;
    if (!file_contains(path, "v.2:2:edb")) pass = 0;
    if (!file_contains(path, "v.3:3:edb")) pass = 0;

    /* full query via the mmap path == in-memory result. */
    memset(&after, 0, sizeof(after));
    if (dl_query(db, "v", vcb, &after) != 4 || !vset_eq(&after, &before))
        pass = 0;

    /* bound query: leading (4) spans arities 1,2,3. */
    {
        uint32_t lead[1] = {4};
        memset(&bound, 0, sizeof(bound));
        if (dl_query_bound(db, "v", lead, 1, vcb, &bound) != 3) pass = 0;
        memset(&bound_exp, 0, sizeof(bound_exp));
        vset_add(&bound_exp, a1, 1);
        vset_add(&bound_exp, a2a, 2);
        vset_add(&bound_exp, a3, 3);
        if (!vset_eq(&bound, &bound_exp)) pass = 0;
    }

    dl_close(db);
    if (pass) PASS(); else FAIL("snapshot fan-out mismatch");
}

/* ─── T9: magic-sets rejection ────────────────────────────────────────── */

static void t9_magic_reject(void)
{
    char dir[256];
    dl_db *db;
    uint32_t e1[2] = {1,2};
    int pass = 1;

    TEST("T9 magic-sets rejects variadic programs");

    db = fresh_db(dir, sizeof(dir), "t9");
    assert(db);
    assert(dl_declare_relation_variadic(db, "v") == 0);
    assert(dl_declare_relation(db, "edge", 2) == 0);
    assert(dl_declare_relation(db, "tc", 2) == 0);
    addf(db, "v", e1, 2);
    addf(db, "edge", e1, 2);
    assert(dl_load_rules(db,
        "tc(X,Y) :- edge(X,Y).\n"
        "tc(X,Y) :- tc(X,Z), edge(Z,Y).\n") == 0);
    assert(dl_compile(db) == 0);

    {
        uint32_t lead[1] = {1};
        if (dl_query_magic(db, "tc", lead, 1, vcb, NULL) != -1) pass = 0;
        if (dl_query_magic_adorn(db, "tc", "bf", lead, 1, vcb, NULL) != -1)
            pass = 0;
    }

    dl_close(db);
    if (pass) PASS(); else FAIL("magic-sets did not reject");
}

/* ─── T10: backward-compat coexistence ────────────────────────────────── */

static void t10_backward_compat(void)
{
    char dir[256], path[512];
    dl_db *db;
    uint32_t f2[2] = {1,2};
    uint32_t v3[3] = {8,9,10};
    tset fixed_t = {0};
    vset vv;
    int pass = 1;

    TEST("T10 backward-compat: fixed relation unchanged alongside variadic");

    db = fresh_db(dir, sizeof(dir), "t10");
    assert(db);
    assert(dl_declare_relation(db, "f", 2) == 0);
    assert(dl_declare_relation_variadic(db, "v") == 0);
    addf(db, "f", f2, 2);
    addf(db, "v", v3, 3);
    /* the fixed relation's WAL exists from the declare (before any save). */
    snprintf(path, sizeof(path), "%s/f.wal", dir); if (!file_exists(path)) pass = 0;

    dl_close(db);

    /* fixed path still uses its v1 file names (post-close: compacted). */
    snprintf(path, sizeof(path), "%s/f.dafsa", dir); if (!file_exists(path)) pass = 0;
    /* no stray fixed-style file for the variadic relation. */
    snprintf(path, sizeof(path), "%s/v.dafsa", dir); if (file_exists(path)) pass = 0;

    snprintf(path, sizeof(path), "%s/rels.txt", dir);
    if (!file_contains(path, "f:2:edb")) pass = 0;
    if (!file_contains(path, "v:*:edb")) pass = 0;

    db = dl_open(dir);
    assert(db);
    dl_prefix(db, "f", NULL, 0, tcb, &fixed_t);
    if (fixed_t.count != 1 ||
        memcmp(fixed_t.data, f2, sizeof(f2)) != 0)
        pass = 0;
    memset(&vv, 0, sizeof(vv));
    if (dl_prefix(db, "v", NULL, 0, vcb, &vv) != 1) pass = 0;
    dl_close(db);
    tset_free(&fixed_t);

    if (pass) PASS(); else FAIL("fixed path changed behavior");
}

/* ─── main ─────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("=== v2 variable-arity tests ===\n");

    t1_single_variant();
    t2_variable_tail();
    t3_rules_oracle();
    t4_property();
    t5_gating();
    t6_compile_errors();
    t7_persistence();
    t8_snapshot();
    t9_magic_reject();
    t10_backward_compat();

    printf("\n%d tests run, %d failed.\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
