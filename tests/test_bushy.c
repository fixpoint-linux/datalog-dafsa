/*
 * test_bushy.c — BUSHY join plan correctness tests (v2)
 *
 * Covers:
 *   T1  cross-subtree-probe atom compiles to SCAN not LOOKUP (bytecode)
 *   T2  interface arity > MAX_ARITY forces a left-deep fallback (no OP_MAT_*)
 *   T3  bushy rules are IVM/DRed-ineligible (route to full re-eval)
 *   T4  hand-crafted arity-2 shape corpus (path/clique/barbell + controls):
 *       bushy == left-deep(reorder) == body-order left-deep, byte-for-byte
 *   T5  true-star (distinct hub vars) has no valid 2+2 split → left-deep only
 *   T6  randomized corpus (3..7 atoms) — same equivalence oracle
 *
 * The equivalence oracle reuses the IVM pattern: compile+materialize each
 * rule in a fresh database under each plan and compare the SORTED head view
 * byte-for-byte.  Join is associative/commutative, so bushy must equal
 * left-deep by construction; this test is the enforcement backstop.
 */
#include "dl.h"
#include "compiler.h"    /* g_bushy/g_reorder, vm_opcode, vm_instr */
#include "vm.h"
#include "dl_internal.h" /* dl_db layout (crules inspection)       */
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

/* ─── Tuple collection (sorted, byte-for-byte comparable) ─────────────── */

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

static size_t g_cmp_sz;
static int cmp_tuple(const void *a, const void *b)
{
    int r = memcmp(a, b, g_cmp_sz);
    return (r > 0) - (r < 0);
}

static void collect(dl_db *db, const char *rel, tset *out)
{
    memset(out, 0, sizeof(*out));
    dl_prefix(db, rel, NULL, 0, tcb, out);
    if (out->count > 1) {
        g_cmp_sz = (size_t)out->arity * sizeof(uint32_t);
        qsort(out->data, (size_t)out->count, g_cmp_sz, cmp_tuple);
    }
}

static int sets_equal(const tset *a, const tset *b)
{
    long i;
    if (a->count != b->count || a->arity != b->arity) return 0;
    for (i = 0; i < a->count; i++)
        if (memcmp(a->data + (size_t)i * a->arity,
                   b->data + (size_t)i * b->arity,
                   (size_t)a->arity * sizeof(uint32_t)) != 0)
            return 0;
    return 1;
}

/* ─── Helpers ─────────────────────────────────────────────────────────── */

static void rm_dir(const char *dir)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    system(cmd);
}

static void load_rows(dl_db *db, const char *dir, const char *rel,
                      uint8_t arity, const uint32_t *cols, int nfacts)
{
    char path[512];
    int i, c;
    FILE *f;
    snprintf(path, sizeof(path), "%s/%s.csv", dir, rel);
    f = fopen(path, "w");
    assert(f);
    for (i = 0; i < nfacts; i++) {
        for (c = 0; c < arity; c++) {
            if (c > 0) fputc(',', f);
            fprintf(f, "%u", cols[(size_t)i * arity + (size_t)c]);
        }
        fputc('\n', f);
    }
    fclose(f);
    assert(dl_load_facts(db, rel, path) == nfacts);
}

/* arity-2 fact corpus: complete 2-node graph (values {1,2}) — every join is
 * non-empty and bounded by 2^(n+1) ≤ 256 tuples, so the property test stays
 * fast while still exercising real joins. */
static const uint32_t FACTS2[] = {
    1,1, 1,2, 2,1, 2,2
};
#define NFACTS2 4

/* Compile+materialize a rule under a given (bushy, reorder) plan and return
 * the sorted head view.  All body relations are arity-2. */
static void materialize2(const char *dirname, const char *rules,
                         int nrels, int bushy, int reorder, tset *out_head)
{
    char dir[256];
    char relname[16];
    int i;
    snprintf(dir, sizeof(dir), "build-tmp/%s", dirname);
    rm_dir(dir);
    dl_db *db = dl_open(dir);
    assert(db);
    for (i = 0; i < nrels; i++) {
        snprintf(relname, sizeof(relname), "r%d", i);
        assert(dl_declare_relation(db, relname, 2) == 0);
    }
    for (i = 0; i < nrels; i++) {
        snprintf(relname, sizeof(relname), "r%d", i);
        load_rows(db, dir, relname, 2, FACTS2, NFACTS2);
    }
    g_bushy = bushy;
    g_reorder = reorder;
    assert(dl_load_rules(db, rules) == 0);
    assert(dl_compile(db) == 0);
    collect(db, "ans", out_head);
    dl_close(db);
    rm_dir(dir);
}

