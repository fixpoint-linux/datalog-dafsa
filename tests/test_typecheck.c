/*
 * test_typecheck.c — S3 Dhall rule-typechecker tests: dl_typecheck_rules
 *
 * Builds a typed schema, parses a Datalog program with parse_create/parse_rules,
 * and calls dl_typecheck_rules directly (bypassing dl_load_rules) to assert
 * accept / reject for the worked-example rules and a battery of negative cases.
 *
 * Schema (matching the S3 plan's worked example):
 *   node(1)     [Natural]
 *   edge(2)     [Natural, Natural]   (src, dst)
 *   weight(2)   [Natural, Natural]   (a, w)  — w is Natural
 *   tc(2)       [Natural, Text]      (src, dst) — dst is Text
 *   label(1)    [Text]
 *   total(1)    [Natural]
 *   bar(2)      [Natural, Natural]
 *
 * Standalone, links libdatalog.so.
 */
#include "dl.h"
#include "schema.h"
#include "parser.h"

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

static void build_schema(dl_schema *s)
{
    memset(s, 0, sizeof(*s));
    {
        dl_colspec c[] = { {.tag=DLT_NATURAL} };
        assert(dl_schema_add(s, "node", 1, c, 0) == 0);
    }
    {
        dl_colspec c[] = { {.tag=DLT_NATURAL}, {.tag=DLT_NATURAL} };
        assert(dl_schema_add(s, "edge", 2, c, 0) == 0);
    }
    {
        dl_colspec c[] = { {.tag=DLT_NATURAL}, {.tag=DLT_NATURAL} };
        assert(dl_schema_add(s, "weight", 2, c, 0) == 0);
    }
    {
        dl_colspec c[] = { {.tag=DLT_NATURAL}, {.tag=DLT_TEXT} };
        assert(dl_schema_add(s, "tc", 2, c, 0) == 0);
    }
    {
        dl_colspec c[] = { {.tag=DLT_TEXT} };
        assert(dl_schema_add(s, "label", 1, c, 0) == 0);
    }
    {
        dl_colspec c[] = { {.tag=DLT_NATURAL} };
        assert(dl_schema_add(s, "total", 1, c, 0) == 0);
    }
    {
        dl_colspec c[] = { {.tag=DLT_NATURAL}, {.tag=DLT_NATURAL} };
        assert(dl_schema_add(s, "bar", 2, c, 0) == 0);
    }
}

/* Parse `src` into rules, call dl_typecheck_rules, free rules.  Returns the
 * typecheck return value (0 = accept, -1 = reject).  `want_msg` (if non-NULL)
 * must appear in errbuf on a reject. */
static int check_prog(const dl_schema *s, const char *src, const char *want_msg)
{
    parser *p = parse_create(src);
    int n = 0, rc;
    rule **rules;
    char errbuf[512];

    if (!p) { FAIL("parse_create failed"); return 999; }
    rules = parse_rules(p, &n);
    if (!rules) { FAIL("parse_rules failed"); parse_free(p); return 999; }

    errbuf[0] = '\0';
    rc = dl_typecheck_rules(s, (void *)rules, n, errbuf, sizeof(errbuf));

    if (want_msg) {
        if (rc != -1) { printf("  (rc=%d) ", rc); }
        else if (strstr(errbuf, want_msg) == NULL) {
            printf("  (errbuf='%s') ", errbuf);
            FAIL("reject message mismatch");
        }
    }

    {
        int i;
        for (i = 0; i < n; i++) rule_free(rules[i]);
        free(rules);
    }
    parse_free(p);
    return rc;
}

/* ─── Positive: worked-example rules ──────────────────────────────────── */

static void test_positive(void)
{
    dl_schema s;
    build_schema(&s);

    TEST("positive: transitive closure tc");
    if (check_prog(&s,
            "tc(X,Y) :- edge(X,Z), tc(Z,Y).\n", NULL) == 0)
        PASS();
    else
        FAIL("tc closure rejected");

    TEST("positive: bar from edge + weight");
    if (check_prog(&s,
            "bar(X,W) :- edge(X,Y), weight(Y,W).\n", NULL) == 0)
        PASS();
    else
        FAIL("bar rejected");

    TEST("positive: head/body var consistent across atoms");
    if (check_prog(&s,
            "bar(X,Y) :- edge(X,Y).\n"
            "bar(X,Y) :- bar(X,Z), edge(Z,Y).\n", NULL) == 0)
        PASS();
    else
        FAIL("multi-rule program rejected");
}

