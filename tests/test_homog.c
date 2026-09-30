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
#include "compiler.h"    /* compile_last_error (T23: the LSP error sink) */

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

/* S3b helpers: arity-2 variants of the two loaders above (comparison rules
 * need two columns to relate). */
static void load_rows_u32_2(const char *rel, uint8_t arity,
                            const uint32_t *cols, int nrows)
{
    char path[256];
    FILE *f;
    int i, c;

    assert(arity == 2);
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

static void load_rows_sym2(const char *rel, uint8_t arity,
                           const char **cells, int nrows)
{
    assert(arity == 2);
    load_rows_sym(rel, arity, cells, nrows);
}

/* S3c: arity-8 loader for the wide-rule fixtures (the kind tables are
 * per-rule var tables, so the ceiling probe needs many COLUMNS). */
static void load_rows_u32_8(const char *rel, const uint32_t *row, int nrows)
{
    char path[256];
    FILE *f;
    int i, c;

    assert(dl_declare_relation(g_db, rel, 8) == 0);
    snprintf(path, sizeof path, "build-tmp/homogdb/%s.csv", rel);
    f = fopen(path, "w");
    assert(f);
    for (i = 0; i < nrows; i++) {
        for (c = 0; c < 8; c++) {
            if (c) fputc(',', f);
            fprintf(f, "%u", row[c]);
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

/* ─── T18: S3a (R1) — the JOIN rule: cross-kind join is LOUD ───────────── */
/* MEASURED before S3a: e={1,2} int + f={a,b} sym, q(X):-e(X),f(X). -> rc=0
 * and q={a,b} from the ID COLLISION (a interns to 1, joins raw 1); the
 * standard answer is {} (no value is both an int and a symbol).  The
 * var-kind table makes the join LOUD, naming BOTH sites. */
static void test_join_cross_kind_loud(void)
{
    uint32_t e_rows[2];
    int rc;

    TEST("homog: q(X):-e(X),f(X) int-sym join is LOUD (dl_load_rules)");

    setup();
    e_rows[0] = 1; e_rows[1] = 2;
    load_rows_u32("e", 1, e_rows, 2);
    {
        const char *cells[2] = { "a", "b" };
        load_rows_sym("f", 1, cells, 2);
    }

    rc = dl_load_rules(g_db, "q(X):-e(X),f(X).\n");
    if (rc != -1) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("cross-kind join accepted by dl_load_rules");
        return;
    }
    teardown();
    PASS();

    TEST("homog: same join via dl_query_rules_ro is LOUD");
    setup();
    e_rows[0] = 1; e_rows[1] = 2;
    load_rows_u32("e", 1, e_rows, 2);
    {
        const char *cells[2] = { "a", "b" };
        load_rows_sym("f", 1, cells, 2);
    }
    {
        tuple_set res;
        long n;
        memset(&res, 0, sizeof res);
        n = dl_query_rules_ro(g_db, "q2(X):-e(X),f(X).\n", "q2", tset_cb, &res);
        tset_free(&res);
        if (n >= 0) {
            printf("(n=%ld) ", n);
            teardown();
            FAIL("cross-kind join answered via rules_ro");
            return;
        }
    }
    teardown();
    PASS();
}

/* ─── T19: J1 — the join against an IDB-PROPAGATED kind is LOUD ────────── */
/* q's int kind is derived by the head-kind fixpoint (q(X):-e(X)); the var
 * table sees it through the same cache, so p(X):-q(X),j(X) with j sym is
 * loud.  MEASURED before S3a: rc=0, p={a,b} (collision).  Also covers the
 * SPLIT-load merged path (dl.zig merged-set check): loading the two rules
 * as separate calls must reject on the SECOND call. */
static void test_join_idb_propagated_loud(void)
{
    uint32_t e_rows[2];
    int rc;

    TEST("homog: p(X):-q(X),j(X) with q derived-int, j sym is LOUD");

    setup();
    e_rows[0] = 1; e_rows[1] = 2;
    load_rows_u32("e", 1, e_rows, 2);
    {
        const char *cells[2] = { "a", "b" };
        load_rows_sym("j", 1, cells, 2);
    }

    rc = dl_load_rules(g_db, "q(X):-e(X).\np(X):-q(X),j(X).\n");
    if (rc != -1) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("IDB-propagated cross-kind join accepted");
        return;
    }
    teardown();
    PASS();

    TEST("homog: the same join split across two load_rules calls is LOUD");

    setup();
    e_rows[0] = 1; e_rows[1] = 2;
    load_rows_u32("e", 1, e_rows, 2);
    {
        const char *cells[2] = { "a", "b" };
        load_rows_sym("j", 1, cells, 2);
    }
    if (dl_load_rules(g_db, "q(X):-e(X).\n") != 0) {
        teardown();
        FAIL("defining rule failed to load");
        return;
    }
    rc = dl_load_rules(g_db, "p(X):-q(X),j(X).\n");
    if (rc != -1) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("split load evaded the join check (merged-set hole)");
        return;
    }
    teardown();
    PASS();
}

/* ─── T20: J3 — the NEGATED cross-kind join is LOUD ─────────────────────── */
/* A negated atom still probes the column: e={1,2} int, s={a} sym,
 * p(X):-e(X),!s(X). MEASURED before S3a: rc=0 and p={} (a interns to 1,
 * the negation eats raw 1); the standard answer is {1,2}. */
static void test_join_negated_cross_kind_loud(void)
{
    uint32_t e_rows[2];
    int rc;

    TEST("homog: p(X):-e(X),!s(X) int-sym negated join is LOUD");

    setup();
    e_rows[0] = 1; e_rows[1] = 2;
    load_rows_u32("e", 1, e_rows, 2);
    {
        const char *cells[1] = { "a" };
        load_rows_sym("s", 1, cells, 1);
    }

    rc = dl_load_rules(g_db, "p(X):-e(X),!s(X).\n");
    if (rc != -1) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("negated cross-kind join accepted");
        return;
    }
    teardown();
    PASS();
}

/* ─── T21: same-kind joins and the mixed-head control LOAD ──────────────── */
static void test_join_same_kind_controls(void)
{
    uint32_t e_rows[2];
    tuple_set res;
    long n;
    int rc;

    TEST("homog: int-int join control loads and answers {2}");

    setup();
    e_rows[0] = 1; e_rows[1] = 2;
    load_rows_u32("e", 1, e_rows, 2);
    {
        uint32_t g_rows[2]; g_rows[0] = 2; g_rows[1] = 3;
        load_rows_u32("g", 1, g_rows, 2);
    }
    rc = dl_load_rules(g_db, "q(X):-e(X),g(X).\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("int-int join control failed to load");
        return;
    }
    assert(dl_compile(g_db) == 0);
    memset(&res, 0, sizeof res);
    n = dl_query(g_db, "q", tset_cb, &res);
    if (n < 0 || res.count != 1 || res.data[0] != 2) {
        printf("(n=%ld cnt=%ld) ", n, res.count);
        tset_free(&res);
        teardown();
        FAIL("int-int join: q != {2}");
        return;
    }
    tset_free(&res);
    teardown();
    PASS();

    TEST("homog: sym-sym join control loads and answers {b}");

    setup();
    {
        const char *ec[2] = { "a", "b" };
        const char *fc[2] = { "b", "c" };
        load_rows_sym("e", 1, ec, 2);
        load_rows_sym("f", 1, fc, 2);
    }
    rc = dl_load_rules(g_db, "q(X):-e(X),f(X).\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("sym-sym join control failed to load");
        return;
    }
    assert(dl_compile(g_db) == 0);
    memset(&res, 0, sizeof res);
    n = dl_query(g_db, "q", tset_cb, &res);
    if (n < 0 || res.count != 1 ||
        res.data[0] != dl_intern_str_find(g_db, "b")) {
        printf("(n=%ld cnt=%ld) ", n, res.count);
        tset_free(&res);
        teardown();
        FAIL("sym-sym join: q != {b}");
        return;
    }
    tset_free(&res);
    teardown();
    PASS();

    TEST("homog: negated same-kind join control (J4) loads; p={2}");

    setup();
    e_rows[0] = 1; e_rows[1] = 2;
    load_rows_u32("e", 1, e_rows, 2);
    {
        uint32_t g_rows[1]; g_rows[0] = 1;
        load_rows_u32("g", 1, g_rows, 1);
    }
    rc = dl_load_rules(g_db, "p(X):-e(X),!g(X).\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("negated same-kind join control failed to load");
        return;
    }
    assert(dl_compile(g_db) == 0);
    memset(&res, 0, sizeof res);
    n = dl_query(g_db, "p", tset_cb, &res);
    if (n < 0 || res.count != 1 || res.data[0] != 2) {
        printf("(n=%ld cnt=%ld) ", n, res.count);
        tset_free(&res);
        teardown();
        FAIL("negated same-kind join: p != {2}");
        return;
    }
    tset_free(&res);
    teardown();
    PASS();

    TEST("homog: mixed head across rules (FP13) is LOUD (G2)");

    /* G2 (flipped from the S1 FP-defense control): a head column that is
     * INT in one rule and SYM in another is ITSELF the defect, not merely
     * a permissive input to a later check.  MEASURED: p(X):-e(X).
     * p(X):-f(X). with e=int{1,2} f=sym{a,b} makes p hold 2 ROWS where
     * the standard is 4 — the sym ids a=1,b=2 dedupe against the raw ints
     * 1,2 in the shared u32 value space (MEASURED pre-G2: both rules
     * rc=0, p=2 rows; with non-colliding ids e={100,200} p correctly
     * holds 4 — the bug is the id-collision dedup, the same root defect
     * as the S3a join).  Nothing but this self-imposed control pinned
     * mixed-head permissiveness: docs/language.html never mentions it
     * (rg 'mixed' = empty) and no other suite derives a mixed head. */
    setup();
    e_rows[0] = 1; e_rows[1] = 2;
    load_rows_u32("e", 1, e_rows, 2);
    {
        const char *cells[2] = { "a", "b" };
        load_rows_sym("f", 1, cells, 2);
    }
    rc = dl_load_rules(g_db, "p(X):-e(X).\np(X):-f(X).\n");
    if (rc != -1) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("mixed head across rules accepted (G2 hole)");
        return;
    }
    {
        const char *msg = compile_last_error(NULL);
        if (msg == NULL ||
            strstr(msg, "head column p/0") == NULL ||
            strstr(msg, "rule 1") == NULL ||
            strstr(msg, "rule 2") == NULL) {
            printf("(msg=%s) ", msg ? msg : "(null)");
            teardown();
            FAIL("mixed-head diagnostic does not name the head column and both rules");
            return;
        }
    }
    teardown();
    PASS();

    TEST("homog: fresh/unknown relation joined stays permissive");

    setup();
    e_rows[0] = 1; e_rows[1] = 2;
    load_rows_u32("e", 1, e_rows, 2);
    assert(dl_declare_relation(g_db, "g", 1) == 0); /* declared, EMPTY */
    rc = dl_load_rules(g_db, "q(X):-e(X),g(X).\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("join against a fresh/empty relation rejected");
        return;
    }
    teardown();
    PASS();
}

/* ─── T22: WFS path — the join check reaches every strategy path ────────── */
static void test_join_wfs_paths(void)
{
    uint32_t e_rows[2];
    tuple_set res;
    long n;

    TEST("homog: WFS rejects the cross-kind join (LOUD) and answers int-int");

    setup();
    e_rows[0] = 1; e_rows[1] = 2;
    load_rows_u32("e", 1, e_rows, 2);
    {
        const char *cells[2] = { "a", "b" };
        load_rows_sym("f", 1, cells, 2);
    }
    memset(&res, 0, sizeof res);
    n = dl_query_wfs_ro(g_db, "qw(X):-e(X),f(X).\n", "qw", 0, tset_cb, &res);
    tset_free(&res);
    if (n >= 0) {
        printf("(n=%ld) ", n);
        teardown();
        FAIL("WFS answered the cross-kind join");
        return;
    }

    /* int-int control through WFS: g={2,3} joined with e={1,2} -> {2} */
    {
        uint32_t g_rows[2]; g_rows[0] = 2; g_rows[1] = 3;
        load_rows_u32("g", 1, g_rows, 2);
    }
    memset(&res, 0, sizeof res);
    n = dl_query_wfs_ro(g_db, "qi(X):-e(X),g(X).\n", "qi", 0, tset_cb, &res);
    if (n < 0 || res.count != 1 || res.data[0] != 2) {
        printf("(n=%ld cnt=%ld) ", n, res.count);
        tset_free(&res);
        teardown();
        FAIL("WFS int-int join control: qi != {2}");
        return;
    }
    tset_free(&res);
    teardown();
    PASS();
}

