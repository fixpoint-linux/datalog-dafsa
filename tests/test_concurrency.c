/*
 * test_concurrency.c — shared-lock read-only opens + dirty-tracked closes
 *
 * T1: writer-held -> dl_open fails DL_E_LOCKED while dl_open_ro succeeds
 *     (and sees the writer's WAL'd fact in memory).
 * T2: two concurrent dl_open_ro handles both succeed (same process AND two
 *     forked reader processes).
 * T3: RO sees committed txn facts (crashed writer, in-memory txn.wal
 *     replay) without consuming/truncating txn.wal; a later RW open+close
 *     compacts + consumes it.
 * T4: RO close leaves ALL db files byte-identical (hash before/after; with
 *     a pending per-relation WAL record so any stray compaction would show).
 * T5: RO write-APIs rejected (-1; interning/term APIs -> 0) while the read
 *     APIs still work.  dl_load_facts is checked against an EXISTING csv
 *     (positive control: the same csv loads fine on an RW handle) so the
 *     rejection is the RO guard, not a missing file.  Note dl_load_facts
 *     returns the post-load TOTAL tuple count (1 pre-existing + 1 loaded
 *     = 2), not the number of newly-added facts.
 * T6: crash-sim — child dl_add_fact then _exit(0) without close; RO open
 *     sees the fact via in-memory WAL replay without mutating files; a
 *     subsequent RW open+close compacts it.
 * T7: clean RW open+close (no writes) leaves all db files byte-identical
 *     (dirty tracking suppresses the save+fsync storm); rels.txt is
 *     additionally verified UNTOUCHED (inode+mtime unchanged), not merely
 *     byte-identical.
 */

#include "dl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
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

static void mk_db_dir(const char *dir)
{
    assert(mkdir(dir, 0755) == 0 || errno == EEXIST);
}

/* dl_prefix callback: count tuples. */
static int count_cb(const uint32_t *cols, uint8_t arity, void *user)
{
    (void)cols; (void)arity;
    (*(long *)user)++;
    return 0;
}

/* dir + "/" + name into dst (NUL-terminated), or empty string on overflow. */
static void join_path(char *dst, size_t cap, const char *dir, const char *name)
{
    size_t dl = strlen(dir), nl = strlen(name);
    if (dl + nl + 2 > cap) { dst[0] = '\0'; return; }
    memcpy(dst, dir, dl);
    dst[dl] = '/';
    memcpy(dst + dl + 1, name, nl + 1);
}

/* FNV-1a over every regular file in `dir` (name + content, sorted by name
 * for stable ordering).  0 on success, -1 on error.  Catches ANY byte-level
 * mutation (compaction, WAL truncation, rels.txt rewrite, interner save). */
static int dir_hash(const char *dir, unsigned long long *out)
{
    DIR *d;
    struct dirent *de;
    char names[64][256];
    size_t n = 0, i, j;
    unsigned long long h = 1469598103934665603ULL;
    char path[4096];

    d = opendir(dir);
    if (!d) return -1;
    while ((de = readdir(d)) != NULL) {
        struct stat st;
        size_t len;
        if (de->d_name[0] == '.') continue;   /* . / .. / dotfiles */
        len = strlen(de->d_name);
        if (len >= sizeof(names[0]) || n >= 64) continue;
        join_path(path, sizeof(path), dir, de->d_name);
        if (path[0] == '\0') continue;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        memcpy(names[n], de->d_name, len + 1);
        n++;
    }
    closedir(d);

    /* insertion sort by name (deterministic hash order) */
    for (i = 1; i < n; i++) {
        char tmp[256];
        memcpy(tmp, names[i], sizeof(tmp));
        j = i;
        while (j > 0 && strcmp(names[j - 1], tmp) > 0) {
            memcpy(names[j], names[j - 1], sizeof(tmp));
            j--;
        }
        memcpy(names[j], tmp, sizeof(tmp));
    }

    for (i = 0; i < n; i++) {
        unsigned char buf[65536];
        const char *p;
        size_t got;
        FILE *f;

        for (p = names[i]; *p; p++) {
            h ^= (unsigned char)*p;
            h *= 1099511628211ULL;
        }
        h ^= 0xff;
        h *= 1099511628211ULL;

        join_path(path, sizeof(path), dir, names[i]);
        if (path[0] == '\0') return -1;
        f = fopen(path, "rb");
        if (!f) return -1;
        while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
            size_t k;
            for (k = 0; k < got; k++) {
                h ^= buf[k];
                h *= 1099511628211ULL;
            }
        }
        fclose(f);
    }
    *out = h;
    return 0;
}