/* ─── Negative cases ──────────────────────────────────────────────────── */

static void test_int_text_mixing(void)
{
    dl_schema s;
    build_schema(&s);

    /* W is Natural (weight.w) in body but Text (tc.dst) in head. */
    TEST("reject: int/text mixing (tc(A,W) :- weight(A,W))");
    if (check_prog(&s, "tc(A,W) :- weight(A,W).\n", "variable W is Natural") == -1)
        PASS();
    else
        FAIL("mixing bug not rejected");
}

static void test_undeclared_relation(void)
{
    dl_schema s;
    build_schema(&s);

    TEST("reject: undeclared body relation");
    if (check_prog(&s, "bar(X,Y) :- mystery(X,Y).\n", "not declared") == -1)
        PASS();
    else
        FAIL("undeclared relation not rejected");

    TEST("reject: undeclared head relation");
    if (check_prog(&s, "ghost(X) :- node(X).\n", "not declared") == -1)
        PASS();
    else
        FAIL("undeclared head not rejected");
}

static void test_text_comparison(void)
{
    dl_schema s;
    build_schema(&s);

    /* Both N,M are Text via label; the < comparison then constrains them to
     * Natural -> the typechecker reports N is Natural here (<) but Text. */
    TEST("reject: Text comparison (<)");
    if (check_prog(&s, "bar(X,Y) :- label(N), label(M), N < M.\n", "variable N is Natural") == -1)
        PASS();
    else
        FAIL("Text comparison not rejected");
}

static void test_aggregate_over_text(void)
{
    dl_schema s;
    build_schema(&s);

    /* sum over a Text column: head total(S) sets S Natural, then label(S)
     * needs Text -> conflict (reports S is Text here (label)). */
    TEST("reject: aggregate over Text column");
    if (check_prog(&s, "total(S) :- label(S), S = sum(S).\n", "variable S is Text") == -1)
        PASS();
    else
        FAIL("sum-over-Text not rejected");

    TEST("accept: count aggregate");
    if (check_prog(&s, "total(N) :- edge(X,Y), N = count().\n", NULL) == 0)
        PASS();
    else
        FAIL("count aggregate rejected");
}

static void test_int_const_in_text_col(void)
{
    dl_schema s;
    build_schema(&s);

    /* tc.dst is Text but gets an int constant 5. */
    TEST("reject: int constant in Text column");
    if (check_prog(&s, "tc(X,5) :- edge(X,Y).\n", "int constant") == -1)
        PASS();
    else
        FAIL("int-in-Text-col not rejected");
}

static void test_arity_mismatch(void)
{
    dl_schema s;
    build_schema(&s);

    TEST("reject: arity mismatch (edge used with 1 arg)");
    if (check_prog(&s, "bar(X) :- edge(X).\n", "arity") == -1)
        PASS();
    else
        FAIL("arity mismatch not rejected");
}

static void test_reserved_builtin_head(void)
{
    dl_schema s;
    build_schema(&s);

    TEST("reject: reserved builtin name as rule head");
    if (check_prog(&s, "concat(X,Y) :- edge(X,Y).\n", "reserved builtin") == -1)
        PASS();
    else
        FAIL("reserved builtin head not rejected");
}

/* ─── Stage B: List/Optional/Enum column type rules ───────────────────── */

