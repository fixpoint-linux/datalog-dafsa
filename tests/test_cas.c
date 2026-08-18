/*
 * test_cas.c — CAS / optimistic-concurrency transaction tests
 *
 * T1: CAS success 0→1→2 (dl_cas_revision); dl_rev_get confirms each.
 * T2: conflict — stale expected → DL_E_CONFLICT (2), rev unchanged, no fact
 *     applied.
 * T3: stale-retry loop — read rev via dl_rev_get, dl_txn_begin +
 *     dl_txn_cas(rev, rev+1) + dl_txn_add_fact, dl_txn_commit; on
 *     DL_E_CONFLICT re-read and retry until success (contention simulated by
 *     pre-bumping rev between attempts).
 * T4: multi-op atomic commit — cas+add+delete in one txn, all-or-nothing; AND
 *     a conflict case where a later txn_cas fails (stale expected) → NOTHING
 *     applied incl. earlier buffered adds.
 * T5: rollback — buffer ops, dl_txn_rollback → nothing durable, reopen
 *     confirms absent.
 * T6: crash-in-txn — fork + child opens db, begin txn, buffer ops,
 *     dl_txn_commit up to the DL_FPOINT_TXN_BEFORE_MARKER fault point, _exit
 *     without dl_close; parent reopens → verify facts+rev absent (txn not
 *     committed), txn.wal truncated to header.
 */

#include "dl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>

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

/* ─── Helpers ─────────────────────────────────────────────────────────── */

static void rm_dir(const char *dir)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    system(cmd);
}

/* mkdir -p a build-tmp path (a single-level dir suffices for the tests). */
static void mk_db_dir(const char *dir)
{
    assert(mkdir(dir, 0755) == 0 || errno == EEXIST);
}

/* ─── T1: CAS success 0→1→2, rev_get confirms ─────────────────────────── */
static void test_t1_cas_success(void)
{
    dl_db *db;
    uint32_t r;
    const char *dir = "build-tmp/cast1";

    TEST("T1: dl_cas_revision 0->1->2, dl_rev_get confirms");
    rm_dir(dir);
    mk_db_dir(dir);
    db = dl_open(dir);
    assert(db);

    if (dl_cas_revision(db, "e1", 0, 1) != 0) { FAIL("cas 0->1"); goto out; }
    if (dl_rev_get(db, "e1", &r) != 0 || r != 1) { FAIL("rev not 1 after 0->1"); goto out; }
    if (dl_cas_revision(db, "e1", 1, 2) != 0) { FAIL("cas 1->2"); goto out; }
    if (dl_rev_get(db, "e1", &r) != 0 || r != 2) { FAIL("rev not 2 after 1->2"); goto out; }

    PASS();
out:
    dl_close(db);
    rm_dir(dir);
}

/* ─── T2: conflict — stale expected → DL_E_CONFLICT, rev unchanged ─────── */
static void test_t2_conflict(void)
{
    dl_db *db;
    uint32_t r;
    const char *dir = "build-tmp/cast2";

    TEST("T2: stale expected -> DL_E_CONFLICT(2), rev unchanged, no fact");
    rm_dir(dir);
    mk_db_dir(dir);
    db = dl_open(dir);
    assert(db);

    /* e2 currently starts at 0; CAS it to 1. */
    assert(dl_cas_revision(db, "e2", 0, 1) == 0);
    /* Now expected=0 is stale (current is 1). */
    if (dl_cas_revision(db, "e2", 0, 5) != DL_E_CONFLICT) {
        FAIL("stale cas did not return DL_E_CONFLICT"); goto out;
    }
    /* Rev unchanged (still 1), and no (e2,5) rev row was applied. */
    if (dl_rev_get(db, "e2", &r) != 0 || r != 1) {
        FAIL("rev changed after conflict"); goto out;
    }
    /* No (e2,5) rev row leaked. */
    {
        uint32_t sym = dl_intern_str(db, "e2");
        uint32_t leak[2] = { sym, 5 };
        if (dl_lookup(db, "rev", leak, 2) != 0) {
            FAIL("conflict leaked a rev row"); goto out;
        }
    }

    PASS();
out:
    dl_close(db);
    rm_dir(dir);
}