/* ─── T23: the diagnostic names BOTH join sites ─────────────────────────── */
/* compile_last_error is the LSP error sink (compiler.h:143); the join
 * diagnostic must name the variable and both relations/columns so the
 * offending site is actionable. */
static void test_join_diagnostic_names_sites(void)
{
    uint32_t e_rows[2];

    TEST("homog: join diagnostic names the var and both rel/col sites");

    setup();
    e_rows[0] = 1; e_rows[1] = 2;
    load_rows_u32("e", 1, e_rows, 2);
    {
        const char *cells[2] = { "a", "b" };
        load_rows_sym("f", 1, cells, 2);
    }
    if (dl_load_rules(g_db, "q(X):-e(X),f(X).\n") != -1) {
        teardown();
        FAIL("cross-kind join accepted (diagnostic test)");
        return;
    }
    {
        const char *msg = compile_last_error(NULL);
        if (msg == NULL ||
            strstr(msg, "variable X joins") == NULL ||
            strstr(msg, "e/0") == NULL ||
            strstr(msg, "f/0") == NULL) {
            printf("(msg=%s) ", msg ? msg : "(null)");
            teardown();
            FAIL("join diagnostic does not name the var and both sites");
            return;
        }
    }
    teardown();
    PASS();
}

/* ─── T24: S3b R2 — ordered comparison over SYMBOL variables is LOUD ──── */
/* MEASURED before S3b: pair={mon/tue, mon/wed} sym, q(V1):-pair(V1,V2),
 * pair(V3,V4),V2>=V4. loaded rc=0 and answered 1 row — the engine compares
 * INTERN IDS, which have no order (the orchestrator's "is on or after" case
 * answers ZERO rows silently on other data).  R2 makes any SYM-kind operand
 * of < <= > >= loud.  An int-literal RHS vs a sym var (B4) and a sym var vs
 * a sym var (B3) are both covered; a symbol CONSTANT in an ordered
 * comparison is already a parse error (parser.zig:1297). */
static void test_cmp_ordered_sym_loud(void)
{
    int rc;

    TEST("homog: q(V1):-pair(V1,V2),pair(V3,V4),V2>=V4 (sym cols) is LOUD");

    setup();
    {
        const char *cells[4] = { "mon", "tue", "mon", "wed" };
        load_rows_sym2("pair", 2, cells, 2);
    }
    rc = dl_load_rules(g_db, "q(V1):-pair(V1,V2),pair(V3,V4),V2>=V4.\n");
    if (rc != -1) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("ordered comparison over symbol columns accepted");
        return;
    }
    teardown();
    PASS();

    TEST("homog: sym var vs int literal in ordered cmp (X>1) is LOUD");

    setup();
    {
        const char *cells[2] = { "a", "b" };
        load_rows_sym("f", 1, cells, 2);
    }
    rc = dl_load_rules(g_db, "q(X):-f(X),X>1.\n");
    if (rc != -1) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("ordered comparison sym-var vs int-literal accepted");
        return;
    }
    teardown();
    PASS();

    TEST("homog: sym var vs sym var ordered cmp (X<Y) is LOUD");

    setup();
    {
        const char *cells[4] = { "b", "a", "a", "c" };
        load_rows_sym2("pair", 2, cells, 2);
    }
    rc = dl_load_rules(g_db, "lt(X,Y):-pair(X,Y),X<Y.\n");
    if (rc != -1) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("sym-vs-sym ordered comparison accepted");
        return;
    }
    teardown();
    PASS();
}

/* ─── T25: ordered comparisons over INT columns keep loading ───────────── */
static void test_cmp_ordered_int_controls(void)
{
    tuple_set res;
    long n;
    int rc;

    TEST("homog: int var-var X<Y control loads and answers {(1,2)}");

    setup();
    {
        uint32_t all[4]; all[0] = 1; all[1] = 2; all[2] = 2; all[3] = 1;
        load_rows_u32_2("pair", 2, all, 2);
    }
    rc = dl_load_rules(g_db, "lt(X,Y):-pair(X,Y),X<Y.\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("int var-var ordered comparison failed to load");
        return;
    }
    assert(dl_compile(g_db) == 0);
    memset(&res, 0, sizeof res);
    n = dl_query(g_db, "lt", tset_cb, &res);
    if (n < 0 || res.count != 1 || res.data[0] != 1 || res.data[1] != 2) {
        printf("(n=%ld cnt=%ld) ", n, res.count);
        tset_free(&res);
        teardown();
        FAIL("int var-var X<Y: lt != {(1,2)}");
        return;
    }
    tset_free(&res);
    teardown();
    PASS();

    TEST("homog: int var vs int literal X>1 control loads and answers {2,3}");

    setup();
    {
        uint32_t vr[3]; vr[0] = 1; vr[1] = 2; vr[2] = 3;
        load_rows_u32("val", 1, vr, 3);
    }
    rc = dl_load_rules(g_db, "q(X):-val(X),X>1.\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("int X>1 control failed to load");
        return;
    }
    assert(dl_compile(g_db) == 0);
    memset(&res, 0, sizeof res);
    n = dl_query(g_db, "q", tset_cb, &res);
    if (n < 0 || res.count != 2) {
        printf("(n=%ld cnt=%ld) ", n, res.count);
        tset_free(&res);
        teardown();
        FAIL("int X>1: q != {2,3}");
        return;
    }
    tset_free(&res);
    teardown();
    PASS();
}

/* ─── T26: S3b R3 — equality across kinds is LOUD, same-kind loads ─────── */
/* MEASURED before S3b: q(X,Y):-e(X),f(Y),X=Y with e int / f sym loaded
 * rc=0 and answered 2 rows (a interns to id 1 and joins raw 1) where the
 * standard is {} — no value is both an int and a symbol. */
static void test_eq_cross_kind_loud(void)
{
    uint32_t e_rows[2];
    tuple_set res;
    long n;
    int rc;

    TEST("homog: q(X,Y):-e(X),f(Y),X=Y int-vs-sym is LOUD");

    setup();
    e_rows[0] = 1; e_rows[1] = 2;
    load_rows_u32("e", 1, e_rows, 2);
    {
        const char *cells[2] = { "a", "b" };
        load_rows_sym("f", 1, cells, 2);
    }
    rc = dl_load_rules(g_db, "q(X,Y):-e(X),f(Y),X=Y.\n");
    if (rc != -1) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("cross-kind equality accepted");
        return;
    }
    teardown();
    PASS();

    TEST("homog: X=Y same-kind int control loads; q has 2 rows");

    setup();
    {
        uint32_t all[4]; all[0] = 1; all[1] = 1; all[2] = 2; all[3] = 2;
        load_rows_u32_2("pair", 2, all, 2);
    }
    rc = dl_load_rules(g_db, "q(X,Y):-pair(X,Y),X=Y.\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("same-kind int equality failed to load");
        return;
    }
    assert(dl_compile(g_db) == 0);
    memset(&res, 0, sizeof res);
    n = dl_query(g_db, "q", tset_cb, &res);
    if (n < 0 || res.count != 2) {
        printf("(n=%ld cnt=%ld) ", n, res.count);
        tset_free(&res);
        teardown();
        FAIL("same-kind int equality: q != 2 rows");
        return;
    }
    tset_free(&res);
    teardown();
    PASS();

    TEST("homog: X=Y same-kind sym control loads (bind direction)");

    setup();
    {
        const char *cells[4] = { "a", "a", "b", "b" };
        load_rows_sym2("pair", 2, cells, 2);
    }
    rc = dl_load_rules(g_db, "q(Y):-pair(X,Y),X=Y.\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("same-kind sym equality failed to load");
        return;
    }
    teardown();
    PASS();
}

/* ─── T27: S3b R4 — arithmetic on symbol variables is LOUD ────────────── */
/* MEASURED before S3b: q(X):-f(X),X=1+1 with f sym loaded rc=0 and answered
 * {b} — a=1 + 1 = 2 collides with b's intern id.  The operand form
 * (X=Y+1 with Y sym) compared intern ids as numbers. */
static void test_arith_sym_loud(void)
{
    uint32_t e_rows[2];
    int rc;

    TEST("homog: q(X):-f(X),X=1+1 (sym result var) is LOUD");

    setup();
    {
        const char *cells[2] = { "a", "b" };
        load_rows_sym("f", 1, cells, 2);
    }
    rc = dl_load_rules(g_db, "q(X):-f(X),X=1+1.\n");
    if (rc != -1) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("arithmetic result into a symbol variable accepted");
        return;
    }
    teardown();
    PASS();

    TEST("homog: q(X):-e(X),f(Y),X=Y+1 (sym operand var) is LOUD");

    setup();
    e_rows[0] = 1; e_rows[1] = 2;
    load_rows_u32("e", 1, e_rows, 2);
    {
        const char *cells[2] = { "a", "b" };
        load_rows_sym("f", 1, cells, 2);
    }
    rc = dl_load_rules(g_db, "q(X):-e(X),f(Y),X=Y+1.\n");
    if (rc != -1) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("arithmetic on a symbol operand variable accepted");
        return;
    }
    teardown();
    PASS();

    TEST("homog: int arithmetic controls load (X=1+1 / S=A+B on int cols)");

    setup();
    e_rows[0] = 1; e_rows[1] = 2;
    load_rows_u32("e", 1, e_rows, 2);
    {
        uint32_t all[4]; all[0] = 1; all[1] = 2; all[2] = 2; all[3] = 1;
        load_rows_u32_2("pair", 2, all, 2);
    }
    rc = dl_load_rules(g_db, "q(X):-e(X),X=1+1.\ns(A,B,S):-pair(A,B),S=A+B.\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("int arithmetic control failed to load");
        return;
    }
    teardown();
    PASS();
}

/* ─── T28: != controls — the documented allows keep loading ───────────── */
/* X != foo on an INT column is DOCUMENTED-valid (language.html comparison
 * section: "!= additionally accepts a symbol constant on the right (it
 * interns the symbol and compares symbol ids)") and SUITE-PINNED
 * (test_m9_arith T8d, values high so ids never collide).  X != a on a SYM
 * column, X != 1 on an int column and sym X != Y same-kind are all correct
 * and must keep loading. */
