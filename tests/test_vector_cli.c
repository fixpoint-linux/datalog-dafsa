/*
 * test_vector_cli.c — S4 CLI vsearch/vhybrid tests
 *
 * Exercises the `dl vsearch` and `dl vhybrid` CLI paths via --ivec hex (no
 * Python / fastembed dependency).  A synthetic DB (entity + __vec_q__ +
 * observation + postings) is built with the C API, persisted, then the actual
 * `dl` binary is run as a controlled subprocess against it:
 *   - vsearch returns the correct scan-ranked entity (exact cosine order)
 *   - vsearch --cand-only dumps the ranked sym-ids
 *   - vhybrid intersects lexical hits (mapped obs->entity) with the vector
 *     ranking and emits the intersection in that order
 *   - the C API path is also exercised directly (search + rerank ordering)
 *
 * SUPERSEDED (2026-08-11): the CLI used to take --sig (a 256-bit ITQ
 * signature) and --radius (the MIH pigeonhole budget).  Neither exists any
 * more: the query path is a scan-and-rank over __vec_q__, so the only query
 * input is the pre-quantized int8 vector (--ivec) and the only knobs are --k
 * and --version.  Passing --sig/--radius is now an "unexpected argument"
 * error, which is itself asserted below.
 */
#include "dl.h"
#include "vector.h"
#include "index.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

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

static const char *BASE = "build-tmp/vector-cli";

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

/* ─── Deterministic RNG (LCG) ───────────────────────────────────────────── */
static uint32_t rng_state;
static void rng_seed(uint32_t s) { rng_state = s; }
static uint32_t rng_next(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

static uint32_t pack4(int8_t b0, int8_t b1, int8_t b2, int8_t b3)
{
    return (uint32_t)(uint8_t)b0 | ((uint32_t)(uint8_t)b1 << 8) |
           ((uint32_t)(uint8_t)b2 << 16) | ((uint32_t)(uint8_t)b3 << 24);
}

typedef struct {
    char name[32];
    uint32_t sym, type;
    int8_t vec[VEC_D];
    uint32_t ivec[VEC_IVEC_WORDS];
} ent_t;

static void gen_entity(ent_t *e, const char *name, dl_db *db)
{
    int d;
    snprintf(e->name, sizeof(e->name), "%s", name);
    e->sym = dl_intern_str(db, e->name);
    e->type = dl_intern_str(db, "doc");
    for (d = 0; d < VEC_D; d++) e->vec[d] = (int8_t)(int)(rng_next() % 201u) - 100;
    for (int c = 0; c < VEC_IVEC_WORDS; c++)
        e->ivec[c] = pack4(e->vec[c*4+0], e->vec[c*4+1], e->vec[c*4+2], e->vec[c*4+3]);
}

static void gen_query(ent_t *q, const ent_t *src)
{
    int d;
    snprintf(q->name, sizeof(q->name), "q");
    for (d = 0; d < VEC_D; d++) {
        int v = src->vec[d] + (int)(rng_next() % 7u) - 3;
        if (v > 127) v = 127;
        if (v < -128) v = -128;
        q->vec[d] = (int8_t)v;
    }
    for (int c = 0; c < VEC_IVEC_WORDS; c++)
        q->ivec[c] = pack4(q->vec[c*4+0], q->vec[c*4+1], q->vec[c*4+2], q->vec[c*4+3]);
}

static void declare_vector_rels(dl_db *db)
{
    dl_declare_relation(db, VEC_ENTITY_REL, 2);
    dl_declare_relation(db, "__vec_q__", 3);
}

static int add_entity_to_index(dl_db *db, const ent_t *e)
{
    uint32_t cols[3];
    int c;
    cols[0] = e->sym;
    cols[1] = e->type;
    if (dl_add_fact(db, VEC_ENTITY_REL, cols, 2) != 1) return -1;
    for (c = 0; c < VEC_IVEC_WORDS; c++) {
        cols[0] = e->sym;
        cols[1] = (uint32_t)c;
        cols[2] = e->ivec[c];
        if (dl_add_fact(db, "__vec_q__", cols, 3) != 1) return -1;
    }
    return 0;
}

/* ─── Result collection (API path) ──────────────────────────────────────── */
#define COLL_MAX 4096
typedef struct { uint32_t syms[COLL_MAX]; int n; } coll;
static int coll_search_cb(uint32_t sym, int score, void *user)
{
    coll *c = user;
    (void)score;
    if (c->n < COLL_MAX) c->syms[c->n++] = sym;
    return 0;
}

/* ─── Controlled CLI subprocess ─────────────────────────────────────────── */

/* Run `./dl ...` as a child, capture stdout.  Returns 0 on success with
 * `out` NUL-terminated, -1 on failure (fork/pipe error or 8s read timeout).
 * The parent reads the pipe with a poll deadline so a hung child cannot hang
 * the test; on timeout the child is SIGKILLed and -1 is returned. */
static int run_cli(const char *dbdir, char *const argv[], char *out,
                   size_t outcap)
{
    int fds[2];
    pid_t pid;
    size_t n = 0;
    int status;
    (void)dbdir;   /* the -d <dir> is carried in argv */
    if (pipe(fds) != 0) return -1;
    pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return -1; }
    if (pid == 0) {
        /* child */
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[0]);
        close(fds[1]);
        execv("./dl", argv);
        _exit(127);
    }
    /* parent: read until EOF or a hard deadline */
    close(fds[1]);
    for (;;) {
        struct pollfd pfd;
        ssize_t r;
        if (n + 1 >= outcap) break;
        pfd.fd = fds[0];
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, 8000) <= 0) {
            /* timed out: child is hung — kill it and bail */
            kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0) { /* EINTR */ }
            out[n] = '\0';
            close(fds[0]);
            return -1;
        }
        if (!(pfd.revents & (POLLIN | POLLHUP | POLLERR)))
            continue;
        r = read(fds[0], out + n, outcap - n - 1);
        if (r <= 0) break;
        n += (size_t)r;
    }
    out[n] = '\0';
    close(fds[0]);
    while (waitpid(pid, &status, 0) < 0) { /* EINTR loop */ }
    return 0;
}

