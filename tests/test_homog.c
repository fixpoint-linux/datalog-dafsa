/*
 * test_homog.c — int/symbol column-kind homogeneity (S1) acceptance tests.
 *
 * The bug (MEASURED): every engine value is a bare u32 — a raw integer or a
 * 1-based interned sym_id, one value space, no tag.  With an INTEGER column
 * e={1,2} and the program
 *
 *     q(X):-e(X).  p(X):-e(X),!q(foo).
 *
 * 'foo' interned to sym_id 1, q stored raw ints {1,2}, and OP_NEG_CHECK
 * probed (sym 1 == raw 1) -> TRUE -> the negation failed -> p had ZERO
 * rows, rc=0, no error — on dl_query_rules_ro, WFS, magic and the plain
 * `dl` CLI alike (the symbol-EDB control e={a,b} correctly returned both).
 *
 * S1 makes the constant-vs-column KIND mismatch LOUD at compile_rules (the
 * single chokepoint every strategy path enters) with the diagnostic
 *
 *   compile error: column kind mismatch in REL/COL: constant 'foo' is a
 *   symbol but column holds integers (rule N)
 *
 * Tests:
 *   T1  the reproduced case is now LOUD (rc=-1, message on stderr) via
 *       dl_load_rules AND dl_query_rules_ro
 *   T2  symbol-EDB control (e={a,b}, same program) is UNCHANGED: loads,
 *       p={a,b}
 *   T3  int-constant-vs-int-column control (!q(1) with q derived from
 *       e={1,2}) loads and answers the standard semantics
 *   T4  fresh/empty column stays PERMISSIVE (declared relation, no rows:
 *       no kind is established, no check fires)
 *   T5  the same rejection through the MAGIC path (dl_query_magic_adorn)
 *       and the WFS path (dl_query_wfs_ro)
 *   T6  the TYPED path is unchanged/stricter: an attached schema's
 *       Natural column still rejects a symbol constant at dl_load_rules
 *       (via the typechecker), and the untyped check does not weaken it
 */

#include "dl.h"
#include "schema.h"

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

/* ─── tuple collection (mirrors test_m2.c) ─────────────────────────────── */

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
    assert(arity == ts->arity);
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

static void tset_free(tuple_set *ts) { free(ts->data); memset(ts, 0, sizeof(*ts)); }

/* ─── db helpers ───────────────────────────────────────────────────────── */

static dl_db *g_db;

static void setup(void)
{
    system("rm -rf build-tmp/homogdb");
    g_db = dl_open("build-tmp/homogdb");
    assert(g_db);
}

static void teardown(void)
{
    dl_close(g_db);
    system("rm -rf build-tmp/homogdb");
}

/* Load raw u32 rows through the same declare+CSV+dl_load_facts path the
 * other suites use (cells are written as plain integers; a SYMBOL row is
 * written by passing the interned id of a name — dl_load_facts re-interns
 * the printed form, so 'a'/'b' intern to their ids consistently). */
static void load_rows_u32(const char *rel, uint8_t arity,
                          const uint32_t *cols, int nrows)
{
    char path[256];
    FILE *f;
    int i, c;

    assert(dl_declare_relation(g_db, rel, arity) == 0);
    snprintf(path, sizeof path, "build-tmp/homogdb/%s.csv", rel);
    f = fopen(path, "w");
    assert(f);
    for (i = 0; i < nrows; i++) {
        for (c = 0; c < arity; c++) {
            if (c) fputc(',', f);
            fprintf(f, "%u", cols[(size_t)i * arity + (size_t)c]);
        }
        fputc('\n', f);
    }
    fclose(f);
    assert(dl_load_facts(g_db, rel, path) == nrows);
}

/* Load SYMBOL rows: intern each name first, then store its id.  The CSV
 * cell is the symbol's string, which dl_load_facts interns to the SAME id
 * (the interner is keyed by string). */
static void load_rows_sym(const char *rel, uint8_t arity,
                          const char **cells, int nrows)
{
    char path[256];
    FILE *f;
    int i, c;

    assert(dl_declare_relation(g_db, rel, arity) == 0);
    (void)dl_intern_str(g_db, cells[0]); /* pin ids before the scan */
    snprintf(path, sizeof path, "build-tmp/homogdb/%s.csv", rel);
    f = fopen(path, "w");
    assert(f);
    for (i = 0; i < nrows; i++) {
        for (c = 0; c < arity; c++) {
            if (c) fputc(',', f);
            fprintf(f, "%s", cells[(size_t)i * arity + (size_t)c]);
        }
        fputc('\n', f);
    }
    fclose(f);
    assert(dl_load_facts(g_db, rel, path) == nrows);
}

/* ─── T1: the reproduced case is LOUD ──────────────────────────────────── */

