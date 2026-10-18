/*
 * test_fuzzy.c — Levenshtein (edit-distance) matching over symbols.dafsa.
 *
 * What this suite proves, and what would make each case FAIL:
 *
 *   1. k=0 is exact match                 — a fuzzy automaton that admits
 *                                           distance 1 shows up as extra ids.
 *   2. a distractor at distance k+1 is ABSENT
 *                                         — the can-fail case for the prune:
 *                                           if min(row) > k were not dead (or
 *                                           the k+1 row were still accepting)
 *                                           the distractor appears.
 *   3. a transposition is distance 2, so ABSENT at k=1 and present at k=2
 *                                         — this is the case a naive
 *                                           "swap-tolerant" implementation
 *                                           gets wrong (that is Damerau, not
 *                                           Levenshtein).
 *   4. brute-force oracle over a hand-built vocabulary
 *                                         — every emitted sym_id is checked
 *                                           against an independent full-matrix
 *                                           DP written in this file, and every
 *                                           non-emitted one is checked too.  A
 *                                           wrong accept, a wrong prune or a
 *                                           wrong byte-equivalence class all
 *                                           fail here.
 *   5. `~~` through the VM               — the operator joins entity ROWS (not
 *                                           just symbol ids): default k=1,
 *                                           explicit k, explicit column, and
 *                                           k>=3 refused at compile time.
 *   6. non-ASCII DOCUMENTS byte distance — cafe (4 bytes) vs café (5 bytes) is
 *                                           distance 2, not 1.  This pins the
 *                                           documented limit as behaviour.
 *   7. error paths are loud and leak-free — k>=3 / empty / NULL term report an
 *                                           errmsg with n_states == 0 and no
 *                                           trans/accept to free, and a compile+
 *                                           free loop shows no arena growth.
 */

#include "dl.h"
#include "dafsa.h"
#include "regexwalk.h"

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#define CHECK(cond, msg) do { \
    if (!(cond)) { FAIL(msg); return; } \
} while (0)

/* ─── Reference edit distance (independent of the engine) ──────────────── */

/* Full (m+1)x(n+1) Levenshtein matrix, no pruning, no clamping, no shared code
 * with lev_compile/lev_distance_cap.  This is the oracle. */
static int ref_edit_distance(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    int *prev = malloc((la + 1) * sizeof(int));
    int *cur  = malloc((la + 1) * sizeof(int));
    if (!prev || !cur) { free(prev); free(cur); return -1; }
    for (size_t j = 0; j <= la; j++) prev[j] = (int)j;
    for (size_t i = 1; i <= lb; i++) {
        cur[0] = (int)i;
        for (size_t j = 1; j <= la; j++) {
            int sub = prev[j - 1] + ((a[j - 1] == b[i - 1]) ? 0 : 1);
            int del = prev[j] + 1;
            int ins = cur[j - 1] + 1;
            int m = sub < del ? sub : del;
            if (ins < m) m = ins;
            cur[j] = m;
        }
        int *t = prev; prev = cur; cur = t;
    }
    int d = prev[la];
    free(prev);
    free(cur);
    return d;
}

/* ─── Symbol DAFSA helper: key = name, NUL, sym_id u32BE ───────────────── */

static dafsa *build_sym_dafsa(const char **names, const uint32_t *ids, int n)
{
    dafsa *d = dafsa_create();
    if (!d) return NULL;
    for (int i = 0; i < n; i++) {
        size_t len = strlen(names[i]);
        unsigned char key[512];
        if (len + 5 > sizeof(key)) { dafsa_free(d); return NULL; }
        memcpy(key, names[i], len);
        key[len] = 0x00;
        key[len + 1] = (unsigned char)(ids[i] >> 24);
        key[len + 2] = (unsigned char)(ids[i] >> 16);
        key[len + 3] = (unsigned char)(ids[i] >> 8);
        key[len + 4] = (unsigned char)(ids[i]);
        if (dafsa_add_n(d, key, len + 5) < 0) { dafsa_free(d); return NULL; }
    }
    return d;
}

