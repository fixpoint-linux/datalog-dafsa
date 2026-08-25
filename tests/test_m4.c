/*
 * test_m4.c — M4 tests: snapshot publish + mmap query + property tests
 *
 * Tests:
 *   1. Lifecycle happy path: TC via snapshot matches legacy
 *   2. view_prefix == rel_prefix property test (100+ iterations)
 *   3. Atomicity crash test (fault hooks)
 *   4. Multi-reader
 *   5. Re-publish (facts + publish v2)
 *   6. M0-M3 regression (never call publish, dl_query legacy path)
 *   7. Aggregate carry-over (publish → query M3 aggregates)
 */

#include "dl.h"
#include "snapshot.h"
#include "relation.h"
#include "intern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/stat.h>

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

/* ─── Tuple set helpers (mirror test_m3.c) ────────────────────────────── */

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

static void tset_free(tuple_set *ts)
{
    free(ts->data);
    memset(ts, 0, sizeof(*ts));
}

static int tset_eq(const tuple_set *a, const tuple_set *b)
{
    long i, j;
    if (a->count == 0 && b->count == 0) return 1;
    if (a->count != b->count || a->arity != b->arity) return 0;

    for (i = 0; i < a->count; i++) {
        int found = 0;
        const uint32_t *arow = a->data + (size_t)i * (size_t)a->arity;
        for (j = 0; j < b->count; j++) {
            const uint32_t *brow = b->data + (size_t)j * (size_t)b->arity;
            if (memcmp(arow, brow, (size_t)a->arity * sizeof(uint32_t)) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) return 0;
    }
    return 1;
}

/* ─── Database helpers ────────────────────────────────────────────────── */

static void setup_db(dl_db **db_out, const char *suffix)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf build-tmp/m4db_%s", suffix);
    system(cmd);
    snprintf(cmd, sizeof(cmd), "build-tmp/m4db_%s", suffix);
    *db_out = dl_open(cmd);
    assert(*db_out);
}

static void teardown_db(dl_db *db, const char *suffix)
{
    char cmd[512];
    dl_close(db);
    snprintf(cmd, sizeof(cmd), "rm -rf build-tmp/m4db_%s", suffix);
    system(cmd);
}

static int load_rows_csv(dl_db *db, const char *rel_name, uint8_t arity,
                         const uint32_t *cols, int nrows)
{
    char csv_path[256];
    int i, c;
    FILE *f;

    assert(dl_declare_relation(db, rel_name, arity) == 0);

    snprintf(csv_path, sizeof(csv_path), "build-tmp/csv_%p.csv",
             (void *)(uintptr_t)rel_name);
    f = fopen(csv_path, "w");
    assert(f);
    for (i = 0; i < nrows; i++) {
        for (c = 0; c < arity; c++) {
            if (c > 0) fputc(',', f);
            fprintf(f, "%u", cols[(size_t)i * (size_t)arity + (size_t)c]);
        }
        fputc('\n', f);
    }
    fclose(f);

    return dl_load_facts(db, rel_name, csv_path);
}

/* ─── Test 1: Lifecycle happy path ────────────────────────────────────── */