static void test_repro_loud(void)
{
    uint32_t e_rows[2];
    int rc;

    TEST("homog: e={1,2}; q(X):-e(X). p(X):-e(X),!q(foo). is LOUD");

    setup();
    e_rows[0] = 1; e_rows[1] = 2;
    load_rows_u32("e", 1, e_rows, 2);

    /* dl_load_rules must FAIL */
    rc = dl_load_rules(g_db, "q(X):-e(X).\np(X):-e(X),!q(foo).\n");
    if (rc != -1) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("dl_load_rules accepted the mixed-kind program");
        return;
    }
    PASS();

    /* ...and the read-only one-shot path must fail identically */
    TEST("homog: same program via dl_query_rules_ro is LOUD");
    {
        tuple_set res;
        long n;
        memset(&res, 0, sizeof res);
        n = dl_query_rules_ro(g_db,
            "q2(X):-e(X).\np2(X):-e(X),!q2(foo).\n", "p2", tset_cb, &res);
        tset_free(&res);
        if (n >= 0) {
            printf("(n=%ld) ", n);
            teardown();
            FAIL("dl_query_rules_ro answered the mixed-kind program");
            return;
        }
    }
    teardown();
    PASS();
}

/* ─── T2: symbol-EDB control is UNCHANGED ──────────────────────────────── */

static void test_symbol_control(void)
{
    const char *cells[2];
    tuple_set res;
    long n;
    uint32_t a, b;
    int ok;

    TEST("homog: e={a,b} control loads and p={a,b} (UNCHANGED)");

    setup();
    cells[0] = "a"; cells[1] = "b";
    load_rows_sym("e", 1, cells, 2);

    if (dl_load_rules(g_db, "q(X):-e(X).\np(X):-e(X),!q(foo).\n") != 0) {
        teardown();
        FAIL("symbol-EDB control failed to load");
        return;
    }
    assert(dl_compile(g_db) == 0);

    memset(&res, 0, sizeof res);
    n = dl_query(g_db, "p", tset_cb, &res);
    if (n < 0 || res.count != 2) {
        printf("(n=%ld cnt=%ld) ", n, res.count);
        tset_free(&res);
        teardown();
        FAIL("symbol control: expected p={a,b}");
        return;
    }
    a = dl_intern_str(g_db, "a");
    b = dl_intern_str(g_db, "b");
    ok = (res.count == 2) &&
         ((res.data[0] == a && res.data[1] == b) ||
          (res.data[0] == b && res.data[1] == a));
    tset_free(&res);
    teardown();
    if (!ok) {
        FAIL("symbol control: p is not {a,b}");
        return;
    }
    PASS();
}

/* ─── T3: int-constant-vs-int-column control ───────────────────────────── */

static void test_int_control(void)
{
    uint32_t e_rows[2];
    tuple_set res;
    long n;

    TEST("homog: !q(3) over int e={1,2} loads; p={1,2} (int/int consistent)");

    setup();
    e_rows[0] = 1; e_rows[1] = 2;
    load_rows_u32("e", 1, e_rows, 2);

    if (dl_load_rules(g_db, "q(X):-e(X).\np(X):-e(X),!q(3).\n") != 0) {
        teardown();
        FAIL("int/int control failed to load");
        return;
    }
    assert(dl_compile(g_db) == 0);

    memset(&res, 0, sizeof res);
    n = dl_query(g_db, "p", tset_cb, &res);
    /* 3 is not in e, so q(3) is FALSE and !q(3) succeeds: p={1,2} */
    if (n < 0 || res.count != 2 ||
        ((res.data[0] == 1 && res.data[1] == 2) ||
         (res.data[0] == 2 && res.data[1] == 1)) == 0) {
        printf("(n=%ld cnt=%ld v=%u) ", n, res.count, res.count ? res.data[0] : 0);
        tset_free(&res);
        teardown();
        FAIL("int control: expected p={1,2}");
        return;
    }
    tset_free(&res);
    teardown();
    PASS();
}

/* ─── T4: fresh/empty column stays PERMISSIVE ──────────────────────────── */

static void test_fresh_permissive(void)
{
    uint32_t e_rows[2];

    TEST("homog: declared-but-empty relation is PERMISSIVE (unknown kind)");

    setup();
    e_rows[0] = 1; e_rows[1] = 2;
    load_rows_u32("e", 1, e_rows, 2);
    /* other: declared, ZERO rows -> no kind established -> no check */
    assert(dl_declare_relation(g_db, "other", 1) == 0);

    if (dl_load_rules(g_db, "r(X):-e(X),!other(bar).\n") != 0) {
        teardown();
        FAIL("fresh/empty column was not permissive");
        return;
    }
    teardown();
    PASS();
}