static void test_neq_documented_controls(void)
{
    uint32_t v_rows[3];
    tuple_set res;
    long n;
    int rc;

    TEST("homog: r(X):-val(X),X!=foo int-col DOCUMENTED control loads");

    setup();
    v_rows[0] = 100; v_rows[1] = 200; v_rows[2] = 300;
    load_rows_u32("val", 1, v_rows, 3);
    rc = dl_load_rules(g_db, "r(X):-val(X),X!=foo.\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("X!=foo on an int column rejected (documented-valid, T8d)");
        return;
    }
    assert(dl_compile(g_db) == 0);
    memset(&res, 0, sizeof res);
    n = dl_query(g_db, "r", tset_cb, &res);
    if (n < 0 || res.count != 3) {
        printf("(n=%ld cnt=%ld) ", n, res.count);
        tset_free(&res);
        teardown();
        FAIL("X!=foo int-col control: r != {100,200,300}");
        return;
    }
    tset_free(&res);
    teardown();
    PASS();

    TEST("homog: q(X):-f(X),X!=a sym-col control loads; q={b}");

    setup();
    {
        const char *cells[2] = { "a", "b" };
        load_rows_sym("f", 1, cells, 2);
    }
    rc = dl_load_rules(g_db, "q(X):-f(X),X!=a.\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("X!=a on a sym column rejected");
        return;
    }
    assert(dl_compile(g_db) == 0);
    memset(&res, 0, sizeof res);
    n = dl_query(g_db, "q", tset_cb, &res);
    if (n < 0 || res.count != 1 ||
        res.data[0] != dl_intern_str_find(g_db, "b")) {
        printf("(n=%ld cnt=%ld) ", n, res.count);
        tset_free(&res);
        teardown();
        FAIL("X!=a sym-col control: q != {b}");
        return;
    }
    tset_free(&res);
    teardown();
    PASS();

    TEST("homog: q(X):-val(X),X!=1 int-col control loads; q={2}");

    setup();
    {
        uint32_t vr[2]; vr[0] = 1; vr[1] = 2;
        load_rows_u32("val", 1, vr, 2);
    }
    rc = dl_load_rules(g_db, "q(X):-val(X),X!=1.\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("X!=1 on an int column rejected");
        return;
    }
    assert(dl_compile(g_db) == 0);
    memset(&res, 0, sizeof res);
    n = dl_query(g_db, "q", tset_cb, &res);
    if (n < 0 || res.count != 1 || res.data[0] != 2) {
        printf("(n=%ld cnt=%ld) ", n, res.count);
        tset_free(&res);
        teardown();
        FAIL("X!=1 int-col control: q != {2}");
        return;
    }
    tset_free(&res);
    teardown();
    PASS();

    TEST("homog: q(X):-e(X),e(Y),X!=Y sym same-kind var-var control loads");

    setup();
    {
        const char *cells[2] = { "a", "b" };
        load_rows_sym("e", 1, cells, 2);
    }
    rc = dl_load_rules(g_db, "q(X):-e(X),e(Y),X!=Y.\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("sym X!=Y same-kind rejected");
        return;
    }
    teardown();
    PASS();
}

/* ─── T29: S3b R5 — != var-var across kinds is LOUD; unknown stays soft ── */
/* MEASURED before S3b: r(X,Y):-e(X),f(Y),X!=Y with e int / f sym loaded
 * rc=0 and answered 2 of the 4 standard rows (a=id1 makes 1!=a false).  The
 * S3a join rule does NOT catch it (X and Y are different vars, each with a
 * single kind-consistent site).  Aggregate- and string-produced vars have no
 * relational kind (UNKNOWN) and must stay permissive (FP7/FP8). */
static void test_neq_cross_kind_loud(void)
{
    uint32_t e_rows[2];
    int rc;

    TEST("homog: r(X,Y):-e(X),f(Y),X!=Y int-vs-sym var-var is LOUD");

    setup();
    e_rows[0] = 1; e_rows[1] = 2;
    load_rows_u32("e", 1, e_rows, 2);
    {
        const char *cells[2] = { "a", "b" };
        load_rows_sym("f", 1, cells, 2);
    }
    rc = dl_load_rules(g_db, "r(X,Y):-e(X),f(Y),X!=Y.\n");
    if (rc != -1) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("cross-kind != var-var accepted");
        return;
    }
    teardown();
    PASS();

    TEST("homog: aggregate result N>=2 (FP7) and length N>2 (FP8) load");

    setup();
    {
        uint32_t er[4]; er[0] = 1; er[1] = 2; er[2] = 1; er[3] = 3;
        load_rows_u32_2("edge", 2, er, 2);
        const char *sc[3] = { "ab", "abc", "abcd" };
        load_rows_sym("str", 1, sc, 3);
    }
    rc = dl_load_rules(g_db,
        "cnt(X,N):-edge(X,Y),N=count().\n"
        "big(X):-cnt(X,N),N>=2.\n"
        "long(S):-str(S),N=length(S),N>2.\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("aggregate/str-produced var in an ordered cmp rejected (RK1)");
        return;
    }
    teardown();
    PASS();

    TEST("homog: fresh/unknown column in an ordered cmp stays permissive");

    setup();
    e_rows[0] = 1; e_rows[1] = 2;
    load_rows_u32("e", 1, e_rows, 2);
    assert(dl_declare_relation(g_db, "g", 1) == 0); /* declared, EMPTY */
    rc = dl_load_rules(g_db, "q(X):-e(X),g(X),X>1.\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("unknown-kind operand in an ordered cmp rejected");
        return;
    }
    teardown();
    PASS();
}

/* ─── T30: S3c — aggregate-RESULT head columns carry a kind ────────────── */
/* MEASURED before S3c: with e={1,2} int and f={a,b} sym (a=id1, b=id2),
 * 'c(N):-e(X),N=count(). q(N):-c(N),f(N).' loaded rc=0 and answered q={2}
 * — the count 2 collides with b's sym id; the standard is {}. The count
 * result is a raw u32 by construction, so the head column is INT and the
 * downstream join/eq must be loud. The GROUP-var case was already covered
 * (the group var occurs in a body atom) but untested — pinned here too. */
static void test_agg_result_head_loud(void)
{
    uint32_t e_rows[2];
    int rc;

    TEST("homog: agg-result head joined against sym rel is LOUD");

    setup();
    e_rows[0] = 1; e_rows[1] = 2;
    load_rows_u32("e", 1, e_rows, 2);
    {
        const char *cells[2] = { "a", "b" };
        load_rows_sym("f", 1, cells, 2);
    }
    rc = dl_load_rules(g_db,
        "c(N):-e(X),N=count().\n"
        "q(N):-c(N),f(N).\n");
    if (rc != -1) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("agg-result head join accepted");
        return;
    }
    PASS();

    TEST("homog: agg-result head through equality is LOUD");

    setup();
    e_rows[0] = 1; e_rows[1] = 2;
    load_rows_u32("e", 1, e_rows, 2);
    {
        const char *cells[2] = { "a", "b" };
        load_rows_sym("f", 1, cells, 2);
    }
    rc = dl_load_rules(g_db,
        "c(N):-e(X),N=count().\n"
        "q(N):-c(N),f(Y),N=Y.\n");
    if (rc != -1) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("agg-result head via equality accepted");
        return;
    }
    teardown();
    PASS();

    TEST("homog: agg GROUP var cross-kind is LOUD (covered since S3a)");

    setup();
    e_rows[0] = 1; e_rows[1] = 2;
    load_rows_u32("e", 1, e_rows, 2);
    {
        const char *cells[2] = { "a", "b" };
        load_rows_sym("f", 1, cells, 2);
    }
    rc = dl_load_rules(g_db,
        "cnt(K,N):-f(K),N=count().\n"
        "q(K):-cnt(K,N),e(K).\n");
    if (rc != -1) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("agg GROUP-var cross-kind accepted");
        return;
    }
    teardown();
    PASS();

    TEST("homog: count result in an ordered cmp still loads (FP7)");

    setup();
    e_rows[0] = 1; e_rows[1] = 2;
    load_rows_u32("e", 1, e_rows, 2);
    rc = dl_load_rules(g_db,
        "c(N):-e(X),N=count().\n"
        "big(N):-c(N),N>=1.\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("count result in an ordered cmp rejected");
        return;
    }
    teardown();
    PASS();

    TEST("homog: min result takes the SOURCE kind (sym stays sym)");

    setup();
    {
        const char *cells[2] = { "b", "a" };
        load_rows_sym("f", 1, cells, 2);
    }
    /* min over a sym column produces a sym id: joining it back into the sym
     * relation must stay VALID ... */
    rc = dl_load_rules(g_db,
        "m(N):-f(X),N=min(X).\n"
        "q(N):-m(N),f(N).\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("min(sym) result joined back into sym rejected");
        return;
    }
    PASS();

    /* ... while an ordered comparison over it is loud exactly like any sym
     * var (R2). */
    TEST("homog: min(sym) result in an ordered cmp is LOUD (R2)");

    rc = dl_load_rules(g_db,
        "m(N):-f(X),N=min(X).\n"
        "q(N):-m(N),N>0.\n");
    if (rc != -1) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("min(sym) result in an ordered cmp accepted");
        return;
    }
    teardown();
    PASS();

    TEST("homog: min(int) result joined back into int loads");

    setup();
    e_rows[0] = 2; e_rows[1] = 1;
    load_rows_u32("e", 1, e_rows, 2);
    rc = dl_load_rules(g_db,
        "m(N):-e(X),N=min(X).\n"
        "q(N):-m(N),e(N).\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("min(int) result joined into int rejected");
        return;
    }
    teardown();
    PASS();
}

/* ─── T31: S3c — arithmetic-RESULT head columns carry a kind ───────────── */
/* MEASURED before S3c: 's(N):-e(X),N=X+1. q(N):-s(N),f(N).' loaded rc=0 and
 * answered q={2} (s={2,3}, 2 collides with b's id); standard {}. An arith
 * result is a raw u32 by construction, so the head column is INT. */
static void test_arith_result_head_loud(void)
{
    uint32_t e_rows[2];
    int rc;

    TEST("homog: arith-result head joined against sym rel is LOUD");

    setup();
    e_rows[0] = 1; e_rows[1] = 2;
    load_rows_u32("e", 1, e_rows, 2);
    {
        const char *cells[2] = { "a", "b" };
        load_rows_sym("f", 1, cells, 2);
    }
    rc = dl_load_rules(g_db,
        "s(N):-e(X),N=X+1.\n"
        "q(N):-s(N),f(N).\n");
    if (rc != -1) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("arith-result head join accepted");
        return;
    }
    PASS();

    TEST("homog: arith-result head through equality is LOUD");

    setup();
    e_rows[0] = 1; e_rows[1] = 2;
    load_rows_u32("e", 1, e_rows, 2);
    {
        const char *cells[2] = { "a", "b" };
        load_rows_sym("f", 1, cells, 2);
    }
    rc = dl_load_rules(g_db,
        "s(N):-e(X),N=X+1.\n"
        "q(N):-s(N),f(Y),N=Y.\n");
    if (rc != -1) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("arith-result head via equality accepted");
        return;
    }
    teardown();
    PASS();

    TEST("homog: same-rule arith result joined with a sym var is LOUD");

    setup();
    e_rows[0] = 1; e_rows[1] = 2;
    load_rows_u32("e", 1, e_rows, 2);
    {
        const char *cells[2] = { "a", "b" };
        load_rows_sym("f", 1, cells, 2);
    }
    rc = dl_load_rules(g_db,
        "q(N):-e(X),N=X+1,f(Y),N=Y.\n");
    if (rc != -1) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("same-rule arith-result/sym equality accepted");
        return;
    }
    teardown();
    PASS();

    TEST("homog: arith result into an int head/relation still loads");

    setup();
    e_rows[0] = 1; e_rows[1] = 2;
    load_rows_u32("e", 1, e_rows, 2);
    rc = dl_load_rules(g_db,
        "s(N):-e(X),N=X+1.\n"
        "q(N):-s(N),e(N).\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("arith-result head joined into int rejected");
        return;
    }
    teardown();
    PASS();
}

/* ─── T32: S3c controls — range/list-produced vars in comparisons ──────── */
/* RK1 family: vars produced by range()/list builtins have no relational
 * kind source, so comparisons over them stay permissive (MEASURED: both
 * load). One control line each to pin the family against future
 * over-tightening. */
static void test_range_list_cmp_controls(void)
{
    uint32_t e_rows[2];
    int rc;

    TEST("homog: range-produced var in an ordered cmp loads");

    setup();
    e_rows[0] = 1; e_rows[1] = 2;
    load_rows_u32("e", 1, e_rows, 2);
    rc = dl_load_rules(g_db, "q(X):-range(X,e,1,2),X>0.\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("range-produced var in cmp rejected");
        return;
    }
    teardown();
    PASS();

    TEST("homog: list-produced var (car) into length/cmp loads");

    setup();
    {
        const char *cells[1] = { "[ab]" };
        load_rows_sym("lst", 1, cells, 1);
    }
    rc = dl_load_rules(g_db, "q(N):-lst(L),X=car(L),N=length(X),N>0.\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("list-produced var into cmp rejected");
        return;
    }
    teardown();
    PASS();
}