/* ─── T3: stale-retry loop until CAS succeeds ──────────────────────────── */
static void test_t3_stale_retry(void)
{
    dl_db *db;
    uint32_t r;
    const char *dir = "build-tmp/cast3";
    uint32_t sym_a, sym_b;
    uint32_t cols[2];
    int attempts, conflict_seen;

    TEST("T3: stale CAS retry loop (contention pre-bump) until success");
    rm_dir(dir);
    mk_db_dir(dir);
    db = dl_open(dir);
    assert(db);
    assert(dl_declare_relation(db, "r", 2) == 0);
    sym_a = dl_intern_str(db, "alpha");
    sym_b = dl_intern_str(db, "beta");
    assert(sym_a && sym_b);

    attempts = 0;
    conflict_seen = 0;
    for (;;) {
        int rc;
        attempts++;
        assert(dl_rev_get(db, "e3", &r) == 0);

        /* Simulate contention: on the first two attempts, pre-bump the rev
         * AFTER we read it so the buffered CAS expected becomes stale. */
        if (attempts <= 2) {
            assert(dl_cas_revision(db, "e3", r, r + 1) == 0);
        }

        assert(dl_txn_begin(db) == 0);
        assert(dl_txn_cas(db, "e3", r, r + 1) == 0);  /* stale on attempts 1,2 */
        cols[0] = sym_a; cols[1] = sym_b;
        assert(dl_txn_add_fact(db, "r", cols, 2) == 0);
        rc = dl_txn_commit(db);
        if (rc == 0) break;
        if (rc == DL_E_CONFLICT) { conflict_seen = 1; continue; }
        FAIL("commit returned unexpected error"); goto out;
    }

    if (!conflict_seen) { FAIL("retry loop never saw a conflict"); goto out; }
    if (attempts != 3) { FAIL("expected 3 attempts"); goto out; }
    /* Attempt 1: read 0, bump to 1, CAS(0,1) stale->conflict.  Attempt 2:
     * read 1, bump to 2, CAS(1,2) stale->conflict.  Attempt 3: read 2, no bump,
     * CAS(2,3) success -> final rev = 3. */
    if (dl_rev_get(db, "e3", &r) != 0 || r != 3) {
        FAIL("final rev not as expected"); goto out;
    }
    /* The successful txn's fact must be present. */
    if (dl_lookup(db, "r", cols, 2) != 1) {
        FAIL("committed fact not present"); goto out;
    }

    PASS();
out:
    dl_close(db);
    rm_dir(dir);
}

/* ─── T4: multi-op atomic commit (success + whole-txn conflict) ────────── */
static void test_t4_atomic_commit(void)
{
    dl_db *db;
    uint32_t r;
    const char *dir = "build-tmp/cast4";
    uint32_t sym_a, sym_b;
    uint32_t cols_a[2], cols_b[2];

    TEST("T4a: cas+add+delete in one txn commits atomically");
    rm_dir(dir);
    mk_db_dir(dir);
    db = dl_open(dir);
    assert(db);
    assert(dl_declare_relation(db, "r", 2) == 0);
    sym_a = dl_intern_str(db, "alpha");
    sym_b = dl_intern_str(db, "beta");
    assert(sym_a && sym_b);
    cols_a[0] = sym_a; cols_a[1] = sym_a;   /* deleted inside txn */
    cols_b[0] = sym_b; cols_b[1] = sym_b;   /* added inside txn */

    /* Pre-seed the fact that the txn will delete. */
    assert(dl_add_fact(db, "r", cols_a, 2) == 1);

    assert(dl_txn_begin(db) == 0);
    assert(dl_txn_cas(db, "e4", 0, 1) == 0);
    assert(dl_txn_add_fact(db, "r", cols_b, 2) == 0);
    assert(dl_txn_delete_fact(db, "r", cols_a, 2) == 0);
    if (dl_txn_commit(db) != 0) { FAIL("atomic txn commit"); goto out; }

    if (dl_rev_get(db, "e4", &r) != 0 || r != 1) { FAIL("e4 rev"); goto out; }
    if (dl_lookup(db, "r", cols_b, 2) != 1) { FAIL("added fact missing"); goto out; }
    if (dl_lookup(db, "r", cols_a, 2) != 0) { FAIL("deleted fact still present"); goto out; }
    PASS();

    /* T4b: a later stale CAS aborts the WHOLE txn — earlier buffered adds are
     * NOT applied.  Use a fresh entity/e5 so state is deterministic. */
    TEST("T4b: later stale CAS aborts whole txn (earlier add not applied)");
    {
        uint32_t sym_c = dl_intern_str(db, "gamma");
        uint32_t cols_c[2] = { sym_c, sym_c };
        assert(dl_cas_revision(db, "e5", 0, 1) == 0);   /* e5 rev = 1 */
        assert(dl_txn_begin(db) == 0);
        assert(dl_txn_add_fact(db, "r", cols_c, 2) == 0);  /* buffered add */
        assert(dl_txn_cas(db, "e5", 0, 9) == 0);  /* expected 0, current 1 = stale */
        if (dl_txn_commit(db) != DL_E_CONFLICT) {
            FAIL("expected DL_E_CONFLICT"); goto out;
        }
        /* Nothing applied: no gamma fact, e5 rev unchanged. */
        if (dl_lookup(db, "r", cols_c, 2) != 0) { FAIL("buffered add leaked"); goto out; }
        if (dl_rev_get(db, "e5", &r) != 0 || r != 1) { FAIL("e5 rev changed"); goto out; }
    }
    PASS();
out:
    dl_close(db);
    rm_dir(dir);
}