/* ─── T5: the same rejection through the MAGIC and WFS paths ───────────── */

static void test_magic_and_wfs(void)
{
    uint32_t e_rows[2];
    tuple_set res;
    long n;

    TEST("homog: magic path rejects the mixed-kind program (LOUD)");

    setup();
    /* values >= 10: fresh sym ids are 1..n, so interning 'foo' (id 1)
     * cannot make e's stored ints RESOLVE — kinds are RECORDED (S2), not
     * inferred, so the comment below no longer matters for e; kept >=10 to
     * also exercise large raw ints. */
    e_rows[0] = 10; e_rows[1] = 20;
    load_rows_u32("e", 1, e_rows, 2);
    /* symcol: a symbol-kind column (loaded via STRING cells) for the
     * magic bound check's sound direction */
    {
        const char *cells[2] = { "alpha", "beta" };
        load_rows_sym("symcol", 1, cells, 2);
    }

    /* the resident program itself must be int/int-consistent (a mixed
     * program is already rejected at dl_load_rules by T1) */
    if (dl_load_rules(g_db, "q(X):-e(X).\n") != 0) {
        teardown();
        FAIL("magic setup: int/int program failed to load");
        return;
    }
    assert(dl_compile(g_db) == 0);

    /* the mixed program through the compile path every strategy shares:
     * the magic driver compiles its ADORNED program through the same
     * chokepoint, so this rules-ro probe covers that entry */
    memset(&res, 0, sizeof res);
    {
        n = dl_query_rules_ro(g_db,
            "qm(X):-e(X),!em(foo).\nem(X):-e(X).\n", "qm", tset_cb, &res);
        tset_free(&res);
        if (n >= 0) {
            printf("(n=%ld) ", n);
            teardown();
            FAIL("mixed-kind program answered via the rules-ro path");
            return;
        }
    }

    /* ...and through the magic driver itself.  The sound direction of the
     * u32 API: a bound value that does NOT resolve as a live sym id is
     * DEFINITELY a raw int, so probing it against a SYM-kind column is the
     * id-space collision and must be LOUD (pre-S1 it answered 1 row from
     * the wrong value space).  The reverse (a resolving value vs an int
     * column) is ambiguous and MUST pass — that is the reviewer's probe O:
     * an int bound misread as "a symbol" once any symbol is interned. */
    memset(&res, 0, sizeof res);
    {
        /* v1 = 1 is ambiguous: alpha interns to id 1, so 1 resolves and
         * must NOT reject.  For the LOUD direction use a value ABOVE the
         * interner's live bound (5, with ids 1..4 live) — that value is
         * DEFINITELY a raw int against a sym-kind column. */
        uint32_t loud_v = 5;
        n = dl_query_magic_adorn(g_db, "symcol", "b", &loud_v, 1, tset_cb, &res);
        tset_free(&res);
        if (n >= 0) {
            printf("(n=%ld) ", n);
            teardown();
            FAIL("magic answered a raw-int bound on a symbol column");
            return;
        }
    }
    {
        /* e is int-kind; the bound 1 resolves (ids are interned by now),
         * which is AMBIGUOUS — must NOT reject */
        uint32_t v1 = 1;
        memset(&res, 0, sizeof res);
        n = dl_query_magic_adorn(g_db, "q", "b", &v1, 1, tset_cb, &res);
        tset_free(&res);
        if (n < 0) {
            printf("(n=%ld) ", n);
            teardown();
            FAIL("magic rejected an ambiguous bound on an int column");
            return;
        }
    }

    /* WFS path: same shape, negation over an IDB predicate */
    memset(&res, 0, sizeof res);
    n = dl_query_wfs_ro(g_db, "qw(X):-e(X),!ew(foo).\new(X):-e(X).\n",
                        "qw", 0, tset_cb, &res);
    tset_free(&res);
    if (n >= 0) {
        printf("(n=%ld) ", n);
        teardown();
        FAIL("mixed-kind program answered via the WFS path");
        return;
    }
    teardown();
    PASS();
}

/* ─── T6: the TYPED path stays at least as strict ──────────────────────── */