/* ─── sym_id collection callback ────────────────────────────────────────── */

typedef struct {
    uint32_t ids[64];
    int      n;
    int      overflow;
} id_set;

static int id_collect_cb(uint32_t sym_id, void *user)
{
    id_set *s = (id_set *)user;
    if (s->n >= (int)(sizeof(s->ids) / sizeof(s->ids[0]))) { s->overflow = 1; return 0; }
    s->ids[s->n++] = sym_id;
    return 0;
}

static id_set id_set_new(void)
{
    id_set s;
    memset(&s, 0, sizeof s);
    return s;
}

static int id_set_has(const id_set *s, uint32_t id)
{
    for (int i = 0; i < s->n; i++) if (s->ids[i] == id) return 1;
    return 0;
}

/* ─── (1) k = 0 is exact match ─────────────────────────────────────────── */

static void test_k0_exact(void)
{
    TEST("lev_compile k=0 is exact match only");

    const char *names[] = { "alice", "alica", "alic", "alicex", "bob" };
    const uint32_t ids[] = { 11, 12, 13, 14, 15 };
    dafsa *d = build_sym_dafsa(names, ids, 5);
    CHECK(d, "build dafsa");

    regex_dfa *dfa = lev_compile("alice", 0);
    CHECK(dfa, "lev_compile returned NULL");
    CHECK(!dfa->errmsg, dfa->errmsg);
    CHECK(dfa->n_states > 0, "n_states > 0");

    id_set got = id_set_new();
    long n = symbols_dfa_walk(d, dfa, id_collect_cb, &got);
    CHECK(n == 1, "exactly one match at k=0");
    CHECK(got.n == 1 && got.ids[0] == 11, "the match is 'alice'");
    CHECK(!id_set_has(&got, 12), "alica (distance 1) absent at k=0");

    regex_dfa_free(dfa);
    dafsa_free(d);
    PASS();
}

/* ─── (2) distractor at distance k+1 is absent ─────────────────────────── */

static void test_distractor_k_plus_1(void)
{
    TEST("distractor at distance k+1 is ABSENT");

    /* Distances to "alice": alice 0, alica 1, alic 1, alicex 1, alicaa 2,
     * alixxa 3. */
    const char *names[] = { "alice", "alica", "alic", "alicex", "alicaa", "alixxa" };
    const uint32_t ids[] = { 21, 22, 23, 24, 25, 26 };
    dafsa *d = build_sym_dafsa(names, ids, 6);
    CHECK(d, "build dafsa");

    /* The oracle must agree with the distances the fixture claims. */
    CHECK(ref_edit_distance("alice", "alicaa") == 2, "fixture: alicaa is distance 2");
    CHECK(ref_edit_distance("alice", "alixxa") == 3, "fixture: alixxa is distance 3");

    {
        regex_dfa *dfa = lev_compile("alice", 1);
        CHECK(dfa && !dfa->errmsg, "k=1 compiles");
        id_set got = id_set_new();
        long n = symbols_dfa_walk(d, dfa, id_collect_cb, &got);
        CHECK(n == 4, "k=1 -> 4 matches");
        CHECK(id_set_has(&got, 21) && id_set_has(&got, 22) &&
              id_set_has(&got, 23) && id_set_has(&got, 24), "all distance-1 ids");
        CHECK(!id_set_has(&got, 25), "k+1 distractor (alicaa, d=2) ABSENT");
        CHECK(!id_set_has(&got, 26), "distance-3 entry ABSENT");
        regex_dfa_free(dfa);
    }
    {
        regex_dfa *dfa = lev_compile("alice", 2);
        CHECK(dfa && !dfa->errmsg, "k=2 compiles");
        id_set got = id_set_new();
        long n = symbols_dfa_walk(d, dfa, id_collect_cb, &got);
        CHECK(n == 5, "k=2 -> 5 matches");
        CHECK(id_set_has(&got, 25), "alicaa (d=2) now present");
        CHECK(!id_set_has(&got, 26), "k+1 distractor at k=2 (alixxa, d=3) ABSENT");
        regex_dfa_free(dfa);
    }

    dafsa_free(d);
    PASS();
}