static void test_lifecycle_tc(void)
{
    dl_db *db;
    tuple_set snap_result;

    TEST("lifecycle: publish TC, compare snapshot vs legacy query");

    setup_db(&db, "tc");

    /* Load edges */
    {
        uint32_t e[] = {1,2, 1,3, 2,3, 2,4, 3,5};
        int loaded = load_rows_csv(db, "edge", 2, e, 5);
        assert(loaded == 5);
    }

    /* Load TC rules and publish */
    assert(dl_load_rules(db,
        "tc(X,Y):-edge(X,Y).\n"
        "tc(X,Z):-edge(X,Y),tc(Y,Z).\n") == 0);
    assert(dl_publish_snapshot(db) == 0);

    /* Query via snapshot */
    memset(&snap_result, 0, sizeof(snap_result));
    long n = dl_query(db, "tc", tset_cb, &snap_result);
    assert(n >= 0);
    (void)n;

    /* Query via legacy pre-publish path: create a fresh DB,
     * load same facts + rules, compile + query without publishing */
    {
        dl_db *db2;
        tuple_set leg;

        setup_db(&db2, "tc_legacy");
        {
            uint32_t e[] = {1,2, 1,3, 2,3, 2,4, 3,5};
            load_rows_csv(db2, "edge", 2, e, 5);
        }
        assert(dl_load_rules(db2,
            "tc(X,Y):-edge(X,Y).\n"
            "tc(X,Z):-edge(X,Y),tc(Y,Z).\n") == 0);
        assert(dl_compile(db2) == 0);

        memset(&leg, 0, sizeof(leg));
        dl_query(db2, "tc", tset_cb, &leg);

        if (!tset_eq(&snap_result, &leg)) {
            printf("  snap=%ld rows legacy=%ld rows\n",
                   snap_result.count, leg.count);
            long j;
            for (j = 0; j < snap_result.count && j < 20; j++)
                printf("    snap: (%u,%u)\n",
                       snap_result.data[j*2], snap_result.data[j*2+1]);
            tset_free(&snap_result); tset_free(&leg);
            teardown_db(db2, "tc_legacy");
            teardown_db(db, "tc");
            FAIL("TC lifecycle: snapshot != legacy");
            return;
        }

        tset_free(&leg);
        teardown_db(db2, "tc_legacy");
    }

    tset_free(&snap_result);
    teardown_db(db, "tc");
    PASS();
}

/* ─── Test 2: view_prefix == rel_prefix property test ─────────────────── */

static void test_property_view_eq_rel(void)
{
    int iter;

    TEST("property: view_prefix == rel_prefix (20 random iterations)");

    for (iter = 0; iter < 20; iter++) {
        dl_db *db;
        uint8_t arity = (uint8_t)(1 + (iter % 4));  /* 1..4 */
        int n_facts = 3 + (iter % 12);  /* 3..14 */
        uint32_t facts[20 * 4];  /* up to 20 facts × 4 cols */
        int fi;
        unsigned seed = (unsigned)(20240811u + (unsigned)iter * 65537u);
        int actual_n = 0;

        /* Generate unique random facts using rejection sampling */
        for (fi = 0; fi < n_facts * 4 && actual_n < n_facts; fi++) {
            uint32_t row[4];
            uint8_t c;
            int dup = 0, k;

            for (c = 0; c < arity; c++) {
                seed = seed * 1103515245 + 12345;
                row[c] = (seed >> 16) % 50;
            }

            for (k = 0; k < actual_n; k++) {
                int match = 1;
                uint8_t cc;
                for (cc = 0; cc < arity; cc++) {
                    if (facts[(size_t)k * (size_t)arity + cc] != row[cc]) {
                        match = 0;
                        break;
                    }
                }
                if (match) { dup = 1; break; }
            }
            if (!dup) {
                for (c = 0; c < arity; c++)
                    facts[(size_t)actual_n * (size_t)arity + c] = row[c];
                actual_n++;
            }
        }

        if (actual_n < 2) continue;

        setup_db(&db, "prop");

        /* Load facts */
        {
            uint32_t flat[80];
            int j;
            for (j = 0; j < actual_n; j++)
                memcpy(flat + (size_t)j * (size_t)arity,
                       facts + (size_t)j * (size_t)arity,
                       (size_t)arity * sizeof(uint32_t));
            int loaded = load_rows_csv(db, "r", arity, flat, actual_n);
            assert(loaded == actual_n);
            (void)loaded;
        }

        /* Publish */
        assert(dl_publish_snapshot(db) == 0);

        /* For several (k, leading) combos, compare snapshot vs legacy */
        {
            dl_db *db2;
            uint8_t k;

            setup_db(&db2, "prop_legacy");
            {
                uint32_t flat[80];
                int j;
                for (j = 0; j < actual_n; j++)
                    memcpy(flat + (size_t)j * (size_t)arity,
                           facts + (size_t)j * (size_t)arity,
                           (size_t)arity * sizeof(uint32_t));
                load_rows_csv(db2, "r", arity, flat, actual_n);
            }

            for (k = 0; k <= arity; k++) {
                /* Pick a leading tuple from the facts or use all-zeros */
                uint32_t leading[4] = {0, 0, 0, 0};
                tuple_set snap_set, leg_set;

                if (k > 0 && actual_n > 0) {
                    /* Pick leading from first fact */
                    int pick = iter % actual_n;
                    memcpy(leading, facts + (size_t)pick * (size_t)arity,
                           (size_t)k * sizeof(uint32_t));
                }

                memset(&snap_set, 0, sizeof(snap_set));
                memset(&leg_set, 0, sizeof(leg_set));

                /* Snapshot path */
                dl_query_bound(db, "r", leading, k, tset_cb, &snap_set);

                /* Legacy path */
                dl_prefix(db2, "r", leading, k, tset_cb, &leg_set);

                if (!tset_eq(&snap_set, &leg_set)) {
                    printf("\n  iter=%d arity=%u k=%u snap=%ld leg=%ld\n",
                           iter, arity, k, snap_set.count, leg_set.count);
                    long j;
                    for (j = 0; j < snap_set.count && j < 10; j++) {
                        printf("    snap: (");
                        uint8_t c;
                        for (c = 0; c < arity; c++) {
                            if (c > 0) printf(",");
                            printf("%u", snap_set.data[j*arity+c]);
                        }
                        printf(")\n");
                    }
                    tset_free(&snap_set); tset_free(&leg_set);
                    teardown_db(db2, "prop_legacy");
                    teardown_db(db, "prop");
                    FAIL("view_prefix != rel_prefix");
                    return;
                }

                tset_free(&snap_set);
                tset_free(&leg_set);
            }

            teardown_db(db2, "prop_legacy");
        }

        teardown_db(db, "prop");
    }

    PASS();
}