static void test_typed_path(void)
{
    uint32_t e_rows[2];
    dl_schema s;
    dl_colspec cols[1];
    int rc;

    TEST("homog: attached schema (Natural) still rejects a symbol constant");

    setup();
    e_rows[0] = 1; e_rows[1] = 2;
    load_rows_u32("e", 1, e_rows, 2);

    memset(&s, 0, sizeof s);
    cols[0].tag = DLT_NATURAL;
    assert(dl_schema_add(&s, "e", 1, cols, 0) == 0);
    assert(dl_schema_add(&s, "q", 1, cols, 1) == 0);
    assert(dl_schema_add(&s, "q2", 1, cols, 1) == 0);
    assert(dl_schema_add(&s, "q3", 1, cols, 1) == 0);
    assert(dl_attach_schema(g_db, &s) == 0);

    rc = dl_load_rules(g_db, "q(X):-e(X),!q2(foo).\n");
    if (rc != -1) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("typed path accepted a symbol constant in a Natural column");
        return;
    }

    /* and the consistent typed program still loads (every predicate a
     * declared head or schema relation) */
    rc = dl_load_rules(g_db, "q3(X):-e(X),!q2(7).\nq2(X):-e(X).\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("typed path rejected an int/int consistent program");
        return;
    }
    teardown();
    PASS();
}

/* ─── T7: MULTI-RELATION db with symbols AND small ints MUST LOAD ─────── */
/* The S1 false positive (reviewer probe T): every prior test used a single
 * relation e, so "any interned symbol makes a small-int column read as sym"
 * never fired.  name={alice,bob,carol} interns ids 1..3; edge={1,2 / 2,3}
 * holds raw ints in the SAME range.  The rules must load and answer. */
static void test_multi_relation_int_and_sym(void)
{
    tuple_set res;
    long n;

    TEST("homog: name={alice..} + edge={1,2/2,3}; edge(1,X) WORKS");

    setup();
    {
        const char *cells[3] = { "alice", "bob", "carol" };
        load_rows_sym("name", 1, cells, 3);
    }
    {
        uint32_t edge[4] = { 1, 2, 2, 3 };
        load_rows_u32("edge", 2, edge, 2);
    }

    if (dl_load_rules(g_db, "tc2(X,Y):-edge(X,Y). q(X):-edge(1,X).\n") != 0) {
        teardown();
        FAIL("multi-relation: valid int/sym program failed to load");
        return;
    }
    assert(dl_compile(g_db) == 0);

    memset(&res, 0, sizeof res);
    n = dl_query(g_db, "q", tset_cb, &res);
    /* edge(1,X): only (1,2) -> q={2} */
    if (n < 0 || res.count != 1 || res.data[0] != 2) {
        printf("(n=%ld cnt=%ld) ", n, res.count);
        tset_free(&res);
        teardown();
        FAIL("multi-relation: q != {2}");
        return;
    }
    tset_free(&res);
    teardown();
    PASS();
}

/* ─── T8: the reproduced case is LOUD on the MULTI-RELATION db too ─────── */
static void test_repro_loud_multi_relation(void)
{
    int rc;

    TEST("homog: !q(foo) over int edge is LOUD even with name={} interned");

    setup();
    {
        const char *cells[3] = { "alice", "bob", "carol" };
        load_rows_sym("name", 1, cells, 3);
    }
    {
        uint32_t edge[4] = { 1, 2, 2, 3 };
        load_rows_u32("edge", 2, edge, 2);
    }

    rc = dl_load_rules(g_db, "q(X,Y):-edge(X,Y). p(X):-edge(X,Y),!q(1,foo).\n");
    if (rc != -1) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("multi-relation: mixed-kind program accepted");
        return;
    }
    teardown();
    PASS();
}

/* ─── T9: schema-attached rules_ro agrees with load_rules ──────────────── */
/* The S1 clone dropped d.schema (reviewer probe AA), so an attached Natural
 * schema made dl_load_rules PASS a program dl_query_rules_ro REJECTED. */
static void test_schema_clone_agreement(void)
{
    uint32_t e_rows[2];
    dl_schema s;
    dl_colspec cn;
    int rc;
    long n;
    tuple_set res;

    TEST("homog: attached schema — rules_ro agrees with load_rules");

    setup();
    e_rows[0] = 1; e_rows[1] = 2;
    load_rows_u32("e", 1, e_rows, 2);

    memset(&s, 0, sizeof s);
    memset(&cn, 0, sizeof cn);
    cn.tag = DLT_NATURAL;
    assert(dl_schema_add(&s, "e", 1, &cn, 0) == 0);
    assert(dl_schema_add(&s, "name", 1, &cn, 0) == 0);
    assert(dl_schema_add(&s, "q", 1, &cn, 1) == 0);
    assert(dl_attach_schema(g_db, &s) == 0);

    rc = dl_load_rules(g_db, "q(X):-e(X),e(1).\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("schema: typed int/int program failed to load");
        return;
    }

    memset(&res, 0, sizeof res);
    n = dl_query_rules_ro(g_db, "q2(X):-e(X),e(1).\n", "q2", tset_cb, &res);
    if (n < 0 || res.count != 2) {
        printf("(n=%ld cnt=%ld) ", n, res.count);
        tset_free(&res);
        teardown();
        FAIL("schema: rules_ro rejected what load_rules accepted");
        return;
    }
    tset_free(&res);
    teardown();
    PASS();
}

