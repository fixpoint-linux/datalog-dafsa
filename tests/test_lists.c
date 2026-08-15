/*
 * test_lists.c — v2 LIST / NESTED-TERM tests
 *
 * Covers:
 *   T1  term-store canonical interning: equal lists share one handle,
 *       distinct lists don't; is_list is an EXACT index-range test disjoint
 *       from sym_ids and raw ints; NIL = node 0 (handle == TERM_BASE).
 *   T2  car/cdr/cons/append round-trip vs a brute-force in-memory reference
 *       model (C arrays).
 *   T3  list-valued facts materialize: a rule with a list-literal head
 *       projects the canonical handle; dl_query returns it and it equals a
 *       reference list built independently.
 *   T4  list builtins end-to-end: cons/car/cdr/append (constant and
 *       variable operands) produce the expected tuples.
 *   T5  gating: a list-BUILTIN program routes to the full fixpoint
 *       (vm_*_eligible == 0, vm_*_runs counters stay 0); list VALUES as pure
 *       data do NOT force re-eval (eligible, delta-propagated).
 *   T6  persistence round-trip: build lists, dl_close, dl_open — handles are
 *       identical and car/cdr/equality survive.
 *   T7  boundary: an int LITERAL >= TERM_BASE is rejected loudly by the
 *       parser (31-bit literal cap).  (CSV bulk-data keeps legacy raw-u32
 *       behavior — see the termstore.h / dl.c notes.)
 *   T8  compile-time rejects: comparison over a list literal; a negated list
 *       builtin; a variable inside a list literal (Phase-2 pattern).
 */
#include "dl.h"
#include "vm.h"           /* vm_*_runs counters, eligibility fns */
#include "dl_internal.h"  /* dl_db.terms, db_has_list_builtin */
#include "termstore.h"
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

/* ─── Reference model: a list = a vector of u32 elements ───────────────── */

typedef struct {
    uint32_t *e;
    long      n;
} ref_list;

static ref_list ref_from(const uint32_t *elems, long n)
{
    ref_list r;
    r.n = n;
    r.e = malloc((size_t)(n > 0 ? n : 1) * sizeof(uint32_t));
    if (n > 0) memcpy(r.e, elems, (size_t)n * sizeof(uint32_t));
    return r;
}

static void ref_free(ref_list *r) { free(r->e); r->e = NULL; r->n = 0; }

static int ref_eq(const ref_list *a, const ref_list *b)
{
    long i;
    if (a->n != b->n) return 0;
    for (i = 0; i < a->n; i++)
        if (a->e[i] != b->e[i]) return 0;
    return 1;
}

/* append reference: a ++ b */
static ref_list ref_append(const ref_list *a, const ref_list *b)
{
    ref_list r;
    long i;
    r.n = a->n + b->n;
    r.e = malloc((size_t)(r.n > 0 ? r.n : 1) * sizeof(uint32_t));
    for (i = 0; i < a->n; i++) r.e[i] = a->e[i];
    for (i = 0; i < b->n; i++) r.e[a->n + i] = b->e[i];
    return r;
}

/* Build the canonical handle for a list of elements (right fold). */
static uint32_t mk_list(termstore *ts, const uint32_t *elems, long n)
{
    uint32_t h = TERM_NIL;
    long i;
    for (i = n - 1; i >= 0; i--) {
        uint32_t c = term_cons(ts, elems[i], h);
        assert(c != 0);
        h = c;
    }
    return h;
}

/* Dump a term-store list into a ref_list (via car/cdr). */
static ref_list dump_list(termstore *ts, uint32_t h)
{
    ref_list r;
    long cap = 8;
    r.n = 0;
    r.e = malloc((size_t)cap * sizeof(uint32_t));
    while (term_is_list(ts, h) && h != TERM_NIL) {
        if (r.n >= cap) {
            cap *= 2;
            r.e = realloc(r.e, (size_t)cap * sizeof(uint32_t));
        }
        r.e[r.n++] = term_car(ts, h);
        h = term_cdr(ts, h);
    }
    return r;
}

/* ─── Tuple collector (single arity) ───────────────────────────────────── */

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

/* ─── Helpers ──────────────────────────────────────────────────────────── */

static const char *BASE = "build-tmp/lists";

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

static void addf(dl_db *db, const char *rel, const uint32_t *cols, uint8_t ar)
{
    assert(dl_add_fact(db, rel, cols, ar) == 1);
}