/* Deterministic xorshift32. */
static uint32_t prng_state;
static uint32_t prng(void)
{
    uint32_t x = prng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    prng_state = x;
    return x;
}

/* Generate a connected arity-2 rule with n atoms (3..7), vars A..H (so the
 * join stays within MAX_ARITY=8 and bushy is eligible).  Atom i>=1 reuses one
 * earlier var and adds a fresh var.  All atoms read the SAME relation r0 —
 * the join graph is over variable sharing, so this is equivalent to distinct
 * relations for join planning, but keeps the corpus fast (one CSV load). */
static void gen_rule(int n, uint32_t seed, char *buf, size_t sz)
{
    static const char *varname = "ABCDEFGH";
    char body[768];
    int boff = 0;
    int nvars = 2;
    int i;
    prng_state = seed;
    boff += snprintf(body + boff, sizeof(body) - boff, "r0(A,B)");
    for (i = 1; i < n; i++) {
        int reuse = (int)(prng() % (uint32_t)nvars);
        int fresh = nvars;
        if (nvars < 8) nvars++;
        if (prng() & 1)
            boff += snprintf(body + boff, sizeof(body) - boff,
                             ",r0(%c,%c)", varname[reuse], varname[fresh]);
        else
            boff += snprintf(body + boff, sizeof(body) - boff,
                             ",r0(%c,%c)", varname[fresh], varname[reuse]);
    }
    snprintf(buf, sz, "ans(A,B):-%s.", body);
}

/* ─── T1: cross-subtree-probe atom compiles to SCAN not LOOKUP ────────── */

/* r0(A,B) r1(B,C) r2(A,D) r3(D,C) is a 4-cycle.  A natural 2+2 split
 * {r0,r1} | {r2,r3} has cut {A,C}; r2(A,D) shares A only with the sibling
 * (and D with the later r3), so it must compile to OP_SCAN, not OP_LOOKUP. */
static void test_cross_subtree_scan(void)
{
    static const char *rules =
        "ans(A,B,C,D):-r0(A,B),r1(B,C),r2(A,D),r3(D,C).\n";
    TEST("T1: cross-subtree-probe atom compiles to SCAN not LOOKUP");
    {
        char dir[256] = "build-tmp/bushy-t1";
        rm_dir(dir);
        dl_db *db = dl_open(dir);
        int i, k, found_mat = 0;
        int op_by_body[4] = {-1, -1, -1, -1};
        assert(db);
        for (i = 0; i < 4; i++) {
            char rn[16]; snprintf(rn, sizeof(rn), "r%d", i);
            assert(dl_declare_relation(db, rn, 2) == 0);
        }
        g_bushy = 1; g_reorder = 1;
        assert(dl_load_rules(db, rules) == 0);
        assert(dl_compile(db) == 0);
        for (k = 0; k < db->n_crules; k++) {
            compiled_rule *cr = db->crules[k];
            int ii;
            for (ii = 0; ii < cr->n_instrs; ii++) {
                vm_instr *in = &cr->instrs[ii];
                if (in->op == OP_SCAN || in->op == OP_LOOKUP ||
                    in->op == OP_LOOKUP_PERM || in->op == OP_WALK) {
                    if (in->body_idx < 4 && op_by_body[in->body_idx] < 0)
                        op_by_body[in->body_idx] = in->op;
                }
                if (in->op == OP_MAT_BEGIN || in->op == OP_MAT_JOIN)
                    found_mat = 1;
            }
        }
        if (!found_mat) {
            FAIL("bushy did not fire (no OP_MAT_*)");
        } else if (op_by_body[2] != OP_SCAN) {
            FAIL("body atom r2 (cross-subtree probe) is not OP_SCAN");
        } else if (op_by_body[0] != OP_SCAN) {
            FAIL("body atom r0 (first in subtree) is not OP_SCAN");
        } else if (op_by_body[1] != OP_LOOKUP || op_by_body[3] != OP_LOOKUP) {
            FAIL("intra-subtree joins r1/r3 are not OP_LOOKUP");
        } else {
            PASS();
        }
        dl_close(db);
        rm_dir(dir);
    }
}

/* ─── T2: interface arity > MAX_ARITY forces a left-deep fallback ─────── */