/* ─── T10: >=129 body atoms LOAD with no silent/diagnostic-free failure ──
 * The S1 regression was checkAllRuleConstKinds' atoms[130] buffer: >=130
 * body atoms returned a BARE -1 with no diagnostic at LOAD time.  The kind
 * check itself now iterates head+body directly (no buffer).  The deeper
 * compile (VM join-plan frames) has a separate pre-existing capacity limit
 * (~17 atoms on this shape, MEASURED identical at baseline HEAD), so this
 * test pins the LOAD contract: rc=0, no stderr, for any body size. */
static void test_many_body_atoms(void)
{
    static char src[16384];
    int off;
    uint32_t e_rows[2];

    TEST("homog: 129 body atoms load cleanly (no silent kind-check cap)");

    setup();
    e_rows[0] = 1; e_rows[1] = 2;
    load_rows_u32("e", 1, e_rows, 2);

    off = snprintf(src, sizeof src, "p(X):-e(X)");
    for (int i = 1; i < 129; i++)
        off += snprintf(src + off, sizeof src - off, ",e(X)");
    off += snprintf(src + off, sizeof src - off, ".\n");

    if (dl_load_rules(g_db, src) != 0) {
        teardown();
        FAIL("129 body atoms failed to load");
        return;
    }
    teardown();
    PASS();
}

/* ─── T11: WFS with a symbol constant in an UNRELATED rule ─────────────── */
/* The S1 duplicate-after-buildDomain check (reviewer Z3): the valid program
 * was rejected once any rule mentioned a symbol. */
static void test_wfs_unrelated_symbol(void)
{
    uint32_t e_rows[1];
    tuple_set res;
    long n;

    TEST("homog: WFS answers with a symbol constant in an unrelated rule");

    setup();
    e_rows[0] = 1;
    load_rows_u32("e", 1, e_rows, 1);
    assert(dl_declare_relation(g_db, "e2", 2) == 0); /* declared EMPTY */

    memset(&res, 0, sizeof res);
    n = dl_query_wfs_ro(g_db,
        "q(X):-e(X). r(X):-e2(X,foo). p(X):-e(X),!q(2).\n",
        "p", 0, tset_cb, &res);
    if (n < 0) {
        printf("(n=%ld) ", n);
        tset_free(&res);
        teardown();
        FAIL("WFS rejected a valid program with an unrelated symbol constant");
        return;
    }
    if (res.count != 1 || res.data[0] != 1) {
        printf("(n=%ld cnt=%ld) ", n, res.count);
        tset_free(&res);
        teardown();
        FAIL("WFS: p != {1}");
        return;
    }
    tset_free(&res);
    teardown();
    PASS();
}

/* ─── T12: genuinely-mixed column is LOUD at load AND at add_fact ──────── */
static void test_mixed_column_loud(void)
{
    char path[256];
    FILE *f;
    int rc;

    TEST("homog: mixed int/sym CSV column is LOUD at load");

    setup();
    assert(dl_declare_relation(g_db, "m", 1) == 0);
    snprintf(path, sizeof path, "build-tmp/homogdb/m.csv");
    f = fopen(path, "w");
    assert(f);
    fprintf(f, "1\nfoo\n");
    fclose(f);

    rc = dl_load_facts(g_db, "m", path);
    if (rc != -1) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("mixed CSV column was not rejected");
        return;
    }
    teardown();
    PASS();

    TEST("homog: add_fact of a raw int on a sym column is LOUD");

    setup();
    {
        const char *cells[1] = { "alpha" };
        load_rows_sym("s", 1, cells, 1);
    }
    {
        /* 5 does not resolve as a sym id (only 1..2 are live) -> a
         * DEFINITE raw int against a sym-kind column */
        uint32_t v = 5;
        rc = dl_add_fact(g_db, "s", &v, 1);
        if (rc != -1) {
            printf("(rc=%d) ", rc);
            teardown();
            FAIL("add_fact accepted a definite raw int on a sym column");
            return;
        }
    }
    teardown();
    PASS();

    TEST("homog: add_fact of a legitimate int on an int column still works");

    setup();
    {
        uint32_t e_rows[1] = { 100 };
        load_rows_u32("e", 1, e_rows, 1);
    }
    {
        uint32_t v = 5;
        rc = dl_add_fact(g_db, "e", &v, 1);
        if (rc != 1) {
            printf("(rc=%d) ", rc);
            teardown();
            FAIL("add_fact rejected a legitimate int on an int column");
            return;
        }
    }
    teardown();
    PASS();
}