/* ─── T33: S3c — no var-count ceiling: a wide rule is still fully checked */
/* MEASURED before S3c: the per-rule kind tables held 32 vars and silently
 * `continue`d past the 33rd distinct var, which dropped every LATER
 * occurrence — a 4-relation arity-8 program with 32 filler vars plus the
 * cross-kind join e(X),f(X) loaded rc=0 (evading the identical check that
 * rejects it at 24 fillers). The tables are now MAX_VARS(64)-sized, matching
 * compile_one's loud 64-distinct-var rejection, so a wide rule cannot
 * silently escape the check. */
static void test_wide_rule_still_checked(void)
{
    static uint32_t wrow[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };
    uint32_t e_rows[2];
    char prog[1024], atom[64];
    const char *pc = "ABCDEFGH";
    int r, rc;

    TEST("homog: 33-distinct-var cross-kind join is LOUD (no ceiling)");

    setup();
    for (r = 0; r < 4; r++) {
        char name[8];
        snprintf(name, sizeof name, "w%d", r);
        load_rows_u32_8(name, wrow, 1);
    }
    e_rows[0] = 1; e_rows[1] = 2;
    load_rows_u32("e", 1, e_rows, 2);
    {
        const char *cells[2] = { "a", "b" };
        load_rows_sym("f", 1, cells, 2);
    }
    strcpy(prog, "q(X):-");
    for (r = 0; r < 4; r++) {
        snprintf(atom, sizeof atom,
            "w%d(%c%d,%c%d,%c%d,%c%d,%c%d,%c%d,%c%d,%c%d),",
            r, pc[0], r, pc[1], r, pc[2], r, pc[3], r,
            pc[4], r, pc[5], r, pc[6], r, pc[7], r);
        strcat(prog, atom);
    }
    strcat(prog, "e(X),f(X).");
    rc = dl_load_rules(g_db, prog);
    if (rc != -1) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("33-var cross-kind join accepted (kind check truncated)");
        return;
    }
    teardown();
    PASS();

    TEST("homog: 33-distinct-var all-INT rule still loads");

    setup();
    for (r = 0; r < 4; r++) {
        char name[8];
        snprintf(name, sizeof name, "w%d", r);
        load_rows_u32_8(name, wrow, 1);
    }
    e_rows[0] = 1; e_rows[1] = 2;
    load_rows_u32("e", 1, e_rows, 2);
    strcpy(prog, "q(X):-");
    for (r = 0; r < 4; r++) {
        snprintf(atom, sizeof atom,
            "w%d(%c%d,%c%d,%c%d,%c%d,%c%d,%c%d,%c%d,%c%d),",
            r, pc[0], r, pc[1], r, pc[2], r, pc[3], r,
            pc[4], r, pc[5], r, pc[6], r, pc[7], r);
        strcat(prog, atom);
    }
    strcat(prog, "e(X).");
    rc = dl_load_rules(g_db, prog);
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("33-var all-INT rule rejected");
        return;
    }
    teardown();
    PASS();
}

/* ─── T34: G3 — concat result in an ordered comparison is LOUD ──────────── */
/* MEASURED before G3: s={a,b} sym (a=id1,b=id2), q(N):-s(S),N=concat(S,S),N>2.
 * concat yields an INTERNED STRING id; the ordered cmp N>2 compares intern ids
 * and answered 2 rows (aa=id3>2, bb=id4>2) — meaningless, standard {}.  G3
 * kinds the concat RESULT as SYM, so R2 rejects the ordered cmp.  Kinds ONLY
 * the result: the input S keeps its relational SYM kind, never re-kinded. */
static void test_g3_concat_result_ordered_loud(void)
{
    int rc;

    TEST("homog: concat result in an ordered cmp (N>2) is LOUD");

    setup();
    {
        const char *cells[2] = { "a", "b" };
        load_rows_sym("s", 1, cells, 2);
    }
    rc = dl_load_rules(g_db, "q(N):-s(S),N=concat(S,S),N>2.\n");
    if (rc != -1) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("concat result in ordered cmp accepted");
        return;
    }
    {
        const char *msg = compile_last_error(NULL);
        if (msg == NULL ||
            strstr(msg, "ordered comparison") == NULL ||
            strstr(msg, "symbol variable N") == NULL) {
            printf("(msg=%s) ", msg ? msg : "(null)");
            teardown();
            FAIL("concat-ordered diagnostic does not name the symbol var");
            return;
        }
    }
    teardown();
    PASS();

    TEST("homog: concat result joined against an int rel is LOUD (G3b-shape)");
    /* concat RESULT is SYM; joining it against an int relation is the cross-
     * kind join the S3a check already catches — G3 feeding SYM makes it fire. */

    setup();
    {
        const char *cells[1] = { "x" };
        load_rows_sym("s", 1, cells, 1);
    }
    {
        uint32_t e_rows[2] = { 1, 2 };
        load_rows_u32("f", 1, e_rows, 2);
    }
    rc = dl_load_rules(g_db, "q(N):-s(S),N=concat(S,S),f(N).\n");
    if (rc != -1) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("concat result joined against int rel accepted");
        return;
    }
    teardown();
    PASS();
}

/* ─── T35: G3 — length result joined against a sym rel is LOUD ──────────── */
/* MEASURED before G3: f={a,b} interned FIRST (a=id1,b=id2), then s={x} (len
 * 1); q(N):-s(S),N=length(S),f(N) loaded rc=0 and answered 1 row (N=1 joins
 * a=id1) — silent collision, standard {}.  G3 kinds the length RESULT as INT,
 * so the join INT(length) vs SYM(f) is loud.  Control: length result in an
 * ordered cmp (N>0) stays LOUD-free (N is INT — fine). */
static void test_g3_length_result_join_loud(void)
{
    int rc;

    TEST("homog: length result joined against a sym rel is LOUD");

    setup();
    {
        const char *cells[2] = { "a", "b" };
        load_rows_sym("f", 1, cells, 2);
    }
    {
        const char *cells[1] = { "x" };
        load_rows_sym("s", 1, cells, 1);
    }
    rc = dl_load_rules(g_db, "q(N):-s(S),N=length(S),f(N).\n");
    if (rc != -1) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("length result joined against sym rel accepted");
        return;
    }
    {
        const char *msg = compile_last_error(NULL);
        if (msg == NULL || strstr(msg, "joins integer and symbol columns") == NULL) {
            printf("(msg=%s) ", msg ? msg : "(null)");
            teardown();
            FAIL("length-join diagnostic does not name the join");
            return;
        }
    }
    teardown();
    PASS();

    TEST("homog: length result in an ordered cmp still loads (T3 control)");

    setup();
    {
        const char *cells[2] = { "ab", "cdef" };
        load_rows_sym("s", 1, cells, 2);
    }
    rc = dl_load_rules(g_db, "long(S):-s(S),N=length(S),N>2.\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("length result in ordered cmp rejected (T3 control)");
        return;
    }
    teardown();
    PASS();

    TEST("homog: concat->length->ordered cmp still loads (T6e control)");
    /* concat RESULT Y is SYM, fed as INPUT to length (str_operand_ok allows a
     * var); length RESULT N is INT, N<4 is an int cmp.  G3 kinds ONLY results,
     * never the input operand — Y is never compared. */

    setup();
    {
        const char *cells[1] = { "abc" };
        load_rows_sym("p", 1, cells, 1);
    }
    rc = dl_load_rules(g_db, "r(Y):-p(X),Y=concat(X,\"b\"),N=length(Y),N<4.\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("concat->length->ordered cmp rejected (T6e control)");
        return;
    }
    teardown();
    PASS();
}

/* ─── T36: G3 — list result in an ordered comparison is LOUD ────────────── */
/* MEASURED before G3: q(T):-p(X),L=cons(X,[7]),T=cdr(L),T>0 answered 1 row —
 * cdr returns a TERM HANDLE (>=0x80000000); the ordered cmp T>0 compares
 * handles, which is meaningless.  G3 kinds car/cdr/cons/append RESULTs as
 * KIND_LIST; R2 rejects an ordered cmp over a list var (handles have no
 * order).  Joining a list result against int/sym stays permissive (a handle
 * never collides with a small id — safe by construction). */
static void test_g3_list_result_ordered_loud(void)
{
    int rc;

    TEST("homog: cdr result in an ordered cmp (T>0) is LOUD");

    setup();
    {
        const char *cells[1] = { "[7]" };
        load_rows_sym("p", 1, cells, 1);
    }
    rc = dl_load_rules(g_db, "q(T):-p(X),L=cons(X,[7]),T=cdr(L),T>0.\n");
    if (rc != -1) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("cdr result in ordered cmp accepted");
        return;
    }
    {
        const char *msg = compile_last_error(NULL);
        if (msg == NULL ||
            strstr(msg, "ordered comparison") == NULL ||
            strstr(msg, "list variable T") == NULL) {
            printf("(msg=%s) ", msg ? msg : "(null)");
            teardown();
            FAIL("cdr-ordered diagnostic does not name the list var");
            return;
        }
    }
    teardown();
    PASS();

    TEST("homog: list result joined against an int rel still loads (safe join)");
    /* a term handle never equals a small raw int / low sym id, so joining a
     * list result against an int column is permissive — the join check stays
     * INT-vs-SYM only (RK-G3-LIST). */

    setup();
    {
        const char *cells[1] = { "[7]" };
        load_rows_sym("p", 1, cells, 1);
    }
    {
        uint32_t e_rows[2] = { 1, 2 };
        load_rows_u32("e", 1, e_rows, 2);
    }
    rc = dl_load_rules(g_db, "q(T):-p(X),L=cons(X,[7]),T=cdr(L),e(T).\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("list result joined against int rel rejected");
        return;
    }
    teardown();
    PASS();
}

/* ─── T37: G3 — range result kind is data-dependent; controls load ─────── */
/* range(X, Rel, Lo, Hi): X's kind = the named relation's leading column.
 * Over an int rel -> INT (X>0 fine); over a sym rel -> SYM (X>0 LOUD); over a
 * fresh/unknown rel -> UNKNOWN (permissive, T32 control). */
static void test_g3_range_result_kind(void)
{
    uint32_t e_rows[2];
    int rc;

    TEST("homog: range over an int rel in an ordered cmp still loads");

    setup();
    e_rows[0] = 1; e_rows[1] = 2;
    load_rows_u32("e", 1, e_rows, 2);
    rc = dl_load_rules(g_db, "q(X):-range(X,e,1,2),X>0.\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("range over int rel in ordered cmp rejected");
        return;
    }
    teardown();
    PASS();

    TEST("homog: range over a sym rel in an ordered cmp is LOUD");

    setup();
    {
        const char *cells[2] = { "a", "b" };
        load_rows_sym("g", 1, cells, 2);
    }
    rc = dl_load_rules(g_db, "q(X):-range(X,g,1,3),X>0.\n");
    if (rc != -1) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("range over sym rel in ordered cmp accepted");
        return;
    }
    teardown();
    PASS();

    TEST("homog: range over a fresh rel stays permissive (UNKNOWN)");

    setup();
    rc = dl_load_rules(g_db, "q(X):-range(X,freshrel,1,3),X>0.\n");
    /* range over an UNKNOWN relation is already a loud compile error
     * (range_builtin_valid rejects an unknown Rel) — pin that it is not
     * silently permissive on the cmp, and not a G3 false-reject. */
    if (rc != -1) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("range over unknown rel not rejected");
        return;
    }
    teardown();
    PASS();
}

/* ─── T38: G1 — populate-after-load recheck (the silent-wrong repro) ────── */
/* MEASURED before G1: declare e/1 and f/1 EMPTY; dl_load_rules("q(X):-e(X),
 * f(X).") rc=0 (both columns UNKNOWN -> permissive); then load int e={1,2}
 * (records INT on e — recheck passes, f still UNKNOWN); then load sym
 * f={a,b} (records SYM on f — recheck now sees e INT joined to f SYM via X
 * and must FAIL THE LOAD loudly).  Pre-fix: the second load returned rc=2
 * (success) and a later query answered 2 rows where the standard is {} — the
 * kind was never re-checked after compile.  G1 rechecks the resident AST at
 * the fact-load that newly establishes a kind.  Also pinned via dl_add_fact. */