/* off_t file size, or -1 if missing. */
static long file_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (long)st.st_size;
}

/* ─── T1: writer held -> DL_E_LOCKED for both dl_open and dl_open_ro ───── */
/* POSIX: F_RDLCK conflicts with F_WRLCK, so a shared-lock RO open is
 * rejected while a writer is active (and a writer is rejected while any
 * reader is active).  Once the writer releases, dl_open_ro succeeds and
 * sees the writer's fsync'd WAL fact. */
static void test_t1_writer_held(void)
{
    const char *dir = "build-tmp/conc1";
    int c2p[2], p2c[2];
    pid_t pid;
    int st;
    char b;
    int err = 0;
    dl_db *ro = NULL;

    TEST("T1: writer held -> dl_open+dl_open_ro DL_E_LOCKED; RO ok after");
    rm_dir(dir);
    mk_db_dir(dir);
    {
        dl_db *p = dl_open(dir);
        assert(p);
        assert(dl_declare_relation(p, "r", 2) == 0);
        dl_close(p);
    }

    assert(pipe(c2p) == 0);
    assert(pipe(p2c) == 0);
    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        dl_db *c;
        uint32_t cols[2] = { 1, 2 };
        close(c2p[0]);
        close(p2c[1]);
        c = dl_open(dir);                 /* takes the exclusive lock */
        if (!c) _exit(2);
        if (dl_add_fact(c, "r", cols, 2) != 1) _exit(3);   /* durable in WAL */
        if (write(c2p[1], "r", 1) != 1) _exit(4);          /* ready */
        if (read(p2c[0], &b, 1) != 1) _exit(5);            /* wait for parent */
        dl_close(c);
        _exit(0);
    }
    close(c2p[1]);
    close(p2c[0]);
    assert(read(c2p[0], &b, 1) == 1);     /* child holds the lock now */

    /* RW open must fail with DL_E_LOCKED ... */
    if (dl_open2(dir, &err) != NULL) {
        FAIL("dl_open2 succeeded while a writer holds the lock");
        goto out;
    }
    if (err != DL_E_LOCKED) {
        FAIL("dl_open2 error is not DL_E_LOCKED");
        goto out;
    }
    /* ... and so must the shared-lock RO open (F_RDLCK vs F_WRLCK). */
    if (dl_open_ro(dir, &err) != NULL) {
        FAIL("dl_open_ro succeeded while a writer holds the lock");
        goto out;
    }
    if (err != DL_E_LOCKED) {
        FAIL("dl_open_ro error is not DL_E_LOCKED");
        goto out;
    }

    PASS();
out:
    if (ro) dl_close(ro);
    assert(write(p2c[1], "a", 1) == 1);   /* let the child finish */
    close(c2p[0]);
    close(p2c[1]);
    assert(waitpid(pid, &st, 0) == pid);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        FAIL("writer child did not exit cleanly");
        rm_dir(dir);
        return;
    }

    /* Writer gone: RO open succeeds and sees the fsync'd WAL fact (the
     * child closed cleanly, so it may be compacted into the base too). */
    ro = dl_open_ro(dir, &err);
    if (!ro) {
        FAIL("dl_open_ro failed after the writer released the lock");
        rm_dir(dir);
        return;
    }
    {
        uint32_t cols[2] = { 1, 2 };
        if (dl_lookup(ro, "r", cols, 2) != 1) {
            FAIL("RO handle cannot see the writer's fact");
            dl_close(ro);
            rm_dir(dir);
            return;
        }
    }
    dl_close(ro);
    rm_dir(dir);
}

