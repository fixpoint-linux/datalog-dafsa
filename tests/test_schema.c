/*
 * test_schema.c — S2 Dhall typed-schema tests: dl_schema builder + lookup +
 * dl_attach_schema no-op hook.
 *
 * Tests:
 *   1. dl_schema_add: success path (arity 1..8), name + cols copied, is_idb set.
 *   2. Validation: dup name, arity 0, arity 9, NULL name/cols rejected.
 *   3. dl_schema_find: found / not-found / NULL args.
 *   4. dl_attach_schema: NULL db -> -1; attach NULL (detach) ok.
 *   5. Attach a real schema, then dl_load_rules a well-typed tiny program
 *      compiles (the S3 typechecker runs against the attached schema).
 *   6. Detach (attach NULL) and load again.
 *
 * Standalone, links libdatalog.so.  Uses build-tmp for the scratch db.
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
} while (0)

#define PASS() do { printf("OK\n"); } while (0)
#define FAIL(msg) do { \
    printf("FAIL: %s\n", msg); \
    tests_failed++; \
} while (0)

/* ─── Test 1: builder success path ────────────────────────────────────── */

static void test_add_success(void)
{
    dl_schema s;
    memset(&s, 0, sizeof(s));

    {
        dl_coltype cols[] = { DLT_NATURAL, DLT_TEXT };
        TEST("dl_schema_add arity-2 name+cols+is_idb");
        if (dl_schema_add(&s, "edge", 2, cols, 0) != 0) {
            FAIL("add edge returned nonzero");
        } else {
            if (s.n_rels != 1) { FAIL("n_rels != 1"); }
            else {
                const dl_reldef *r = &s.rels[0];
                if (strcmp(r->name, "edge") != 0) FAIL("name not copied");
                else if (r->arity != 2) FAIL("arity not stored");
                else if (r->is_idb != 0) FAIL("is_idb should be 0");
                else if (r->cols[0] != DLT_NATURAL) FAIL("cols[0] wrong");
                else if (r->cols[1] != DLT_TEXT) FAIL("cols[1] wrong");
                else PASS();
            }
        }
    }

    {
        dl_coltype cols[] = { DLT_TEXT, DLT_TEXT, DLT_TEXT };
        TEST("dl_schema_add arity-3 is_idb=1");
        if (dl_schema_add(&s, "path", 3, cols, 1) != 0) {
            FAIL("add path returned nonzero");
        } else {
            const dl_reldef *r = &s.rels[1];
            if (strcmp(r->name, "path") != 0) FAIL("name not copied");
            else if (r->arity != 3) FAIL("arity not stored");
            else if (r->is_idb != 1) FAIL("is_idb should be 1");
            else if (r->cols[2] != DLT_TEXT) FAIL("cols[2] wrong");
            else PASS();
        }
    }

    /* Longer name is truncated + NUL-terminated. */
    {
        dl_coltype cols[] = { DLT_NATURAL };
        char longname[128];
        memset(longname, 'x', sizeof(longname) - 1);
        longname[sizeof(longname) - 1] = '\0';
        TEST("dl_schema_add long-name truncation");
        if (dl_schema_add(&s, longname, 1, cols, 0) != 0) {
            FAIL("add long name returned nonzero");
        } else {
            const dl_reldef *r = &s.rels[2];
            if (r->name[sizeof(r->name) - 1] != '\0') FAIL("not NUL-terminated");
            else if (strlen(r->name) != sizeof(r->name) - 1) FAIL("not truncated");
            else PASS();
        }
    }
}

/* ─── Test 2: validation rejects ──────────────────────────────────────── */

