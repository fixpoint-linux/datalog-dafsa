/*
 * fairlock_batch.c — one Phase-0 M1b "batch" of the real agent shape, in ONE
 * FRESH PROCESS (the unit the M1b measurement is made of: a fresh process per
 * call, K concurrent slots).
 *
 * The batch, exactly as Phase 0 defines it:
 *   open2 -> declare own relation -> 3 facts with NEW strings ->
 *   publish every 5th batch -> close
 *
 * It is the MEASUREMENT harness for the writer-lock fairness change, not a
 * test: it prints one KEY=VALUE line and exits 0, or exits 2 on an unexpected
 * error.  tests/fairlock_m1b.py drives K of these concurrently and builds the
 * before/after table.
 *
 *   fairlock_batch <db_dir> <slot> <batch>
 *   fairlock_batch <db_dir> <slot> <batch> --reader
 *
 * Output (one line, space separated):
 *   pid=N slot=S batch=B attempts=A refused=R open_ms=O publish_ms=P
 *   total_ms=T open_ts=TS close_ts=TC work_ok=W
 * where
 *   attempts  — dl_open2 calls made (1 = first try; >1 = the open was refused
 *               and the caller retried in user space: the OLD behaviour)
 *   refused   — DL_E_LOCKED returns seen (the starvation signal)
 *   open_ms   — wall time from process start of the open to a handle
 *   open_ts   — CLOCK_MONOTONIC ns at open success; close_ts at dl_close
 *               entry.  [open_ts, close_ts) is the interval in which this
 *               process holds the writer-exclusive byte 0, so the driver's
 *               no-overlap check over these intervals is a STRICTLY STRONGER
 *               statement than "no two mutation windows overlap" (every
 *               mutation window is a subset of this interval).
 *
 * Retry policy (only reachable in fail-fast mode; the default blocks in the
 * kernel and returns on the first call): a SHORT JITTERED sleep, 1-3 ms, so
 * the loser of a race does not spin.  This is the "honest backoff" the
 * measurement is about — the old shape's retries were bimodal (attempt 1, or
 * >10) because the loser was re-refused for 10-22 s.
 *
 * --reader: the reader arm (open_ro -> lookup -> close), used to prove a
 * reader still opens and reads while a writer is live, and that its bounded
 * wait does not turn into a hang.
 */
#include "dl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>

#define RETRY_SLEEP_MIN_NS 1000000L /* 1 ms */
#define RETRY_SLEEP_JITTER_NS 2000000L /* + up to 2 ms */

static long long now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
}

static void sleep_ns(long long ns)
{
    struct timespec req, rem;
    req.tv_sec = (time_t)(ns / 1000000000LL);
    req.tv_nsec = (long)(ns % 1000000000LL);
    while (nanosleep(&req, &rem) != 0 && errno == EINTR)
        req = rem;
}