/* ─── T2: two concurrent read-only opens ───────────────────────────────── */

/* Child body for the forked-reader phase: open RO, look up, close. */
static void t2_reader_child(const char *dir)
{
    dl_db *db;
    uint32_t cols[2] = { 3, 4 };
    int err = 0;
    db = dl_open_ro(dir, &err);
    if (!db) _exit(2);
    if (dl_lookup(db, "r", cols, 2) != 1) _exit(3);
    dl_close(db);
    _exit(0);
}

static void test_t2_two_readers(void)
{
    const char *dir = "build-tmp/conc2";
    uint32_t cols[2] = { 3, 4 };
    int err = 0;

    TEST("T2: two concurrent dl_open_ro handles both succeed");
    rm_dir(dir);
    mk_db_dir(dir);
    {
        dl_db *p = dl_open(dir);
        assert(p);
        assert(dl_declare_relation(p, "r", 2) == 0);
        assert(dl_add_fact(p, "r", cols, 2) == 1);
        dl_close(p);
    }

    /* Same process: two live RO handles at once (fcntl locks are compatible
     * with themselves within a process). */
    {
        dl_db *a = dl_open_ro(dir, &err);
        dl_db *b;
        if (!a) { FAIL("first dl_open_ro failed"); goto out; }
        b = dl_open_ro(dir, &err);
        if (!b) { FAIL("second dl_open_ro failed"); goto out; }
        if (dl_lookup(a, "r", cols, 2) != 1 ||
            dl_lookup(b, "r", cols, 2) != 1) {
            FAIL("RO handle cannot read while another RO handle is open");
            goto out;
        }
        dl_close(b);
        dl_close(a);
    }

    /* Cross process: two forked readers open simultaneously. */
    {
        pid_t p1 = fork(), p2;
        assert(p1 >= 0);
        if (p1 == 0) t2_reader_child(dir);
        p2 = fork();
        assert(p2 >= 0);
        if (p2 == 0) t2_reader_child(dir);
        assert(waitpid(p1, &err, 0) == p1);
        if (!WIFEXITED(err) || WEXITSTATUS(err) != 0) {
            FAIL("reader child 1 failed");
            goto out;
        }
        assert(waitpid(p2, &err, 0) == p2);
        if (!WIFEXITED(err) || WEXITSTATUS(err) != 0) {
            FAIL("reader child 2 failed");
            goto out;
        }
    }
    PASS();
out:
    rm_dir(dir);
}

/* ─── T3: RO sees committed txn facts (in-memory txn.wal replay) ───────── */
static void test_t3_ro_sees_txn_facts(void)
{
    const char *dir = "build-tmp/conc3";
    const char *txnwal = "build-tmp/conc3/txn.wal";
    uint32_t cols[2] = { 9, 10 };
    pid_t pid;
    int st;
    long before, after_ro;

    TEST("T3: RO sees committed txn facts; txn.wal not consumed by RO close");
    rm_dir(dir);
    mk_db_dir(dir);
    {
        dl_db *p = dl_open(dir);
        assert(p);
        assert(dl_declare_relation(p, "r", 2) == 0);
        dl_close(p);
    }

    /* Child commits a txn (durable in txn.wal) then _exit WITHOUT close:
     * the base DAFSA is never compacted, txn.wal keeps the committed
     * records. */
    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        dl_db *c;
        uint32_t tc[2] = { 9, 10 };
        c = dl_open(dir);
        if (!c) _exit(2);
        if (dl_txn_begin(c) != 0) _exit(3);
        if (dl_txn_add_fact(c, "r", tc, 2) != 0) _exit(4);
        if (dl_txn_commit(c) != 0) _exit(5);
        _exit(0);   /* crash-sim: no dl_close, no compaction */
    }
    assert(waitpid(pid, &st, 0) == pid);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        FAIL("txn child did not exit cleanly");
        rm_dir(dir);
        return;
    }

    before = file_size(txnwal);
    if (before <= 16) { FAIL("txn.wal has no committed records"); goto out; }

    /* RO open replays the committed prefix in memory. */
    {
        dl_db *ro = dl_open_ro(dir, NULL);
        if (!ro) { FAIL("dl_open_ro failed"); goto out; }
        if (dl_lookup(ro, "r", cols, 2) != 1) {
            FAIL("RO handle does not see the committed txn fact");
            goto out;
        }
        dl_close(ro);
    }
    after_ro = file_size(txnwal);
    if (after_ro != before) {
        FAIL("RO close mutated txn.wal (truncation leaked through)");
        goto out;
    }

    /* A read-write open+close compacts + consumes the txn WAL. */
    {
        dl_db *w = dl_open(dir);
        dl_db *ro;
        assert(w);
        dl_close(w);
        if (file_size(txnwal) != 16) {
            FAIL("RW open+close did not compact/consume txn.wal");
            goto out;
        }
        ro = dl_open_ro(dir, NULL);
        assert(ro);
        if (dl_lookup(ro, "r", cols, 2) != 1) {
            FAIL("committed txn fact lost after RW compaction");
            goto out;
        }
        dl_close(ro);
    }
    PASS();
