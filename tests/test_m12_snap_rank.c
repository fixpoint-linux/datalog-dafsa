/*
 * test_m12_snap_rank.c — snapshot (mmap dafsa_view) rank/select/range_count
 *
 * Verifies that the PUBLIC dl_rank/dl_select/dl_range_count/dl_count route to
 * the mmap snapshot view when a snapshot is current (db->snap_version > 0),
 * and to the in-memory relation otherwise.  This mirrors the verified snapshot
 * routing that dl_query/dl_pattern already use (snapshot_query_scan), extended
 * to the Tier-2 order-statistics family.
 *
 *   T1  publish then read:      dl_rank(ts[i])==i, dl_select(i)==ts[i],
 *                               dl_range_count==span, dl_count==N over the
 *                               mmap view, for arities 1..4 and a suffix-sharing
 *                               cross-product.
 *   T2  snapshot-vs-live:       publish, then dl_add_fact (live diverges) =>
 *                               dl_rank/dl_select/dl_count still read the
 *                               PUBLISHED view, NOT the new fact; after
 *                               re-publish they reflect it.
 *   T3  empty snapshot:         dl_count==0, dl_select(0)==-1, dl_rank(any)==0.
 *   T4  rejections (published): unknown rel / variadic rel / arity mismatch =>
 *                               UINT64_MAX (rank/count) / -1 (select).
 *   T5  no-publish sanity:      before any publish, dl_rank reads the in-memory
 *                               relation (routing gated on snap_version).
 *
 * Build: make tests/test_m12_snap_rank (link ALL_OBJS) — see Makefile.
 */
#include "dl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>

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

#define MAXA 8

/* ─── Tuple buffer (dl_query output, already in lex/numeric order) ─────── */

typedef struct {
    uint32_t *data;    /* count*arity u32s */
    long      count;
    uint8_t   arity;
    long      cap;
} tset;

static int tset_add(tset *t, const uint32_t *cols, uint8_t arity)
{
    if (t->count >= t->cap) {
        long nc = t->cap ? t->cap * 2 : 256;
        uint32_t *nd = realloc(t->data, (size_t)nc * arity * sizeof(uint32_t));
        if (!nd) return -1;
        t->data = nd;
        t->cap = nc;
    }
    memcpy(t->data + (size_t)t->count * arity, cols,
           (size_t)arity * sizeof(uint32_t));
    t->count++;
    return 0;
}

static void tset_free(tset *t) { free(t->data); memset(t, 0, sizeof(*t)); }

static int sink_cb(const uint32_t *cols, uint8_t arity, void *user)
{
    return tset_add((tset *)user, cols, arity);
}

/* Enumerate the relation via dl_query.  After a publish, dl_query routes to
 * the mmap snapshot (snapshot_query_scan), giving the PUBLISHED lex order;
 * before any publish it evaluates the in-memory relation. */
static long collect(dl_db *db, const char *rel, uint8_t arity, tset *ts)
{
    ts->arity = arity;
    return dl_query(db, rel, sink_cb, ts);
}