static void test_add_rejects(void)
{
    dl_schema s;
    memset(&s, 0, sizeof(s));
    {
        dl_coltype cols[] = { DLT_NATURAL };
        if (dl_schema_add(&s, "a", 1, cols, 0) != 0) { FAIL("seed add failed"); return; }
    }

    TEST("dup name rejected");
    { dl_coltype c = DLT_NATURAL; if (dl_schema_add(&s, "a", 1, &c, 0) != 0) PASS(); else FAIL("dup accepted"); }

    TEST("arity 0 rejected");
    { dl_coltype c = DLT_NATURAL; if (dl_schema_add(&s, "b", 0, &c, 0) != 0) PASS(); else FAIL("arity 0 accepted"); }

    TEST("arity 9 rejected");
    { dl_coltype c = DLT_NATURAL; if (dl_schema_add(&s, "b", 9, &c, 0) != 0) PASS(); else FAIL("arity 9 accepted"); }

    TEST("NULL name rejected");
    { dl_coltype c = DLT_NATURAL; if (dl_schema_add(&s, NULL, 1, &c, 0) != 0) PASS(); else FAIL("NULL name accepted"); }

    TEST("NULL cols rejected");
    if (dl_schema_add(&s, "b", 1, NULL, 0) != 0) PASS(); else FAIL("NULL cols accepted");

    TEST("NULL schema rejected");
    { dl_coltype c = DLT_NATURAL; if (dl_schema_add(NULL, "b", 1, &c, 0) != 0) PASS(); else FAIL("NULL schema accepted"); }
}

/* ─── Test 3: find ────────────────────────────────────────────────────── */

static void test_find(void)
{
    dl_schema s;
    memset(&s, 0, sizeof(s));
    {
        dl_coltype cols[] = { DLT_NATURAL, DLT_TEXT };
        if (dl_schema_add(&s, "edge", 2, cols, 0) != 0) { FAIL("seed add failed"); return; }
    }

    TEST("find existing");
    {
        const dl_reldef *r = dl_schema_find(&s, "edge");
        if (r && r->arity == 2 && r->cols[1] == DLT_TEXT) PASS(); else FAIL("find wrong result");
    }

    TEST("find missing -> NULL");
    if (dl_schema_find(&s, "nope") == NULL) PASS(); else FAIL("find missing returned non-NULL");

    TEST("find NULL name -> NULL");
    if (dl_schema_find(&s, NULL) == NULL) PASS(); else FAIL("find NULL name returned non-NULL");

    TEST("find NULL schema -> NULL");
    if (dl_schema_find(NULL, "edge") == NULL) PASS(); else FAIL("find NULL schema returned non-NULL");
}

/* ─── Test 4: attach + hook is a no-op on load ────────────────────────── */

static void test_attach_hook(void)
{
    dl_schema s;
    memset(&s, 0, sizeof(s));
    {
        dl_coltype cols[] = { DLT_NATURAL, DLT_NATURAL };
        if (dl_schema_add(&s, "edge", 2, cols, 0) != 0) { FAIL("schema seed failed"); return; }
    }
    {
        dl_coltype cols[] = { DLT_NATURAL, DLT_NATURAL };
        if (dl_schema_add(&s, "reach", 2, cols, 1) != 0) { FAIL("schema seed failed"); return; }
    }

    system("rm -rf build-tmp/schema");

    dl_db *db = dl_open("build-tmp/schema");
    assert(db);

    TEST("attach NULL db -> -1");
    if (dl_attach_schema(NULL, &s) == -1) PASS(); else FAIL("NULL db not rejected");

    TEST("attach NULL schema (detach) ok");
    if (dl_attach_schema(db, NULL) == 0) PASS(); else FAIL("attach NULL returned nonzero");

    /* Load with no schema attached: baseline compile. */
    assert(dl_declare_relation(db, "edge", 2) == 0);
    assert(dl_load_rules(db, "reach(X,Y) :- edge(X,Y).\n") == 0);

    /* Attach a real schema; the S3 typechecker must accept this well-typed
     * program (reach and edge are both declared in the schema). */
    TEST("attach real schema, load well-typed program compiles");
    if (dl_attach_schema(db, &s) != 0) FAIL("attach returned nonzero");
    else if (dl_load_rules(db, "reach(X,Y) :- edge(X,Y).\n") == 0) PASS();
    else FAIL("load with schema attached failed");

    /* Detach, load again. */
    TEST("detach (attach NULL) then load again");
    if (dl_attach_schema(db, NULL) != 0) FAIL("detach returned nonzero");
    else if (dl_load_rules(db, "reach(X,Y) :- edge(X,Y).\n") == 0) PASS();
    else FAIL("load after detach failed");

    dl_close(db);
    system("rm -rf build-tmp/schema");
}

int main(void)
{
    printf("=== Dhall schema tests ===\n");
    test_add_success();
    test_add_rejects();
    test_find();
    test_attach_hook();

    printf("\n%d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