static void test_arity_fallback(void)
{
    /* 9 distinct vars across the join → root join output arity 9 > 8, so the
     * trigger must refuse bushy and emit a plain left-deep plan. */
    static const char *rules =
        "ans(A,B):-r0(A,B,C),r1(C,D,E),r2(E,F,G),r3(G,H,I).\n";
    TEST("T2: interface arity > 8 falls back to left-deep");
    {
        char dir[256] = "build-tmp/bushy-t2";
        rm_dir(dir);
        dl_db *db = dl_open(dir);
        int i, k, found_mat = 0;
        assert(db);
        for (i = 0; i < 4; i++) {
            char rn[16]; snprintf(rn, sizeof(rn), "r%d", i);
            assert(dl_declare_relation(db, rn, 3) == 0);
        }
        g_bushy = 1; g_reorder = 1;
        assert(dl_load_rules(db, rules) == 0);
        assert(dl_compile(db) == 0);
        for (k = 0; k < db->n_crules; k++) {
            compiled_rule *cr = db->crules[k];
            int ii;
            for (ii = 0; ii < cr->n_instrs; ii++)
                if (cr->instrs[ii].op == OP_MAT_BEGIN ||
                    cr->instrs[ii].op == OP_MAT_JOIN)
                    found_mat = 1;
        }
        if (found_mat)
            FAIL("bushy fired despite interface arity > 8");
        else
            PASS();
        dl_close(db);
        rm_dir(dir);
    }
}

/* ─── T3: bushy rules are IVM/DRed-ineligible (full re-eval) ──────────── */

static void test_ivm_gates(void)
{
    static const char *rules =
        "ans(A,B,C,D):-r0(A,B),r1(B,C),r2(A,D),r3(D,C).\n";
    TEST("T3: bushy rules are IVM/DRed-ineligible");
    {
        char dir[256] = "build-tmp/bushy-t3";
        rm_dir(dir);
        dl_db *db = dl_open(dir);
        int i, has_mat = 0, k;
        assert(db);
        for (i = 0; i < 4; i++) {
            char rn[16]; snprintf(rn, sizeof(rn), "r%d", i);
            assert(dl_declare_relation(db, rn, 2) == 0);
        }
        g_bushy = 1; g_reorder = 1;
        assert(dl_load_rules(db, rules) == 0);
        assert(dl_compile(db) == 0);
        for (k = 0; k < db->n_crules; k++) {
            compiled_rule *cr = db->crules[k];
            int ii;
            for (ii = 0; ii < cr->n_instrs; ii++)
                if (cr->instrs[ii].op == OP_MAT_BEGIN ||
                    cr->instrs[ii].op == OP_MAT_JOIN)
                    has_mat = 1;
        }
        if (!has_mat) {
            FAIL("bushy did not fire (nothing to gate)");
        } else if (vm_ivm_eligible(db) != 0) {
            FAIL("bushy rule wrongly IVM-insert-eligible");
        } else if (vm_dred_eligible(db) != 0) {
            FAIL("bushy rule wrongly DRed-eligible");
        } else {
            PASS();
        }
        dl_close(db);
        rm_dir(dir);
    }
}

/* ─── Property oracle ─────────────────────────────────────────────────── */

static void check_equiv(const char *rules, int nrels)
{
    tset bush, left, body;
    materialize2("bushy-pa", rules, nrels, 1, 1, &bush);
    materialize2("bushy-pb", rules, nrels, 0, 1, &left);
    materialize2("bushy-pc", rules, nrels, 0, 0, &body);
    if (!sets_equal(&bush, &left) || !sets_equal(&bush, &body)) {
        printf("\n    bushy %ld rows vs left-deep %ld vs body %ld\n",
               bush.count, left.count, body.count);
        long j;
        for (j = 0; j < bush.count && j < 10; j++)
            printf("      bush (%u,%u)\n", bush.data[j*2], bush.data[j*2+1]);
        for (j = 0; j < left.count && j < 10; j++)
            printf("      left (%u,%u)\n", left.data[j*2], left.data[j*2+1]);
        FAIL("bushy != left-deep != body-order (see dump)");
    } else {
        PASS();
    }
    tset_free(&bush); tset_free(&left); tset_free(&body);
}

/* ─── T4: hand-crafted arity-2 shape corpus ───────────────────────────── */