/* Schema with List/Optional/Enum columns + scalar-typed output columns. */
static void build_param_schema(dl_schema *s)
{
    memset(s, 0, sizeof(*s));
    {
        dl_colspec c[] = { {.tag=DLT_LIST, .elem=DLT_TEXT} };   /* List<Text> */
        assert(dl_schema_add(s, "taglist", 1, c, 0) == 0);
    }
    {
        dl_colspec c[] = { {.tag=DLT_TEXT} };
        assert(dl_schema_add(s, "str1", 1, c, 0) == 0);
    }
    {
        dl_colspec c[] = { {.tag=DLT_LIST, .elem=DLT_TEXT} };
        assert(dl_schema_add(s, "out_list", 1, c, 0) == 0);
    }
    {
        dl_colspec c[] = { {.tag=DLT_TEXT} };
        assert(dl_schema_add(s, "out_text", 1, c, 0) == 0);
    }
    {
        dl_colspec c[] = { {.tag=DLT_NATURAL} };
        assert(dl_schema_add(s, "out_nat", 1, c, 0) == 0);
    }
    {
        dl_colspec c[] = { {.tag=DLT_OPTIONAL, .elem=DLT_TEXT} };
        assert(dl_schema_add(s, "maybe", 1, c, 0) == 0);
    }
    {
        dl_colspec c[] = { {.tag=DLT_OPTIONAL, .elem=DLT_TEXT} };
        assert(dl_schema_add(s, "out_opt", 1, c, 0) == 0);
    }
    {
        dl_colspec c[] = { {.tag=DLT_ENUM, .n_evalues=3,
                            .evalues={{"red"},{"green"},{"blue"}}} };
        assert(dl_schema_add(s, "color", 1, c, 0) == 0);
    }
    {
        dl_colspec c[] = { {.tag=DLT_ENUM, .n_evalues=3,
                            .evalues={{"red"},{"green"},{"blue"}}} };
        assert(dl_schema_add(s, "color_out", 1, c, 0) == 0);
    }
}

static void test_list_builtins(void)
{
    dl_schema s;
    build_param_schema(&s);

    TEST("accept: member(X,L) L typed List<Text> -> X Text");
    if (check_prog(&s, "out_text(X) :- taglist(L), member(X, L).\n", NULL) == 0)
        PASS(); else FAIL("member over typed List rejected");

    TEST("accept: car(R,L) L List<Text> -> R Text");
    if (check_prog(&s, "out_text(R) :- taglist(L), car(R, L).\n", NULL) == 0)
        PASS(); else FAIL("car over List rejected");

    TEST("accept: cdr(R,L) L List<Text> -> R List<Text>");
    if (check_prog(&s, "out_list(R) :- taglist(L), cdr(R, L).\n", NULL) == 0)
        PASS(); else FAIL("cdr over List rejected");

    TEST("accept: cons(R,H,T) T List<Text> -> H Text, R List<Text>");
    if (check_prog(&s, "out_list(R) :- taglist(T), cons(R, H, T).\n", NULL) == 0)
        PASS(); else FAIL("cons over List rejected");

    TEST("accept: append(R,A,B) A List<Text> -> B,R List<Text>");
    if (check_prog(&s, "out_list(R) :- taglist(A), append(R, A, B).\n", NULL) == 0)
        PASS(); else FAIL("append over List rejected");

    TEST("reject: member with untyped list operand (cannot infer)");
    if (check_prog(&s, "out_text(X) :- member(X, L).\n", "cannot infer") == -1)
        PASS(); else FAIL("member untyped not rejected");

    TEST("reject: member elem type conflict (X Natural vs List<Text> elem)");
    if (check_prog(&s, "out_nat(X) :- taglist(L), member(X, L).\n",
                   "but Natural") == -1)
        PASS(); else FAIL("member elem conflict not rejected");

    TEST("reject: TOK_LIST literal in list builtin");
    if (check_prog(&s, "out_list(R) :- cons(R, x, [a,b]).\n", "list literal") == -1)
        PASS(); else FAIL("list literal in cons not rejected");

    TEST("reject: range still not typed");
    if (check_prog(&s, "out_nat(A) :- str1(X), range(A, str1, X, Y).\n",
                   "range") == -1)
        PASS(); else FAIL("range unexpectedly typed");
}

