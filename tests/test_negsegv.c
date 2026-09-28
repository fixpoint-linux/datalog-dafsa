/*
 * test_negsegv.c — regression: dl_query_rules_ro must survive a compile
 * failure cleanly (return -1, no crash, db still usable).
 *
 * The pinned root cause: compile_rules published *out_n INCREMENTALLY but
 * *out_rules only at the very end, so a mid-program compile error (here:
 * unsafe negation in excl_w after two good rules) left n_crules>0 with
 * crules==NULL.  dl_query_rules_ro's cleanup loop then dereferenced
 * crules.? with i<n_crules — SIGSEGV in ReleaseFast, "attempt to use null
 * value" panic in Debug — and the partial array + compiled rules leaked.
 *
 * Fix under test: compile_rules now publishes the out-params ONLY on
 * success (null/0 on every failure path, partials freed internally), and
 * the rules_ro cleanup loop is null-guarded as defence in depth.
 *
 * Checks:
 *   (a) the repro: good query first, then the failing one -> -1, cleanly;
 *   (b) the db REMAINS USABLE: a later successful query on the same db
 *       still answers the correct tuples;
 *   (c) control: the same compile failure with NO prior successful query
 *       also returns -1 cleanly (and the still db answers afterwards).
 */

#include "dl.h"

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

#define CHECK(cond, msg) do { \
    if (!(cond)) { FAIL(msg); return; } \
} while(0)

/* ─── tuple collection (mirrors test_wfs.c) ──────────────────────────── */

typedef struct {
    uint32_t *data;
    long      count;
    long      cap;
    uint8_t   arity;
} tset;

static int tset_cb(const uint32_t *cols, uint8_t arity, void *user)
{
    tset *t = (tset *)user;
    if (t->arity == 0) t->arity = arity;
    if (arity != t->arity) return 1;
    if (t->count >= t->cap) {
        long nc = t->cap ? t->cap * 2 : 64;
        uint32_t *nd = realloc(t->data,
            (size_t)nc * (size_t)t->arity * sizeof(uint32_t));
        if (!nd) return 1;
        t->data = nd;
        t->cap = nc;
    }
    memcpy(t->data + (size_t)t->count * (size_t)t->arity,
           cols, (size_t)t->arity * sizeof(uint32_t));
    t->count++;
    return 0;
}

static void tset_reset(tset *t) { t->count = 0; }

static void tset_free(tset *t)
{
    free(t->data);
    t->data = NULL;
    t->cap = 0;
    t->count = 0;
}

/* ─── db helpers (one db per scenario, like test_wfs.c) ──────────────── */

static dl_db *g_db;

static void setup(const char *tag)
{
    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf build-tmp/negsegv_%s", tag);
    system(cmd);
    g_db = dl_open(cmd + sizeof "rm -rf " - 1);
    if (!g_db) { fprintf(stderr, "fatal: dl_open failed\n"); exit(2); }
}

static void teardown(const char *tag)
{
    char cmd[128];
    dl_close(g_db);
    snprintf(cmd, sizeof cmd, "rm -rf build-tmp/negsegv_%s", tag);
    system(cmd);
}

static void load_facts(void)
{
    uint32_t p1, c1, plays_row[2], elig_row[1];

    if (dl_declare_relation(g_db, "plays", 2) != 0 ||
        dl_declare_relation(g_db, "eligible", 1) != 0 ||
        dl_declare_relation(g_db, "enrolled", 1) != 0 ||
        dl_declare_relation(g_db, "beaten", 1) != 0) {
        fprintf(stderr, "fatal: declare failed\n");
        exit(2);
    }
    p1 = dl_intern_str(g_db, "p1");
    c1 = dl_intern_str(g_db, "c1");
    plays_row[0] = p1; plays_row[1] = c1;
    elig_row[0] = p1;
    if (dl_add_fact(g_db, "plays", plays_row, 2) < 0 ||
        dl_add_fact(g_db, "eligible", elig_row, 1) < 0) {
        fprintf(stderr, "fatal: add_fact failed\n");
        exit(2);
    }
}