/* Verify rank==i, select(i)==ts[i], range_count over the ordered buffer. */
static void check_invariants(dl_db *db, const char *rel, const tset *ts)
{
    long i, a, b;
    uint32_t out[MAXA];
    uint64_t r;

    for (i = 0; i < ts->count; i++) {
        const uint32_t *t = ts->data + (size_t)i * ts->arity;
        r = dl_rank(db, rel, t, ts->arity);
        if (r != (uint64_t)i) { FAIL("rank == index"); return; }

        if (dl_select(db, rel, (uint64_t)i, out, ts->arity) != 0) {
            FAIL("select returned error"); return;
        }
        if (memcmp(out, t, (size_t)ts->arity * sizeof(uint32_t)) != 0) {
            FAIL("select tuple mismatch"); return;
        }
    }

    /* select->rank round-trip. */
    for (i = 0; i < ts->count; i++) {
        if (dl_select(db, rel, (uint64_t)i, out, ts->arity) != 0) continue;
        if (dl_rank(db, rel, out, ts->arity) != (uint64_t)i) {
            FAIL("select->rank round-trip"); return;
        }
    }

    /* range_count(ts[a], ts[b]) == b-a for many [a,b). */
    for (i = 0; i < 200 && ts->count > 1; i++) {
        a = (long)(rand() % (unsigned)(ts->count + 1));
        b = (long)(rand() % (unsigned)(ts->count + 1));
        if (a > b) { long t = a; a = b; b = t; }
        if (a >= ts->count) a = ts->count - 1;
        if (b >= ts->count) b = ts->count;
        if (b == ts->count) {
            r = dl_range_count(db, rel,
                               ts->data + (size_t)a * ts->arity,
                               ts->data + (size_t)(ts->count - 1) * ts->arity,
                               ts->arity);
            if (r != (uint64_t)(ts->count - a - 1)) { FAIL("range_count tail"); return; }
        } else if (a < b) {
            r = dl_range_count(db, rel,
                               ts->data + (size_t)a * ts->arity,
                               ts->data + (size_t)b * ts->arity, ts->arity);
            if (r != (uint64_t)(b - a)) { FAIL("range_count [a,b)"); return; }
        }
    }
}

/* ─── DB fixture ───────────────────────────────────────────────────────── */

static const char *BASE = "build-tmp/m12snap";

static dl_db *fresh_db(char *dir_out, size_t cap, const char *name)
{
    char cmd[512];
    snprintf(dir_out, cap, "%s/%s", BASE, name);
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir_out);
    system(cmd);
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", dir_out);
    system(cmd);
    return dl_open(dir_out);
}

/* ─── T1: publish then read rank/select/range_count over the mmap view ─── */

static void t1_publish(char *dir, size_t cap)
{
    dl_db *db = fresh_db(dir, cap, "t1");
    tset ts;
    int i;
    uint32_t cols[4];
    uint8_t arity;
    const char *names[4] = { "r1", "r2", "r3", "r4" };

    memset(&ts, 0, sizeof(ts));
    TEST("T1 publish: rank/select/range/count over mmap view (arities 1..4)");

    for (arity = 1; arity <= 4; arity++) {
        const char *rel = names[arity - 1];
        assert(dl_declare_relation(db, rel, arity) == 0);
        for (i = 0; i < 200; i++) {
            int c;
            for (c = 0; c < arity; c++)
                cols[c] = (uint32_t)(rand() % 40);
            dl_add_fact(db, rel, cols, arity);
        }
    }

    assert(dl_publish_snapshot(db) == 0);

    for (arity = 1; arity <= 4; arity++) {
        const char *rel = names[arity - 1];
        tset_free(&ts); memset(&ts, 0, sizeof(ts));
        if (collect(db, rel, arity, &ts) < 0) { FAIL("collect"); goto out; }
        if (dl_count(db, rel) != (uint64_t)ts.count) { FAIL("dl_count == distinct"); goto out; }
        check_invariants(db, rel, &ts);
        if (tests_failed) goto out;
    }

    PASS();
out:
    tset_free(&ts);
    dl_close(db);
}

/* Suffix-sharing cross-product: n_states << n_tuples, dl_count must be the
 * distinct-key count (NOT a graph path count). */
static void t1b_cross_product(char *dir, size_t cap)
{
    dl_db *db = fresh_db(dir, cap, "t1b");
    tset ts;
    long a, b;
    uint32_t cols[2];
    uint64_t expected_count = 2000;   /* 20 x 100 cross-product */

    memset(&ts, 0, sizeof(ts));
    TEST("T1b publish: suffix-sharing cross-product rank/select/range/count");

    assert(dl_declare_relation(db, "r", 2) == 0);
    for (a = 0; a < 20; a++) {
        for (b = 0; b < 100; b++) {
            cols[0] = (uint32_t)a;
            cols[1] = (uint32_t)b;
            dl_add_fact(db, "r", cols, 2);
        }
    }

    assert(dl_publish_snapshot(db) == 0);

    if (dl_count(db, "r") != expected_count) {
        FAIL("cross-product dl_count == A*B"); goto out;
    }
    if (collect(db, "r", 2, &ts) < 0) { FAIL("collect"); goto out; }
    if (ts.count != (long)expected_count) { FAIL("collect count"); goto out; }
    check_invariants(db, "r", &ts);
    if (tests_failed) goto out;

    {
        uint32_t lo[2] = {0, 0};
        uint32_t hi[2] = {20, 100};
        if (dl_range_count(db, "r", lo, hi, 2) != expected_count) {
            FAIL("cross-product full range"); goto out;
        }
    }
    {
        uint32_t last[2] = {19, 99};
        if (dl_rank(db, "r", last, 2) != expected_count - 1) {
            FAIL("cross-product max rank"); goto out;
        }
    }

    PASS();
out:
    tset_free(&ts);
    dl_close(db);
}