out:
    rm_dir(dir);
}

/* ─── T4: RO close leaves ALL db files byte-identical ──────────────────── */
static void test_t4_ro_close_byte_identical(void)
{
    const char *dir = "build-tmp/conc4";
    unsigned long long h_before = 0, h_after = 0;
    pid_t pid;
    int st;

    TEST("T4: RO close leaves all db files byte-identical (pending WAL)");
    rm_dir(dir);
    mk_db_dir(dir);
    {
        dl_db *p = dl_open(dir);
        assert(p);
        assert(dl_declare_relation(p, "r", 2) == 0);
        dl_close(p);
    }

    /* Crashed writer: one fsync'd per-relation WAL record, never compacted
     * (so any stray rel_compact/rel_save on the RO side would show). */
    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        dl_db *c;
        uint32_t cols[2] = { 7, 8 };
        c = dl_open(dir);
        if (!c) _exit(2);
        if (dl_add_fact(c, "r", cols, 2) != 1) _exit(3);
        _exit(0);   /* crash-sim: no dl_close */
    }
    assert(waitpid(pid, &st, 0) == pid);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        FAIL("writer child did not exit cleanly");
        rm_dir(dir);
        return;
    }

    assert(dir_hash(dir, &h_before) == 0);
    {
        dl_db *ro = dl_open_ro(dir, NULL);
        if (!ro) { FAIL("dl_open_ro failed"); goto out; }
        dl_close(ro);
    }
    assert(dir_hash(dir, &h_after) == 0);
    if (h_before != h_after) {
        FAIL("RO close mutated db files");
        goto out;
    }
    PASS();
out:
    rm_dir(dir);
}

