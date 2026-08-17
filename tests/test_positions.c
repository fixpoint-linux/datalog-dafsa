/*
 * test_positions.c — S1 Dhall-schema: token/atom line:col position fields.
 *
 * Parses a multi-line Datalog snippet and asserts that a known set of tokens
 * (as atoms and atom argument tokens) carry the correct 1-based line/col.
 * The line/col fields are additive; this test guards the offset -> line/col
 * derivation used by the future rule typechecker for `file:line:col` errors.
 */

#include "parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static int check_pos(const char *what, int got_line, int got_col,
                     int exp_line, int exp_col)
{
    if (got_line != exp_line || got_col != exp_col) {
        printf("    %s: got line=%d col=%d, expected line=%d col=%d\n",
               what, got_line, got_col, exp_line, exp_col);
        return 0;
    }
    return 1;
}

static void test_rule_positions(void)
{
    /* Layout (offsets annotated per line, 0-based byte offsets):
     *   line1: "edge(a,b).\n"        -> edge@0(c1) a@5(c6) b@7(c8)
     *   line2: "path(X,Y) :- edge(X,Y).\n"
     *        path@12(c1) body edge@25(c14)
     *   line3: "path(X,Y) :- edge(X,Z),\n"
     *        path@36(c1) body edge@49(c14)
     *   line4: "             path(Z,Y).\n"
     *        body path@73(c14)
     */
    const char *src =
        "edge(a,b).\n"
        "path(X,Y) :- edge(X,Y).\n"
        "path(X,Y) :- edge(X,Z),\n"
        "             path(Z,Y).\n";

    parser *p = parse_create(src);
    int n = 0;
    rule **rules = parse_rules(p, &n);

    TEST("rule head/body positions on multi-line source");

    if (!rules || n != 3) {
        FAIL("expected 3 rules parsed");
        if (rules) { int i; for (i = 0; i < n; i++) rule_free(rules[i]); free(rules); }
        parse_free(p);
        return;
    }

    int ok = 1;

    /* rule 0: edge(a,b). */
    if (!check_pos("rule0 head 'edge'", rules[0]->head->line, rules[0]->head->col, 1, 1)) ok = 0;
    if (!check_pos("rule0 arg 'a'",      rules[0]->head->args[0]->line, rules[0]->head->args[0]->col, 1, 6)) ok = 0;
    if (!check_pos("rule0 arg 'b'",      rules[0]->head->args[1]->line, rules[0]->head->args[1]->col, 1, 8)) ok = 0;

    /* rule 1: path(X,Y) :- edge(X,Y). */
    if (!check_pos("rule1 head 'path'", rules[1]->head->line, rules[1]->head->col, 2, 1)) ok = 0;
    if (!check_pos("rule1 body edge",   rules[1]->body[0]->line, rules[1]->body[0]->col, 2, 14)) ok = 0;

    /* rule 2: path(X,Y) :- edge(X,Z), path(Z,Y). */
    if (!check_pos("rule2 head 'path'", rules[2]->head->line, rules[2]->head->col, 3, 1)) ok = 0;
    if (!check_pos("rule2 body edge",   rules[2]->body[0]->line, rules[2]->body[0]->col, 3, 14)) ok = 0;
    if (!check_pos("rule2 body path",   rules[2]->body[1]->line, rules[2]->body[1]->col, 4, 14)) ok = 0;

    if (!ok) FAIL("position mismatch");
    else PASS();

    { int i; for (i = 0; i < n; i++) rule_free(rules[i]); free(rules); }
    parse_free(p);
}

static void test_empty_src(void)
{
    /* Empty / single-line edge cases must not crash and report sane values. */
    TEST("empty and single-line sources");
    int ok = 1;

    {
        parser *p = parse_create("edge(a,b).");
        int n = 0;
        rule **rules = parse_rules(p, &n);
        if (!rules || n != 1) ok = 0;
        else if (!check_pos("single-line head", rules[0]->head->line,
                            rules[0]->head->col, 1, 1)) ok = 0;
        if (rules) { int i; for (i = 0; i < n; i++) rule_free(rules[i]); free(rules); }
        parse_free(p);
    }
    {
        parser *p = parse_create("");
        int n = 0;
        rule **rules = parse_rules(p, &n);
        if (rules) { int i; for (i = 0; i < n; i++) rule_free(rules[i]); free(rules); }
        parse_free(p);
    }

    if (!ok) FAIL("single-line/empty failure");
    else PASS();
}

int main(void)
{
    printf("Parser position tests\n");
    printf("=====================\n\n");

    test_rule_positions();
    test_empty_src();

    printf("\n---\n");
    printf("%d tests run, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