/* ─── T1: canonical interning + is_list boundary ───────────────────────── */

static void t1_termstore(void)
{
    termstore *ts;
    uint32_t a, b, c, d;
    int pass = 1;

    TEST("T1 term-store canonical interning + is_list boundary");
    ts = term_create();
    assert(ts);

    /* NIL = node 0 */
    if (term_node_count(ts) != 1) pass = 0;
    if (!term_is_list(ts, TERM_NIL)) pass = 0;
    if (term_is_list(ts, 0) || term_is_list(ts, 42) ||
        term_is_list(ts, 0x7FFFFFFFu) || term_is_list(ts, 0xFFFFFFFFu))
        pass = 0;

    /* [1,2,3] built twice → same handle; [2,1] different */
    a = term_cons(ts, 1, TERM_NIL);
    b = term_cons(ts, 2, a);
    c = term_cons(ts, 3, b);
    {
        uint32_t a2 = term_cons(ts, 1, TERM_NIL);
        uint32_t b2 = term_cons(ts, 2, a2);
        uint32_t c2 = term_cons(ts, 3, b2);
        if (a2 != a || b2 != b || c2 != c) pass = 0;
    }
    d = term_cons(ts, 1, term_cons(ts, 2, TERM_NIL));   /* [1,2] */
    if (d == b) pass = 0;   /* [1,2] != [2,1] */

    if (term_length(ts, c) != 3) pass = 0;
    if (term_car(ts, c) != 3 || term_car(ts, term_cdr(ts, c)) != 2) pass = 0;

    /* improper tail rejected */
    if (term_cons(ts, 9, 12345) != 0) pass = 0;

    /* append(_, non-list) rejected */
    if (term_append(ts, c, 12345) != 0) pass = 0;

    term_free(ts);
    if (pass) PASS(); else FAIL("term-store invariants violated");
}

/* ─── T2: car/cdr/cons/append vs reference ─────────────────────────────── */

static void t2_roundtrip(void)
{
    termstore *ts;
    const uint32_t l1[] = {1, 2, 3};
    const uint32_t l2[] = {4, 5};
    uint32_t h1, h2, ha;
    ref_list r1, r2, ra, d1;
    int pass = 1;

    TEST("T2 car/cdr/cons/append round-trip vs reference");
    ts = term_create();
    assert(ts);

    h1 = mk_list(ts, l1, 3);
    h2 = mk_list(ts, l2, 2);
    ha = term_append(ts, h1, h2);

    r1 = ref_from(l1, 3);
    r2 = ref_from(l2, 2);
    ra = ref_append(&r1, &r2);
    d1 = dump_list(ts, h1);

    if (!ref_eq(&d1, &r1)) pass = 0;
    {
        ref_list da = dump_list(ts, ha);
        if (!ref_eq(&da, &ra)) pass = 0;
        ref_free(&da);
    }

    /* append(NIL, x) == x; append(x, NIL) == x */
    if (term_append(ts, TERM_NIL, h1) != h1) pass = 0;
    if (term_append(ts, h1, TERM_NIL) != h1) pass = 0;

    ref_free(&r1); ref_free(&r2); ref_free(&ra); ref_free(&d1);
    term_free(ts);
    if (pass) PASS(); else FAIL("car/cdr/cons/append diverged from reference");
}

/* ─── T3: list-valued facts materialize + query ────────────────────────── */

static void t3_list_facts(void)
{
    char dir[256];
    dl_db *db;
    const uint32_t f1[1] = {7};
    tset got;
    int pass = 1;

    TEST("T3 list-valued facts materialize + query");
    db = fresh_db(dir, sizeof(dir), "t3");
    assert(db);
    assert(dl_declare_relation(db, "p", 1) == 0);
    addf(db, "p", f1, 1);

    /* q holds the canonical [1,2,3] handle in column 0. */
    assert(dl_load_rules(db, "q([1,2,3], X) :- p(X).\n") == 0);
    if (dl_publish_snapshot(db) != 0) pass = 0;

    memset(&got, 0, sizeof(got));
    if (dl_query(db, "q", tcb, &got) != 1 || got.arity != 2) pass = 0;
    if (got.count == 1) {
        uint32_t expect[3] = {1, 2, 3};
        uint32_t h = got.data[0];
        if (!term_is_list(db->terms, h)) pass = 0;
        if (h != mk_list(db->terms, expect, 3)) pass = 0;
        if (got.data[1] != 7) pass = 0;
    } else {
        pass = 0;
    }

    tset_free(&got);
    dl_close(db);
    if (pass) PASS(); else FAIL("list-valued fact wrong handle/value");
}