/* ─── T5: RO write-APIs rejected, read APIs work ───────────────────────── */
static void test_t5_ro_write_apis_rejected(void)
{
    const char *dir = "build-tmp/conc5";
    const char *csv = "build-tmp/conc5/facts.csv";  /* inside the db dir is
        fine: only rels.txt/symbols.dafsa are meta, unknown files ignored */
    uint32_t cols[2] = { 1, 2 };
    uint32_t sym;
    long n;

    TEST("T5: RO write-APIs rejected; read APIs work");
    rm_dir(dir);
    mk_db_dir(dir);
    /* A REAL csv: a nonexistent path returns -1 on ANY handle, which would
     * make the RO load_facts check below pass trivially. */
    {
        FILE *f = fopen(csv, "w");
        assert(f);
        fputs("9,9\n", f);
        fclose(f);
    }
    {
        dl_db *p = dl_open(dir);
        assert(p);
        assert(dl_declare_relation(p, "r", 2) == 0);
        sym = dl_intern_str(p, "alpha");
        assert(sym);
        cols[0] = sym; cols[1] = sym;
        assert(dl_add_fact(p, "r", cols, 2) == 1);
        dl_close(p);
    }

    {
        dl_db *ro = dl_open_ro(dir, NULL);
        uint32_t rc[2];
        if (!ro) { FAIL("dl_open_ro failed"); goto out; }

        rc[0] = 1; rc[1] = 2;
        if (dl_declare_relation(ro, "x", 2) != -1) { FAIL("declare allowed"); goto out; }
        if (dl_declare_relation_variadic(ro, "y") != -1) { FAIL("declare_variadic allowed"); goto out; }
        if (dl_add_fact(ro, "r", rc, 2) != -1) { FAIL("add_fact allowed"); goto out; }
        if (dl_delete_fact(ro, "r", rc, 2) != -1) { FAIL("delete_fact allowed"); goto out; }
        if (dl_load_facts(ro, "r", csv) != -1) { FAIL("load_facts allowed"); goto out; }
        if (dl_cas_revision(ro, "e", 0, 1) != -1) { FAIL("cas allowed"); goto out; }
        if (dl_txn_begin(ro) != -1) { FAIL("txn_begin allowed"); goto out; }
        if (dl_publish_snapshot(ro) != -1) { FAIL("publish allowed"); goto out; }
        if (dl_compile(ro) != -1) { FAIL("compile allowed"); goto out; }
        if (dl_load_rules(ro, "q(x) :- r(x,x).") != -1) { FAIL("load_rules allowed"); goto out; }
        if (dl_intern_str(ro, "beta") != 0) { FAIL("intern_str allowed"); goto out; }
        if (dl_term_cons(ro, sym, 0) != 0) { FAIL("term_cons allowed"); goto out; }
        if (dl_term_append(ro, 0, 0) != 0) { FAIL("term_append allowed"); goto out; }

        /* Read APIs still work on the RO handle. */
        rc[0] = sym; rc[1] = sym;
        if (dl_lookup(ro, "r", rc, 2) != 1) { FAIL("dl_lookup broken on RO"); goto out; }
        if (dl_intern_str_find(ro, "alpha") != sym) { FAIL("intern_str_find broken on RO"); goto out; }
        if (dl_intern_str_of(ro, sym) == NULL) { FAIL("intern_str_of broken on RO"); goto out; }
        n = 0;
        if (dl_prefix(ro, "r", NULL, 0, count_cb, &n) < 0 || n != 1) {
            FAIL("dl_prefix broken on RO");
            goto out;
        }
        {
            uint32_t rev = 99;
            if (dl_rev_get(ro, "alpha", &rev) != 0 || rev != 0) {
                FAIL("dl_rev_get broken on RO");
                goto out;
            }
        }
        dl_close(ro);
    }

    /* Positive control: the SAME csv loads on an RW handle, so the RO -1
     * above was the read-only guard, not a bad/missing file.  dl_load_facts
     * returns the post-load TOTAL tuple count, not the newly-added count:
     * relation r already holds (alpha,alpha) from dl_add_fact above, and the
     * csv adds (9,9), so expect 1 + 1 = 2. */
    {
        dl_db *w = dl_open(dir);
        if (!w) { FAIL("T5 rw control: dl_open failed"); goto out; }
        if (dl_load_facts(w, "r", csv) != 2) {
            dl_close(w);
            FAIL("T5 rw control: load_facts failed on RW handle");
            goto out;
        }
        dl_close(w);
    }
    PASS();
out:
    rm_dir(dir);
}

