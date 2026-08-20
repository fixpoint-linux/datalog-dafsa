/*
 * bench.c — Quick demonstration benchmark for the DAFSA-backed Datalog VM.
 *
 * Three in-memory workloads over temp databases under /tmp:
 *   1. Transitive closure on random graphs (N = 100, 200, 500)
 *   2. Prefix query throughput on an arity-3 relation (~50k facts)
 *   3. Regex walk over a published snapshot (~10k facts)
 *
 * Prints a tabular summary and exits 0.  Kept deliberately small so it
 * completes in a few seconds — a demo, not a rigorous benchmark.
 */

#include "dl.h"
#include "regexwalk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ─── Time helper ────────────────────────────────────────────────────────── */

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ─── Small deterministic PRNG (xorshift32) ─────────────────────────────── */

static uint32_t rng_state = 0x12345678u;

static uint32_t rng_next(void)
{
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

/* ─── Tuple counting callback ───────────────────────────────────────────── */

typedef struct { long count; } cnt_ctx;

static int count_cb(const uint32_t *cols, uint8_t arity, void *user)
{
    (void)cols; (void)arity;
    ((cnt_ctx *)user)->count++;
    return 0;
}

/* ─── Workload 1: transitive closure on random graphs ───────────────────── */

static void bench_tc(void)
{
    static const long sizes[] = {100, 200, 500};
    size_t i;

    printf("%-20s | %-6s | %9s | %9s | %9s\n",
           "workload", "N", "time_s", "tuples", "tuples/s");

    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        long N = sizes[i];
        char dir[160], cmd[200];
        long e, ne = 2 * N;

        snprintf(dir, sizeof(dir), "/tmp/bench_tc_%d_%ld",
                 (int)getpid(), N);
        snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
        system(cmd);

        dl_db *db = dl_open(dir);
        if (!db) {
            printf("  tc N=%ld: dl_open failed\n", N);
            continue;
        }
        if (dl_declare_relation(db, "edge", 2) != 0) {
            printf("  tc N=%ld: declare failed\n", N);
            dl_close(db); continue;
        }

        /* ~2N random edges over N nodes (allows self-loops + dups). */
        for (e = 0; e < ne; e++) {
            uint32_t cols[2];
            cols[0] = rng_next() % (uint32_t)N;
            cols[1] = rng_next() % (uint32_t)N;
            dl_add_fact(db, "edge", cols, 2);
        }

        /* Recursive transitive closure. */
        if (dl_load_rules(db,
                "tc(X,Y) :- edge(X,Y).\n"
                "tc(X,Y) :- edge(X,Z), tc(Z,Y).\n") != 0) {
            printf("  tc N=%ld: load_rules failed\n", N);
            dl_close(db); continue;
        }

        double t0 = now();
        if (dl_compile(db) != 0) {
            printf("  tc N=%ld: compile failed\n", N);
            dl_close(db); continue;
        }
        if (dl_publish_snapshot(db) != 0) {
            printf("  tc N=%ld: publish failed\n", N);
            dl_close(db); continue;
        }
        cnt_ctx cnt = {0};
        long n = dl_query(db, "tc", count_cb, &cnt);
        double dt = now() - t0;
        if (n < 0) {
            printf("  tc N=%ld: query failed\n", N);
            dl_close(db); continue;
        }
        printf("%-20s | %-6ld | %9.4f | %9ld | %9.0f\n",
               "tc", N, dt, cnt.count,
               (dt > 0 ? (double)cnt.count / dt : 0.0));

        dl_close(db);
        snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
        system(cmd);
    }
}

/* ─── Workload 2: prefix query throughput ───────────────────────────────── */