/* The two programs from the pinned repro.  BAD_PROG appends a rule whose
 * negated atom uses DW1, a variable bound only inside the (fine)
 * enrolled_w rule — unsafe negation, rejected mid-program AFTER two good
 * rules compiled, which is exactly the window the old incremental out_n
 * publication left inconsistent. */
static const char *GOOD_PROG =
    "eligible_w(V1, DW1) :- eligible(V1), DW1 = 1.\n"
    "enrolled_w(V4, DD) :- plays(V4, V5), eligible_w(V4, DW1), "
    "!beaten(V5), DD = DW1 + 1.\n";

static const char *BAD_PROG =
    "eligible_w(V1, DW1) :- eligible(V1), DW1 = 1.\n"
    "enrolled_w(V4, DD) :- plays(V4, V5), eligible_w(V4, DW1), "
    "!beaten(V5), DD = DW1 + 1.\n"
    "excl_w(V1, DD) :- plays(V1, V2), !enrolled_w(V1, DW1), "
    "DD = DW1 + 1.\n";

/* (a)+(b) the repro: good query, failing query -> -1 cleanly, db usable */
static void test_repro_then_usable(void)
{
    tset res;
    long n;

    TEST("negsegv: good query answers, then unsafe-negation compile "
         "failure returns -1 cleanly");

    setup("repro");
    load_facts();

    memset(&res, 0, sizeof res);
    n = dl_query_rules_ro(g_db, GOOD_PROG, "enrolled_w", tset_cb, &res);
    /* eligible_w(p1,1), beaten(c1) empty -> enrolled_w = {(p1, 2)}:
     * DD = DW1 + 1 = 1 + 1, p1 is the first interned symbol (id 1). */
    CHECK(n == 1 && res.count == 1 && res.data[0] == 1 && res.data[1] == 2,
          "good query did not answer (1,2)");
    tset_free(&res);

    /* the crash: same db, program with the unsafe-negation tail.
     * Must return -1 WITHOUT crashing (the old build SIGSEGV'd here). */
    n = dl_query_rules_ro(g_db, BAD_PROG, "excl_w", tset_cb, &res);
    CHECK(n == -1, "unsafe-negation program did not return -1");

    /* (b) the db is still usable: the good program still answers. */
    tset_reset(&res);
    n = dl_query_rules_ro(g_db, GOOD_PROG, "enrolled_w", tset_cb, &res);
    CHECK(n == 1 && res.count == 1 && res.data[0] == 1 && res.data[1] == 2,
          "db unusable after failed compile");
    tset_free(&res);

    teardown("repro");
    PASS();
}

/* (c) control: compile failure with NO prior successful query */
static void test_failure_without_prior_query(void)
{
    tset res;
    long n;

    TEST("negsegv: compile failure with no prior query returns -1 cleanly");

    setup("ctrl");
    load_facts();

    memset(&res, 0, sizeof res);
    n = dl_query_rules_ro(g_db, BAD_PROG, "excl_w", tset_cb, &res);
    CHECK(n == -1, "unsafe-negation program did not return -1");

    /* db still answers afterwards */
    n = dl_query_rules_ro(g_db, GOOD_PROG, "enrolled_w", tset_cb, &res);
    CHECK(n == 1 && res.count == 1 && res.data[0] == 1 && res.data[1] == 2,
          "db unusable after failed compile");
    tset_free(&res);

    teardown("ctrl");
    PASS();
}

int main(void)
{
    printf("test_negsegv: dl_query_rules_ro compile-failure isolation\n");
    test_repro_then_usable();
    test_failure_without_prior_query();
    printf("\n%d/%d tests passed\n", tests_run - tests_failed, tests_run);
    return tests_failed ? 1 : 0;
}