static void test_g1_populate_after_load_loud(void)
{
    uint32_t e_rows[2];
    int rc;

    TEST("homog: populate-after-load: 2nd fact-load fails LOUD (CSV)");

    setup();
    assert(dl_declare_relation(g_db, "e", 1) == 0);
    assert(dl_declare_relation(g_db, "f", 1) == 0);
    rc = dl_load_rules(g_db, "q(X):-e(X),f(X).\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("rules over empty cols failed to load (G1 precondition)");
        return;
    }
    /* first load: e=int{1,2}.  e newly records INT; recheck sees e INT, f
     * UNKNOWN -> no conflict -> load succeeds. */
    e_rows[0] = 1; e_rows[1] = 2;
    load_rows_u32("e", 1, e_rows, 2);
    /* second load: f=sym{a,b}.  f newly records SYM; recheck sees e INT
     * joined to f SYM via X -> LOUD.  The LOAD fails, not a later query. */
    {
        char path[256];
        FILE *f;
        assert(dl_declare_relation(g_db, "f", 1) == 0); /* idempotent */
        (void)dl_intern_str(g_db, "a"); /* pin sym ids */
        snprintf(path, sizeof path, "build-tmp/homogdb/f.csv");
        f = fopen(path, "w");
        assert(f);
        fprintf(f, "a\nb\n");
        fclose(f);
        rc = dl_load_facts(g_db, "f", path);
        if (rc != -1) {
            printf("(rc=%d) ", rc);
            teardown();
            FAIL("2nd fact-load (sym f after int e) accepted (G1 hole)");
            return;
        }
    }
    {
        const char *msg = compile_last_error(NULL);
        if (msg == NULL ||
            strstr(msg, "joins integer and symbol columns") == NULL) {
            printf("(msg=%s) ", msg ? msg : "(null)");
            teardown();
            FAIL("populate-after-load diagnostic does not name the join");
            return;
        }
    }
    teardown();
    PASS();

    TEST("homog: populate-after-load: add_fact fails LOUD (const-vs-col)");

    /* The raw u32 API records INT only (kindNoteRaw); a sym-establishing
     * add_fact is ambiguous and does not record.  So the add_fact loud path
     * is a CONSTANT-vs-column conflict: rules with a sym constant against e
     * load rc=0 while e is UNKNOWN, then an int add_fact newly records INT
     * on e and the recheck sees sym-constant 'foo' vs e INT -> LOUD.  This
     * is the S1 repro shape deferred to the fact-load. */
    setup();
    assert(dl_declare_relation(g_db, "e", 1) == 0);
    rc = dl_load_rules(g_db, "q(X):-e(X).\np(X):-e(X),!q(foo).\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("rules over empty e failed to load (add_fact precondition)");
        return;
    }
    {
        uint32_t v = 77; /* definite int (not a live sym id) on e's first row */
        rc = dl_add_fact(g_db, "e", &v, 1);
        if (rc != -1) {
            printf("(rc=%d) ", rc);
            teardown();
            FAIL("int add_fact after a sym-constant rule accepted (G1 hole)");
            return;
        }
    }
    {
        const char *msg = compile_last_error(NULL);
        if (msg == NULL ||
            strstr(msg, "symbol but column holds integers") == NULL) {
            printf("(msg=%s) ", msg ? msg : "(null)");
            teardown();
            FAIL("add_fact G1 diagnostic does not name the const-vs-col conflict");
            return;
        }
    }
    teardown();
    PASS();
}

/* ─── T39: G1 — rollback restores col_kind on a failed load ────────────── */
/* RK-G1-rollback (MUST-MEASURE): the fact-load path is NOT atomic by
 * ordering.  A mid-load recheck failure must RESTORE col_kind to its pre-
 * load state so a failed load leaves no partial kind.  Probe: the 2nd
 * (failing) load records SYM on f then the recheck rejects; f's col_kind
 * must be back to UNKNOWN (0).  Verified by RETRYING f with an int load —
 * if rollback worked, f is UNKNOWN and int records INT (success); if
 * rollback failed, f is stuck at SYM and an int load hits the existing-
 * kind conflict (mixed -> loud).  Also pins that the FIRST CSV row
 * establishes the kind and a LATER relation (f) triggers the failure. */
static void test_g1_rollback_restores_kind(void)
{
    uint32_t rows[2];
    int rc;

    TEST("homog: G1 rollback restores col_kind (retry int after failed sym)");

    setup();
    assert(dl_declare_relation(g_db, "e", 1) == 0);
    assert(dl_declare_relation(g_db, "f", 1) == 0);
    assert(dl_load_rules(g_db, "q(X):-e(X),f(X).\n") == 0);
    rows[0] = 1; rows[1] = 2;
    load_rows_u32("e", 1, rows, 2);
    /* failing load: f=sym{a,b} records SYM, recheck rejects, rollback clears
     * f's col_kind back to UNKNOWN. */
    {
        char path[256];
        FILE *ff;
        (void)dl_intern_str(g_db, "a");
        snprintf(path, sizeof path, "build-tmp/homogdb/f.csv");
        ff = fopen(path, "w");
        assert(ff);
        fprintf(ff, "a\nb\n");
        fclose(ff);
        rc = dl_load_facts(g_db, "f", path);
        if (rc != -1) {
            printf("(rc=%d) ", rc);
            teardown();
            FAIL("sym f load after int e not rejected (rollback precondition)");
            return;
        }
    }
    /* rollback proof: f's col_kind was restored to UNKNOWN, so an int load
     * now records INT (success).  Had rollback failed, f would be stuck at
     * SYM and this int load would hit a mixed-kind conflict (loud). */
    rows[0] = 5; rows[1] = 6;
    {
        char path[256];
        FILE *ff;
        assert(dl_declare_relation(g_db, "f", 1) == 0);
        snprintf(path, sizeof path, "build-tmp/homogdb/f.csv");
        ff = fopen(path, "w");
        assert(ff);
        fprintf(ff, "5\n6\n");
        fclose(ff);
        rc = dl_load_facts(g_db, "f", path);
        if (rc != 2) {
            printf("(rc=%d) ", rc);
            teardown();
            FAIL("int f load after failed sym f rejected (rollback FAILED)");
            return;
        }
    }
    teardown();
    PASS();

    TEST("homog: G1 rollback — 1st row establishes kind, later row in SAME csv");

    /* Within ONE CSV: g/2, rows "1,a" then "2,b".  col0->INT (row1), col1->
     * SYM (row1).  Rule q(X):-g(X,Y),X=Y joins X across g/0 (INT) and =Y
     * (g/1 SYM) -> equality cross-kind LOUD.  The recheck fires at the end
     * of the g load; rollback must restore BOTH col0 and col1 to UNKNOWN.
     * Proven by retrying g with two int columns (loads). */
    setup();
    assert(dl_declare_relation(g_db, "g", 2) == 0);
    assert(dl_load_rules(g_db, "q(X):-g(X,Y),X=Y.\n") == 0);
    {
        char path[256];
        FILE *ff;
        (void)dl_intern_str(g_db, "a");
        snprintf(path, sizeof path, "build-tmp/homogdb/g.csv");
        ff = fopen(path, "w");
        assert(ff);
        fprintf(ff, "1,a\n2,b\n");
        fclose(ff);
        rc = dl_load_facts(g_db, "g", path);
        if (rc != -1) {
            printf("(rc=%d) ", rc);
            teardown();
            FAIL("mixed 2-col g load after rules accepted (rollback precondition)");
            return;
        }
    }
    /* rollback proof: g's col_kind[0] and [1] restored to UNKNOWN, so a
     * two-int-column load now succeeds. */
    {
        char path[256];
        FILE *ff;
        snprintf(path, sizeof path, "build-tmp/homogdb/g.csv");
        ff = fopen(path, "w");
        assert(ff);
        fprintf(ff, "5,6\n7,8\n");
        fclose(ff);
        rc = dl_load_facts(g_db, "g", path);
        if (rc < 0) {
            printf("(rc=%d) ", rc);
            teardown();
            FAIL("int g load after failed mixed g rejected (rollback FAILED)");
            return;
        }
    }
    teardown();
    PASS();
}

/* ─── T40: G1 — false-positive controls (the plan's FP list) ───────────── */
static void test_g1_controls(void)
{
    uint32_t rows[2];
    tuple_set res;
    long n;
    int rc;

    TEST("homog: G1 control — relation filled over SEVERAL loads (same kind)");

    /* rules once, int facts in TWO loads: the 2nd is idempotent-kind (no
     * 0->definite transition) -> recheck does NOT fire -> both load.  The
     * 2nd load unions into the pre-existing base, so it returns the TOTAL
     * row count (not nrows); we only require it succeeds. */
    setup();
    assert(dl_declare_relation(g_db, "e", 1) == 0);
    assert(dl_declare_relation(g_db, "f", 1) == 0);
    assert(dl_load_rules(g_db, "q(X):-e(X),f(X).\n") == 0);
    rows[0] = 1; rows[1] = 2;
    load_rows_u32("e", 1, rows, 2); /* 1st load: e 0->INT, recheck passes */
    {
        char path[256];
        FILE *ff;
        rows[0] = 3; rows[1] = 4;
        assert(dl_declare_relation(g_db, "e", 1) == 0); /* idempotent */
        snprintf(path, sizeof path, "build-tmp/homogdb/e.csv");
        ff = fopen(path, "w");
        assert(ff);
        fprintf(ff, "3\n4\n");
        fclose(ff);
        /* 2nd load: e already INT (no 0->definite) -> no recheck -> success */
        rc = dl_load_facts(g_db, "e", path);
        if (rc < 0) {
            printf("(rc=%d) ", rc);
            teardown();
            FAIL("2nd int load into e rejected (idempotent-kind false reject)");
            return;
        }
    }
    rows[0] = 1; rows[1] = 2;
    load_rows_u32("f", 1, rows, 2); /* f INT; recheck sees e INT, f INT -> ok */
    teardown();
    PASS();

    TEST("homog: G1 control — facts BEFORE rules (dlb order) loads + queries");

    /* facts first (ast_rules empty -> recheck skipped), then rules (compile
     * check passes: both INT).  Query q -> 2 rows. */
    setup();
    rows[0] = 1; rows[1] = 2;
    load_rows_u32("e", 1, rows, 2);
    rows[0] = 1; rows[1] = 2;
    load_rows_u32("f", 1, rows, 2);
    rc = dl_load_rules(g_db, "q(X):-e(X),f(X).\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("facts-first then rules rejected (dlb order broken)");
        return;
    }
    assert(dl_compile(g_db) == 0);
    memset(&res, 0, sizeof res);
    n = dl_query(g_db, "q", tset_cb, &res);
    if (n < 0 || res.count != 2) {
        printf("(n=%ld cnt=%ld) ", n, res.count);
        tset_free(&res);
        teardown();
        FAIL("facts-first control: expected q=2 rows");
        return;
    }
    tset_free(&res);
    teardown();
    PASS();

    TEST("homog: G1 control — INT col after rules, NO conflicting rule, loads");

    /* an INT column established AFTER the rules with an innocent rule still
     * loads: e INT after rules, rule uses e alone -> recheck passes. */
    setup();
    assert(dl_declare_relation(g_db, "e", 1) == 0);
    assert(dl_load_rules(g_db, "q(X):-e(X).\n") == 0);
    rows[0] = 1; rows[1] = 2;
    load_rows_u32("e", 1, rows, 2); /* newly records INT; recheck sees e INT,
                                    * no conflicting site -> ok */
    teardown();
    PASS();
}

/* ─── T41: G1 — flag precision (0->definite only; raw-API sym stays permissive) */
/* RK-G1-flag-precision: only a 0->definite transition counts.  kindNoteRaw
 * catches INT-establishing-after-rules (a definite int on a column's first
 * row, when rules are resident).  A sym-establishing add_fact is AMBIGUOUS
 * to the raw u32 API (a resolving value may be a raw int below next_id), so
 * it neither records nor rejects — it stays permissive, the SAME floor as
 * kindNoteRaw always had.  This pins both transitions. */