static void bench_prefix(void)
{
    char csv[160], dir[160], cmd[200];
    const long NFACTS = 50000;
    const long NBINDS = 100000;
    FILE *f;
    long i;

    /* a in [0,200), b in [0,100), c in [0,10): ~2.5 facts per (a,b). */
    const uint32_t DA = 200, DB = 100, DC = 10;

    snprintf(dir, sizeof(dir), "/tmp/bench_prefix_%d", (int)getpid());
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    system(cmd);
    snprintf(csv, sizeof(csv), "/tmp/bench_prefix_%d.csv", (int)getpid());

    f = fopen(csv, "w");
    if (!f) { printf("  prefix: cannot open csv\n"); return; }
    for (i = 0; i < NFACTS; i++) {
        fprintf(f, "%u,%u,%u\n",
                rng_next() % DA, rng_next() % DB, rng_next() % DC);
    }
    fclose(f);

    dl_db *db = dl_open(dir);
    if (!db) { printf("  prefix: dl_open failed\n"); remove(csv); return; }
    if (dl_declare_relation(db, "r", 3) != 0) {
        printf("  prefix: declare failed\n"); dl_close(db); remove(csv); return;
    }
    if (dl_load_facts(db, "r", csv) < 0) {
        printf("  prefix: load_facts failed\n"); dl_close(db); remove(csv); return;
    }
    if (dl_publish_snapshot(db) != 0) {
        printf("  prefix: publish failed\n"); dl_close(db); remove(csv); return;
    }

    double t0 = now();
    cnt_ctx cnt = {0};
    for (i = 0; i < NBINDS; i++) {
        uint32_t leading[2];
        leading[0] = rng_next() % DA;
        leading[1] = rng_next() % DB;
        if (dl_query_bound(db, "r", leading, 2, count_cb, &cnt) < 0) {
            printf("  prefix: bound query failed\n");
            dl_close(db); remove(csv); return;
        }
    }
    double dt = now() - t0;
    printf("%-20s | %-6s | %9.4f | %9ld | %9.0f\n",
           "prefix-bind", "50k", dt, cnt.count,
           (dt > 0 ? (double)cnt.count / dt : 0.0));

    dl_close(db);
    remove(csv);
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    system(cmd);
}

/* ─── Workload 3: regex walk over a published snapshot ──────────────────── */

static void bench_regex(void)
{
    char csv[160], dir[160], cmd[200];
    const long NFACTS = 10000;
    FILE *f;
    long i;

    /* Single column.  Values in [0, 1e6) — all < 2^24 so the u32BE key's
     * top byte is always 0x00 (pattern "\x00.*" matches every fact), while
     * the wide range keeps 10k random facts essentially all distinct. */
    const uint32_t DV = 1000000;

    snprintf(dir, sizeof(dir), "/tmp/bench_regex_%d", (int)getpid());
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    system(cmd);
    snprintf(csv, sizeof(csv), "/tmp/bench_regex_%d.csv", (int)getpid());

    f = fopen(csv, "w");
    if (!f) { printf("  regex: cannot open csv\n"); return; }
    for (i = 0; i < NFACTS; i++)
        fprintf(f, "%u\n", rng_next() % DV);
    fclose(f);

    dl_db *db = dl_open(dir);
    if (!db) { printf("  regex: dl_open failed\n"); remove(csv); return; }
    if (dl_declare_relation(db, "r", 1) != 0) {
        printf("  regex: declare failed\n"); dl_close(db); remove(csv); return;
    }
    if (dl_load_facts(db, "r", csv) < 0) {
        printf("  regex: load_facts failed\n"); dl_close(db); remove(csv); return;
    }
    if (dl_publish_snapshot(db) != 0) {
        printf("  regex: publish failed\n"); dl_close(db); remove(csv); return;
    }

    /* Keys are 4 bytes u32BE + trailing \0.  Match first byte 0x00
     * (all values < 2^24) followed by anything — matches all facts. */
    regex_dfa *dfa = regex_compile("\\x00.*");
    if (!dfa || dfa->errmsg) {
        printf("  regex: bad pattern: %s\n", dfa ? dfa->errmsg : "(alloc)");
        regex_dfa_free(dfa); dl_close(db); remove(csv); return;
    }

    double t0 = now();
    cnt_ctx cnt = {0};
    long n = dl_pattern(db, "r", 0, dfa, count_cb, &cnt);
    double dt = now() - t0;
    regex_dfa_free(dfa);
    if (n < 0) {
        printf("  regex: pattern query failed\n");
        dl_close(db); remove(csv); return;
    }
    printf("%-20s | %-6s | %9.4f | %9ld | %9.0f\n",
           "regex-walk", "10k", dt, cnt.count,
           (dt > 0 ? (double)cnt.count / dt : 0.0));

    dl_close(db);
    remove(csv);
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    system(cmd);
}

int main(void)
{
    bench_tc();
    bench_prefix();
    bench_regex();
    return 0;
}