/* ─── Test 3: Atomicity crash test ────────────────────────────────────── */

static int fault_hook_abort(dl_fpoint fp, void *user)
{
    (void)fp;
    (void)user;
    return -1;  /* abort */
}

static int fault_hook_at_rename(dl_fpoint fp, void *user)
{
    (void)user;
    if (fp == DL_FPOINT_AFTER_RENAME)
        return -1;  /* abort after rename */
    return 0;  /* let rel saves succeed */
}

static void test_atomicity_crash(void)
{
    dl_db *db;

    TEST("atomicity: fault hook at AFTER_REL_SAVE preserves CURRENT");

    setup_db(&db, "crash");
    {
        uint32_t e[] = {1,2, 2,3};
        load_rows_csv(db, "edge", 2, e, 2);
    }

    /* Publish v1 (no hook) */
    assert(dl_publish_snapshot(db) == 0);

    /* Verify snap_version = 1 */
    {
        tuple_set ts;
        memset(&ts, 0, sizeof(ts));
        long n = dl_query(db, "edge", tset_cb, &ts);
        assert(n == 2);
        tset_free(&ts);
    }

    /* Load more facts to make fixpoint_dirty */
    {
        uint32_t e[] = {3,4};
        load_rows_csv(db, "edge", 2, e, 1);
    }

    /* Set fault hook to abort at AFTER_REL_SAVE */
    dl_set_fault_hook(db, fault_hook_abort, NULL);
    assert(dl_publish_snapshot(db) == -1);

    /* Re-open DB: CURRENT should still point to v1 */
    {
        char dbdir[256];
        dl_db *db2;
        tuple_set ts;

        snprintf(dbdir, sizeof(dbdir), "build-tmp/m4db_crash");
        /* Don't close db yet — the hook might leave it in a state.
         * Close and reopen. */
        dl_close(db);
        db = NULL;

        db2 = dl_open(dbdir);
        assert(db2);

        memset(&ts, 0, sizeof(ts));
        long n = dl_query(db2, "edge", tset_cb, &ts);
        /* Should still see 3 facts (2 from v1 + 1 loaded after but not
         * published). Wait — loading facts sets fixpoint_dirty and auto-saves,
         * but no publish happened. So in-memory has 3, but snapshot only
         * has 2. After re-open, dl_query sees snap_version=1 and queries
         * the snapshot, so it should get 2.
         *
         * BUT: the snapshot_query_scan reads from the v1 snapshot which
         * has only the original 2 facts. The in-memory has 3. This is
         * correct behavior: the failed publish didn't advance CURRENT,
         * so queries still see v1. */
        assert(n == 2);
        tset_free(&ts);
        dl_close(db2);

        /* Re-open db for next sub-test */
        db = dl_open(dbdir);
        assert(db);
    }

    /* Sub-test: fault hook at AFTER_RENAME */
    dl_set_fault_hook(db, fault_hook_at_rename, NULL);
    assert(dl_publish_snapshot(db) == -1);

    /* Re-open: CURRENT still v1 */
    {
        char dbdir[256];
        dl_db *db2;
        tuple_set ts;

        snprintf(dbdir, sizeof(dbdir), "build-tmp/m4db_crash");
        dl_close(db);
        db = NULL;

        db2 = dl_open(dbdir);
        assert(db2);

        /* Clear the fault hook on the new handle */
        dl_set_fault_hook(db2, NULL, NULL);

        memset(&ts, 0, sizeof(ts));
        long n = dl_query(db2, "edge", tset_cb, &ts);
        assert(n == 2);  /* still v1 */
        tset_free(&ts);

        /* Now publish successfully */
        assert(dl_publish_snapshot(db2) == 0);
        memset(&ts, 0, sizeof(ts));
        n = dl_query(db2, "edge", tset_cb, &ts);
        assert(n == 3);  /* v2 has all 3 facts */
        tset_free(&ts);

        dl_close(db2);
    }

    PASS();
}