static void test_g1_flag_precision(void)
{
    uint32_t v;
    int rc;

    TEST("homog: G1 flag — definite int after rules via add_fact rechecks");

    setup();
    assert(dl_declare_relation(g_db, "e", 1) == 0);
    assert(dl_declare_relation(g_db, "f", 1) == 0);
    assert(dl_load_rules(g_db, "q(X):-e(X),f(X).\n") == 0);
    /* definite int 77 on e's first row records INT; recheck passes (f
     * UNKNOWN). */
    v = 77;
    rc = dl_add_fact(g_db, "e", &v, 1);
    if (rc != 1) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("definite int add_fact after rules rejected (flag false reject)");
        return;
    }
    /* definite int on f's first row records INT; recheck sees e INT, f INT
     * -> no conflict -> add succeeds. */
    v = 88;
    rc = dl_add_fact(g_db, "f", &v, 1);
    if (rc != 1) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("definite int add_fact on f rejected (same-kind recheck false reject)");
        return;
    }
    teardown();
    PASS();

    TEST("homog: G1 flag — sym add_fact after rules stays permissive (floor)");

    /* a resolving sym id is ambiguous to the raw API: it neither records nor
     * rejects, so no 0->definite transition -> no recheck -> the add is
     * permissive.  This is the documented kindNoteRaw floor (RK-G1-flag). */
    setup();
    assert(dl_declare_relation(g_db, "e", 1) == 0);
    assert(dl_load_rules(g_db, "q(X):-e(X).\n") == 0);
    {
        uint32_t sym = dl_intern_str(g_db, "a"); /* resolves -> ambiguous */
        rc = dl_add_fact(g_db, "e", &sym, 1);
        if (rc != 1) {
            printf("(rc=%d) ", rc);
            teardown();
            FAIL("ambiguous sym add_fact after rules rejected (contract change)");
            return;
        }
    }
    teardown();
    PASS();
}

/* ─── T42: G2 — cross-rule mixed-head conflict controls ────────────────── */
/* A head column INT in one rule and SYM in another is LOUD (the flipped
 * T21 covers the single-load repro).  These pin the FALSE-POSITIVE fence:
 * every same-kind multi-rule head, every unknown-half pair, and the G3
 * result-kind heads fed the SAME kind by two rules must keep loading; the
 * split-load and chained-IDB shapes must be loud on the SECOND rule. */
static void test_g2_mixed_head_controls(void)
{
    uint32_t rows[4];
    tuple_set res;
    long n;
    int rc;

    TEST("homog: G2 — mixed head split across loads is LOUD on the 2nd");

    setup();
    rows[0] = 1; rows[1] = 2;
    load_rows_u32("e", 1, rows, 2);
    {
        const char *cells[2] = { "a", "b" };
        load_rows_sym("f", 1, cells, 2);
    }
    rc = dl_load_rules(g_db, "p(X):-e(X).\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("int-head rule failed to load (precondition)");
        return;
    }
    rc = dl_load_rules(g_db, "p(X):-f(X).\n");
    if (rc != -1) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("mixed head across SPLIT loads accepted (G2 hole)");
        return;
    }
    {
        const char *msg = compile_last_error(NULL);
        if (msg == NULL ||
            strstr(msg, "head column p/0") == NULL ||
            strstr(msg, "rule 1") == NULL ||
            strstr(msg, "rule 2") == NULL) {
            printf("(msg=%s) ", msg ? msg : "(null)");
            teardown();
            FAIL("split-load mixed-head diagnostic does not name both rules");
            return;
        }
    }
    teardown();
    PASS();

    TEST("homog: G2 — mixed head with non-colliding ids is still LOUD");

    /* e={100,200} do NOT collide with the sym ids of a,b, so the rows are
     * 'correct' — but the mixed head is still the defect (a column with no
     * single value space), and G2 rejects it on kind alone. */
    setup();
    rows[0] = 100; rows[1] = 200;
    load_rows_u32("e", 1, rows, 2);
    {
        const char *cells[2] = { "a", "b" };
        load_rows_sym("f", 1, cells, 2);
    }
    rc = dl_load_rules(g_db, "p(X):-e(X).\np(X):-f(X).\n");
    if (rc != -1) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("mixed head with non-colliding ids accepted (kind-only defect)");
        return;
    }
    teardown();
    PASS();

    TEST("homog: G2 — int-int same head loads and answers the union");

    setup();
    rows[0] = 1; rows[1] = 2;
    load_rows_u32("e", 1, rows, 2);
    rows[0] = 3; rows[1] = 4;
    load_rows_u32("f", 1, rows, 2);
    rc = dl_load_rules(g_db, "p(X):-e(X).\np(X):-f(X).\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("int-int same head rejected (G2 false positive)");
        return;
    }
    assert(dl_compile(g_db) == 0);
    memset(&res, 0, sizeof res);
    n = dl_query(g_db, "p", tset_cb, &res);
    if (n < 0 || res.count != 4) {
        printf("(n=%ld cnt=%ld) ", n, res.count);
        tset_free(&res);
        teardown();
        FAIL("int-int same head: p != {1,2,3,4}");
        return;
    }
    tset_free(&res);
    teardown();
    PASS();

    TEST("homog: G2 — sym-sym same head loads and answers the union");

    setup();
    {
        const char *ec[2] = { "a", "b" };
        const char *fc[2] = { "c", "d" };
        load_rows_sym("e", 1, ec, 2);
        load_rows_sym("f", 1, fc, 2);
    }
    rc = dl_load_rules(g_db, "p(X):-e(X).\np(X):-f(X).\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("sym-sym same head rejected (G2 false positive)");
        return;
    }
    assert(dl_compile(g_db) == 0);
    memset(&res, 0, sizeof res);
    n = dl_query(g_db, "p", tset_cb, &res);
    if (n < 0 || res.count != 4) {
        printf("(n=%ld cnt=%ld) ", n, res.count);
        tset_free(&res);
        teardown();
        FAIL("sym-sym same head: p != {a,b,c,d}");
        return;
    }
    tset_free(&res);
    teardown();
    PASS();

    TEST("homog: G2 — transitive closure (2 rules, same head) loads");

    /* the one real multi-rule-same-head shape in the wild: both rules feed
     * tc/0 and tc/1 INT from the same edge relation. */
    setup();
    {
        uint32_t edges[12] = { 1, 2, 2, 3, 3, 4, 1, 3, 2, 4, 4, 1 };
        load_rows_u32_2("edge", 2, edges, 6);
    }
    rc = dl_load_rules(g_db, "tc(X,Y):-edge(X,Y).\ntc(X,Y):-edge(X,Z),tc(Z,Y).\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("transitive closure rejected (G2 false positive)");
        return;
    }
    assert(dl_compile(g_db) == 0);
    memset(&res, 0, sizeof res);
    n = dl_query(g_db, "tc", tset_cb, &res);
    if (n < 0 || res.count != 16) { /* full closure: the 4->1 cycle closes it */
        printf("(n=%ld cnt=%ld) ", n, res.count);
        tset_free(&res);
        teardown();
        FAIL("transitive closure: tc row count wrong");
        return;
    }
    tset_free(&res);
    teardown();
    PASS();

    TEST("homog: G2 — chained mixed head through an IDB relation is LOUD");

    /* the conflict arrives through PROPAGATION (q sym via ff, p int via e,
     * p sym via q) — the fixpoint must name the two head-feeding rules. */
    setup();
    rows[0] = 1; rows[1] = 2;
    load_rows_u32("e", 1, rows, 2);
    {
        const char *cells[2] = { "a", "b" };
        load_rows_sym("ff", 1, cells, 2);
    }
    rc = dl_load_rules(g_db, "q(X):-ff(X).\np(X):-e(X).\np(X):-q(X).\n");
    if (rc != -1) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("chained mixed head accepted (G2 hole)");
        return;
    }
    {
        const char *msg = compile_last_error(NULL);
        if (msg == NULL || strstr(msg, "head column p/0") == NULL) {
            printf("(msg=%s) ", msg ? msg : "(null)");
            teardown();
            FAIL("chained mixed-head diagnostic does not name the head column");
            return;
        }
    }
    teardown();
    PASS();

    TEST("homog: G2 — int+unknown head stays permissive (S1/S2 contract)");

    /* one rule feeds the head INT, the other from a declared-but-EMPTY
     * relation (UNKNOWN absorbs — the S1/S2 permissive rule). */
    setup();
    rows[0] = 1; rows[1] = 2;
    load_rows_u32("e", 1, rows, 2);
    assert(dl_declare_relation(g_db, "g", 1) == 0);
    rc = dl_load_rules(g_db, "p(X):-e(X).\np(X):-g(X).\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("int+unknown head rejected (S1/S2 permissive contract)");
        return;
    }
    teardown();
    PASS();

    TEST("homog: G2 — count() INT result fed to ONE head by 2 rules loads");

    /* G3's result-kind table: count()->INT, length()->INT, concat()->SYM.
     * TWO rules feeding the SAME result kind to the SAME head column must
     * still load (the G2 check only rejects int-vs-sym across rules). */
    setup();
    rows[0] = 1; rows[1] = 2;
    load_rows_u32("e", 1, rows, 2);
    rows[0] = 5; rows[1] = 6;
    load_rows_u32("g", 1, rows, 2);
    rc = dl_load_rules(g_db,
        "c(N):-e(X),N=count().\nc(N):-g(X),N=count().\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("count() INT results to one head rejected (G2 false positive)");
        return;
    }
    teardown();
    PASS();

    TEST("homog: G2 — length() INT result fed to ONE head by 2 rules loads");

    setup();
    {
        const char *ec[2] = { "a", "bb" };
        const char *fc[2] = { "ccc", "dddd" };
        load_rows_sym("e", 1, ec, 2);
        load_rows_sym("f", 1, fc, 2);
    }
    rc = dl_load_rules(g_db,
        "c(N):-e(X),N=length(X).\nc(N):-f(X),N=length(X).\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("length() INT results to one head rejected (G2 false positive)");
        return;
    }
    teardown();
    PASS();

    TEST("homog: G2 — concat() SYM result fed to ONE head by 2 rules loads");

    setup();
    {
        const char *ec[2] = { "a", "b" };
        const char *fc[2] = { "c", "d" };
        load_rows_sym("e", 1, ec, 2);
        load_rows_sym("f", 1, fc, 2);
    }
    rc = dl_load_rules(g_db,
        "c(N):-e(X),N=concat(X,X).\nc(N):-f(X),N=concat(X,X).\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("concat() SYM results to one head rejected (G2 false positive)");
        return;
    }
    teardown();
    PASS();
}

/* ─── T43: review — car result is the ELEMENT, not a handle (FP control) ─── */
/* MEASURED (review BLOCKER): car returns s.head[h-TERM_BASE] — the raw
 * element u32 (test_lists T4/T9 pin H==X/H==1/H==7).  A program comparing
 * the car result against an int is VALID: e={7},
 * q(H):-e(X),L=cons(X,[8]),H=car(L),H>5. answers H=7.  The first G3 cut
 * kinded car KIND_LIST and rejected this exact program ("ordered comparison
 * > over list variable H") — a false positive.  car now stays UNKNOWN
 * (data-dependent: int, sym, or a nested-list handle); cons/cdr/append
 * remain KIND_LIST (their results ARE handles, T36 pins them loud). */