/* ─── T5: rollback — nothing durable, reopen confirms absent ───────────── */
static void test_t5_rollback(void)
{
    dl_db *db;
    uint32_t r;
    const char *dir = "build-tmp/cast5";
    uint32_t sym_a;
    uint32_t cols[2];

    TEST("T5: dl_txn_rollback discards ops; nothing durable across reopen");
    rm_dir(dir);
    mk_db_dir(dir);
    db = dl_open(dir);
    assert(db);
    assert(dl_declare_relation(db, "r", 2) == 0);
    sym_a = dl_intern_str(db, "alpha");
    assert(sym_a);
    cols[0] = sym_a; cols[1] = sym_a;

    assert(dl_txn_begin(db) == 0);
    assert(dl_txn_add_fact(db, "r", cols, 2) == 0);
    assert(dl_txn_cas(db, "e6", 0, 1) == 0);
    if (dl_txn_rollback(db) != 0) { FAIL("rollback"); goto out; }

    /* In-memory nothing applied. */
    if (dl_lookup(db, "r", cols, 2) != 0) { FAIL("add applied despite rollback"); goto out; }
    if (dl_rev_get(db, "e6", &r) != 0 || r != 0) { FAIL("rev bumped despite rollback"); goto out; }
    dl_close(db);

    /* Reopen: nothing durable. */
    db = dl_open(dir);
    assert(db);
    if (dl_lookup(db, "r", cols, 2) != 0) { FAIL("add durable despite rollback"); goto out; }
    if (dl_rev_get(db, "e6", &r) != 0 || r != 0) { FAIL("rev durable despite rollback"); goto out; }
    PASS();
out:
    if (db) dl_close(db);
    rm_dir(dir);
}

/* ─── T6: crash-in-txn (fork + fault point + _exit) ────────────────────── */

/* Fault hook: abort commit at DL_FPOINT_TXN_BEFORE_MARKER, leaving a torn
 * tail (data records, no COMMIT marker) on disk. */
static int t6_fault(dl_fpoint fp, void *user)
{
    (void)user;
    if (fp == DL_FPOINT_TXN_BEFORE_MARKER) return 1;   /* simulated crash */
    return 0;
}

static void test_t6_crash_in_txn(void)
{
    const char *dir = "build-tmp/cast6";
    const char *wal = "build-tmp/cast6/txn.wal";
    pid_t pid;
    int st;
    struct stat sb;

    TEST("T6: crash-in-txn (fork+fault point+_exit) -> not committed, wal header");
    rm_dir(dir);
    mk_db_dir(dir);

    /* Parent declares the relation so it is persisted before the child forks. */
    {
        dl_db *p = dl_open(dir);
        assert(p);
        assert(dl_declare_relation(p, "r", 2) == 0);
        dl_close(p);
    }

    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        /* Child: open, begin txn, buffer ops, commit up to the fault point,
         * then _exit WITHOUT dl_close so the WAL is left exactly as written. */
        dl_db *c = dl_open(dir);
        uint32_t sym_a = dl_intern_str(c, "alpha");
        uint32_t sym_b = dl_intern_str(c, "beta");
        uint32_t cols[2] = { sym_a, sym_b };
        int rc;
        if (!c) _exit(2);
        dl_set_fault_hook(c, t6_fault, NULL);
        if (dl_txn_begin(c) != 0) _exit(3);
        if (dl_txn_cas(c, "e7", 0, 1) != 0) _exit(4);
        if (dl_txn_add_fact(c, "r", cols, 2) != 0) _exit(5);
        rc = dl_txn_commit(c);          /* fault fires -> -1, torn tail left */
        _exit(rc == -1 ? 0 : 6);        /* 0 == fault fired as expected */
    }

    assert(waitpid(pid, &st, 0) == pid);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        FAIL("child did not hit the fault point cleanly");
        rm_dir(dir);
        return;
    }

    /* Parent reopens: the transaction must NOT be committed. */
    {
        dl_db *db = dl_open(dir);
        uint32_t r;
        uint32_t sym_a = dl_intern_str(db, "alpha");
        uint32_t sym_b = dl_intern_str(db, "beta");
        uint32_t cols[2] = { sym_a, sym_b };
        assert(db);
        if (dl_rev_get(db, "e7", &r) != 0 || r != 0) { FAIL("rev committed"); goto out; }
        if (dl_lookup(db, "r", cols, 2) != 0) { FAIL("fact committed"); goto out; }
        dl_close(db);
    }

    /* txn.wal truncated to the 16-byte header (torn tail dropped). */
    if (stat(wal, &sb) != 0) { FAIL("txn.wal missing"); goto out; }
    if (sb.st_size != 16) { FAIL("txn.wal not truncated to header"); goto out; }

    PASS();
out:
    rm_dir(dir);
}

/* ─── Main ─────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("CAS / transaction tests\n");

    test_t1_cas_success();
    test_t2_conflict();
    test_t3_stale_retry();
    test_t4_atomic_commit();
    test_t5_rollback();
    test_t6_crash_in_txn();

    printf("\n%d tests run, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