/* ─── T4: list builtins end-to-end ─────────────────────────────────────── */

static void t4_builtins(void)
{
    char dir[256];
    dl_db *db;
    const uint32_t f1[1] = {1};
    const uint32_t f2[1] = {2};
    tset got;
    int pass = 1;

    TEST("T4 list builtins cons/car/cdr/append end-to-end");
    db = fresh_db(dir, sizeof(dir), "t4");
    assert(db);
    assert(dl_declare_relation(db, "p", 1) == 0);
    addf(db, "p", f1, 1);
    addf(db, "p", f2, 1);

    /* cons + car + cdr + append all in one rule; heads carry the results. */
    assert(dl_load_rules(db,
        "r(X, H, T, A) :- p(X), L = cons(X, [7,8]), H = car(L), "
        "T = cdr(L), A = append(L, [9]).\n") == 0);
    if (dl_publish_snapshot(db) != 0) pass = 0;

    memset(&got, 0, sizeof(got));
    if (dl_query(db, "r", tcb, &got) != 2 || got.arity != 4) pass = 0;
    if (got.count == 2) {
        long i;
        for (i = 0; i < got.count; i++) {
            const uint32_t *row = got.data + (size_t)i * 4;
            uint32_t X = row[0], H = row[1], T = row[2], A = row[3];
            /* H == X, T == [7,8], A == [X,7,8,9] */
            if (H != X) pass = 0;
            {
                uint32_t t_exp[2] = {7, 8};
                if (T != mk_list(db->terms, t_exp, 2)) pass = 0;
            }
            {
                uint32_t a_exp[4] = {X, 7, 8, 9};
                if (A != mk_list(db->terms, a_exp, 4)) pass = 0;
            }
        }
    } else {
        pass = 0;
    }

    tset_free(&got);
    dl_close(db);
    if (pass) PASS(); else FAIL("list builtins produced wrong tuples");
}

/* ─── T5: gating (list-builtin → full re-eval; pure data → eligible) ───── */

static void t5_gating(void)
{
    char dir[256];
    dl_db *db;
    const uint32_t f1[1] = {1};
    const uint32_t f2[1] = {2};
    int pass = 1;

    TEST("T5 gating: list-builtin routes to full re-eval; data stays IVM");

    /* (a) list-builtin program: ineligible everywhere. */
    db = fresh_db(dir, sizeof(dir), "t5a");
    assert(db);
    assert(dl_declare_relation(db, "p", 1) == 0);
    addf(db, "p", f1, 1);
    assert(dl_load_rules(db, "q(X, L) :- p(X), L = cons(X, []).\n") == 0);
    if (!db_has_list_builtin(db)) pass = 0;
    if (vm_ivm_eligible(db) != 0 || vm_dred_eligible(db) != 0 ||
        vm_agg_eligible(db) != 0)
        pass = 0;

    /* Initial publish consumes the rule-load full-reeval flag. */
    if (dl_publish_snapshot(db) != 0) pass = 0;

    vm_propagate_runs = 0;
    vm_dred_runs = 0;
    vm_agg_runs = 0;
    addf(db, "p", f2, 1);
    if (dl_publish_snapshot(db) != 0) pass = 0;
    if (vm_propagate_runs != 0 || vm_dred_runs != 0 || vm_agg_runs != 0)
        pass = 0;   /* full re-eval, no incremental path taken */
    {
        tset got;
        memset(&got, 0, sizeof(got));
        if (dl_query(db, "q", tcb, &got) != 2) pass = 0;
        tset_free(&got);
    }
    dl_close(db);

    /* (b) pure-data program (list literal head, NO list builtin): eligible. */
    db = fresh_db(dir, sizeof(dir), "t5b");
    assert(db);
    assert(dl_declare_relation(db, "p", 1) == 0);
    addf(db, "p", f1, 1);
    assert(dl_load_rules(db, "q([1,2], X) :- p(X).\n") == 0);
    if (db_has_list_builtin(db)) pass = 0;
    if (vm_ivm_eligible(db) != 1) pass = 0;

    /* Initial publish consumes the rule-load full-reeval flag. */
    if (dl_publish_snapshot(db) != 0) pass = 0;

    vm_propagate_runs = 0;
    addf(db, "p", f2, 1);
    if (dl_publish_snapshot(db) != 0) pass = 0;
    if (vm_propagate_runs != 1) pass = 0;   /* delta-propagated, no re-eval */
    {
        tset got;
        memset(&got, 0, sizeof(got));
        if (dl_query(db, "q", tcb, &got) != 2) pass = 0;
        tset_free(&got);
    }
    dl_close(db);

    if (pass) PASS(); else FAIL("gating mis-routed");
}