static void test_car_result_is_element(void)
{
    tuple_set res;
    long n;
    int rc;

    TEST("homog: car result (the element) in an ordered cmp LOADS and answers");

    setup();
    {
        uint32_t rows[1] = { 7 };
        load_rows_u32("e", 1, rows, 1);
    }
    rc = dl_load_rules(g_db, "q(H):-e(X),L=cons(X,[8]),H=car(L),H>5.\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("car result in ordered cmp rejected (false positive)");
        return;
    }
    assert(dl_compile(g_db) == 0);
    memset(&res, 0, sizeof res);
    n = dl_query(g_db, "q", tset_cb, &res);
    if (n < 0 || res.count != 1 || (res.count == 1 && res.data[0] != 7)) {
        printf("(n=%ld cnt=%ld) ", n, res.count);
        tset_free(&res);
        teardown();
        FAIL("car case: expected q={7}");
        return;
    }
    tset_free(&res);
    teardown();
    PASS();

    TEST("homog: control — cons result in an ordered cmp is still LOUD");

    /* cons/cdr/append results ARE handles; the same shape over cons must
     * stay loud (the FP fix must not have loosened the list track). */
    setup();
    {
        uint32_t rows[1] = { 7 };
        load_rows_u32("e", 1, rows, 1);
    }
    rc = dl_load_rules(g_db, "q(L):-e(X),L=cons(X,[8]),L>5.\n");
    if (rc != -1) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("cons result in ordered cmp accepted (list track loosened)");
        return;
    }
    {
        const char *msg = compile_last_error(NULL);
        if (msg == NULL ||
            strstr(msg, "ordered comparison") == NULL ||
            strstr(msg, "list variable L") == NULL) {
            printf("(msg=%s) ", msg ? msg : "(null)");
            teardown();
            FAIL("cons-ordered diagnostic does not name the list var");
            return;
        }
    }
    teardown();
    PASS();

    TEST("homog: arithmetic over a list handle is LOUD (review R4)");

    /* MEASURED (review): q(M):-e(X),L=cons(X,[8]),M=L+1. loaded rc=0 and
     * answered 2147483651 (0x80000003 = TERM_BASE+3) — handle arithmetic.
     * R4 now rejects KIND_LIST operands exactly as it rejects SYM ones. */
    setup();
    {
        uint32_t rows[1] = { 7 };
        load_rows_u32("e", 1, rows, 1);
    }
    rc = dl_load_rules(g_db, "q(M):-e(X),L=cons(X,[8]),M=L+1.\n");
    if (rc != -1) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("arithmetic over a list handle accepted (silent-wrong)");
        return;
    }
    {
        const char *msg = compile_last_error(NULL);
        if (msg == NULL ||
            strstr(msg, "arithmetic on list variable L") == NULL) {
            printf("(msg=%s) ", msg ? msg : "(null)");
            teardown();
            FAIL("list-arithmetic diagnostic does not name the list var");
            return;
        }
    }
    teardown();
    PASS();

    TEST("homog: member generator over an all-int literal kinds X (INT)");

    /* MEASURED (review): q(X):-s(V),L=cons(V,[c]),member(X,L),X>0. loads
     * and compares INTERN IDS of the sym elements.  The direct-literal
     * form is the kindable one: member(X,[7,9]) binds X to definite INTs,
     * so X>0 is meaningful.  A sym literal now rejects; the documented
     * member-generator floor is only the data-dependent shapes (a var
     * list operand — the cons(L,...)/member(X,L) chain — mixed/nested
     * literals, a | tail). */
    setup();
    {
        uint32_t rows[1] = { 5 };
        load_rows_u32("e", 1, rows, 1);
    }
    rc = dl_load_rules(g_db, "q(X):-e(X),member(X,[7,9]),X>0.\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("member over an all-int literal + ordered cmp rejected (FP)");
        return;
    }
    teardown();
    PASS();

    TEST("homog: member generator over a sym literal + ordered cmp is LOUD");

    /* X's only site is the member generator over a sym literal, so the
     * ordered cmp is the reject site (with an int column joined in, the
     * join check fires first — same defect, different message). */
    setup();
    rc = dl_load_rules(g_db, "q(X):-member(X,[a,b]),X>0.\n");
    if (rc != -1) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("member over a sym literal + ordered cmp accepted (intern-id cmp)");
        return;
    }
    {
        const char *msg = compile_last_error(NULL);
        if (msg == NULL ||
            strstr(msg, "ordered comparison") == NULL ||
            strstr(msg, "symbol variable X") == NULL) {
            printf("(msg=%s) ", msg ? msg : "(null)");
            teardown();
            FAIL("member-sym-literal diagnostic does not name the var kind");
            return;
        }
    }
    teardown();
    PASS();
}

/* ─── T44: review — a failed load leaves the kind usable (all-or-nothing) ── */
/* MEASURED (review): a g/2 CSV "1,5\na,7" fails at row 2 (mixed column 0),
 * but row 1 had already recorded col0=INT — a later VALID sym-only load is
 * then rejected forever (and the poison persists to rels.txt).  The G1
 * rollback only covered the recheck failure; every failure return in the
 * parse loop now rolls the kind back the same way. */
static void test_mincsv_failure_no_poison(void)
{
    int rc;

    TEST("homog: mid-CSV mixed failure leaves no kind poison");

    setup();
    assert(dl_declare_relation(g_db, "g", 2) == 0);
    {
        char path[256];
        FILE *ff;
        (void)dl_intern_str(g_db, "x");
        snprintf(path, sizeof path, "build-tmp/homogdb/g.csv");
        ff = fopen(path, "w");
        assert(ff);
        fprintf(ff, "1,5\na,7\n");
        fclose(ff);
        rc = dl_load_facts(g_db, "g", path);
        if (rc != -1) {
            printf("(rc=%d) ", rc);
            teardown();
            FAIL("mixed mid-CSV load accepted (precondition)");
            return;
        }
    }
    /* the poison probe: a VALID sym-only load into the same column must
     * now SUCCEED (pre-fix it was rejected forever: col0 stuck INT). */
    {
        char path[256];
        FILE *ff;
        snprintf(path, sizeof path, "build-tmp/homogdb/g.csv");
        ff = fopen(path, "w");
        assert(ff);
        fprintf(ff, "x,7\n");
        fclose(ff);
        rc = dl_load_facts(g_db, "g", path);
        if (rc < 0) {
            printf("(rc=%d) ", rc);
            teardown();
            FAIL("valid sym load after a mid-CSV failure rejected (kind POISON)");
            return;
        }
    }
    teardown();
    PASS();
}

/* ─── T45: review — recorded-kind (facts-first) head conflict is LOUD ────── */
/* MEASURED (review): p holding int FACTS {1,2} plus a rule p(X):-f(X).
 * with f sym loads rc=0 and answers 2 rows where the standard is 4 — the
 * same silent-wrong G2 closes, reached through the ORDINARY facts-then-
 * rules order.  The head fold now seeds the column's recorded/schema kind,
 * so the data-established kind conflicts with the rule's derivation. */
static void test_g2_recorded_kind_head_loud(void)
{
    uint32_t rows[2];
    int rc;

    TEST("homog: recorded INT facts + sym-deriving rule head is LOUD");

    setup();
    assert(dl_declare_relation(g_db, "p", 1) == 0);
    assert(dl_declare_relation(g_db, "f", 1) == 0);
    rows[0] = 1; rows[1] = 2;
    load_rows_u32("p", 1, rows, 2); /* facts first: p records INT */
    {
        const char *cells[2] = { "a", "b" };
        load_rows_sym("f", 1, cells, 2);
    }
    rc = dl_load_rules(g_db, "p(X):-f(X).\n");
    if (rc != -1) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("recorded-kind head conflict accepted (G2 facts-first hole)");
        return;
    }
    {
        const char *msg = compile_last_error(NULL);
        if (msg == NULL ||
            strstr(msg, "head column p/0") == NULL) {
            printf("(msg=%s) ", msg ? msg : "(null)");
            teardown();
            FAIL("recorded-kind head diagnostic does not name the column");
            return;
        }
    }
    teardown();
    PASS();

    TEST("homog: control — recorded INT facts + int-deriving rule loads");

    /* the same facts-first shape with MATCHING kinds must keep loading
     * (the seed must not reject a consistent program). */
    setup();
    assert(dl_declare_relation(g_db, "p", 1) == 0);
    assert(dl_declare_relation(g_db, "f", 1) == 0);
    rows[0] = 1; rows[1] = 2;
    load_rows_u32("p", 1, rows, 2);
    rows[0] = 3; rows[1] = 4;
    load_rows_u32("f", 1, rows, 2);
    rc = dl_load_rules(g_db, "p(X):-f(X).\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("recorded-kind head with matching kinds rejected (false positive)");
        return;
    }
    teardown();
    PASS();
}

/* ─── T46: review — txn rollback undoes the buffer-time kind record ──────── */
/* MEASURED (review): dl_txn_add_fact records col_kind at BUFFER time;
 * dl_txn_rollback did not undo it (pre-existing, but the G1 recheck made
 * the stale kind load-fatal, widening the blast radius).  Rollback now
 * restores the snapshot taken at the first buffered add per relation. */
static void test_txn_rollback_undoes_kind(void)
{
    uint32_t v;
    int rc;

    TEST("homog: txn rollback undoes the buffer-time kind record");

    setup();
    assert(dl_declare_relation(g_db, "e", 1) == 0);
    assert(dl_txn_begin(g_db) == 0);
    v = 77; /* definite int on e's first row: records INT at buffer time */
    rc = dl_txn_add_fact(g_db, "e", &v, 1);
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("txn add of a definite int rejected (precondition)");
        return;
    }
    assert(dl_txn_rollback(g_db) == 0);
    /* the poison probe: a sym CSV load into e must now SUCCEED (the
     * buffered INT kind must not have survived the rollback). */
    {
        char path[256];
        FILE *ff;
        (void)dl_intern_str(g_db, "a");
        snprintf(path, sizeof path, "build-tmp/homogdb/e.csv");
        ff = fopen(path, "w");
        assert(ff);
        fprintf(ff, "a\nb\n");
        fclose(ff);
        rc = dl_load_facts(g_db, "e", path);
        if (rc < 0) {
            printf("(rc=%d) ", rc);
            teardown();
            FAIL("sym load after txn rollback rejected (kind leaked past rollback)");
            return;
        }
    }
    teardown();
    PASS();

    TEST("homog: control — txn COMMIT keeps the buffer-time kind record");

    /* the snapshot is a rollback artifact only: a committed txn's kind
     * recording must survive as before. */
    setup();
    assert(dl_declare_relation(g_db, "e", 1) == 0);
    assert(dl_txn_begin(g_db) == 0);
    v = 77;
    rc = dl_txn_add_fact(g_db, "e", &v, 1);
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("txn add of a definite int rejected (commit precondition)");
        return;
    }
    rc = dl_txn_commit(g_db);
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("txn commit failed (precondition)");
        return;
    }
    /* an int CSV load into the now-INT e must still succeed (idempotent
     * kind); a SYM load must fail (the committed INT kind is real). */
    {
        char path[256];
        FILE *ff;
        uint32_t rows[1] = { 78 };
        load_rows_u32("e", 1, rows, 1);
        (void)dl_intern_str(g_db, "a");
        snprintf(path, sizeof path, "build-tmp/homogdb/e.csv");
        ff = fopen(path, "w");
        assert(ff);
        fprintf(ff, "a\n");
        fclose(ff);
        rc = dl_load_facts(g_db, "e", path);
        if (rc != -1) {
            printf("(rc=%d) ", rc);
            teardown();
            FAIL("sym load after committed int txn accepted (kind not recorded)");
            return;
        }
    }
    teardown();
    PASS();
}

/* ─── T47: review B1 — a REJECTED dl_add_fact leaves no kind record ─────── */
/* MEASURED (review): p3/3 with col2 recorded SYM (via a rule whose head
 * var is fed by a sym relation); dl_add_fact(p3,(7,8,9)) fails at col2
 * (9 is not a live sym id), but kindNoteRaw's left-to-right walk had
 * ALREADY recorded cols 0-1 INT — a later sym-only CSV into p3 was then
 * rejected FOREVER (and the poison persisted to rels.txt).  The rejected
 * add is now all-or-nothing for kinds, like dl_load_facts. */