/* ─── T6: crash-sim — RO replay sees the fact, RW close compacts it ────── */
static void test_t6_crash_then_ro_replay(void)
{
    const char *dir = "build-tmp/conc6";
    const char *relwal = "build-tmp/conc6/r.wal";
    uint32_t cols[2] = { 5, 6 };
    unsigned long long h_before = 0, h_after = 0;
    pid_t pid;
    int st;

    TEST("T6: crashed writer's WAL fact visible via RO; RW open+close compacts");
    rm_dir(dir);
    mk_db_dir(dir);
    {
        dl_db *p = dl_open(dir);
        assert(p);
        assert(dl_declare_relation(p, "r", 2) == 0);
        dl_close(p);
    }

    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        dl_db *c;
        c = dl_open(dir);
        if (!c) _exit(2);
        if (dl_add_fact(c, "r", cols, 2) != 1) _exit(3);
        _exit(0);   /* crash-sim: fact only in r.wal on disk */
    }
    assert(waitpid(pid, &st, 0) == pid);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        FAIL("writer child did not exit cleanly");
        rm_dir(dir);
        return;
    }
    if (file_size(relwal) <= 16) { FAIL("r.wal has no pending record"); goto out; }

    assert(dir_hash(dir, &h_before) == 0);
    {
        dl_db *ro = dl_open_ro(dir, NULL);
        if (!ro) { FAIL("dl_open_ro failed"); goto out; }
        if (dl_lookup(ro, "r", cols, 2) != 1) {
            FAIL("RO handle does not see the crashed writer's WAL fact");
            goto out;
        }
        dl_close(ro);
    }
    assert(dir_hash(dir, &h_after) == 0);
    if (h_before != h_after) {
        FAIL("RO open+close mutated files (WAL replay was not in-memory)");
        goto out;
    }

    /* RW open replays + compacts at open; the close is clean (relation not
     * dirty).  The fact must now live in the base DAFSA. */
    {
        dl_db *w = dl_open(dir);
        dl_db *ro;
        assert(w);
        if (dl_lookup(w, "r", cols, 2) != 1) { FAIL("fact lost on RW open"); goto out; }
        dl_close(w);
        if (file_size(relwal) != 16) {
            FAIL("RW open+close did not compact r.wal");
            goto out;
        }
        ro = dl_open_ro(dir, NULL);
        assert(ro);
        if (dl_lookup(ro, "r", cols, 2) != 1) {
            FAIL("fact lost after RW compaction");
            goto out;
        }
        dl_close(ro);
    }
    PASS();
out:
    rm_dir(dir);
}

/* ─── T7: clean RW open+close leaves files byte-identical ──────────────── */
static void test_t7_clean_rw_close_noop(void)
{
    const char *dir = "build-tmp/conc7";
    unsigned long long h_before = 0, h_after = 0;
    uint32_t sym;
    uint32_t cols[2];

    TEST("T7: clean RW open+close (no writes) leaves all files identical");
    rm_dir(dir);
    mk_db_dir(dir);
    {
        dl_db *p = dl_open(dir);
        assert(p);
        assert(dl_declare_relation(p, "r", 2) == 0);
        sym = dl_intern_str(p, "alpha");
        assert(sym);
        cols[0] = sym; cols[1] = sym;
        assert(dl_add_fact(p, "r", cols, 2) == 1);
        dl_close(p);
    }

    assert(dir_hash(dir, &h_before) == 0);
    {
        struct stat st_before, st_after;
        char rels_path[512];
        dl_db *w;

        /* meta_dirty regression check: a rewrite is atomic (tmp+rename), so
         * it changes the inode; a same-inode rewrite would still move mtime.
         * Content-identity alone can't catch the rewrite (it'd be
         * byte-identical anyway). */
        snprintf(rels_path, sizeof(rels_path), "%s/rels.txt", dir);
        assert(stat(rels_path, &st_before) == 0);

        w = dl_open(dir);   /* touch NOTHING */
        assert(w);
        dl_close(w);

        assert(stat(rels_path, &st_after) == 0);
        if (st_before.st_ino != st_after.st_ino ||
            st_before.st_mtim.tv_sec != st_after.st_mtim.tv_sec ||
            st_before.st_mtim.tv_nsec != st_after.st_mtim.tv_nsec) {
            FAIL("no-op RW session rewrote rels.txt (meta_dirty not clean)");
            goto out;
        }
    }
    assert(dir_hash(dir, &h_after) == 0);
    if (h_before != h_after) {
        FAIL("clean RW close rewrote files (dirty tracking broken)");
        goto out;
    }
    PASS();
out:
    rm_dir(dir);
}

/* ─── Main ─────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("concurrency (shared-lock RO + dirty-tracked close) tests\n");

    test_t1_writer_held();
    test_t2_two_readers();
    test_t3_ro_sees_txn_facts();
    test_t4_ro_close_byte_identical();
    test_t5_ro_write_apis_rejected();
    test_t6_crash_then_ro_replay();
    test_t7_clean_rw_close_noop();

    printf("\n%d tests run, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