/* ─── T2: snapshot-vs-live divergence ─────────────────────────────────── */

static void t2_snap_vs_live(char *dir, size_t cap)
{
    dl_db *db = fresh_db(dir, cap, "t2");
    tset ts;
    uint32_t cols[2];
    uint32_t new_fact[2] = {99, 99};
    uint32_t out[MAXA];

    memset(&ts, 0, sizeof(ts));
    TEST("T2 snapshot-vs-live: publish, add fact, reads stay on published view");

    assert(dl_declare_relation(db, "r", 2) == 0);
    cols[0] = 1; cols[1] = 10; dl_add_fact(db, "r", cols, 2);
    cols[0] = 2; cols[1] = 20; dl_add_fact(db, "r", cols, 2);
    cols[0] = 3; cols[1] = 30; dl_add_fact(db, "r", cols, 2);

    /* No-publish sanity: live in-memory routing reads all 3. */
    if (dl_count(db, "r") != 3) { FAIL("pre-publish dl_count == 3"); goto out; }

    assert(dl_publish_snapshot(db) == 0);
    if (collect(db, "r", 2, &ts) < 0 || ts.count != 3) { FAIL("collect v1"); goto out; }

    /* Add a 4th fact to the LIVE relation.  snap_version is NOT reset by
     * dl_add_fact, so dl_rank/dl_select/dl_count must still read the v1 view. */
    dl_add_fact(db, "r", new_fact, 2);

    if (dl_count(db, "r") != 3) { FAIL("post-add dl_count still published 3"); goto out; }
    if (dl_rank(db, "r", new_fact, 2) != 3) { FAIL("post-add rank(new) == published count"); goto out; }
    if (dl_select(db, "r", 3, out, 2) != -1) { FAIL("post-add select(3) out of published range"); goto out; }
    /* Published tuples unchanged: select(1) still == ts[1]. */
    if (dl_select(db, "r", 1, out, 2) != 0) { FAIL("post-add select(1)"); goto out; }
    if (memcmp(out, ts.data + 2, 2 * sizeof(uint32_t)) != 0) {
        FAIL("post-add select(1) != published tuple"); goto out;
    }

    /* Re-publish reflects the new fact. */
    assert(dl_publish_snapshot(db) == 0);
    if (dl_count(db, "r") != 4) { FAIL("re-publish dl_count == 4"); goto out; }
    if (dl_rank(db, "r", new_fact, 2) != 3) { FAIL("re-publish rank(new) == 3"); goto out; }
    if (dl_select(db, "r", 3, out, 2) != 0) { FAIL("re-publish select(3)"); goto out; }
    if (memcmp(out, new_fact, 2 * sizeof(uint32_t)) != 0) {
        FAIL("re-publish select(3) == new fact"); goto out;
    }

    PASS();
out:
    tset_free(&ts);
    dl_close(db);
}

/* ─── T3: empty snapshot ──────────────────────────────────────────────── */