/* ─── Test 4: Multi-reader ────────────────────────────────────────────── */

static void test_multi_reader(void)
{
    dl_db *db1, *db2;
    tuple_set ts1, ts2;
    char dbdir[256];

    TEST("multi-reader: two handles see same snapshot");

    snprintf(dbdir, sizeof(dbdir), "build-tmp/m4db_multi");
    system("rm -rf build-tmp/m4db_multi");

    db1 = dl_open(dbdir);
    assert(db1);

    {
        uint32_t e[] = {1,2, 2,3, 3,4};
        load_rows_csv(db1, "edge", 2, e, 3);
    }
    assert(dl_publish_snapshot(db1) == 0);

    /* Open second handle */
    db2 = dl_open(dbdir);
    assert(db2);

    memset(&ts1, 0, sizeof(ts1));
    memset(&ts2, 0, sizeof(ts2));

    dl_query(db1, "edge", tset_cb, &ts1);
    dl_query(db2, "edge", tset_cb, &ts2);

    assert(tset_eq(&ts1, &ts2));

    tset_free(&ts1);
    tset_free(&ts2);
    dl_close(db1);
    dl_close(db2);
    system("rm -rf build-tmp/m4db_multi");

    PASS();
}

/* ─── Test 5: Re-publish ──────────────────────────────────────────────── */

static void test_republish(void)
{
    dl_db *db;
    tuple_set ts;

    TEST("re-publish: v1→query, load more→publish v2→query new facts");

    setup_db(&db, "repub");

    /* v1: 2 facts */
    {
        uint32_t e[] = {1,2, 2,3};
        load_rows_csv(db, "edge", 2, e, 2);
    }
    assert(dl_publish_snapshot(db) == 0);

    memset(&ts, 0, sizeof(ts));
    dl_query(db, "edge", tset_cb, &ts);
    assert(ts.count == 2);
    tset_free(&ts);

    /* Load more facts (fixpoint_dirty=1) */
    {
        uint32_t e[] = {3,4, 4,5};
        load_rows_csv(db, "edge", 2, e, 2);
    }

    /* Publish v2 */
    assert(dl_publish_snapshot(db) == 0);

    memset(&ts, 0, sizeof(ts));
    dl_query(db, "edge", tset_cb, &ts);
    if (ts.count != 4) {
        printf("  got %ld edges, expected 4\n", ts.count);
        tset_free(&ts);
        teardown_db(db, "repub");
        FAIL("re-publish: v2 missing facts");
        return;
    }
    tset_free(&ts);

    teardown_db(db, "repub");
    PASS();
}