static double ms_since(long long t0) { return (double)(now_ns() - t0) / 1e6; }

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <db_dir> <slot> <batch> [--reader]\n"
                        "       %s --seed <db_dir> <rel> <n_facts>\n", argv[0], argv[0]);
        return 64;
    }
    /* Seeding mode: fill a relation with <n> facts in ONE process (the CSV
     * loader path costs ~0.6 ms/fact; this is ~0.1 ms/fact).  It exists only
     * so the measurement starts from a store with CONTENT — a publish on an
     * empty store is ~0.2 ms and would make the lock hold unrepresentative. */
    if (strcmp(argv[1], "--seed") == 0) {
        if (argc < 5) {
            fprintf(stderr, "usage: %s --seed <db_dir> <rel> <n_facts>\n", argv[0]);
            return 64;
        }
        int err = 0;
        dl_db *sdb = dl_open2(argv[2], &err);
        if (!sdb) {
            fprintf(stderr, "fairlock_batch: seed open failed err=%d\n", err);
            return 2;
        }
        if (dl_declare_relation(sdb, argv[3], 2) != 0) {
            dl_close(sdb);
            return 2;
        }
        long n = strtol(argv[4], NULL, 10);
        long added = 0;
        for (long i = 0; i < n; i++) {
            char a[64], b[64];
            snprintf(a, sizeof a, "%s_k%ld", argv[3], i);
            snprintf(b, sizeof b, "%s_v%ld", argv[3], i * 3);
            uint32_t cols[2];
            cols[0] = dl_intern_str(sdb, a);
            cols[1] = dl_intern_str(sdb, b);
            if (dl_add_fact(sdb, argv[3], cols, 2) == 1)
                added++;
        }
        dl_close(sdb);
        printf("seeded=%ld\n", added);
        return added == n ? 0 : 3;
    }
    if (argc < 4) {
        fprintf(stderr, "usage: %s <db_dir> <slot> <batch> [--reader]\n", argv[0]);
        return 64;
    }
    const char *dir = argv[1];
    const char *slot = argv[2];
    const char *batch = argv[3];
    int reader = 0;
    int no_retry = 0;
    double hold_ms = 0.0;
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--reader") == 0)
            reader = 1;
        else if (strcmp(argv[i], "--no-retry") == 0)
            no_retry = 1; /* surface the engine's refusal instead of
                           * reproducing the caller's user-space retry loop */
        else if (strcmp(argv[i], "--hold-ms") == 0 && i + 1 < argc)
            hold_ms = atof(argv[++i]);
    }

    const long long t_start = now_ns();
    int attempts = 0, refused = 0, err = 0;
    dl_db *db = NULL;

    for (;;) {
        attempts++;
        err = 0;
        db = reader ? dl_open_ro(dir, &err) : dl_open2(dir, &err);
        if (db)
            break;
        if (err != DL_E_LOCKED) {
            fprintf(stderr, "fairlock_batch: open failed err=%d\n", err);
            return 2;
        }
        refused++;
        if (no_retry)
            break;
        /* jitter from pid+batch+attempt: no RNG needed, and two slots never
         * line up on the same sleep. */
        long long j = (long long)(getpid() * 2654435761u + (unsigned)attempts * 7919u +
                                  (unsigned)strtoul(batch, NULL, 10) * 104729u) %
                      RETRY_SLEEP_JITTER_NS;
        sleep_ns(RETRY_SLEEP_MIN_NS + (j < 0 ? -j : j));
    }
    if (!db) {
        fprintf(stderr, "fairlock_batch: open refused err=%d attempts=%d\n",
                err, attempts);
        return 2;
    }
    const long long t_open = now_ns();
    const double open_ms = (double)(t_open - t_start) / 1e6;
    /* Deterministic handshake for the crash / reader tests: the caller needs
     * to know the handle (and so the writer-exclusive byte) is held BEFORE it
     * acts, rather than guessing with a sleep. */
    fprintf(stderr, "LOCKED pid=%ld ts=%lld\n", (long)getpid(), (long long)t_open);
    fflush(stderr);

    int work_ok = 1;
    double publish_ms = 0.0;
    long bnum = strtol(batch, NULL, 10);

    if (reader) {
        char rel[64];
        snprintf(rel, sizeof rel, "s%s", slot);
        uint32_t cols[2] = { 1, 2 };
        /* a miss is fine (the relation may not be declared yet); a -1 is not */
        if (dl_lookup(db, rel, cols, 2) < 0)
            work_ok = 0;
    } else {
        char rel[64];
        snprintf(rel, sizeof rel, "s%s", slot);
        if (dl_declare_relation(db, rel, 2) != 0) {
            fprintf(stderr, "fairlock_batch: declare %s failed\n", rel);
            dl_close(db);
            return 2;
        }
        /* 3 facts whose columns are NEW strings: intern_str appends to the
         * symbol table (the 50ms -> 0.05ms path from 110b5f9). */
        for (int i = 0; i < 3; i++) {
            char a[96], b[96];
            snprintf(a, sizeof a, "n%s_%ld_%d_a", slot, bnum, i);
            snprintf(b, sizeof b, "n%s_%ld_%d_b", slot, bnum, i);
            uint32_t cols[2];
            cols[0] = dl_intern_str(db, a);
            cols[1] = dl_intern_str(db, b);
            if (cols[0] == 0 || cols[1] == 0) {
                work_ok = 0;
                break;
            }
            if (dl_add_fact(db, rel, cols, 2) != 1) {
                work_ok = 0;
                break;
            }
        }
        /* publish every 5th batch: the expensive mutation window (~90 ms
         * worst case) that the other writers queue behind. */
        if (bnum % 5 == 0) {
            long long tp = now_ns();
            if (dl_publish_snapshot(db) != 0)
                work_ok = 0;
            publish_ms = (double)(now_ns() - tp) / 1e6;
        }
    }

    /* Deterministic hold for the crash / reader tests: keep the handle (and so
     * byte 0) held for a controlled span instead of racing the caller. */
    if (hold_ms > 0.0)
        sleep_ns((long long)(hold_ms * 1e6));

    const long long t_before_close = now_ns();
    dl_close(db);
    const long long t_close = now_ns();
    const double total_ms = (double)(t_close - t_start) / 1e6;

    printf("pid=%ld slot=%s batch=%ld attempts=%d refused=%d open_ms=%.3f "
           "publish_ms=%.3f total_ms=%.3f open_ts=%lld close_ts=%lld work_ok=%d\n",
           (long)getpid(), slot, bnum, attempts, refused, open_ms, publish_ms,
           total_ms, (long long)t_open, (long long)t_before_close, work_ok);
    (void)ms_since;
    return work_ok ? 0 : 3;
}