static int cli_has(const char *hay, const char *needle)
{
    return strstr(hay, needle) != NULL;
}

static void hex_word(uint32_t w, char *buf)
{
    snprintf(buf, 9, "%08x", w);
}

static void ivec_hex_str(const uint32_t *ivec, char *out)
{
    int i;
    out[0] = '\0';
    for (i = 0; i < VEC_IVEC_WORDS; i++) {
        char b[9];
        hex_word(ivec[i], b);
        strcat(out, b);
    }
}

/* ─── Test 1: vsearch CLI end-to-end ────────────────────────────────────── */
static void t_vsearch_cli(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_vsearch");
    ent_t ents[3], q;
    int src = 0;
    char ivec[VEC_IVEC_WORDS * 8 + 1];
    char out[65536];
    char *argv[16];
    int i;

    TEST("vsearch CLI: scan-and-rank ordering");

    declare_vector_rels(db);
    rng_seed(0xF00Du);
    for (i = 0; i < 3; i++) {
        char nm[32];
        snprintf(nm, sizeof(nm), "ent%d", i);
        gen_entity(&ents[i], nm, db);
        if (add_entity_to_index(db, &ents[i]) != 0) {
            FAIL("build index");
            dl_close(db);
            return;
        }
    }
    gen_query(&q, &ents[src]);
    dl_close(db);   /* persist so the subprocess can open the dir */

    ivec_hex_str(q.ivec, ivec);

    i = 0;
    argv[i++] = "dl";
    argv[i++] = "-d";
    argv[i++] = (char *)dir;
    argv[i++] = "vsearch";
    argv[i++] = "dummy";
    argv[i++] = "--ivec";
    argv[i++] = ivec;
    argv[i++] = "--k";
    argv[i++] = "10";
    argv[i++] = NULL;

    if (run_cli(dir, argv, out, sizeof(out)) != 0) {
        FAIL("subprocess failed");
        return;
    }
    if (cli_has(out, "vector search failed") || cli_has(out, "failed")) {
        FAIL("vsearch reported an error");
        return;
    }
    /* The query is a near-copy of ents[0] -> its scan-ranked top-1 must be
     * ents[0] and the name must be printed via print_value.  The FIRST
     * printed line is the top-1 (the emission order is the ranking order). */
    {
        char first[256];
        size_t k = 0;
        while (out[k] != '\n' && out[k] != '\0' && k + 1 < sizeof first) {
            first[k] = out[k];
            k++;
        }
        first[k] = '\0';
        if (strcmp(first, ents[src].name) != 0) {
            printf("  (got first line: %s)", first);
            FAIL("vsearch top-1 is not the true nearest entity");
            return;
        }
    }
    if (!cli_has(out, ents[src].name)) {
        printf("  (got: %s)", out);
        FAIL("vsearch output missing the true nearest entity");
        return;
    }
    PASS();
}