/* ─── (3) transposition is distance 2, not 1 ───────────────────────────── */

static void test_transposition_is_not_one(void)
{
    TEST("transposition is distance 2 (ABSENT at k=1, present at k=2)");

    const char *names[] = { "abcd", "abdc", "abc", "abce" };
    const uint32_t ids[] = { 31, 32, 33, 34 };
    dafsa *d = build_sym_dafsa(names, ids, 4);
    CHECK(d, "build dafsa");

    CHECK(ref_edit_distance("abcd", "abdc") == 2, "fixture: abdc is distance 2");

    {
        regex_dfa *dfa = lev_compile("abcd", 1);
        CHECK(dfa && !dfa->errmsg, "k=1 compiles");
        id_set got = id_set_new();
        (void)symbols_dfa_walk(d, dfa, id_collect_cb, &got);
        CHECK(!id_set_has(&got, 32), "transposition abdc ABSENT at k=1");
        CHECK(id_set_has(&got, 31) && id_set_has(&got, 33) && id_set_has(&got, 34),
              "deletion/insertion matches present at k=1");
        regex_dfa_free(dfa);
    }
    {
        regex_dfa *dfa = lev_compile("abcd", 2);
        CHECK(dfa && !dfa->errmsg, "k=2 compiles");
        id_set got = id_set_new();
        (void)symbols_dfa_walk(d, dfa, id_collect_cb, &got);
        CHECK(id_set_has(&got, 32), "transposition present at k=2");
        regex_dfa_free(dfa);
    }

    dafsa_free(d);
    PASS();
}

/* ─── (4) brute-force oracle over a whole vocabulary ───────────────────── */

static void test_brute_force_oracle(void)
{
    TEST("walk == brute-force edit distance over the whole vocabulary");

    /* Deliberately ragged: shared prefixes, a numeric, an empty-ish short name,
     * a long one, and two that differ from several needles. */
    const char *names[] = {
        "a", "ab", "abc", "abcd", "abce", "abcdx", "abd", "acd",
        "alice", "alise", "alicae", "alic", "alicee", "bob", "bobby",
        "carol", "caroline", "0", "00", "xyzzy",
        "abcabc", "abcab", "bcabc", "ababab", "cab",
    };
    const int n_names = (int)(sizeof(names) / sizeof(names[0]));
    uint32_t ids[64];
    for (int i = 0; i < n_names; i++) ids[i] = (uint32_t)(100 + i);

    dafsa *d = build_sym_dafsa(names, ids, n_names);
    CHECK(d, "build dafsa");

    const char *needles[] = { "abcd", "alice", "abc", "bobby", "xyz", "0", "abdce" };
    const int n_needles = (int)(sizeof(needles) / sizeof(needles[0]));

    for (int ni = 0; ni < n_needles; ni++) {
        for (uint32_t k = 0; k <= 2; k++) {
            regex_dfa *dfa = lev_compile(needles[ni], k);
            CHECK(dfa, "lev_compile NULL");
            CHECK(!dfa->errmsg, dfa->errmsg);

            id_set got = id_set_new();
            (void)symbols_dfa_walk(d, dfa, id_collect_cb, &got);
            CHECK(!got.overflow, "id_set overflow");

            int expected = 0;
            for (int i = 0; i < n_names; i++) {
                int dist = ref_edit_distance(needles[ni], names[i]);
                int want = (dist <= (int)k);
                int have = id_set_has(&got, ids[i]);
                if (want) expected++;
                if (want != have) {
                    printf("\n    MISMATCH needle='%s' k=%u name='%s' dist=%d "
                           "brute_force=%s walk=%s\n",
                           needles[ni], k, names[i], dist,
                           want ? "match" : "absent", have ? "match" : "absent");
                    FAIL("walk disagrees with brute force");
                    regex_dfa_free(dfa);
                    dafsa_free(d);
                    return;
                }
            }
            if (got.n != expected) {
                printf("\n    COUNT MISMATCH needle='%s' k=%u expected=%d got=%d\n",
                       needles[ni], k, expected, got.n);
                FAIL("match count disagrees with brute force");
                regex_dfa_free(dfa);
                dafsa_free(d);
                return;
            }
            regex_dfa_free(dfa);
        }
    }

    dafsa_free(d);
    PASS();
}