static void test_optional_enum(void)
{
    dl_schema s;
    build_param_schema(&s);

    /* Optional<Text> in =/relational (equality/print type). */
    TEST("accept: Optional<Text> column round-trips through a var");
    if (check_prog(&s, "out_opt(X) :- maybe(X).\n", NULL) == 0)
        PASS(); else FAIL("Optional relational rejected");

    TEST("accept: Optional<Text> = Optional<Text>");
    if (check_prog(&s, "out_opt(X) :- maybe(Y), X = Y.\n", NULL) == 0)
        PASS(); else FAIL("Optional equality rejected");

    TEST("reject: Optional not orderable (<)");
    if (check_prog(&s, "out_nat(A) :- maybe(X), maybe(Y), X < Y.\n", NULL) == -1)
        PASS(); else FAIL("Optional ordering not rejected");

    /* Enum in =/relational. */
    TEST("accept: Enum column round-trips through a var");
    if (check_prog(&s, "color_out(X) :- color(X).\n", NULL) == 0)
        PASS(); else FAIL("Enum relational rejected");

    TEST("accept: Enum = Enum");
    if (check_prog(&s, "color_out(X) :- color(Y), X = Y.\n", NULL) == 0)
        PASS(); else FAIL("Enum equality rejected");

    TEST("reject: Enum not orderable (<)");
    if (check_prog(&s, "out_nat(A) :- color(X), color(Y), X < Y.\n", NULL) == -1)
        PASS(); else FAIL("Enum ordering not rejected");

    TEST("accept: List = List (structural equality via handle)");
    if (check_prog(&s, "out_list(X) :- taglist(Y), X = Y.\n", NULL) == 0)
        PASS(); else FAIL("List equality rejected");
}

static void test_inequality(void)
{
    dl_schema s;
    memset(&s, 0, sizeof(s));
    {
        dl_colspec c[] = { {.tag=DLT_NATURAL} };
        assert(dl_schema_add(&s, "val", 1, c, 0) == 0);
    }
    {
        dl_colspec c[] = { {.tag=DLT_TEXT} };
        assert(dl_schema_add(&s, "lab", 1, c, 0) == 0);
    }
    {
        dl_colspec c[] = { {.tag=DLT_TEXT}, {.tag=DLT_TEXT} };
        assert(dl_schema_add(&s, "tt", 2, c, 0) == 0);
    }
    {
        dl_colspec c[] = { {.tag=DLT_NATURAL} };
        assert(dl_schema_add(&s, "r", 1, c, 0) == 0);
    }

    /* `!=` is a raw u32 inequality (type-agnostic): a Natural var compared
     * to a symbol constant, and Text != Text, are both VALID programs. */
    TEST("accept: != with symbol constant (Natural var)");
    if (check_prog(&s, "r(X) :- val(X), X != foo.\n", NULL) == 0)
        PASS();
    else
        FAIL("X != symbol rejected (should accept)");

    TEST("accept: Text != Text");
    if (check_prog(&s, "tt(X,Y) :- lab(X), lab(Y), X != Y.\n", NULL) == 0)
        PASS();
    else
        FAIL("Text != Text rejected (should accept)");

    /* Ordering on Text is still rejected (v1 raw-cmp id order is meaningless). */
    TEST("reject: Text ordering < still rejected");
    if (check_prog(&s, "tt(X,Y) :- lab(X), lab(Y), X < Y.\n",
                   "variable X is Natural") == -1)
        PASS();
    else
        FAIL("Text ordering < not rejected");
}

static void test_list_assignment_v1(void)
{
    dl_schema s;
    build_schema(&s);

    /* List assignment `[X|Xs] = L` must be rejected EXPLICITLY, not silently
     * mishandled (pattern vars skipped / misleading 'untyped variable L'). */
    TEST("reject: list assignment not yet typed");
    if (check_prog(&s, "bar(X,Y) :- edge(X,Y), [H|T] = L.\n",
                   "list assignment") == -1)
        PASS();
    else
        FAIL("list assignment not rejected");
}

static void test_untyped_var(void)
{
    dl_schema s;
    build_schema(&s);

    /* W appears only in a plain equality X = W with neither side typed by any
     * column; it stays untyped -> error. */
    TEST("reject: untyped variable");
    if (check_prog(&s, "bar(X,Y) :- edge(X,Z), W = Q.\n", "untyped variable") == -1)
        PASS();
    else
        FAIL("untyped variable not rejected");
}

/* ─── Stage A: flat-scalar column type rules ──────────────────────────── */