static void test_shape_corpus(void)
{
    static const char *shapes[] = {
        /* path (chain) — 4 and 5 atoms */
        "ans(A,B):-r0(A,B),r1(B,C),r2(C,D),r3(D,E).",
        "ans(A,B):-r0(A,B),r1(B,C),r2(C,D),r3(D,E),r4(E,F).",
        /* clique (every atom shares the hub var X) — 4 and 5 atoms */
        "ans(X,A):-r0(X,A),r1(X,B),r2(X,C),r3(X,D).",
        "ans(X,A):-r0(X,A),r1(X,B),r2(X,C),r3(X,D),r4(X,E).",
        /* barbell: two triangles joined by one var */
        "ans(A,D):-r0(A,B),r1(B,C),r2(A,C),r3(D,E),r4(E,F),r5(D,F),r6(C,D).",
        /* left-deep controls (n<4, bushy==left-deep trivially) */
        "ans(A,B):-r0(A,B),r1(B,C),r2(C,D).",
        "ans(A,B):-r0(A,B),r1(B,C).",
    };
    int n = (int)(sizeof(shapes) / sizeof(shapes[0]));
    int i;
    for (i = 0; i < n; i++) {
        TEST("T4: shape corpus (bushy == left-deep == body-order)");
        check_equiv(shapes[i], 8);
    }
}

/* ─── T5: true-star (distinct hub vars) → no valid split → left-deep ──── */

/* Center r0(X1,X2,X3,X4) (arity 4) shares a DISTINCT var with each leaf, so
 * removing the center disconnects the leaves: no 2+2 split has both halves
 * connected, so bushy must refuse and fall back to left-deep. */
static void test_true_star_leftdeep(void)
{
    static const char *rules =
        "ans(X1,X2):-r0(X1,X2,X3,X4),r1(X1,A),r2(X2,B),r3(X3,C),r4(X4,D).\n";
    TEST("T5: true-star (distinct hubs) compiles to left-deep");
    {
        char dir[256] = "build-tmp/bushy-t5";
        rm_dir(dir);
        dl_db *db = dl_open(dir);
        int i, k, found_mat = 0;
        assert(db);
        assert(dl_declare_relation(db, "r0", 4) == 0);
        for (i = 1; i < 5; i++) {
            char rn[16]; snprintf(rn, sizeof(rn), "r%d", i);
            assert(dl_declare_relation(db, rn, 2) == 0);
        }
        g_bushy = 1; g_reorder = 1;
        assert(dl_load_rules(db, rules) == 0);
        assert(dl_compile(db) == 0);
        for (k = 0; k < db->n_crules; k++) {
            compiled_rule *cr = db->crules[k];
            int ii;
            for (ii = 0; ii < cr->n_instrs; ii++)
                if (cr->instrs[ii].op == OP_MAT_BEGIN ||
                    cr->instrs[ii].op == OP_MAT_JOIN)
                    found_mat = 1;
        }
        if (found_mat)
            FAIL("bushy fired despite no valid 2+2 split");
        else
            PASS();
        dl_close(db);
        rm_dir(dir);
    }
}

/* ─── T7: DISTINCT per-relation fact corpus (deep-review addition) ─────── */
/*
 * The uniform FACTS2 corpus (same complete {1,2}x{1,2} graph on every
 * relation) saturates the head view: mutation testing showed that breaking
 * OP_MAT_JOIN's key-equality check or swapping the interface shared-prefix
 * order still yields identical (saturated) results, so those bugs pass T4/T6.
 * Distinct, skewed fact sets per relation make wrong joins observably wrong.
 */
typedef struct { const char *csv; int nlines; } relfacts;

static void materialize_distinct(const char *dirname, const relfacts *rf,
                                 int nrels, const char *rules,
                                 int bushy, int reorder, tset *out_head)
{
    char dir[256];
    char relname[16];
    int i;
    snprintf(dir, sizeof(dir), "build-tmp/%s", dirname);
    rm_dir(dir);
    dl_db *db = dl_open(dir);
    assert(db);
    for (i = 0; i < nrels; i++) {
        snprintf(relname, sizeof(relname), "r%d", i);
        assert(dl_declare_relation(db, relname, 2) == 0);
    }
    for (i = 0; i < nrels; i++) {
        char path[512];
        FILE *f;
        snprintf(relname, sizeof(relname), "r%d", i);
        snprintf(path, sizeof(path), "%s/%s.csv", dir, relname);
        f = fopen(path, "w");
        assert(f);
        fputs(rf[i].csv, f);
        fclose(f);
        assert(dl_load_facts(db, relname, path) == rf[i].nlines);
    }
    g_bushy = bushy;
    g_reorder = reorder;
    assert(dl_load_rules(db, rules) == 0);
    assert(dl_compile(db) == 0);
    collect(db, "ans", out_head);
    dl_close(db);
    rm_dir(dir);
}