/* ─── (5) `~~` through the VM, joining entity rows ─────────────────────── */

typedef struct {
    uint32_t *data;
    long      count;
    long      cap;
    uint8_t   arity;
} tset;

static int tset_cb(const uint32_t *cols, uint8_t arity, void *user)
{
    tset *ts = (tset *)user;
    if (ts->arity == 0) ts->arity = arity;
    if (ts->count >= ts->cap) {
        long nc = ts->cap ? ts->cap * 2 : 64;
        uint32_t *nd = realloc(ts->data, (size_t)nc * (size_t)arity * sizeof(uint32_t));
        if (!nd) return 1;
        ts->data = nd;
        ts->cap = nc;
    }
    memcpy(ts->data + (size_t)ts->count * arity, cols, (size_t)arity * sizeof(uint32_t));
    ts->count++;
    return 0;
}

static int tset_has(const tset *ts, uint32_t c0, uint32_t c1)
{
    for (long i = 0; i < ts->count; i++) {
        const uint32_t *row = ts->data + (size_t)i * ts->arity;
        if (row[0] == c0 && row[1] == c1) return 1;
    }
    return 0;
}

static void setup_db(dl_db **db_out, const char *suffix)
{
    char cmd[512];
    char path[512];
    snprintf(cmd, sizeof(cmd), "rm -rf build-tmp/fuzzy_%s", suffix);
    if (system(cmd) != 0) { /* best effort: an absent dir is fine */ }
    snprintf(path, sizeof(path), "build-tmp/fuzzy_%s", suffix);
    *db_out = dl_open(path);
}

/* p(name, city) with everything a k=2 neighbourhood needs. */
static dl_db *make_city_db(const char *suffix)
{
    dl_db *db = NULL;
    setup_db(&db, suffix);
    if (!db) return NULL;
    if (dl_declare_relation(db, "p", 2) != 0) { dl_close(db); return NULL; }
    FILE *f = fopen("build-tmp/fuzzy_city.csv", "w");
    if (!f) { dl_close(db); return NULL; }
    fprintf(f, "alice,nyc\nalise,nyc\nalica,la\nbob,nyc\ncarol,sf\n");
    fclose(f);
    if (dl_load_facts(db, "p", "build-tmp/fuzzy_city.csv") != 5) { dl_close(db); return NULL; }
    return db;
}

