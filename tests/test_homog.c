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

    TEST("homog: mixed head across rules (FP13) stays permissive");

    setup();
    e_rows[0] = 1; e_rows[1] = 2;
    load_rows_u32("e", 1, e_rows, 2);
    {
        const char *cells[2] = { "a", "b" };
        load_rows_sym("f", 1, cells, 2);
    }
    rc = dl_load_rules(g_db, "p(X):-e(X).\np(X):-f(X).\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("mixed head across rules rejected (per-rule consistency only)");
        return;
    }
    /* ...and a downstream rule over the mixed head joins NOTHING definite:
     * p's column kind folded to MIXED, which is permissive by contract */
    rc = dl_load_rules(g_db, "r(X):-p(X),e(X).\n");
    if (rc != 0) {
        printf("(rc=%d) ", rc);
        teardown();
        FAIL("downstream rule over a mixed head rejected");
        return;
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

    printf("\n%d/%d tests passed\n", tests_run - tests_failed, tests_run);
    return tests_failed ? 1 : 0;
}