/* Schema with one column per new scalar type (all EDB, arity 1). */
static void build_scalar_schema(dl_schema *s)
{
    memset(s, 0, sizeof(*s));
    {
        dl_colspec c[] = { {.tag=DLT_DATE} };
        assert(dl_schema_add(s, "d1", 1, c, 0) == 0);
    }
    {
        dl_colspec c[] = { {.tag=DLT_TIMESTAMP} };
        assert(dl_schema_add(s, "ts1", 1, c, 0) == 0);
    }
    {
        dl_colspec c[] = { {.tag=DLT_BOOL} };
        assert(dl_schema_add(s, "b1", 1, c, 0) == 0);
    }
    {
        dl_colspec c[] = { {.tag=DLT_CHAR} };
        assert(dl_schema_add(s, "ch1", 1, c, 0) == 0);
    }
    {
        dl_colspec c[] = { {.tag=DLT_SIGNED} };
        assert(dl_schema_add(s, "sg1", 1, c, 0) == 0);
    }
    {
        dl_colspec c[] = { {.tag=DLT_DATE} };  /* Date-typed aggregate result */
        assert(dl_schema_add(s, "date_out", 1, c, 0) == 0);
    }
    {
        dl_colspec c[] = { {.tag=DLT_NATURAL} };
        assert(dl_schema_add(s, "nat_out", 1, c, 0) == 0);
    }
}

static void test_scalar_ordering(void)
{
    dl_schema s;
    build_scalar_schema(&s);

    /* Ordering comparisons accept the orderable scalars (same type both sides). */
    TEST("accept: Date < Date");
    if (check_prog(&s, "nat_out(A) :- d1(X), d1(Y), X < Y.\n", NULL) == 0)
        PASS(); else FAIL("Date ordering rejected");

    TEST("accept: Timestamp <= Timestamp");
    if (check_prog(&s, "nat_out(A) :- ts1(X), ts1(Y), X <= Y.\n", NULL) == 0)
        PASS(); else FAIL("Timestamp ordering rejected");

    TEST("accept: Bool ordering (same type)");
    if (check_prog(&s, "nat_out(A) :- b1(X), b1(Y), X < Y.\n", NULL) == 0)
        PASS(); else FAIL("Bool ordering rejected");

    TEST("accept: Char ordering (same type)");
    if (check_prog(&s, "nat_out(A) :- ch1(X), ch1(Y), X > Y.\n", NULL) == 0)
        PASS(); else FAIL("Char ordering rejected");

    /* Signed is NOT orderable (zigzag breaks u32 order) -> reject. */
    TEST("reject: Signed ordering <");
    if (check_prog(&s, "nat_out(A) :- sg1(X), sg1(Y), X < Y.\n", NULL) == -1)
        PASS(); else FAIL("Signed ordering not rejected");

    /* Arithmetic is Natural-only: a Signed var in `A = X + 1` must be rejected. */
    TEST("reject: arithmetic over Signed");
    if (check_prog(&s, "nat_out(A) :- sg1(X), A = X + 1.\n", NULL) == -1)
        PASS(); else FAIL("Signed arithmetic not rejected");
}

static void test_scalar_minmax(void)
{
    dl_schema s;
    build_scalar_schema(&s);

    /* min/max over Date: result takes the operand's (Date) type. */
    TEST("accept: min over Date (result Date)");
    if (check_prog(&s, "date_out(M) :- d1(X), M = min(X).\n", NULL) == 0)
        PASS(); else FAIL("min over Date rejected");

    TEST("accept: max over Date (result Date)");
    if (check_prog(&s, "date_out(M) :- d1(X), M = max(X).\n", NULL) == 0)
        PASS(); else FAIL("max over Date rejected");

    /* min/max over Signed must be rejected (not orderable). */
    TEST("reject: min over Signed");
    if (check_prog(&s, "nat_out(M) :- sg1(X), M = min(X).\n", NULL) == -1)
        PASS(); else FAIL("min over Signed not rejected");

    /* min/max over Bool must be rejected (only Natural/Timestamp/Date). */
    TEST("reject: min over Bool");
    if (check_prog(&s, "nat_out(M) :- b1(X), M = min(X).\n", NULL) == -1)
        PASS(); else FAIL("min over Bool not rejected");
}

int main(void)
{
    printf("=== Dhall rule typechecker tests ===\n");
    test_positive();
    test_int_text_mixing();
    test_undeclared_relation();
    test_text_comparison();
    test_aggregate_over_text();
    test_int_const_in_text_col();
    test_arity_mismatch();
    test_reserved_builtin_head();
    test_inequality();
    test_list_assignment_v1();
    test_untyped_var();
    test_scalar_ordering();
    test_scalar_minmax();
    test_list_builtins();
    test_optional_enum();

    printf("\n%d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