/* ─── T6: persistence round-trip ───────────────────────────────────────── */

static void t6_persistence(void)
{
    char dir[256];
    dl_db *db;
    uint32_t elems[3] = {1, 2, 3};
    uint32_t h_before, fact[1];
    tset got;
    int pass = 1;

    TEST("T6 persistence round-trip (terms.bin save/load, handles stable)");
    db = fresh_db(dir, sizeof(dir), "t6");
    assert(db);
    assert(dl_declare_relation(db, "f", 1) == 0);

    h_before = mk_list(db->terms, elems, 3);
    fact[0] = h_before;
    addf(db, "f", fact, 1);
    dl_close(db);

    db = dl_open(dir);
    assert(db);

    memset(&got, 0, sizeof(got));
    if (dl_prefix(db, "f", NULL, 0, tcb, &got) != 1) pass = 0;
    if (got.count == 1) {
        uint32_t h = got.data[0];
        if (h != h_before) pass = 0;               /* stable handle */
        if (!term_is_list(db->terms, h)) pass = 0;
        if (term_length(db->terms, h) != 3) pass = 0;
        if (term_car(db->terms, h) != 1) pass = 0;
        if (mk_list(db->terms, elems, 3) != h) pass = 0;  /* re-intern == */
    } else {
        pass = 0;
    }

    tset_free(&got);
    dl_close(db);
    if (pass) PASS(); else FAIL("persistence round-trip diverged");
}

/* ─── T7: boundary int >= TERM_BASE rejected ───────────────────────────── */

static void t7_boundary(void)
{
    char dir[256];
    dl_db *db;
    int pass = 1;

    TEST("T7 boundary: int literal >= TERM_BASE rejected (parser)");
    db = fresh_db(dir, sizeof(dir), "t7");
    assert(db);
    assert(dl_declare_relation(db, "p", 1) == 0);

    /* parser: an int literal >= TERM_BASE (2^31 = 2147483648) is rejected. */
    if (dl_load_rules(db, "q(X) :- p(X), X = 2147483648.\n") == 0)
        pass = 0;

    /* boundary-1 is still a legal int literal. */
    if (dl_load_rules(db, "q(X) :- p(X), X = 2147483647.\n") != 0)
        pass = 0;

    dl_close(db);
    if (pass) PASS(); else FAIL("boundary int not rejected");
}

/* ─── T8: compile-time rejects ─────────────────────────────────────────── */

static void t8_rejects(void)
{
    char dir[256];
    dl_db *db;
    int pass = 1;

    TEST("T8 compile-time rejects (comparison/list-literal, negated, var-in-list)");
    db = fresh_db(dir, sizeof(dir), "t8");
    assert(db);
    assert(dl_declare_relation(db, "p", 1) == 0);

    /* comparison over a list literal (parser rejects TOK_LIST rhs). */
    if (dl_load_rules(db, "q(X) :- p(X), X < [1,2].\n") == 0) pass = 0;

    /* negated list builtin (range restriction). */
    if (dl_load_rules(db, "q(X) :- p(X), !(L = cons(X, [])).\n") == 0) pass = 0;

    /* variable inside a list literal (Phase-2 pattern). */
    if (dl_load_rules(db, "q(X) :- p(X), [X|_] = [1,2].\n") == 0) pass = 0;

    /* control: a valid pure-data list rule still compiles. */
    if (dl_load_rules(db, "q([1,2]) :- p(X).\n") != 0) pass = 0;

    dl_close(db);
    if (pass) PASS(); else FAIL("expected compile error not raised");
}

/* ─── main ─────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("=== v2 lists / nested-term tests ===\n");

    t1_termstore();
    t2_roundtrip();
    t3_list_facts();
    t4_builtins();
    t5_gating();
    t6_persistence();
    t7_boundary();
    t8_rejects();

    printf("\n%d tests run, %d failed.\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