/* ─── Test 1b: --sig/--radius are gone (removed CLI surface) ────────────── */
static void t_vsearch_removed_flags(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_removed_flags");
    ent_t e, q;
    char ivec[VEC_IVEC_WORDS * 8 + 1];
    char sig[65];
    char out[65536];
    char *argv[16];
    int i;

    TEST("vsearch rejects the removed --sig/--radius flags");

    declare_vector_rels(db);
    rng_seed(0x11u);
    gen_entity(&e, "only", db);
    if (add_entity_to_index(db, &e) != 0) { FAIL("build index"); dl_close(db); return; }
    gen_query(&q, &e);
    dl_close(db);
    ivec_hex_str(q.ivec, ivec);
    memset(sig, '0', sizeof(sig) - 1);
    sig[sizeof(sig) - 1] = '\0';

    /* --sig: rejected */
    i = 0;
    argv[i++] = "dl";
    argv[i++] = "-d";
    argv[i++] = (char *)dir;
    argv[i++] = "vsearch";
    argv[i++] = "dummy";
    argv[i++] = "--ivec";
    argv[i++] = ivec;
    argv[i++] = "--sig";
    argv[i++] = sig;
    argv[i++] = NULL;
    if (run_cli(dir, argv, out, sizeof(out)) != 0) { FAIL("subprocess failed"); return; }
    if (!cli_has(out, "unexpected argument")) {
        printf("  (got: %s)", out);
        FAIL("--sig must be rejected");
        return;
    }

    /* --radius: rejected */
    i = 0;
    argv[i++] = "dl";
    argv[i++] = "-d";
    argv[i++] = (char *)dir;
    argv[i++] = "vsearch";
    argv[i++] = "dummy";
    argv[i++] = "--ivec";
    argv[i++] = ivec;
    argv[i++] = "--radius";
    argv[i++] = "16";
    argv[i++] = NULL;
    if (run_cli(dir, argv, out, sizeof(out)) != 0) { FAIL("subprocess failed"); return; }
    if (!cli_has(out, "unexpected argument")) {
        printf("  (got: %s)", out);
        FAIL("--radius must be rejected");
        return;
    }

    PASS();
}

/* ─── Test 2: vsearch --cand-only (real MIH candidate dump) ─────────────── */
static void t_vsearch_cand_only(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_candonly");
    ent_t ents[4], q;
    char ivec[VEC_IVEC_WORDS * 8 + 1];
    char out[65536];
    char *argv[16];
    int i;

    TEST("vsearch --cand-only dumps the ranked sym-ids");

    declare_vector_rels(db);
    rng_seed(0xC0DECAu);
    for (i = 0; i < 4; i++) {
        char nm[32];
        snprintf(nm, sizeof(nm), "doc%d", i);
        gen_entity(&ents[i], nm, db);
        if (add_entity_to_index(db, &ents[i]) != 0) {
            FAIL("build index");
            dl_close(db);
            return;
        }
    }
    gen_query(&q, &ents[0]);
    dl_close(db);

    ivec_hex_str(q.ivec, ivec);

    i = 0;
    argv[i++] = "dl";
    argv[i++] = "-d";
    argv[i++] = (char *)dir;
    argv[i++] = "vsearch";
    argv[i++] = "dummy";
    argv[i++] = "--ivec";
    argv[i++] = ivec;
    argv[i++] = "--k";
    argv[i++] = "100";
    argv[i++] = "--cand-only";
    argv[i++] = NULL;

    if (run_cli(dir, argv, out, sizeof(out)) != 0) {
        FAIL("subprocess failed");
        return;
    }
    if (cli_has(out, "vector search failed")) {
        FAIL("cand-only reported an error");
        return;
    }
    /* Every emitted line must be a bare u32 sym-id, the query's source must
     * be among them, AND it must be FIRST (the dump is the ranking). */
    {
        char *line = strtok(out, "\n");
        int saw_src = 0, rank = 0;
        while (line) {
            char *end;
            long v = strtol(line, &end, 10);
            if (end == line || *end != '\0') {
                FAIL("cand-only emitted a non-integer line");
                return;
            }
            if ((uint32_t)v == ents[0].sym) {
                saw_src = 1;
                if (rank != 0) {
                    FAIL("cand-only did not put the true source first");
                    return;
                }
            }
            rank++;
            line = strtok(NULL, "\n");
        }
        if (!saw_src) {
            FAIL("cand-only missing the true source candidate");
            return;
        }
        if (rank != 4) {
            printf("  (emitted %d of 4 ranked syms)\n", rank);
            FAIL("cand-only should emit every live vector for k=100");
            return;
        }
    }
    PASS();
}