static void test_addfact_reject_no_poison(void)
{
    uint32_t v3[3];
    int rc;

    TEST("homog: rejected dl_add_fact leaves no kind poison");

    setup();
    assert(dl_declare_relation(g_db, "gx", 2) == 0); /* stays EMPTY: kind unknown */
    assert(dl_declare_relation(g_db, "fs", 1) == 0);
    assert(dl_declare_relation(g_db, "p3", 3) == 0);
    {
        const char *cells[2] = { "aa", "bb" };
        load_rows_sym("fs", 1, cells, 2);
    }
    /* recordHeadKindsPub records p3 col2 = SYM (head var fed by fs); the
     * empty gx contributes nothing, so cols 0-1 stay UNKNOWN. */
    rc = dl_load_rules(g_db, "p3(X,Y,Z):-gx(X,Y),fs(Z).\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("p3 rule load rejected (precondition)");
        return;
    }
    v3[0] = 7; v3[1] = 8; v3[2] = 9; /* 9 not a live sym id: col2 rejects */
    rc = dl_add_fact(g_db, "p3", v3, 3);
    if (rc != -1) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("mixed add_fact accepted (precondition)");
        return;
    }
    /* the poison probe: a sym-only CSV into p3 must now SUCCEED (pre-fix
     * cols 0-1 were left recorded INT by the partial walk). */
    {
        char path[256];
        FILE *ff;
        snprintf(path, sizeof path, "build-tmp/homogdb/p3.csv");
        ff = fopen(path, "w");
        assert(ff);
        fprintf(ff, "x,y,zz\n");
        fclose(ff);
        rc = dl_load_facts(g_db, "p3", path);
        if (rc < 0) {
            printf("(rc=%d) ", rc);
            teardown();
            FAIL("sym load after rejected add_fact rejected (kind POISON)");
            return;
        }
    }
    teardown();
    PASS();

    TEST("homog: rejected dl_add_fact leaves no persisted kind (reopen)");

    /* same shape, but the poison probe runs in a FRESH session: the close
     * must not have flushed the partial recording to rels.txt. */
    setup();
    assert(dl_declare_relation(g_db, "gx", 2) == 0);
    assert(dl_declare_relation(g_db, "fs", 1) == 0);
    assert(dl_declare_relation(g_db, "p3", 3) == 0);
    {
        const char *cells[2] = { "aa", "bb" };
        load_rows_sym("fs", 1, cells, 2);
    }
    assert(dl_load_rules(g_db, "p3(X,Y,Z):-gx(X,Y),fs(Z).\n") == 0);
    v3[0] = 7; v3[1] = 8; v3[2] = 9;
    assert(dl_add_fact(g_db, "p3", v3, 3) == -1);
    dl_close(g_db); /* writes rels.txt */
    g_db = dl_open("build-tmp/homogdb");
    assert(g_db);
    {
        char path[256];
        FILE *ff;
        snprintf(path, sizeof path, "build-tmp/homogdb/p3.csv");
        ff = fopen(path, "w");
        assert(ff);
        fprintf(ff, "x,y,zz\n");
        fclose(ff);
        rc = dl_load_facts(g_db, "p3", path);
        if (rc < 0) {
            printf("(rc=%d) ", rc);
            teardown();
            FAIL("sym load after reopen rejected (PERSISTED kind poison)");
            return;
        }
    }
    teardown();
    PASS();
}

/* ─── T48: review B2 — a FAILED dl_txn_commit leaves no kind record ─────── */
/* MEASURED (review): begin; txn_add_fact(w,(77)) records w col0=INT at
 * BUFFER time; txn_cas(ent,999,1) buffered with a wrong expected; commit
 * returns DL_E_CONFLICT (the ROUTINE optimistic-concurrency failure) —
 * and the buffer-time kind LEAKED, persisted to rels.txt by the close, so
 * a sym CSV into w was rejected even after reopen.  Every commit-failure
 * return now aborts with the same kind restore the rollback does; the
 * eager rels.txt flush the CAS path can trigger is rewritten to the
 * restored state. */
static void test_txn_commit_fail_no_poison(void)
{
    uint32_t wv, rev0;
    int rc;

    TEST("homog: CAS-conflict commit failure leaves no kind poison");

    setup();
    assert(dl_declare_relation(g_db, "w", 1) == 0);
    assert(dl_declare_relation(g_db, "ent", 1) == 0);
    rev0 = 1;
    load_rows_u32("ent", 1, &rev0, 1); /* current revision row */
    assert(dl_txn_begin(g_db) == 0);
    wv = 77; /* definite int on w's first row: records INT at buffer time */
    rc = dl_txn_add_fact(g_db, "w", &wv, 1);
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("txn add of a definite int rejected (precondition)");
        return;
    }
    assert(dl_txn_cas(g_db, "ent", 999, 2) == 0); /* WRONG expected */
    rc = dl_txn_commit(g_db);
    if (rc != 2) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("commit did not return DL_E_CONFLICT (precondition)");
        return;
    }
    /* the poison probe: a sym CSV load into w must now SUCCEED. */
    {
        char path[256];
        FILE *ff;
        (void)dl_intern_str(g_db, "a");
        snprintf(path, sizeof path, "build-tmp/homogdb/w.csv");
        ff = fopen(path, "w");
        assert(ff);
        fprintf(ff, "a\n");
        fclose(ff);
        rc = dl_load_facts(g_db, "w", path);
        if (rc < 0) {
            printf("(rc=%d) ", rc);
            teardown();
            FAIL("sym load after CAS-conflict commit rejected (kind POISON)");
            return;
        }
    }
    teardown();
    PASS();

    TEST("homog: CAS-conflict commit poison does not persist (reopen)");

    /* same shape; reopen after the failed commit and close, then probe. */
    setup();
    assert(dl_declare_relation(g_db, "w", 1) == 0);
    assert(dl_declare_relation(g_db, "ent", 1) == 0);
    rev0 = 1;
    load_rows_u32("ent", 1, &rev0, 1);
    assert(dl_txn_begin(g_db) == 0);
    wv = 77;
    assert(dl_txn_add_fact(g_db, "w", &wv, 1) == 0);
    assert(dl_txn_cas(g_db, "ent", 999, 2) == 0);
    assert(dl_txn_commit(g_db) == 2);
    dl_close(g_db); /* must not leave the poisoned kind in rels.txt */
    g_db = dl_open("build-tmp/homogdb");
    assert(g_db);
    {
        char path[256];
        FILE *ff;
        (void)dl_intern_str(g_db, "a");
        snprintf(path, sizeof path, "build-tmp/homogdb/w.csv");
        ff = fopen(path, "w");
        assert(ff);
        fprintf(ff, "a\n");
        fclose(ff);
        rc = dl_load_facts(g_db, "w", path);
        if (rc < 0) {
            printf("(rc=%d) ", rc);
            teardown();
            FAIL("sym load after reopen rejected (PERSISTED kind poison)");
            return;
        }
    }
    teardown();
    PASS();

    TEST("homog: control — a COMMITTED txn keeps its kind record");

    /* the restore is a FAILURE-path artifact only: a successful commit's
     * kind recording must survive exactly as before (mirror of T46). */
    setup();
    assert(dl_declare_relation(g_db, "w", 1) == 0);
    assert(dl_txn_begin(g_db) == 0);
    wv = 77;
    assert(dl_txn_add_fact(g_db, "w", &wv, 1) == 0);
    rc = dl_txn_commit(g_db);
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("commit failed (precondition)");
        return;
    }
    {
        char path[256];
        FILE *ff;
        (void)dl_intern_str(g_db, "a");
        snprintf(path, sizeof path, "build-tmp/homogdb/w.csv");
        ff = fopen(path, "w");
        assert(ff);
        fprintf(ff, "a\n");
        fclose(ff);
        rc = dl_load_facts(g_db, "w", path);
        if (rc != -1) {
            printf("(rc=%d) ", rc);
            teardown();
            FAIL("sym load after COMMITTED int txn accepted (kind not recorded)");
            return;
        }
    }
    teardown();
    PASS();
}

/* ─── T49: review B3 — recorded-kind seed vs an EMPTY relation ──────────── */
/* DECISION (orchestrator): the G2 recorded-kind seed applies only when the
 * relation actually HOLDS ROWS.  A FRESH program deriving symbols into an
 * EMPTY relation whose INT kind was persisted by a PREVIOUS session's
 * rules must LOAD (HEAD did; deriving symbols into an empty column is
 * harmless, and the int-rows-later case is covered by G1's recheck).  The
 * genuinely-conflicting shape — the relation HOLDS int rows — stays LOUD,
 * with the diagnostic naming the recorded facts. */
static void test_recorded_kind_empty_relation_loads(void)
{
    uint32_t rows[2];
    int rc;

    TEST("homog: sym rule into an EMPTY recorded-INT relation loads");

    setup();
    assert(dl_declare_relation(g_db, "e2", 1) == 0);
    assert(dl_declare_relation(g_db, "pp", 1) == 0);
    assert(dl_declare_relation(g_db, "fs2", 1) == 0);
    rows[0] = 1; rows[1] = 2;
    load_rows_u32("e2", 1, rows, 2);
    /* recordHeadKindsPub records pp col0 = INT from the rule's own head */
    assert(dl_load_rules(g_db, "pp(X):-e2(X).\n") == 0);
    dl_close(g_db); /* persists the recorded kind to rels.txt */
    g_db = dl_open("build-tmp/homogdb");
    assert(g_db);
    /* FRESH session: rules gone, pp EMPTY (nothing was ever derived), a
     * NEW sym program derives into it — must LOAD (pre-B3 it was rc=-1,
     * a false positive of the S1 tradition). */
    {
        const char *cells[2] = { "aa", "bb" };
        load_rows_sym("fs2", 1, cells, 2);
    }
    rc = dl_load_rules(g_db, "pp(X):-fs2(X).\n");
    if (rc != 0) {
        const char *msg = compile_last_error(NULL);
        printf("(rc=%d msg=%s) ", rc, msg ? msg : "(null)");
        teardown();
        FAIL("sym rule into an EMPTY recorded-INT relation rejected (over-strict)");
        return;
    }
    teardown();
    PASS();

    TEST("homog: recorded-INT relation WITH rows vs sym rule stays LOUD");

    /* the control: the relation actually HOLDS int rows — the collision
     * is real and must stay loud, naming the recorded facts. */
    setup();
    assert(dl_declare_relation(g_db, "pp2", 1) == 0);
    assert(dl_declare_relation(g_db, "fs3", 1) == 0);
    rows[0] = 5; rows[1] = 6;
    load_rows_u32("pp2", 1, rows, 2); /* facts first: pp2 records INT, has rows */
    {
        const char *cells[2] = { "aa", "bb" };
        load_rows_sym("fs3", 1, cells, 2);
    }
    rc = dl_load_rules(g_db, "pp2(X):-fs3(X).\n");
    if (rc != -1) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("recorded-INT relation with rows vs sym rule accepted (silent-wrong)");
        return;
    }
    {
        const char *msg = compile_last_error(NULL);
        if (msg == NULL ||
            strstr(msg, "head column pp2/0") == NULL ||
            strstr(msg, "recorded facts") == NULL) {
            printf("(msg=%s) ", msg ? msg : "(null)");
            teardown();
            FAIL("with-rows diagnostic does not name the recorded facts");
            return;
        }
    }
    teardown();
    PASS();
}

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
    test_join_cross_kind_loud();
    test_join_idb_propagated_loud();
    test_join_negated_cross_kind_loud();
    test_join_same_kind_controls();
    test_join_wfs_paths();
    test_join_diagnostic_names_sites();
    test_cmp_ordered_sym_loud();
    test_cmp_ordered_int_controls();
    test_eq_cross_kind_loud();
    test_arith_sym_loud();
    test_neq_documented_controls();
    test_neq_cross_kind_loud();
    test_agg_result_head_loud();
    test_arith_result_head_loud();
    test_range_list_cmp_controls();
    test_wide_rule_still_checked();
    test_g3_concat_result_ordered_loud();
    test_g3_length_result_join_loud();
    test_g3_list_result_ordered_loud();
    test_g3_range_result_kind();
    test_g1_populate_after_load_loud();
    test_g1_rollback_restores_kind();
    test_g1_controls();
    test_g1_flag_precision();
    test_g2_mixed_head_controls();
    test_car_result_is_element();
    test_mincsv_failure_no_poison();
    test_g2_recorded_kind_head_loud();
    test_txn_rollback_undoes_kind();
    test_addfact_reject_no_poison();
    test_txn_commit_fail_no_poison();
    test_recorded_kind_empty_relation_loads();

    printf("\n%d/%d tests passed\n", tests_run - tests_failed, tests_run);
    return tests_failed ? 1 : 0;
}