static void check_equiv_distinct(const char *rules, const relfacts *rf,
                                 int nrels)
{
    tset bush, left, body;
    materialize_distinct("bushy-da", rf, nrels, rules, 1, 1, &bush);
    materialize_distinct("bushy-db", rf, nrels, rules, 0, 1, &left);
    materialize_distinct("bushy-dc", rf, nrels, rules, 0, 0, &body);
    TEST("T7: distinct-fact corpus (bushy == left-deep == body-order)");
    if (!sets_equal(&bush, &left) || !sets_equal(&bush, &body)) {
        FAIL("T7: distinct-fact corpus mismatch (see dump)");
    } else {
        PASS();
    }
    tset_free(&bush); tset_free(&left); tset_free(&body);
}

static void test_distinct_corpus(void)
{
    /* Per-relation DISTINCT, skewed arity-2 fact sets (the mutation test that
     * the uniform FACTS2 corpus masks).  Each is a different-size relation so
     * a wrong OP_MAT_JOIN key-equality or a swapped interface prefix produces
     * observably wrong results. */
    static const relfacts rf[] = {
        { "1,10\n2,20\n3,30\n4,40\n", 4 },   /* r0 */
        { "10,100\n20,200\n30,300\n", 3 },   /* r1 */
        { "100,1000\n200,2000\n300,3000\n400,4000\n500,5000\n", 5 }, /* r2 */
        { "1000,7\n2000,8\n", 2 },           /* r3 */
        { "7,700\n8,800\n9,900\n", 3 },      /* r4 */
        { "100,11\n200,22\n", 2 },           /* r5 */
        { "1,2\n3,4\n5,6\n7,8\n9,10\n", 5 }, /* r6 */
    };

    /* 4-cycle with skewed per-relation facts (path: r0-r1-r2-r3-r4). */
    check_equiv_distinct(
        "ans(A,E):-r0(A,B),r1(B,C),r2(C,D),r3(D,E),r4(E,F).\n",
        rf, 5);

    /* Clique on a shared hub var X, skewed facts. */
    check_equiv_distinct(
        "ans(X,A):-r0(X,A),r1(X,B),r2(X,C),r3(X,D).\n",
        rf, 4);

    /* Barbell: two skewed triangles joined by one var. */
    check_equiv_distinct(
        "ans(A,F):-r0(A,B),r1(B,C),r2(A,C),r3(D,E),r4(E,F),r5(D,F),r6(C,D).\n",
        rf, 7);

    /* Hash-collision discriminator: the 1-column keys 1 and 9 hash into the
     * same OP_MAT_JOIN bucket (hcap 8 -> slot 4).  The bushy split puts r0/r1
     * (half L, share Y) on one side and r2/r3 (half R, share Z) on the other,
     * joined on the cut var X.  L's X key = 1, R's X key = 9: distinct keys,
     * same bucket.  If the key-equality check were dropped (a silent-wrong-
     * answer OP_MAT_JOIN bug) they would falsely join; distinct values make it
     * observable.  (Mutation test: removing mat_join_build's key-equality
     * check, or swapping the interface shared-prefix order, both break this.) */
    {
        static const relfacts cf[] = {
            { "1,10\n", 1 },   /* r0: X=1, Y=10 */
            { "10,7\n", 1 },   /* r1: Y=10, W=7 */
            { "9,20\n", 1 },   /* r2: X=9, Z=20 (same bucket as X=1) */
            { "20,8\n", 1 },   /* r3: Z=20, Q=8 */
        };
        check_equiv_distinct(
            "ans(X,W,Q):-r0(X,Y),r1(Y,W),r2(X,Z),r3(Z,Q).\n",
            cf, 4);
    }
}

/* ─── T6: randomized corpus (3..7 atoms) ──────────────────────────────── */

static void test_random_corpus(void)
{
    char rule[1024];
    int seed;
    for (seed = 100; seed < 110; seed++) {
        int n = 3 + (seed % 5);   /* 3..7 */
        gen_rule(n, (uint32_t)seed, rule, sizeof(rule));
        TEST("T6: random corpus (bushy == left-deep == body-order)");
        check_equiv(rule, 1);   /* single relation r0 */
    }
}

int main(void)
{
    printf("=== BUSHY join plan tests ===\n");
    test_cross_subtree_scan();
    test_arity_fallback();
    test_ivm_gates();
    test_shape_corpus();
    test_true_star_leftdeep();
    test_distinct_corpus();
    test_random_corpus();
    printf("\n%d tests, %d failures\n", tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