/* ─── Test 3: vhybrid CLI (intersection + rerank) ───────────────────────── */
static void t_vhybrid_cli(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_vhybrid");
    ent_t ents[3], q;
    char ivec[VEC_IVEC_WORDS * 8 + 1];
    char out[65536];
    char *argv[18];
    uint32_t term_sym, obs_sym;
    int i;

    TEST("vhybrid CLI: lexical ∩ vector intersection, ranked");

    /* Vector rels + entity. */
    declare_vector_rels(db);
    rng_seed(0xBEEFu);
    for (i = 0; i < 3; i++) {
        char nm[32];
        snprintf(nm, sizeof(nm), "e%d", i);
        gen_entity(&ents[i], nm, db);
        if (add_entity_to_index(db, &ents[i]) != 0) {
            FAIL("build vector index");
            dl_close(db);
            return;
        }
    }

    /* Lexical: observation(e0,"red fox"), observation(e1,"blue fox"),
     * e2 has no matching term.  Only e0 and e1 match "fox". */
    dl_declare_relation(db, "observation", 2);
    {
        uint32_t c[2];
        uint32_t fox = dl_intern_str(db, "red fox");
        uint32_t blue = dl_intern_str(db, "blue fox");
        c[0] = ents[0].sym; c[1] = fox;   dl_add_fact(db, "observation", c, 2);
        c[0] = ents[1].sym; c[1] = blue;  dl_add_fact(db, "observation", c, 2);
    }
    aux_index_ensure_postings(db);
    term_sym = dl_intern_str(db, "fox");
    obs_sym = dl_intern_str(db, "red fox");
    aux_index_add_posting(db, term_sym, obs_sym);
    obs_sym = dl_intern_str(db, "blue fox");
    aux_index_add_posting(db, term_sym, obs_sym);

    gen_query(&q, &ents[0]);
    dl_close(db);

    ivec_hex_str(q.ivec, ivec);

    i = 0;
    argv[i++] = "dl";
    argv[i++] = "-d";
    argv[i++] = (char *)dir;
    argv[i++] = "vhybrid";
    argv[i++] = "fox";
    argv[i++] = "dummy";
    argv[i++] = "--ivec";
    argv[i++] = ivec;
    argv[i++] = "--k";
    argv[i++] = "10";
    argv[i++] = NULL;

    if (run_cli(dir, argv, out, sizeof(out)) != 0) {
        FAIL("subprocess failed");
        return;
    }
    /* e0 and e1 are lexical hits AND vector candidates (near copies of e0);
     * e2 is a vector candidate but NOT a lexical hit, so it must NOT appear.
     * The top-1 of the ranked intersection must be e0 — and it must be the
     * FIRST emitted line, i.e. the intersection keeps the vector ranking. */
    {
        char first[256];
        size_t k = 0;
        while (out[k] != '\n' && out[k] != '\0' && k + 1 < sizeof first) {
            first[k] = out[k];
            k++;
        }
        first[k] = '\0';
        if (strcmp(first, ents[0].name) != 0) {
            printf("  (got first line: %s)", first);
            FAIL("vhybrid top-1 is not the ranked best intersection");
            return;
        }
    }
    if (!cli_has(out, ents[0].name)) {
        printf("  (got: %s)", out);
        FAIL("vhybrid missing the ranked top entity");
        return;
    }
    if (cli_has(out, ents[2].name)) {
        FAIL("vhybrid must exclude the vector-only (non-lexical) entity");
        return;
    }
    PASS();
}

/* ─── Test 4: C API path directly ───────────────────────────────────────── */
static void t_api_path(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_api");
    ent_t ents[3], q;
    coll res = {{0}, 0}, rk = {{0}, 0};
    int i;

    TEST("C API: dl_vector_search + dl_vector_rerank");

    declare_vector_rels(db);
    rng_seed(0x5555u);
    for (i = 0; i < 3; i++) {
        char nm[32];
        snprintf(nm, sizeof(nm), "api%d", i);
        gen_entity(&ents[i], nm, db);
        if (add_entity_to_index(db, &ents[i]) != 0) {
            FAIL("build index");
            dl_close(db);
            return;
        }
    }
    gen_query(&q, &ents[0]);

    long n = dl_vector_search(db, q.ivec, COLL_MAX, coll_search_cb, &res);
    if (n < 0) { FAIL("dl_vector_search error"); dl_close(db); return; }
    long r = dl_vector_rerank(db, q.ivec, res.syms, res.n, 1,
                              coll_search_cb, &rk);
    if (r < 0) { FAIL("dl_vector_rerank error"); dl_close(db); return; }
    if (rk.n < 1 || rk.syms[0] != ents[0].sym) {
        FAIL("rerank top-1 != true nearest");
        dl_close(db);
        return;
    }
    PASS();
    dl_close(db);
}

int main(void)
{
    printf("=== test_vector_cli (S4) ===\n");

    t_vsearch_cli();
    t_vsearch_removed_flags();
    t_vsearch_cand_only();
    t_vhybrid_cli();
    t_api_path();

    printf("\n");
    if (tests_failed == 0)
        printf("All %d tests passed.\n", tests_run);
    else
        printf("%d/%d tests passed, %d failed.\n",
               tests_run - tests_failed, tests_run, tests_failed);

    return tests_failed != 0;
}