static void test_vm_operator(void)
{
    TEST("~~ operator through the VM joins entity rows");

    dl_db *db = make_city_db("vmop");
    CHECK(db, "open db");
    const uint32_t alice = dl_intern_str_find(db, "alice");
    const uint32_t alise = dl_intern_str_find(db, "alise");
    const uint32_t alica = dl_intern_str_find(db, "alica");
    const uint32_t bob   = dl_intern_str_find(db, "bob");
    const uint32_t nyc   = dl_intern_str_find(db, "nyc");
    const uint32_t la    = dl_intern_str_find(db, "la");
    CHECK(alice && alise && alica && bob && nyc && la, "interned names");

    /* default k = 1, default col = 0 */
    {
        CHECK(dl_load_rules(db, "q(X,Y) :- p(X,Y) ~~ 'alice'.") == 0, "rule loads");
        tset ts = {0};
        long n = dl_query(db, "q", tset_cb, &ts);
        CHECK(n == 3, "k=1 default: 3 rows (alice, alise, alica)");
        CHECK(tset_has(&ts, alice, nyc), "alice/nyc present");
        CHECK(tset_has(&ts, alise, nyc), "alise/nyc present (distance 1)");
        CHECK(tset_has(&ts, alica, la), "alica/la present (distance 1)");
        CHECK(!tset_has(&ts, bob, nyc), "bob/nyc ABSENT (distance 4)");
        free(ts.data);
    }
    /* k = 0 is exact */
    {
        CHECK(dl_load_rules(db, "q0(X,Y) :- p(X,Y) ~~ 0 'alice'.") == 0, "k=0 rule loads");
        tset ts = {0};
        long n = dl_query(db, "q0", tset_cb, &ts);
        CHECK(n == 1, "k=0: exactly one row");
        CHECK(tset_has(&ts, alice, nyc), "k=0 keeps alice/nyc only");
        CHECK(!tset_has(&ts, alise, nyc), "k=0 drops alise/nyc");
        free(ts.data);
    }
    /* explicit column: ~~ k col 'term' filters on column 1 */
    {
        CHECK(dl_load_rules(db, "q1(X,Y) :- p(X,Y) ~~ 1 1 'nyx'.") == 0, "col=1 rule loads");
        tset ts = {0};
        long n = dl_query(db, "q1", tset_cb, &ts);
        CHECK(n == 3, "col 1 'nyx' k=1 -> the 3 nyc rows");
        CHECK(tset_has(&ts, alice, nyc), "alice/nyc matched via column 1");
        CHECK(tset_has(&ts, alise, nyc), "alise/nyc matched via column 1");
        CHECK(tset_has(&ts, bob, nyc), "bob/nyc matched via column 1");
        CHECK(!tset_has(&ts, alica, la), "alica/la ABSENT (column 1 is 'la', distance 3)");
        free(ts.data);
    }
    /* out-of-range column is refused at compile time */
    {
        CHECK(dl_load_rules(db, "q2(X,Y) :- p(X,Y) ~~ 1 5 'alice'.") != 0,
              "col 5 on arity 2 rejected");
    }
    /* k >= 3 is refused at compile time, loudly (not degraded) */
    {
        CHECK(dl_load_rules(db, "q3(X,Y) :- p(X,Y) ~~ 3 'alice'.") != 0,
              "k=3 refused by the compiler");
    }

    dl_close(db);
    PASS();
}

/* ─── (6) non-ASCII documents BYTE distance ────────────────────────────── */

typedef struct {
    char     names[16][64];
    uint32_t dist[16];
    int      n;
} fuzzy_hits;

static int fuzzy_hit_cb(uint32_t sym_id, const char *name, size_t name_len,
                        uint32_t dist, void *user)
{
    fuzzy_hits *h = (fuzzy_hits *)user;
    (void)sym_id;
    if (h->n >= 16) return 0;
    if (name_len >= sizeof(h->names[0])) name_len = sizeof(h->names[0]) - 1;
    memcpy(h->names[h->n], name, name_len);
    h->names[h->n][name_len] = '\0';
    h->dist[h->n] = dist;
    h->n++;
    return 0;
}

static fuzzy_hits fuzzy_hits_new(void)
{
    fuzzy_hits h;
    memset(&h, 0, sizeof h);
    return h;
}

static int fuzzy_hits_has(const fuzzy_hits *h, const char *name, uint32_t *dist_out)
{
    for (int i = 0; i < h->n; i++) {
        if (strcmp(h->names[i], name) == 0) {
            if (dist_out) *dist_out = h->dist[i];
            return 1;
        }
    }
    return 0;
}

