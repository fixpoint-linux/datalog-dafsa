/*
 * test_m4_review.c — Adversarial tests for M4 review
 *
 * Tests beyond the existing suite:
 *   1. view_prefix vs rel_prefix: stop-early, empty relation,
 *      single tuple, mixed types, exhaustive for all k
 *   2. Atomic publish: first-publish abort
 *   3. Query routing: mutate after publish, pre-publish path
 *   4. View cache: re-publish correctness, dl_close cleanup
 *   5. dl_query_bound: exact bind, partial bind on snapshot path
 *   6. Edge cases: nonexistent goal, empty snapshot, double publish
 *   7. High arity (4-6)
 *   8. Re-publish staleness contract
 *   9. CLI commands
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
#include <unistd.h>

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

/* ─── Tuple helpers ────────────────────────────────────────────────────── */

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

/* ─── Stop-early callback ──────────────────────────────────────────────── */

typedef struct {
    long max_count;  /* stop after this many tuples */
    long emitted;
} stop_early_ctx;

static int stop_early_cb(const uint32_t *cols, uint8_t arity, void *user)
{
    stop_early_ctx *ctx = (stop_early_ctx *)user;
    (void)cols; (void)arity;
    ctx->emitted++;
    if (ctx->emitted >= ctx->max_count)
        return 1;  /* stop */
    return 0;
}

static int count_cb(const uint32_t *cols, uint8_t arity, void *user)
{
    long *counter = (long *)user;
    (void)cols; (void)arity;
    (*counter)++;
    return 0;
}

/* ─── DB helpers ───────────────────────────────────────────────────────── */

static void setup_db(dl_db **db_out, const char *suffix)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf build-tmp/adb_%s", suffix);
    system(cmd);
    snprintf(cmd, sizeof(cmd), "build-tmp/adb_%s", suffix);
    *db_out = dl_open(cmd);
    assert(*db_out);
}

static void teardown_db(dl_db *db, const char *suffix)
{
    char cmd[512];
    dl_close(db);
    snprintf(cmd, sizeof(cmd), "rm -rf build-tmp/adb_%s", suffix);
    system(cmd);
}