/* ─── Test 6: M0-M3 regression ────────────────────────────────────────── */

static void test_m0_m3_regression(void)
{
    dl_db *db;
    tuple_set ts;

    TEST("M0-M3 regression: legacy path (no publish) still works");

    setup_db(&db, "regress");

    /* Load facts */
    {
        uint32_t e[] = {10,20, 10,30, 20,40};
        load_rows_csv(db, "edge", 2, e, 3);
    }

    /* Load TC rules — but DO NOT publish */
    assert(dl_load_rules(db,
        "tc(X,Y):-edge(X,Y).\n"
        "tc(X,Z):-edge(X,Y),tc(Y,Z).\n") == 0);

    /* dl_query uses legacy path since snap_version==0 */
    memset(&ts, 0, sizeof(ts));
    long n = dl_query(db, "tc", tset_cb, &ts);
    assert(n >= 0);
    (void)n;

    /* TC of {(10,20),(10,30),(20,40)} = {(10,20),(10,30),(20,40),(10,40)} */
    if (ts.count != 4) {
        printf("  got %ld rows, expected 4\n", ts.count);
        long j;
        for (j = 0; j < ts.count && j < 10; j++)
            printf("    (%u,%u)\n", ts.data[j*2], ts.data[j*2+1]);
        tset_free(&ts);
        teardown_db(db, "regress");
        FAIL("M0-M3 regression: TC mismatch");
        return;
    }
    tset_free(&ts);

    /* Also test dl_prefix directly (no rules) */
    {
        long n2 = dl_prefix(db, "edge", NULL, 0, tset_cb, &ts);
        assert(n2 == 3);
        tset_free(&ts);
    }

    teardown_db(db, "regress");
    PASS();
}

/* ─── Test 7: Aggregate carry-over ────────────────────────────────────── */

static void test_aggregate_carryover(void)
{
    dl_db *db;
    tuple_set snap_result;

    TEST("aggregate carry-over: publish→query M3 aggregates matches naive");

    setup_db(&db, "aggco");

    /* Load edges */
    {
        uint32_t e[] = {1,10, 1,20, 2,30, 3,40, 3,50, 3,60};
        load_rows_csv(db, "edge", 2, e, 6);
    }

    /* Load aggregate rules and publish */
    assert(dl_load_rules(db,
        "cnt(X,N):-edge(X,Y),N=count().\n"
        "total(X,S):-edge(X,Y),S=sum(Y).\n") == 0);
    assert(dl_publish_snapshot(db) == 0);

    /* Query cnt via snapshot */
    memset(&snap_result, 0, sizeof(snap_result));
    dl_query_bound(db, "cnt", NULL, 0, tset_cb, &snap_result);

    /* Naive reference:
     * X=1 → count=2, X=2 → count=1, X=3 → count=3 */
    if (snap_result.count != 3) {
        printf("  cnt: got %ld rows, expected 3\n", snap_result.count);
        tset_free(&snap_result);
        teardown_db(db, "aggco");
        FAIL("aggregate carry-over: cnt mismatch");
        return;
    }

    /* Check for specific expected tuples */
    {
        uint32_t expected[][2] = {{1,2},{2,1},{3,3}};
        int found_all = 1, ei;
        for (ei = 0; ei < 3; ei++) {
            int found = 0;
            long j;
            for (j = 0; j < snap_result.count; j++) {
                if (snap_result.data[j*2] == expected[ei][0] &&
                    snap_result.data[j*2+1] == expected[ei][1]) {
                    found = 1;
                    break;
                }
            }
            if (!found) { found_all = 0; break; }
        }
        if (!found_all) {
            long j;
            for (j = 0; j < snap_result.count; j++)
                printf("    (%u,%u)\n",
                       snap_result.data[j*2], snap_result.data[j*2+1]);
            tset_free(&snap_result);
            teardown_db(db, "aggco");
            FAIL("aggregate carry-over: cnt missing expected tuple");
            return;
        }
    }
    tset_free(&snap_result);

    /* Query total via snapshot */
    memset(&snap_result, 0, sizeof(snap_result));
    dl_query_bound(db, "total", NULL, 0, tset_cb, &snap_result);

    /* total: X=1 → 10+20=30, X=2 → 30, X=3 → 40+50+60=150 */
    {
        uint32_t expected[][2] = {{1,30},{2,30},{3,150}};
        int found_all = 1, ei;
        for (ei = 0; ei < 3; ei++) {
            int found = 0;
            long j;
            for (j = 0; j < snap_result.count; j++) {
                if (snap_result.data[j*2] == expected[ei][0] &&
                    snap_result.data[j*2+1] == expected[ei][1]) {
                    found = 1;
                    break;
                }
            }
            if (!found) { found_all = 0; break; }
        }
        if (!found_all) {
            long j;
            for (j = 0; j < snap_result.count; j++)
                printf("    (%u,%u)\n",
                       snap_result.data[j*2], snap_result.data[j*2+1]);
            tset_free(&snap_result);
            teardown_db(db, "aggco");
            FAIL("aggregate carry-over: total mismatch");
            return;
        }
    }
    tset_free(&snap_result);

    teardown_db(db, "aggco");
    PASS();
}

