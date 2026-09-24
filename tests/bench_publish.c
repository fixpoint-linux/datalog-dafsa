/*
 * bench_publish.c — publish-cost-vs-store-size + materialization/serialization
 * split harness (selfreg-dl-storage handoff, STEP 2 deliverable).
 *
 * For each N in a size ladder, build a store of N facts spread over R
 * relations, publish once (baseline), then measure a SECOND publish with a
 * tiny delta (K new facts in ONE relation). Timing instrumentation hooks
 * (optional -DINST) are provided by the engine when built with
 * -Ddl_publish_timing; without it the harness still measures wall totals.
 *
 * Usage: bench_publish [max_n] [step] [rels]
 *   default: N in {10k, 40k, 160k, 640k}, 8 relations
 */

#include "dl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* publish timing counters (dl.zig, selfreg-dl-storage) */
typedef struct {
    uint64_t n_publishes;
    uint64_t t_intern_ns, t_terms_ns, t_rel_save_ns, t_rel_other_ns;
    uint64_t t_fsync_ns, t_rename_ns, t_current_ns, t_prune_ns;
    uint64_t t_ivm_ns, t_total_ns;
} dl_pub_timing;

extern void dl_publish_timing_get(dl_pub_timing *out);
extern void dl_publish_timing_reset(void);

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void print_timing(const char *tag, const dl_pub_timing *t)
{
    double n = (double)t->n_publishes;
    if (n == 0) n = 1;
    printf("  [%s] per-publish ms: intern=%.2f terms=%.2f rel_save=%.2f rel_other=%.2f "
           "fsync=%.2f rename=%.2f current=%.2f prune=%.2f ivm=%.2f | total=%.2f (n=%llu)\n",
           tag,
           t->t_intern_ns / 1e6 / n, t->t_terms_ns / 1e6 / n,
           t->t_rel_save_ns / 1e6 / n, t->t_rel_other_ns / 1e6 / n,
           t->t_fsync_ns / 1e6 / n, t->t_rename_ns / 1e6 / n,
           t->t_current_ns / 1e6 / n, t->t_prune_ns / 1e6 / n,
           t->t_ivm_ns / 1e6 / n, t->t_total_ns / 1e6 / n,
           (unsigned long long)t->n_publishes);
    fflush(stdout);
}

/* xorshift PRNG so runs are deterministic */
static uint32_t rng_state = 0x9e3779b9u;
static uint32_t rng_next(void)
{
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    rng_state = x;
    return x;
}

int main(int argc, char **argv)
{
    long max_n = (argc > 1) ? atol(argv[1]) : 640000;
    long step0 = (argc > 2) ? atol(argv[2]) : 10000;
    int nrels = (argc > 3) ? atoi(argv[3]) : 8;

    printf("%-10s %-10s %-12s %-12s %-12s %-12s\n",
           "N", "rels", "build_s", "pub1_s", "pub2_s", "delta_facts");
    fflush(stdout);

    for (long n = step0; n <= max_n; n *= 4) {
        char dir[160], cmd[200];
        snprintf(dir, sizeof(dir), "/tmp/bench_pub_%d_%ld", (int)getpid(), n);
        snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
        if (system(cmd) != 0) { /* best effort */ }

        dl_db *db = dl_open(dir);
        if (!db) { printf("open failed\n"); return 1; }

        double t0 = now();

        char name[32];
        for (int r = 0; r < nrels; r++) {
            snprintf(name, sizeof(name), "rel%d", r);
            if (dl_declare_relation(db, name, 3) != 0) {
                printf("declare failed\n"); return 1;
            }
        }

        long per = n / nrels;
        for (int r = 0; r < nrels; r++) {
            snprintf(name, sizeof(name), "rel%d", r);
            for (long i = 0; i < per; i++) {
                uint32_t cols[3];
                cols[0] = rng_next();
                cols[1] = rng_next();
                cols[2] = (uint32_t)r;
                if (dl_add_fact(db, name, cols, 3) != 1) {
                    /* dup or error: ignore for bench purposes */
                }
            }
        }

        double t_build = now() - t0;

        /* publish 1: full store, first snapshot */
        dl_publish_timing_reset();
        t0 = now();
        if (dl_publish_snapshot(db) != 0) {
            printf("publish1 failed\n"); return 1;
        }
        double t_pub1 = now() - t0;
        dl_pub_timing tm1;
        dl_publish_timing_get(&tm1);
        print_timing("pub1", &tm1);

        /* small delta: 100 facts into ONE relation */
        for (long i = 0; i < 100; i++) {
            uint32_t cols[3];
            cols[0] = 900000;
            cols[1] = (uint32_t)i;
            cols[2] = 0;
            if (dl_add_fact(db, "rel0", cols, 3) != 1) { /* dup: ok */ }
        }

        /* publish 2: same store size, delta of 100 facts */
        dl_publish_timing_reset();
        t0 = now();
        if (dl_publish_snapshot(db) != 0) {
            printf("publish2 failed\n"); return 1;
        }
        double t_pub2 = now() - t0;
        dl_pub_timing tm2;
        dl_publish_timing_get(&tm2);
        print_timing("pub2-delta100", &tm2);

        printf("%-10ld %-10d %-12.4f %-12.4f %-12.4f %-12ld\n",
               n, nrels, t_build, t_pub1, t_pub2, 100L);
        fflush(stdout);

        dl_close(db);
        snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
        if (system(cmd) != 0) { /* best effort */ }
    }

    return 0;
}