static int load_rows_csv(dl_db *db, const char *rel_name, uint8_t arity,
                         const uint32_t *cols, int nrows)
{
    char csv_path[256];
    int i, c;
    FILE *f;

    assert(dl_declare_relation(db, rel_name, arity) == 0);

    snprintf(csv_path, sizeof(csv_path), "build-tmp/adb_csv_%s.csv", rel_name);
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

/* ─── Test A1: Stop-early callback semantics ───────────────────────────── */

static void test_stop_early(void)
{
    dl_db *db;

    TEST("stop-early: view_prefix stop-early count matches rel_prefix");

    setup_db(&db, "se");

    {
        uint32_t f[30];
        int i;
        for (i = 0; i < 10; i++) {
            f[i*2] = (uint32_t)(i + 1);
            f[i*2+1] = (uint32_t)(i * 10);
        }
        load_rows_csv(db, "r", 2, f, 10);
    }
    assert(dl_publish_snapshot(db) == 0);

    /* Snapshot path: stop after 3 */
    {
        stop_early_ctx sec = {3, 0};
        long n = dl_query_bound(db, "r", NULL, 0, stop_early_cb, &sec);
        if (n != 3 || sec.emitted != 3) {
            printf("  snap: n=%ld emitted=%ld\n", n, sec.emitted);
            teardown_db(db, "se");
            FAIL("snapshot stop-early count");
            return;
        }
    }

    /* Legacy path: stop after 3 */
    {
        dl_db *db2;
        stop_early_ctx sec = {3, 0};
        setup_db(&db2, "se_legacy");
        {
            uint32_t f[30];
            int i;
            for (i = 0; i < 10; i++) {
                f[i*2] = (uint32_t)(i + 1);
                f[i*2+1] = (uint32_t)(i * 10);
            }
            load_rows_csv(db2, "r", 2, f, 10);
        }
        long n = dl_prefix(db2, "r", NULL, 0, stop_early_cb, &sec);
        if (n != 3 || sec.emitted != 3) {
            printf("  legacy: n=%ld emitted=%ld\n", n, sec.emitted);
            teardown_db(db2, "se_legacy");
            teardown_db(db, "se");
            FAIL("legacy stop-early count");
            return;
        }
        teardown_db(db2, "se_legacy");
    }

    teardown_db(db, "se");
    PASS();
}

/* ─── Test A2: Empty relation ──────────────────────────────────────────── */

static void test_empty_relation(void)
{
    dl_db *db;

    TEST("empty-relation: view_prefix on empty relation returns 0");

    setup_db(&db, "empty");
    assert(dl_declare_relation(db, "r", 3) == 0);
    assert(dl_publish_snapshot(db) == 0);

    {
        long count = 0;
        long n = dl_query_bound(db, "r", NULL, 0, count_cb, &count);
        if (n != 0 || count != 0) {
            printf("  n=%ld\n", n);
            teardown_db(db, "empty");
            FAIL("empty full scan");
            return;
        }
    }

    {
        uint32_t leading[] = {1, 2};
        long count = 0;
        long n = dl_query_bound(db, "r", leading, 2, count_cb, &count);
        if (n != 0 || count != 0) {
            printf("  bound n=%ld\n", n);
            teardown_db(db, "empty");
            FAIL("empty bound");
            return;
        }
    }

    teardown_db(db, "empty");
    PASS();
}

/* ─── Test A3: Single tuple, all k levels ──────────────────────────────── */

static void test_single_tuple(void)
{
    dl_db *db;
    uint8_t k;

    TEST("single-tuple: one tuple, all k levels 0..arity");

    setup_db(&db, "st");
    {
        uint32_t f[] = {42, 99, 7};
        load_rows_csv(db, "r", 3, f, 1);
    }
    assert(dl_publish_snapshot(db) == 0);

    for (k = 0; k <= 3; k++) {
        uint32_t leading[3] = {42, 99, 7};
        long count = 0;
        long n = dl_query_bound(db, "r", leading, k, count_cb, &count);
        if (n != 1 || count != 1) {
            printf("  k=%u: n=%ld count=%ld\n", k, n, count);
            teardown_db(db, "st");
            FAIL("single tuple k-level");
            return;
        }
    }

    /* Absent exact */
    {
        uint32_t leading[] = {42, 99, 8};
        long count = 0;
        long n = dl_query_bound(db, "r", leading, 3, count_cb, &count);
        if (n != 0) {
            printf("  absent n=%ld\n", n);
            teardown_db(db, "st");
            FAIL("absent exact");
            return;
        }
    }

    teardown_db(db, "st");
    PASS();
}

/* ─── Test A4: Exhaustive view_prefix vs rel_prefix ────────────────────── */

static void test_exhaustive_view_vs_rel(void)
{
    int iter;

    TEST("exhaustive: view_prefix==rel_prefix with stop-early+all k (200 iters)");

    for (iter = 0; iter < 200; iter++) {
        dl_db *db, *db2;
        uint8_t arity = (uint8_t)(1 + (iter % 4));
        int n_facts = 2 + (iter % 10);
        uint32_t facts[20 * 4];
        int actual_n = 0;
        int fi;
        unsigned seed = (unsigned)(20240901u + (unsigned)iter * 31337u);

        for (fi = 0; fi < n_facts * 4 && actual_n < n_facts; fi++) {
            uint32_t row[4];
            uint8_t c;
            int dup = 0, kk;

            for (c = 0; c < arity; c++) {
                seed = seed * 1103515245 + 12345;
                row[c] = (seed >> 16) % 100;
            }

            for (kk = 0; kk < actual_n; kk++) {
                int match = 1;
                uint8_t cc;
                for (cc = 0; cc < arity; cc++) {
                    if (facts[(size_t)kk * (size_t)arity + cc] != row[cc]) {
                        match = 0; break;
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

        if (actual_n < 1) continue;
        if (actual_n > 15) actual_n = 15;

        setup_db(&db, "evr");
        {
            uint32_t flat[80];
            int j;
            for (j = 0; j < actual_n; j++)
                memcpy(flat + (size_t)j * (size_t)arity,
                       facts + (size_t)j * (size_t)arity,
                       (size_t)arity * sizeof(uint32_t));
            load_rows_csv(db, "r", arity, flat, actual_n);
        }
        assert(dl_publish_snapshot(db) == 0);

        setup_db(&db2, "evr_legacy");
        {
            uint32_t flat[80];
            int j;
            for (j = 0; j < actual_n; j++)
                memcpy(flat + (size_t)j * (size_t)arity,
                       facts + (size_t)j * (size_t)arity,
                       (size_t)arity * sizeof(uint32_t));
            load_rows_csv(db2, "r", arity, flat, actual_n);
        }

        {
            uint8_t k;
            for (k = 0; k <= arity; k++) {
                int stop_at;
                for (stop_at = 1; stop_at <= actual_n + 1; stop_at++) {
                    uint32_t leading[4] = {0,0,0,0};
                    stop_early_ctx snap_sec, leg_sec;
                    long snap_n, leg_n;

                    if (k > 0 && actual_n > 0) {
                        int pick = (iter * k * stop_at) % actual_n;
                        memcpy(leading, facts + (size_t)pick * (size_t)arity,
                               (size_t)k * sizeof(uint32_t));
                    }

                    snap_sec.max_count = stop_at; snap_sec.emitted = 0;
                    leg_sec.max_count = stop_at; leg_sec.emitted = 0;

                    snap_n = dl_query_bound(db, "r", leading, k,
                                            stop_early_cb, &snap_sec);
                    leg_n = dl_prefix(db2, "r", leading, k,
                                      stop_early_cb, &leg_sec);

                    if (snap_n != leg_n) {
                        printf("\n  iter=%d a=%u k=%u stop=%d "
                               "snap=%ld leg=%ld\n",
                               iter, arity, k, stop_at, snap_n, leg_n);
                        teardown_db(db2, "evr_legacy");
                        teardown_db(db, "evr");
                        FAIL("stop-early count divergence");
                        return;
                    }
                    if (snap_sec.emitted != leg_sec.emitted) {
                        printf("\n  iter=%d a=%u k=%u stop=%d "
                               "s_emit=%ld l_emit=%ld\n",
                               iter, arity, k, stop_at,
                               snap_sec.emitted, leg_sec.emitted);
                        teardown_db(db2, "evr_legacy");
                        teardown_db(db, "evr");
                        FAIL("stop-early emitted divergence");
                        return;
                    }
                }

                /* Full enumeration */
                {
                    uint32_t leading[4] = {0,0,0,0};
                    tuple_set snap_set, leg_set;
                    if (k > 0 && actual_n > 0) {
                        int pick = (iter * 3) % actual_n;
                        memcpy(leading, facts + (size_t)pick * (size_t)arity,
                               (size_t)k * sizeof(uint32_t));
                    }
                    memset(&snap_set, 0, sizeof(snap_set));
                    memset(&leg_set, 0, sizeof(leg_set));
                    dl_query_bound(db, "r", leading, k, tset_cb, &snap_set);
                    dl_prefix(db2, "r", leading, k, tset_cb, &leg_set);
                    if (!tset_eq(&snap_set, &leg_set)) {
                        printf("\n  iter=%d a=%u k=%u snap=%ld leg=%ld\n",
                               iter, arity, k, snap_set.count, leg_set.count);
                        tset_free(&snap_set); tset_free(&leg_set);
                        teardown_db(db2, "evr_legacy");
                        teardown_db(db, "evr");
                        FAIL("tuple set divergence");
                        return;
                    }
                    tset_free(&snap_set);
                    tset_free(&leg_set);
                }
            }
        }

        teardown_db(db2, "evr_legacy");
        teardown_db(db, "evr");
    }
    PASS();
}

/* ─── Test A5: Query routing — mutate after publish ───────────────────── */

static void test_query_routing_mutate(void)
{
    dl_db *db;
    tuple_set ts;

    TEST("query-routing: post-publish mutation invisible to snapshot query");

    setup_db(&db, "qrm");
    {
        uint32_t f[] = {1,10, 2,20, 3,30};
        load_rows_csv(db, "edge", 2, f, 3);
    }
    assert(dl_publish_snapshot(db) == 0);

    memset(&ts, 0, sizeof(ts));
    long n = dl_query(db, "edge", tset_cb, &ts);
    if (n != 3 || ts.count != 3) {
        printf("  n=%ld count=%ld\n", n, ts.count);
        tset_free(&ts); teardown_db(db, "qrm");
        FAIL("pre-mutate");
        return;
    }
    tset_free(&ts);

    /* Mutate working copy (not published) */
    {
        uint32_t f[] = {4,40};
        load_rows_csv(db, "edge", 2, f, 1);
    }

    /* Snapshot query must still return 3 */
    memset(&ts, 0, sizeof(ts));
    n = dl_query(db, "edge", tset_cb, &ts);
    if (n != 3) {
        printf("  post-mutate snapshot: n=%ld expected=3\n", n);
        tset_free(&ts); teardown_db(db, "qrm");
        FAIL("mutation leaked to snapshot query");
        return;
    }
    tset_free(&ts);

    /* dl_prefix (legacy direct) sees the mutation (4 facts) */
    n = dl_prefix(db, "edge", NULL, 0, tset_cb, &ts);
    if (n != 4) {
        printf("  dl_prefix: n=%ld expected=4\n", n);
        tset_free(&ts); teardown_db(db, "qrm");
        FAIL("dl_prefix missed mutation");
        return;
    }
    tset_free(&ts);

    teardown_db(db, "qrm");
    PASS();
}

/* ─── Test A6: Pre-publish legacy path ─────────────────────────────────── */

static void test_pre_publish_legacy(void)
{
    dl_db *db;
    tuple_set ts;

    TEST("pre-publish: dl_query before publish uses legacy VM path");

    setup_db(&db, "ppl");
    {
        uint32_t f[] = {5,6, 7,8};
        load_rows_csv(db, "edge", 2, f, 2);
    }
    assert(dl_load_rules(db,
        "tc(X,Y):-edge(X,Y).\n"
        "tc(X,Z):-edge(X,Y),tc(Y,Z).\n") == 0);

    memset(&ts, 0, sizeof(ts));
    long n = dl_query(db, "tc", tset_cb, &ts);
    if (n != 2 || ts.count != 2) {
        printf("  TC: n=%ld count=%ld\n", n, ts.count);
        tset_free(&ts); teardown_db(db, "ppl");
        FAIL("legacy TC");
        return;
    }
    tset_free(&ts);

    /* Now publish and verify snapshot path */
    assert(dl_publish_snapshot(db) == 0);
    memset(&ts, 0, sizeof(ts));
    n = dl_query(db, "tc", tset_cb, &ts);
    if (n != 2 || ts.count != 2) {
        printf("  post-publish TC: n=%ld count=%ld\n", n, ts.count);
        tset_free(&ts); teardown_db(db, "ppl");
        FAIL("post-publish TC");
        return;
    }
    tset_free(&ts);

    teardown_db(db, "ppl");
    PASS();
}

/* ─── Test A7: Re-publish cache invalidation ───────────────────────────── */

static void test_republish_cache(void)
{
    dl_db *db;
    tuple_set ts;

    TEST("republish-cache: after re-publish, queries see new data");

    setup_db(&db, "rpc");
    {
        uint32_t f[] = {1,100, 2,200};
        load_rows_csv(db, "data", 2, f, 2);
    }
    assert(dl_publish_snapshot(db) == 0);

    /* Warm the cache */
    memset(&ts, 0, sizeof(ts));
    dl_query(db, "data", tset_cb, &ts);
    if (ts.count != 2) {
        printf("  v1: %ld\n", ts.count);
        tset_free(&ts); teardown_db(db, "rpc");
        FAIL("v1 query");
        return;
    }
    tset_free(&ts);

    /* Add more and republish */
    {
        uint32_t f[] = {3,300, 4,400};
        load_rows_csv(db, "data", 2, f, 2);
    }
    assert(dl_publish_snapshot(db) == 0);

    /* Must see 4 (cache was invalidated) */
    memset(&ts, 0, sizeof(ts));
    dl_query(db, "data", tset_cb, &ts);
    if (ts.count != 4) {
        printf("  v2: got %ld, expected 4\n", ts.count);
        long j;
        for (j = 0; j < ts.count && j < 10; j++)
            printf("    (%u,%u)\n", ts.data[j*2], ts.data[j*2+1]);
        tset_free(&ts); teardown_db(db, "rpc");
        FAIL("cache not invalidated on republish");
        return;
    }
    tset_free(&ts);

    teardown_db(db, "rpc");
    PASS();
}

/* ─── Test A8: dl_close cache cleanup ──────────────────────────────────── */

static void test_close_cache_cleanup(void)
{
    dl_db *db;
    int i;
    char dbdir[256];

    TEST("close-cache: dl_close doesn't leak after many cached queries");

    snprintf(dbdir, sizeof(dbdir), "build-tmp/adb_ccc");
    system("rm -rf build-tmp/adb_ccc");

    db = dl_open(dbdir);
    assert(db);
    {
        uint32_t f[] = {1,2, 3,4, 5,6, 7,8, 9,10};
        load_rows_csv(db, "r", 2, f, 5);
    }
    assert(dl_publish_snapshot(db) == 0);

    for (i = 0; i < 50; i++) {
        tuple_set ts;
        memset(&ts, 0, sizeof(ts));
        dl_query(db, "r", tset_cb, &ts);
        assert(ts.count == 5);
        tset_free(&ts);
    }

    /* Close and reopen WITHOUT deleting (preserves snapshot) */
    dl_close(db);
    db = dl_open(dbdir);
    assert(db);
    {
        tuple_set ts;
        memset(&ts, 0, sizeof(ts));
        dl_query(db, "r", tset_cb, &ts);
        assert(ts.count == 5);
        tset_free(&ts);
    }
    dl_close(db);
    system("rm -rf build-tmp/adb_ccc");
    PASS();
}

/* ─── Test A9: First publish with fault abort ──────────────────────────── */

static int fault_always_abort(dl_fpoint fp, void *user)
{
    (void)fp; (void)user;
    return -1;
}

static void test_first_publish_abort(void)
{
    dl_db *db;
    char dbdir[256];

    TEST("first-publish-abort: abort first publish leaves no CURRENT");

    snprintf(dbdir, sizeof(dbdir), "build-tmp/adb_fpa");
    system("rm -rf build-tmp/adb_fpa");
    db = dl_open(dbdir);
    assert(db);

    {
        uint32_t f[] = {1,2};
        load_rows_csv(db, "r", 2, f, 1);
    }

    dl_set_fault_hook(db, fault_always_abort, NULL);
    assert(dl_publish_snapshot(db) == -1);

    /* CURRENT must NOT exist */
    {
        char cur_path[512];
        snprintf(cur_path, sizeof(cur_path), "%s/snapshots/CURRENT", dbdir);
        FILE *f = fopen(cur_path, "r");
        if (f) {
            fclose(f);
            dl_close(db);
            system("rm -rf build-tmp/adb_fpa");
            FAIL("CURRENT exists after abort");
            return;
        }
    }

    /* No versioned dir */
    {
        char sdir[512];
        struct stat st;
        snprintf(sdir, sizeof(sdir), "%s/snapshots/1", dbdir);
        if (stat(sdir, &st) == 0) {
            dl_close(db);
            system("rm -rf build-tmp/adb_fpa");
            FAIL("version dir exists after abort");
            return;
        }
    }

    dl_close(db);

    /* Reopen: legacy path works */
    db = dl_open(dbdir);
    assert(db);
    dl_set_fault_hook(db, NULL, NULL);

    {
        tuple_set ts;
        memset(&ts, 0, sizeof(ts));
        long n = dl_query(db, "r", tset_cb, &ts);
        if (n != 1) {
            printf("  legacy n=%ld\n", n);
            tset_free(&ts); dl_close(db);
            system("rm -rf build-tmp/adb_fpa");
            FAIL("legacy after abort");
            return;
        }
        tset_free(&ts);
    }

    /* Now publish successfully */
    assert(dl_publish_snapshot(db) == 0);
    {
        tuple_set ts;
        memset(&ts, 0, sizeof(ts));
        long n = dl_query(db, "r", tset_cb, &ts);
        if (n != 1) {
            printf("  snapshot n=%ld\n", n);
            tset_free(&ts); dl_close(db);
            system("rm -rf build-tmp/adb_fpa");
            FAIL("snapshot after successful publish");
            return;
        }
        tset_free(&ts);
    }

    dl_close(db);
    system("rm -rf build-tmp/adb_fpa");
    PASS();
}

/* ─── Test A10: Nonexistent goal ───────────────────────────────────────── */

static void test_nonexistent_goal(void)
{
    dl_db *db;

    TEST("nonexistent-goal: query nonexistent relation returns -1");

    setup_db(&db, "neg");
    {
        uint32_t f[] = {1,2};
        load_rows_csv(db, "r", 2, f, 1);
    }
    assert(dl_publish_snapshot(db) == 0);

    long n = dl_query(db, "nonexistent", count_cb, NULL);
    if (n != -1) {
        printf("  snapshot: n=%ld\n", n);
        teardown_db(db, "neg");
        FAIL("snapshot nonexistent should be -1");
        return;
    }

    /* Legacy path */
    {
        dl_db *db2;
        setup_db(&db2, "neg2");
        {
            uint32_t f[] = {1,2};
            load_rows_csv(db2, "r", 2, f, 1);
        }
        n = dl_query(db2, "nonexistent", count_cb, NULL);
        if (n != -1) {
            printf("  legacy: n=%ld\n", n);
            teardown_db(db2, "neg2");
            teardown_db(db, "neg");
            FAIL("legacy nonexistent should be -1");
            return;
        }
        teardown_db(db2, "neg2");
    }

    teardown_db(db, "neg");
    PASS();
}

/* ─── Test A11: Empty snapshot ─────────────────────────────────────────── */

static void test_empty_snapshot(void)
{
    dl_db *db;

    TEST("empty-snapshot: publish with 0 relations succeeds");

    setup_db(&db, "esnap");
    assert(dl_publish_snapshot(db) == 0);

    long n = dl_query(db, "anything", count_cb, NULL);
    if (n != -1) {
        printf("  n=%ld\n", n);
        teardown_db(db, "esnap");
        FAIL("empty snapshot query should be -1");
        return;
    }

    teardown_db(db, "esnap");
    PASS();
}

/* ─── Test A12: dl_query_bound exact bind on snapshot ──────────────────── */

static void test_bound_exact_snapshot(void)
{
    dl_db *db;
    tuple_set ts;

    TEST("bound-exact: dl_query_bound k=arity on snapshot path");

    setup_db(&db, "bes");
    {
        uint32_t f[] = {1,2,3, 1,2,4, 1,2,5, 2,3,6};
        load_rows_csv(db, "r", 3, f, 4);
    }
    assert(dl_publish_snapshot(db) == 0);

    /* Exact bind */
    {
        uint32_t leading[] = {1, 2, 3};
        memset(&ts, 0, sizeof(ts));
        long n = dl_query_bound(db, "r", leading, 3, tset_cb, &ts);
        if (n != 1 || ts.count != 1) {
            printf("  [1,2,3]: n=%ld\n", n);
            tset_free(&ts); teardown_db(db, "bes");
            FAIL("exact bind");
            return;
        }
        tset_free(&ts);
    }

    /* Absent exact */
    {
        uint32_t leading[] = {1, 2, 9};
        long count = 0;
        long n = dl_query_bound(db, "r", leading, 3, count_cb, &count);
        if (n != 0 || count != 0) {
            printf("  [1,2,9]: n=%ld\n", n);
            teardown_db(db, "bes");
            FAIL("absent exact");
            return;
        }
    }

    /* Partial bind */
    {
        uint32_t leading[] = {1, 2};
        memset(&ts, 0, sizeof(ts));
        long n = dl_query_bound(db, "r", leading, 2, tset_cb, &ts);
        if (n != 3 || ts.count != 3) {
            printf("  [1,2]: n=%ld\n", n);
            tset_free(&ts); teardown_db(db, "bes");
            FAIL("partial bind");
            return;
        }
        tset_free(&ts);
    }

    teardown_db(db, "bes");
    PASS();
}

/* ─── Test A13: Publish twice without changes ──────────────────────────── */

static void test_publish_idempotent(void)
{
    dl_db *db;
    long count;

    TEST("publish-idempotent: double publish without changes");

    setup_db(&db, "pi");
    {
        uint32_t f[] = {1,2, 3,4};
        load_rows_csv(db, "r", 2, f, 2);
    }
    assert(dl_publish_snapshot(db) == 0);
    assert(dl_publish_snapshot(db) == 0);

    count = 0;
    long n = dl_query(db, "r", count_cb, &count);
    if (n != 2 || count != 2) {
        printf("  n=%ld count=%ld\n", n, count);
        teardown_db(db, "pi");
        FAIL("double publish");
        return;
    }

    teardown_db(db, "pi");
    PASS();
}

/* ─── Test A14: High arity (4-6) on snapshot path ──────────────────────── */

static void test_arity_high(void)
{
    dl_db *db;
    long count;

    TEST("arity-high: arity 4-6 relations on snapshot path");

    /* arity 4 */
    setup_db(&db, "ah4");
    {
        uint32_t f[] = {1,2,3,4, 1,2,3,5, 1,2,4,6, 2,3,4,7};
        load_rows_csv(db, "r4", 4, f, 4);
    }
    assert(dl_publish_snapshot(db) == 0);

    count = 0;
    long n = dl_query(db, "r4", count_cb, &count);
    if (n != 4 || count != 4) {
        printf("  a4: n=%ld count=%ld\n", n, count);
        teardown_db(db, "ah4"); FAIL("arity 4"); return;
    }

    {
        uint32_t leading[] = {1,2};
        tuple_set ts;
        memset(&ts, 0, sizeof(ts));
        n = dl_query_bound(db, "r4", leading, 2, tset_cb, &ts);
        if (n != 3) {
            printf("  a4 k=2: n=%ld expected=3\n", n);
            tset_free(&ts); teardown_db(db, "ah4"); FAIL("arity 4 prefix"); return;
        }
        tset_free(&ts);
    }
    teardown_db(db, "ah4");

    /* arity 6 */
    setup_db(&db, "ah6");
    {
        uint32_t f[] = {10,20,30,40,50,60, 10,20,30,40,50,61};
        load_rows_csv(db, "r6", 6, f, 2);
    }
    assert(dl_publish_snapshot(db) == 0);

    count = 0;
    n = dl_query(db, "r6", count_cb, &count);
    if (n != 2 || count != 2) {
        printf("  a6: n=%ld count=%ld\n", n, count);
        teardown_db(db, "ah6"); FAIL("arity 6"); return;
    }

    {
        uint32_t leading[] = {10,20,30,40,50};
        tuple_set ts;
        memset(&ts, 0, sizeof(ts));
        n = dl_query_bound(db, "r6", leading, 5, tset_cb, &ts);
        if (n != 2) {
            printf("  a6 k=5: n=%ld\n", n);
            tset_free(&ts); teardown_db(db, "ah6"); FAIL("arity 6 prefix"); return;
        }
        tset_free(&ts);
    }
    teardown_db(db, "ah6");
    PASS();
}

/* ─── Test A15: Re-publish staleness ───────────────────────────────────── */

static void test_republish_staleness(void)
{
    dl_db *db1, *db2;
    tuple_set ts;
    char dbdir[256];

    TEST("republish-staleness: handle opened before republish sees old snapshot");

    snprintf(dbdir, sizeof(dbdir), "build-tmp/adb_rs");
    system("rm -rf build-tmp/adb_rs");

    db1 = dl_open(dbdir);
    assert(db1);
    {
        uint32_t f[] = {1,2, 2,3};
        load_rows_csv(db1, "edge", 2, f, 2);
    }
    assert(dl_publish_snapshot(db1) == 0);

    /* Open second handle NOW (sees v1) */
    db2 = dl_open(dbdir);
    assert(db2);

    /* Republish from db1 */
    {
        uint32_t f[] = {3,4, 4,5};
        load_rows_csv(db1, "edge", 2, f, 2);
    }
    assert(dl_publish_snapshot(db1) == 0);

    /* db1 sees v2 */
    memset(&ts, 0, sizeof(ts));
    dl_query(db1, "edge", tset_cb, &ts);
    assert(ts.count == 4);
    tset_free(&ts);

    /* db2 still sees v1 (frozen at open time) */
    memset(&ts, 0, sizeof(ts));
    long n = dl_query(db2, "edge", tset_cb, &ts);
    if (n != 2) {
        printf("  db2: n=%ld expected=2\n", n);
        tset_free(&ts);
        dl_close(db1); dl_close(db2);
        system("rm -rf build-tmp/adb_rs");
        FAIL("db2 should see v1");
        return;
    }
    tset_free(&ts);

    dl_close(db2);
    dl_close(db1);
    system("rm -rf build-tmp/adb_rs");
    PASS();
}

/* ─── Test A16: CLI commands ───────────────────────────────────────────── */

static void test_cli_commands(void)
{
    TEST("cli: dl publish + dl bound + dl query");

    /* Resolve the repo root at runtime (works regardless of where the repo
     * is mounted — the sandbox uses /workspace, the host uses
     * /home/arch/projects/datalog-dafsa).  The CLI is run with cwd == repo
     * root, so relative build-tmp/ paths resolve from here. */
    char base[4096];
    if (!getcwd(base, sizeof(base))) { FAIL("getcwd"); return; }
    char data_csv[4160], cli_dir[4160], cmd[8400];

    snprintf(data_csv, sizeof(data_csv), "%s/build-tmp/adb_cli_data.csv", base);
    snprintf(cli_dir, sizeof(cli_dir), "%s/build-tmp/adb_cli", base);

    /* Create CSV */
    {
        FILE *f = fopen(data_csv, "w");
        if (!f) { FAIL("cli data csv fopen"); return; }
        fprintf(f, "1,2\n2,3\n3,4\n");
        fclose(f);
    }

    /* Load */
    {
        snprintf(cmd, sizeof(cmd),
            "cd '%s' && "
            "rm -rf build-tmp/adb_cli && "
            "./dl -d build-tmp/adb_cli load build-tmp/adb_cli_data.csv "
            "--rel edge >/dev/null 2>&1", base);
        if (system(cmd) != 0) { FAIL("cli load"); return; }
    }

    /* Publish */
    {
        snprintf(cmd, sizeof(cmd),
            "cd '%s' && ./dl -d build-tmp/adb_cli publish >/dev/null 2>&1", base);
        if (system(cmd) != 0) { FAIL("cli publish"); return; }
    }

    /* Bound */
    {
        snprintf(cmd, sizeof(cmd),
            "cd '%s' && ./dl -d build-tmp/adb_cli bound edge 2 2>/dev/null", base);
        FILE *f = popen(cmd, "r");
        if (!f) { FAIL("cli bound"); return; }
        char buf[256];
        int lines = 0;
        while (fgets(buf, sizeof(buf), f)) {
            if (strstr(buf, "2 3")) lines++;
        }
        pclose(f);
        if (lines != 1) {
            printf("  lines=%d\n", lines);
            FAIL("cli bound output");
            return;
        }
    }

    /* Query */
    {
        snprintf(cmd, sizeof(cmd),
            "cd '%s' && ./dl -d build-tmp/adb_cli query "
            "'tc(X,Y):-edge(X,Y).' tc 2>/dev/null", base);
        FILE *f = popen(cmd, "r");
        if (!f) { FAIL("cli query"); return; }
        char buf[256];
        int lines = 0;
        while (fgets(buf, sizeof(buf), f)) lines++;
        pclose(f);
        if (lines != 3) {
            printf("  lines=%d\n", lines);
            FAIL("cli query output");
            return;
        }
    }

    snprintf(cmd, sizeof(cmd), "rm -rf '%s' '%s'", cli_dir, data_csv);
    system(cmd);
    PASS();
}

/* ─── Main ─────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("M4 Review Adversarial Tests\n");
    printf("===========================\n\n");

    test_stop_early();
    test_empty_relation();
    test_single_tuple();
    test_exhaustive_view_vs_rel();
    test_query_routing_mutate();
    test_pre_publish_legacy();
    test_republish_cache();
    test_close_cache_cleanup();
    test_first_publish_abort();
    test_nonexistent_goal();
    test_empty_snapshot();
    test_bound_exact_snapshot();
    test_publish_idempotent();
    test_arity_high();
    test_republish_staleness();
    test_cli_commands();

    printf("\n---\n");
    printf("%d tests run, %d failed\n", tests_run, tests_failed);

    return tests_failed ? 1 : 0;
}