static void test_non_ascii_byte_distance(void)
{
    TEST("non-ASCII: byte distance, cafe vs café = 2");

    dl_db *db = NULL;
    setup_db(&db, "utf8");
    CHECK(db, "open db");
    CHECK(dl_declare_relation(db, "e", 1) == 0, "declare e/1");
    {
        FILE *f = fopen("build-tmp/fuzzy_utf8.csv", "w");
        CHECK(f, "csv");
        /* café is 5 bytes: c a f é(2).  cafe is 4 a/s/c/i/i bytes. */
        fprintf(f, "cafe\ncaf\xc3\xa9\nbo\xc3\xaet\n");
        fclose(f);
        CHECK(dl_load_facts(db, "e", "build-tmp/fuzzy_utf8.csv") == 3, "loaded 3 rows");
    }

    /* Byte distance cafe <-> café is 2 (é is two bytes).  Documented limit:
     * a codepoint-aware distance would call it 1. */
    {
        fuzzy_hits h = fuzzy_hits_new();
        long n = dl_fuzzy_lookup(db, "cafe", 1, fuzzy_hit_cb, &h);
        CHECK(n == 1, "k=1 finds only the exact ASCII cafe");
        CHECK(fuzzy_hits_has(&h, "cafe", NULL), "cafe present at k=1");
        CHECK(!fuzzy_hits_has(&h, "caf\xc3\xa9", NULL),
              "café ABSENT at k=1 (byte distance is 2, not 1)");
    }
    {
        fuzzy_hits h = fuzzy_hits_new();
        long n = dl_fuzzy_lookup(db, "cafe", 2, fuzzy_hit_cb, &h);
        CHECK(n == 2, "k=2 finds cafe + café");
        uint32_t d = 99;
        CHECK(fuzzy_hits_has(&h, "caf\xc3\xa9", &d), "café present at k=2");
        CHECK(d == 2, "and its reported distance is 2 (BYTES)");
    }
    /* The C oracle's own distance agrees. */
    CHECK(ref_edit_distance("cafe", "caf\xc3\xa9") == 2, "reference agrees: 2");

    dl_close(db);
    PASS();
}

/* ─── (7) dl_fuzzy_lookup contract + distances ─────────────────────────── */

static void test_dl_fuzzy_lookup(void)
{
    TEST("dl_fuzzy_lookup: ids, distances, and the error contract");

    dl_db *db = make_city_db("lookup");
    CHECK(db, "open db");
    {
        fuzzy_hits h = fuzzy_hits_new();
        long n = dl_fuzzy_lookup(db, "alice", 1, fuzzy_hit_cb, &h);
        CHECK(n == 3, "3 hits at k=1");
        uint32_t d = 99;
        CHECK(fuzzy_hits_has(&h, "alice", &d) && d == 0, "alice at distance 0");
        CHECK(fuzzy_hits_has(&h, "alise", &d) && d == 1, "alise at distance 1");
        CHECK(fuzzy_hits_has(&h, "alica", &d) && d == 1, "alica at distance 1");
        CHECK(!fuzzy_hits_has(&h, "bob", NULL), "bob absent");
    }
    /* k=0 is exact */
    {
        fuzzy_hits h = fuzzy_hits_new();
        long n = dl_fuzzy_lookup(db, "alice", 0, fuzzy_hit_cb, &h);
        CHECK(n == 1, "k=0: 1 hit");
    }
    /* every reported distance is the reference distance */
    {
        fuzzy_hits h = fuzzy_hits_new();
        (void)dl_fuzzy_lookup(db, "alice", 2, fuzzy_hit_cb, &h);
        for (int i = 0; i < h.n; i++) {
            int want = ref_edit_distance("alice", h.names[i]);
            if ((uint32_t)want != h.dist[i]) {
                printf("\n    distance mismatch for '%s': engine=%u reference=%d\n",
                       h.names[i], h.dist[i], want);
                FAIL("reported distance is not the byte edit distance");
                dl_close(db);
                return;
            }
        }
    }
    /* error contract */
    CHECK(dl_fuzzy_lookup(db, "alice", 3, fuzzy_hit_cb, NULL) == -1, "k=3 -> -1");
    CHECK(dl_fuzzy_lookup(NULL, "alice", 1, fuzzy_hit_cb, NULL) == -1, "NULL db -> -1");
    CHECK(dl_fuzzy_lookup(db, NULL, 1, fuzzy_hit_cb, NULL) == -1, "NULL needle -> -1");
    CHECK(dl_fuzzy_lookup(db, "alice", 1, NULL, NULL) == -1, "NULL cb -> -1");
    CHECK(dl_fuzzy_lookup(db, "", 1, fuzzy_hit_cb, NULL) == -1, "empty needle -> -1");
    /* a lookup must not corrupt later ones */
    {
        fuzzy_hits h = fuzzy_hits_new();
        CHECK(dl_fuzzy_lookup(db, "alica", 1, fuzzy_hit_cb, &h) >= 1,
              "lookup still works after the error calls");
    }
    /* the ids really are interner ids */
    {
        fuzzy_hits h = fuzzy_hits_new();
        (void)dl_fuzzy_lookup(db, "alice", 0, fuzzy_hit_cb, &h);
        CHECK(h.n == 1, "k=0 one hit");
    }
    dl_close(db);
    PASS();
}