/* ─── T13: kinds PERSIST across processes (CLI load/query are separate) ── */
static void test_kinds_persist(void)
{
    int rc;

    TEST("homog: recorded kinds survive close+reopen (rels.txt)");

    setup();
    {
        uint32_t e_rows[2] = { 1, 2 };
        load_rows_u32("e", 1, e_rows, 2);
    }
    dl_close(g_db); /* writes rels.txt with e's kinds */

    g_db = dl_open("build-tmp/homogdb");
    assert(g_db);
    rc = dl_load_rules(g_db, "q(X):-e(X). p(X):-e(X),!q(foo).\n");
    if (rc != -1) {
        printf("(rc=%d) ", rc);
        dl_close(g_db);
        system("rm -rf build-tmp/homogdb");
        g_db = NULL;
        FAIL("reopened db lost the recorded kinds");
        return;
    }
    teardown();
    PASS();
}

/* ─── T14: a SPLIT dl_load_rules must not evade the kind check ────────── */
/* review SH2: each load checked only its OWN rules, so loading
 * 'q(X):-e(X).' and then 'p(X):-e(X),!q(foo).' as two calls missed the
 * cross-rule conflict (q's head kind is propagated only by the first
 * load).  The second call must reject with the same diagnostic the
 * equivalent single combined load produces. */
static void test_split_load_loud(void)
{
    int rc;

    TEST("homog: split load_rules rejects the cross-rule conflict");

    setup();
    {
        uint32_t e_rows[2] = { 1, 2 };
        load_rows_u32("e", 1, e_rows, 2);
    }
    rc = dl_load_rules(g_db, "q(X):-e(X).\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("first (clean) load failed");
        return;
    }
    rc = dl_load_rules(g_db, "p(X):-e(X),!q(foo).\n");
    if (rc != -1) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("second load evaded the kind check (split-load hole)");
        return;
    }
    teardown();
    PASS();

    TEST("homog: split load of an int-constant variant still loads");

    /* control: the same split with an INT constant is consistent with q's
     * propagated int kind and must stay accepted */
    setup();
    {
        uint32_t e_rows[2] = { 1, 2 };
        load_rows_u32("e", 1, e_rows, 2);
    }
    rc = dl_load_rules(g_db, "q(X):-e(X).\n");
    if (rc != 0) {
        teardown();
        FAIL("first (clean) load failed (control)");
        return;
    }
    rc = dl_load_rules(g_db, "p(X):-e(X),!q(3).\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("split load rejected a consistent int constant (false reject)");
        return;
    }
    teardown();
    PASS();
}

/* ─── T15: add_fact ambiguity pins — no false kind record ─────────────── */
/* review SH1: an UNRECORDED column holding AMBIGUOUS (resolving) values
 * got KIND_INT recorded the moment a definite int arrived, mislabelling a
 * column that holds BOTH spaces.  Record only when the fact is the
 * column's FIRST row. */
static void test_addfact_ambiguity_pins(void)
{
    int rc;

    TEST("homog: definite int AFTER an ambiguous sym does not record int");

    setup();
    assert(dl_declare_relation(g_db, "v", 1) == 0);
    {
        /* foo interns to a live id -> add_fact sees it as ambiguous
         * (resolving): accepted, never recorded */
        uint32_t foo = dl_intern_str(g_db, "foo");
        uint32_t v77 = 77; /* not a live sym id: definite int */
        rc = dl_add_fact(g_db, "v", &foo, 1);
        if (rc != 1) {
            printf("(rc=%d) ", rc);
            teardown();
            FAIL("add_fact rejected the ambiguous symbol");
            return;
        }
        rc = dl_add_fact(g_db, "v", &v77, 1);
        if (rc != 1) {
            printf("(rc=%d) ", rc);
            teardown();
            FAIL("add_fact rejected the definite int (mixed col is stored, only the RECORD was wrong)");
            return;
        }
    }
    /* the false record is the bug: rels.txt must carry NO kind for v */
    dl_close(g_db); /* writes rels.txt */
    {
        char path[256];
        char line[256];
        FILE *f;
        int bad = 0;
        snprintf(path, sizeof path, "build-tmp/homogdb/rels.txt");
        f = fopen(path, "r");
        if (!f) {
            system("rm -rf build-tmp/homogdb");
            g_db = NULL;
            FAIL("rels.txt missing");
            return;
        }
        while (fgets(line, sizeof line, f)) {
            if (strncmp(line, "v:1:edb:", 8) == 0)
                bad = 1; /* a kinds field was persisted for v */
        }
        fclose(f);
        if (bad) {
            system("rm -rf build-tmp/homogdb");
            g_db = NULL;
            FAIL("a kind was recorded for an ambiguous-then-definite column");
            return;
        }
    }
    system("rm -rf build-tmp/homogdb");
    g_db = NULL;
    PASS();

    TEST("homog: definite int as the column's FIRST row still records");

    setup();
    assert(dl_declare_relation(g_db, "w", 1) == 0);
    {
        /* w is EMPTY: the definite 77 is genuinely the first value */
        uint32_t v = 77;
        uint32_t foo;
        rc = dl_add_fact(g_db, "w", &v, 1);
        if (rc != 1) {
            printf("(rc=%d) ", rc);
            teardown();
            FAIL("first-row definite int was not added");
            return;
        }
        /* pin the documented permissive contract: an ambiguous (resolving)
         * value on an int-RECORDED column is accepted (the u32 API cannot
         * observe its space) — so a future tightening is visible here. */
        foo = dl_intern_str(g_db, "foo");
        rc = dl_add_fact(g_db, "w", &foo, 1);
        if (rc != 1) {
            printf("(rc=%d) ", rc);
            teardown();
            FAIL("ambiguous sym on an int-recorded column was rejected (contract change)");
            return;
        }
    }
    teardown();
    PASS();
}