static void t3_empty(char *dir, size_t cap)
{
    dl_db *db = fresh_db(dir, cap, "t3");
    uint32_t cols[2] = {1, 2};
    uint32_t out[MAXA];

    TEST("T3 empty snapshot: count 0, select -1, rank 0");

    assert(dl_declare_relation(db, "r", 2) == 0);
    assert(dl_publish_snapshot(db) == 0);

    if (dl_count(db, "r") != 0) { FAIL("empty count"); goto out; }
    if (dl_select(db, "r", 0, out, 2) != -1) { FAIL("empty select(0)"); goto out; }
    if (dl_rank(db, "r", cols, 2) != 0) { FAIL("empty rank"); goto out; }
    if (dl_range_count(db, "r", cols, cols, 2) != 0) { FAIL("empty range_count"); goto out; }

    PASS();
out:
    dl_close(db);
}

/* ─── T4: rejections over a published snapshot ─────────────────────────── */

static void t4_reject_published(char *dir, size_t cap)
{
    dl_db *db = fresh_db(dir, cap, "t4");
    uint32_t cols[2] = {1, 2};
    uint32_t out[MAXA];

    TEST("T4 rejections over published snapshot");

    assert(dl_declare_relation(db, "r", 2) == 0);
    assert(dl_declare_relation_variadic(db, "v") == 0);
    dl_add_fact(db, "r", cols, 2);
    assert(dl_publish_snapshot(db) == 0);

    /* Unknown rel. */
    if (dl_rank(db, "nope", cols, 2) != UINT64_MAX) { FAIL("rank unknown"); goto out; }
    if (dl_select(db, "nope", 0, out, 2) != -1) { FAIL("select unknown"); goto out; }
    if (dl_range_count(db, "nope", cols, cols, 2) != UINT64_MAX) { FAIL("range unknown"); goto out; }
    if (dl_count(db, "nope") != UINT64_MAX) { FAIL("count unknown"); goto out; }

    /* Arity mismatch. */
    if (dl_rank(db, "r", cols, 1) != UINT64_MAX) { FAIL("rank arity mismatch"); goto out; }
    if (dl_select(db, "r", 0, out, 1) != -1) { FAIL("select arity mismatch"); goto out; }
    if (dl_range_count(db, "r", cols, cols, 3) != UINT64_MAX) { FAIL("range arity mismatch"); goto out; }

    /* Variadic rejected over the snapshot. */
    if (dl_rank(db, "v", cols, 2) != UINT64_MAX) { FAIL("rank variadic"); goto out; }
    if (dl_select(db, "v", 0, out, 2) != -1) { FAIL("select variadic"); goto out; }
    if (dl_count(db, "v") != UINT64_MAX) { FAIL("count variadic"); goto out; }

    PASS();
out:
    dl_close(db);
}

/* ─── T5: no-publish sanity (in-memory routing) ────────────────────────── */

static void t5_no_publish(char *dir, size_t cap)
{
    dl_db *db = fresh_db(dir, cap, "t5");
    tset ts;
    uint32_t cols[2];

    memset(&ts, 0, sizeof(ts));
    TEST("T5 no-publish sanity: in-memory routing (snap_version == 0)");

    assert(dl_declare_relation(db, "r", 2) == 0);
    cols[0] = 5; cols[1] = 50; dl_add_fact(db, "r", cols, 2);
    cols[0] = 1; cols[1] = 10; dl_add_fact(db, "r", cols, 2);
    cols[0] = 3; cols[1] = 30; dl_add_fact(db, "r", cols, 2);

    if (collect(db, "r", 2, &ts) < 0) { FAIL("collect"); goto out; }
    if (dl_count(db, "r") != (uint64_t)ts.count) { FAIL("count"); goto out; }
    check_invariants(db, "r", &ts);
    if (tests_failed) goto out;

    PASS();
out:
    tset_free(&ts);
    dl_close(db);
}

/* ─── main ─────────────────────────────────────────────────────────────── */

int main(void)
{
    char dir[512];

    printf("=== M12 snapshot (mmap view) rank/select/range_count tests ===\n");
    srand(987654);

    t1_publish(dir, sizeof(dir));
    t1b_cross_product(dir, sizeof(dir));
    t2_snap_vs_live(dir, sizeof(dir));
    t3_empty(dir, sizeof(dir));
    t4_reject_published(dir, sizeof(dir));
    t5_no_publish(dir, sizeof(dir));

    printf("\n%d tests run, %d failed.\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