/* ─── (8) error paths: loud, well-formed, and leak-free ────────────────── */

static void test_error_paths_and_leaks(void)
{
    TEST("refusals are loud, the struct stays freeable, no arena growth");

    {
        regex_dfa *dfa = lev_compile("abcdefgh", 3);
        CHECK(dfa, "lev_compile returned NULL (must return an errmsg dfa)");
        CHECK(dfa->errmsg != NULL, "k=3 must set errmsg");
        CHECK(strstr(dfa->errmsg, "k must be 0..2") != NULL, "errmsg names the limit");
        CHECK(dfa->n_states == 0, "n_states == 0 on refusal");
        CHECK(dfa->trans == NULL && dfa->accept == NULL, "nothing to free on refusal");
        regex_dfa_free(dfa);
    }
    {
        regex_dfa *dfa = lev_compile("", 1);
        CHECK(dfa && dfa->errmsg != NULL, "empty term refused");
        CHECK(dfa->n_states == 0 && dfa->trans == NULL, "empty term: no states");
        regex_dfa_free(dfa);
    }
    {
        regex_dfa *dfa = lev_compile(NULL, 1);
        CHECK(dfa && dfa->errmsg != NULL, "NULL term refused");
        regex_dfa_free(dfa);
    }
    /* k=3 must not silently degrade to k=2: the same term compiles at k=2. */
    {
        regex_dfa *ok = lev_compile("abcdefgh", 2);
        CHECK(ok && ok->errmsg == NULL && ok->n_states > 0, "same term compiles at k=2");
        regex_dfa_free(ok);
    }

    /* Leak check: compile+free a real automaton many times and compare the
     * arena's in-use bytes before/after.  A missing free of the trans table
     * (n_states*1024 bytes per call) shows up as tens of MB here. */
    {
        regex_dfa *probe = lev_compile("abcdefgh", 2);
        CHECK(probe && probe->errmsg == NULL, "probe compiles");
        size_t table_bytes = (size_t)probe->n_states * 256u * sizeof(uint32_t);
        regex_dfa_free(probe);
        CHECK(table_bytes > 0, "probe has a transition table");

        const int iters = 200;
        const size_t budget = table_bytes * (size_t)iters / 4; /* 4x margin */
        struct mallinfo2 before = mallinfo2();
        for (int i = 0; i < iters; i++) {
            regex_dfa *dfa = lev_compile("abcdefgh", 2);
            if (!dfa || dfa->errmsg) { FAIL("compile failed inside the leak loop"); return; }
            regex_dfa_free(dfa);
            regex_dfa *bad = lev_compile("abcdefgh", 3); /* error path too */
            regex_dfa_free(bad);
        }
        struct mallinfo2 after = mallinfo2();
        size_t grew = after.uordblks > before.uordblks
                          ? after.uordblks - before.uordblks : 0;
        if (grew > budget) {
            printf("\n    arena grew by %zu bytes over %d compile/free cycles "
                   "(one table = %zu bytes, budget %zu)\n",
                   grew, iters, table_bytes, budget);
            FAIL("lev_compile leaks");
            return;
        }
        printf("[table=%zuB grew=%zuB budget=%zuB] ", table_bytes, grew, budget);
    }

    PASS();
}

/* ─── main ─────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("test_fuzzy: Levenshtein edit-distance matching over symbols.dafsa\n");

    test_k0_exact();
    test_distractor_k_plus_1();
    test_transposition_is_not_one();
    test_brute_force_oracle();
    test_vm_operator();
    test_non_ascii_byte_distance();
    test_dl_fuzzy_lookup();
    test_error_paths_and_leaks();

    printf("test_fuzzy: %d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