/* ─── T16: IDB head kinds PERSIST across processes ────────────────────── */
/* review SH3: proc1 materializes q from 'q(X):-e(X).'; the kind never
 * reached rels.txt, so proc2's 'p(X):-e(X),!q(foo).' collided silently.
 * The head kinds recorded at load time ride to proc2 via rels.txt. */
static void test_idb_kind_persists(void)
{
    int rc;

    TEST("homog: IDB head kind persists to a second process (rels.txt)");

    setup();
    {
        uint32_t e_rows[2] = { 1, 2 };
        load_rows_u32("e", 1, e_rows, 2);
    }
    rc = dl_load_rules(g_db, "q(X):-e(X).\n");
    if (rc != 0) {
        teardown();
        FAIL("defining rule failed to load");
        return;
    }
    /* materialize q in THIS process (the CLI flow publishes after load) */
    if (dl_publish_snapshot(g_db) != 0) {
        teardown();
        FAIL("publish failed");
        return;
    }
    dl_close(g_db); /* writes rels.txt: q:1:idb:n */

    g_db = dl_open("build-tmp/homogdb");
    assert(g_db);
    rc = dl_load_rules(g_db, "p(X):-e(X),!q(foo).\n");
    if (rc != -1) {
        printf("(rc=%d) ", rc);
        dl_close(g_db);
        system("rm -rf build-tmp/homogdb");
        g_db = NULL;
        FAIL("second process did not reject the collision (IDB kind lost)");
        return;
    }
    teardown();
    PASS();

    TEST("homog: symbol-IDB head kind persists too (control)");

    /* control: e/f symbolic -> q(sym) -> the SECOND process's program is
     * consistent and must stay accepted, with correct NEGATION semantics
     * (q={a} from f, so p = e \\ q = {b}) — proving the persisted kind
     * does not merely reject everything symbol-flavoured */
    setup();
    {
        const char *ecells[2] = { "a", "b" };
        const char *fcells[1] = { "a" };
        load_rows_sym("e", 1, ecells, 2);
        load_rows_sym("f", 1, fcells, 1);
    }
    rc = dl_load_rules(g_db, "q(X):-f(X).\n");
    if (rc != 0) {
        teardown();
        FAIL("symbol defining rule failed to load");
        return;
    }
    /* materialize q here — proc2's negation must see q={a} */
    if (dl_publish_snapshot(g_db) != 0) {
        teardown();
        FAIL("publish failed (control)");
        return;
    }
    dl_close(g_db);

    g_db = dl_open("build-tmp/homogdb");
    assert(g_db);
    rc = dl_load_rules(g_db, "p(X):-e(X),!q(X).\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        dl_close(g_db);
        system("rm -rf build-tmp/homogdb");
        g_db = NULL;
        FAIL("symbol-IDB kind persisted as a false reject");
        return;
    }
    {
        tuple_set res;
        long n;
        memset(&res, 0, sizeof res);
        /* publish first — the CLI flow does, and proc1's snapshot predates
         * p (dl_query prefers the published snapshot) */
        if (dl_publish_snapshot(g_db) != 0) {
            teardown();
            FAIL("publish failed (proc2)");
            return;
        }
        n = dl_query(g_db, "p", tset_cb, &res);
        if (n < 0 || res.count != 1 ||
            res.data[0] != dl_intern_str_find(g_db, "b")) {
            printf("(n=%ld cnt=%ld) ", n, res.count);
            tset_free(&res);
            teardown();
            FAIL("p != {b} after the IDB-kind reload");
            return;
        }
        tset_free(&res);
    }
    teardown();
    PASS();
}