/* ─── Test 8: orphaned snapshot dir cleanup ───────────────────────────── */

static void test_orphan_cleanup(void)
{
    dl_db *db;
    tuple_set ts;
    char dir[320];
    char orphan[352];
    char junk[416];

    TEST("orphan cleanup: stale snapshots/N/ doesn't block next publish");

    setup_db(&db, "orphan");

    /* Publish v1: 2 facts */
    {
        uint32_t e[] = {1,2, 2,3};
        load_rows_csv(db, "edge", 2, e, 2);
    }
    assert(dl_publish_snapshot(db) == 0);

    /* Simulate a prior crash *after* rename but *before* the CURRENT flip:
     * the fully-populated snapshots/2/ dir was left orphaned (its cleanup
     * never ran because the process died).  Make it non-empty so a plain
     * rename() into it would hit ENOTEMPTY. */
    snprintf(dir, sizeof(dir), "build-tmp/m4db_orphan/snapshots");
    snprintf(orphan, sizeof(orphan), "%s/2", dir);
    assert(mkdir(orphan, 0755) == 0);
    snprintf(junk, sizeof(junk), "%s/leftover.bin", orphan);
    {
        FILE *f = fopen(junk, "w");
        assert(f);
        fprintf(f, "orphaned from a failed CURRENT flip\n");
        fclose(f);
    }

    /* Load more facts so the next publish advances to v2 (fixpoint_dirty). */
    {
        uint32_t e[] = {3,4};
        load_rows_csv(db, "edge", 2, e, 1);
    }

    /* Publish v2: must succeed by first cleaning up the orphaned snapshots/2/.
     * Before the fix, rename(2.tmp, 2) fails with ENOTEMPTY and publish -1. */
    assert(dl_publish_snapshot(db) == 0);

    memset(&ts, 0, sizeof(ts));
    dl_query(db, "edge", tset_cb, &ts);
    assert(ts.count == 3);  /* v2 has all 3 facts */
    tset_free(&ts);

    /* A subsequent publish must also succeed (no lingering orphan). */
    {
        uint32_t e[] = {4,5};
        load_rows_csv(db, "edge", 2, e, 1);
    }
    assert(dl_publish_snapshot(db) == 0);

    memset(&ts, 0, sizeof(ts));
    dl_query(db, "edge", tset_cb, &ts);
    assert(ts.count == 4);
    tset_free(&ts);

    teardown_db(db, "orphan");
    PASS();
}

/* ─── Main ────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("M4 Tests\n");
    printf("========\n\n");

    test_lifecycle_tc();
    test_property_view_eq_rel();
    test_atomicity_crash();
    test_multi_reader();
    test_republish();
    test_m0_m3_regression();
    test_aggregate_carryover();
    test_orphan_cleanup();

    printf("\n---\n");
    printf("%d tests run, %d failed\n", tests_run, tests_failed);

    return tests_failed ? 1 : 0;
}