/* ─── T17: head-kind PROPAGATION must not record onto an AMBIGUOUS column ── */
/* review3 D2: v={foo} via add_fact is ambiguous (resolving) and deliberately
 * UNRECORDED; w={77} is a definite first-row int and records INT.  The rule
 * load "v(X):-w(X)." propagated w's INT onto v and PERSISTED it (rels.txt
 * v:1:edb:n) while v HOLDS foo — so a later LEGITIMATE "p(X):-v(foo)." was
 * rejected loud.  Fix: recordHeadKindsPub records only onto EMPTY relations
 * (mirror of the kindNoteRaw guard); IDB head rels are empty at load_rules,
 * so the legitimate IDB-kind persistence (T16) still works. */
static void test_propagation_skips_ambiguous(void)
{
    int rc;

    TEST("homog: load_rules does not record a propagated kind onto an ambiguous column");

    setup();
    assert(dl_declare_relation(g_db, "v", 1) == 0);
    assert(dl_declare_relation(g_db, "w", 1) == 0);
    {
        /* foo interns to a live id -> add_fact sees it as ambiguous:
         * accepted, never recorded */
        uint32_t foo = dl_intern_str(g_db, "foo");
        uint32_t v77 = 77; /* not a live sym id: definite first-row int */
        rc = dl_add_fact(g_db, "v", &foo, 1);
        if (rc != 1) {
            printf("(rc=%d) ", rc);
            teardown();
            FAIL("add_fact rejected the ambiguous symbol");
            return;
        }
        rc = dl_add_fact(g_db, "w", &v77, 1);
        if (rc != 1) {
            printf("(rc=%d) ", rc);
            teardown();
            FAIL("add_fact rejected the definite int");
            return;
        }
    }
    rc = dl_load_rules(g_db, "v(X):-w(X).\n");
    if (rc != 0) {
        teardown();
        FAIL("propagating rule failed to load");
        return;
    }
    /* the false record is the bug: rels.txt must carry NO kind for v */
    dl_close(g_db); /* writes rels.txt */
    {
        char path[256];
        char line[256];
        FILE *f;
        int bad = 0;
        snprintf(path, sizeof path, "build-tmp/homogdb/rels.txt");
        f = fopen(path, "r");
        if (!f) {
            system("rm -rf build-tmp/homogdb");
            g_db = NULL;
            FAIL("rels.txt missing");
            return;
        }
        while (fgets(line, sizeof line, f)) {
            if (strncmp(line, "v:1:edb:", 8) == 0)
                bad = 1; /* a kinds field was persisted for v */
        }
        fclose(f);
        if (bad) {
            system("rm -rf build-tmp/homogdb");
            g_db = NULL;
            FAIL("propagation recorded a kind onto an ambiguous column");
            return;
        }
    }
    g_db = NULL;
    PASS();

    TEST("homog: later legitimate rule over the ambiguous column is accepted");

    g_db = dl_open("build-tmp/homogdb");
    assert(g_db);
    /* legitimate rule over v's ambiguous column; w grounds the head */
    rc = dl_load_rules(g_db, "p(X):-v(foo),w(X).\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        dl_close(g_db);
        system("rm -rf build-tmp/homogdb");
        g_db = NULL;
        FAIL("legitimate rule over an ambiguous column was rejected");
        return;
    }
    {
        tuple_set res;
        long n;
        memset(&res, 0, sizeof res);
        /* publish first — the CLI flow does (dl_query prefers the snapshot) */
        if (dl_publish_snapshot(g_db) != 0) {
            teardown();
            FAIL("publish failed");
            return;
        }
        n = dl_query(g_db, "p", tset_cb, &res);
        /* p = w's row (77): v(foo) holds, and X comes from w */
        if (n < 0 || res.count != 1 || res.data[0] != 77) {
            printf("(n=%ld cnt=%ld) ", n, res.count);
            tset_free(&res);
            teardown();
            FAIL("p != {77} over the ambiguous column");
            return;
        }
        tset_free(&res);
    }
    teardown();
    PASS();
}

/* ─── main ─────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("test_homog — int/symbol column-kind homogeneity (S2)\n");

    test_repro_loud();
    test_symbol_control();
    test_int_control();
    test_fresh_permissive();
    test_magic_and_wfs();
    test_typed_path();
    test_multi_relation_int_and_sym();
    test_repro_loud_multi_relation();
    test_schema_clone_agreement();
    test_many_body_atoms();
    test_wfs_unrelated_symbol();
    test_mixed_column_loud();
    test_kinds_persist();
    test_split_load_loud();
    test_addfact_ambiguity_pins();
    test_idb_kind_persists();
    test_propagation_skips_ambiguous();

    printf("\n%d/%d tests passed\n", tests_run - tests_failed, tests_run);
    return tests_failed ? 1 : 0;
}
