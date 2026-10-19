/*
 * test_search.c — Full-text search tests (aux_index + dl_search)
 */
#include "dl.h"
#include "index.h"

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

static const char *BASE = "build-tmp/search";

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

/* ─── Tokenizer tests ────────────────────────────────────────────────────── */

static void t_tokenize_basic(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_tokenize_basic");
    (void)db;

    TEST("tokenize basic");
    size_t n;
    char **tokens = tokenize("hello world", &n);
    if (!tokens || n != 2) { FAIL("expected 2 tokens"); dl_close(db); return; }
    if (strcmp(tokens[0], "hello") != 0 || strcmp(tokens[1], "world") != 0) {
        FAIL("unexpected tokens");
        token_free(tokens);
        dl_close(db);
        return;
    }
    token_free(tokens);
    PASS();
    dl_close(db);
}

static void t_tokenize_lowercase(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_tokenize_lowercase");
    (void)db;

    TEST("tokenize lowercase");
    size_t n;
    char **tokens = tokenize("HELLO World", &n);
    if (!tokens || n != 2) { FAIL("expected 2 tokens"); dl_close(db); return; }
    if (strcmp(tokens[0], "hello") != 0 || strcmp(tokens[1], "world") != 0) {
        FAIL("expected lowercase tokens");
        token_free(tokens);
        dl_close(db);
        return;
    }
    token_free(tokens);
    PASS();
    dl_close(db);
}

static void t_tokenize_punctuation(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_tokenize_punctuation");
    (void)db;

    TEST("tokenize punctuation");
    size_t n;
    char **tokens = tokenize("hello, world! how are you?", &n);
    if (!tokens || n != 5) { FAIL("expected 5 tokens"); dl_close(db); return; }
    if (strcmp(tokens[0], "hello") != 0 || strcmp(tokens[1], "world") != 0 ||
        strcmp(tokens[2], "how") != 0 || strcmp(tokens[3], "are") != 0 ||
        strcmp(tokens[4], "you") != 0) {
        FAIL("unexpected tokens");
        token_free(tokens);
        dl_close(db);
        return;
    }
    token_free(tokens);
    PASS();
    dl_close(db);
}

static void t_tokenize_empty(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_tokenize_empty");
    (void)db;

    TEST("tokenize empty");
    size_t n;
    char **tokens = tokenize("", &n);
    if (tokens || n != 0) { FAIL("expected NULL for empty"); dl_close(db); return; }
    PASS();
    dl_close(db);
}

static void t_tokenize_only_punctuation(void)
{
    char dir[512];
    dl_db *fresh_db(char *dir_out, size_t cap, const char *name);
    dl_db *db = fresh_db(dir, sizeof(dir), "t_tokenize_only_punct");
    (void)db;

    TEST("tokenize only punctuation");
    size_t n;
    char **tokens = tokenize("!!!,,,", &n);
    if (tokens || n != 0) { FAIL("expected NULL for punctuation-only"); dl_close(db); return; }
    PASS();
    dl_close(db);
}

/* ─── aux_index tests ───────────────────────────────────────────────────── */

static void t_aux_index_ensure(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_aux_index_ensure");

    TEST("aux_index_ensure_postings");
    if (aux_index_ensure_postings(db) != 0) {
        FAIL("failed to ensure postings");
        dl_close(db);
        return;
    }
    /* Call again - should be idempotent */
    if (aux_index_ensure_postings(db) != 0) {
        FAIL("second ensure failed");
        dl_close(db);
        return;
    }
    PASS();
    dl_close(db);
}

static void t_aux_index_add(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_aux_index_add");

    TEST("aux_index_add_posting");
    aux_index_ensure_postings(db);
    uint32_t term = dl_intern_str(db, "test");
    uint32_t obs = dl_intern_str(db, "obs1");
    if (aux_index_add_posting(db, term, obs) != 1) {
        FAIL("failed to add posting");
        dl_close(db);
        return;
    }
    /* Duplicate should return 0 */
    if (aux_index_add_posting(db, term, obs) != 0) {
        FAIL("duplicate should return 0");
        dl_close(db);
        return;
    }
    PASS();
    dl_close(db);
}

/* ─── dl_search tests ───────────────────────────────────────────────────── */



static void t_search_single_term(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_search_single");

    TEST("dl_search single term");
    aux_index_ensure_postings(db);
    
    uint32_t term1 = dl_intern_str(db, "hello");
    uint32_t obs1 = dl_intern_str(db, "doc1");
    uint32_t obs2 = dl_intern_str(db, "doc2");
    
    aux_index_add_posting(db, term1, obs1);
    aux_index_add_posting(db, term1, obs2);
    
    uint32_t results[10];
    int scores[10];
    int n = dl_search_top(db, &term1, 1, results, scores, 10);
    if (n != 2) { FAIL("expected 2 results"); dl_close(db); return; }
    PASS();
    dl_close(db);
}

static void t_search_and(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_search_and");

    TEST("dl_search AND (multi-term)");
    aux_index_ensure_postings(db);
    
    uint32_t term1 = dl_intern_str(db, "hello");
    uint32_t term2 = dl_intern_str(db, "world");
    uint32_t obs1 = dl_intern_str(db, "doc1");
    uint32_t obs2 = dl_intern_str(db, "doc2");
    uint32_t obs3 = dl_intern_str(db, "doc3");
    
    /* doc1 has both terms */
    aux_index_add_posting(db, term1, obs1);
    aux_index_add_posting(db, term2, obs1);
    /* doc2 has only term1 */
    aux_index_add_posting(db, term1, obs2);
    /* doc3 has only term2 */
    aux_index_add_posting(db, term2, obs3);
    
    uint32_t terms[2] = {term1, term2};
    uint32_t results[10];
    int scores[10];
    int n = dl_search_top(db, terms, 2, results, scores, 10);
    if (n != 1) { FAIL("expected 1 result (AND)"); dl_close(db); return; }
    if (results[0] != obs1) { FAIL("expected obs1"); dl_close(db); return; }
    PASS();
    dl_close(db);
}

static void t_search_no_match(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_search_no_match");

    TEST("dl_search no match");
    aux_index_ensure_postings(db);
    
    uint32_t term1 = dl_intern_str(db, "hello");
    uint32_t term2 = dl_intern_str(db, "world");
    uint32_t obs1 = dl_intern_str(db, "doc1");
    
    /* Only add term1 to obs1 */
    aux_index_add_posting(db, term1, obs1);
    
    uint32_t terms[2] = {term1, term2};
    uint32_t results[10];
    int scores[10];
    int n = dl_search_top(db, terms, 2, results, scores, 10);
    if (n != 0) { FAIL("expected 0 results"); dl_close(db); return; }
    PASS();
    dl_close(db);
}

static void t_search_top_limit(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_search_top");

    TEST("dl_search_top limit");
    aux_index_ensure_postings(db);
    
    uint32_t term = dl_intern_str(db, "test");
    int i;
    for (i = 0; i < 5; i++) {
        char name[16];
        snprintf(name, sizeof(name), "obs%d", i);
        uint32_t obs = dl_intern_str(db, name);
        aux_index_add_posting(db, term, obs);
    }
    
    uint32_t results[10];
    int scores[10];
    int n = dl_search_top(db, &term, 1, results, scores, 3);
    if (n != 3) { FAIL("expected 3 results (limited)"); dl_close(db); return; }
    PASS();
    dl_close(db);
}

static void t_search_empty_terms(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_search_empty");

    TEST("dl_search empty terms");
    aux_index_ensure_postings(db);
    
    uint32_t results[10];
    int scores[10];
    int n = dl_search_top(db, NULL, 0, results, scores, 10);
    if (n != -1) { FAIL("expected -1 for empty terms"); dl_close(db); return; }
    PASS();
    dl_close(db);
}

/* ─── Brute-force cross-checks for the AND intersection ─────────────────────
 *
 * The engine collects one obs_id set per term and intersects them with a hash
 * index (index.zig IntSet).  These tests deliberately re-derive the expected
 * intersection from the TEST's own dense posting lists with a linear scan, so
 * the expected side shares no code — and therefore no defect — with the engine
 * side.  They can fail: t_search_crosscheck_detects_diff proves the comparator
 * sees a one-element difference.
 */

#define XC_N_DOC 60
#define XC_N_TERM 4
#define XC_N_QUERY 5

/* Term i covers every document whose index is a multiple of XC_MOD[i], so the
 * four posting sets are all different sizes and the query that intersects them
 * cannot always be answered from argument 0 (the engine picks the smallest set
 * to iterate). */
static const int XC_MOD[XC_N_TERM] = {2, 3, 5, 7};
static const char *XC_TERM[XC_N_TERM] = {"xc_alpha", "xc_beta", "xc_gamma", "xc_delta"};

/* Linear membership probe — the naive implementation on purpose. */
static int xc_has(const uint32_t *a, int n, uint32_t v)
{
    int i;
    for (i = 0; i < n; i++)
        if (a[i] == v) return 1;
    return 0;
}

/* Order-insensitive set equality that also rejects duplicates on either side
 * (a duplicate means the engine emitted one document twice). */
static int xc_same_set(const uint32_t *a, int na, const uint32_t *b, int nb)
{
    int i;
    if (na != nb) return 0;
    for (i = 0; i < na; i++)
        if (!xc_has(b, nb, a[i])) return 0;
    for (i = 0; i < nb; i++)
        if (!xc_has(a, na, b[i])) return 0;
    for (i = 1; i < na; i++)
        if (xc_has(a, i, a[i])) return 0;
    for (i = 1; i < nb; i++)
        if (xc_has(b, i, b[i])) return 0;
    return 1;
}

/* Expected intersection: for each document, scan every term's posting list. */
static int xc_expected(const uint32_t *docs, int n_docs,
                       const uint32_t *const *lists, const int *list_n, int n_terms,
                       uint32_t *out)
{
    int d, t, n = 0;
    for (d = 0; d < n_docs; d++) {
        int in_all = 1;
        for (t = 0; t < n_terms; t++) {
            if (!xc_has(lists[t], list_n[t], docs[d])) { in_all = 0; break; }
        }
        if (in_all) out[n++] = docs[d];
    }
    return n;
}

/* Build the corpus: XC_N_DOC documents plus the four posting sets.  Every
 * posting is added TWICE so the engine's dedup has to work too (the expected
 * lists hold each document once). */
static int xc_setup(dl_db *db, uint32_t *docs,
                    uint32_t lists[XC_N_TERM][XC_N_DOC], int *list_n)
{
    int d, t;
    if (aux_index_ensure_postings(db) != 0) return -1;
    for (d = 0; d < XC_N_DOC; d++) {
        char name[16];
        snprintf(name, sizeof(name), "xcdoc%02d", d);
        docs[d] = dl_intern_str(db, name);
        if (docs[d] == 0) return -1;
    }
    for (t = 0; t < XC_N_TERM; t++) {
        uint32_t term = dl_intern_str(db, (char *)XC_TERM[t]);
        if (term == 0) return -1;
        list_n[t] = 0;
        for (d = 0; d < XC_N_DOC; d++) {
            if (d % XC_MOD[t] != 0) continue;
            if (aux_index_add_posting(db, term, docs[d]) != 1) return -1;
            if (aux_index_add_posting(db, term, docs[d]) != 0) return -1; /* deduped */
            lists[t][list_n[t]++] = docs[d];
        }
    }
    return 0;
}

/* Query term-index triples; -1 terminates a row. */
static const int XC_QUERY[XC_N_QUERY][3] = {
    {0, 1, -1},   /* alpha AND beta            -> multiples of 6  */
    {1, 3, -1},   /* beta AND delta            -> multiples of 21 */
    {0, 1, 2},    /* alpha AND beta AND gamma  -> multiples of 30 */
    {0, 3, -1},   /* alpha AND delta           -> multiples of 14 */
    {2, 3, 1},    /* gamma AND delta AND beta  -> multiples of 105 */
};

static void t_search_bruteforce_crosscheck(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_search_xcheck");
    uint32_t docs[XC_N_DOC];
    uint32_t lists[XC_N_TERM][XC_N_DOC];
    int list_n[XC_N_TERM];
    uint32_t got[XC_N_DOC];
    int scores[XC_N_DOC];
    int qi, t;

    TEST("dl_search brute-force cross-check (single + multi term)");
    if (xc_setup(db, docs, lists, list_n) != 0) {
        FAIL("setup: posting add failed");
        dl_close(db);
        return;
    }

    /* Single term: the whole set, duplicates collapsed. */
    {
        uint32_t term = dl_intern_str(db, (char *)XC_TERM[0]);
        const uint32_t *L[1] = {lists[0]};
        int ln[1] = {list_n[0]};
        uint32_t exp[XC_N_DOC];
        int ne = xc_expected(docs, XC_N_DOC, L, ln, 1, exp);
        int n = dl_search_top(db, &term, 1, got, scores, XC_N_DOC);
        if (n < 0) { FAIL("single-term search returned -1"); dl_close(db); return; }
        if (!xc_same_set(got, n, exp, ne)) {
            FAIL("single-term set disagrees with brute force");
            dl_close(db);
            return;
        }
        for (t = 0; t < n; t++) {
            if (scores[t] != 1) { FAIL("single-term score != 1"); dl_close(db); return; }
        }
    }

    for (qi = 0; qi < XC_N_QUERY; qi++) {
        uint32_t terms[3];
        const uint32_t *L[3];
        int ln[3];
        uint32_t exp[XC_N_DOC];
        int n_terms = 0, ne, n;

        for (t = 0; t < 3 && XC_QUERY[qi][t] >= 0; t++) {
            terms[t] = dl_intern_str(db, (char *)XC_TERM[XC_QUERY[qi][t]]);
            L[t] = lists[XC_QUERY[qi][t]];
            ln[t] = list_n[XC_QUERY[qi][t]];
            n_terms++;
        }
        ne = xc_expected(docs, XC_N_DOC, L, ln, n_terms, exp);
        n = dl_search_top(db, terms, n_terms, got, scores, XC_N_DOC);
        if (n < 0) { FAIL("multi-term search returned -1"); dl_close(db); return; }
        if (!xc_same_set(got, n, exp, ne)) {
            FAIL("multi-term intersection disagrees with brute force");
            dl_close(db);
            return;
        }
        for (t = 0; t < n; t++) {
            if (scores[t] != n_terms) { FAIL("AND score != n_terms"); dl_close(db); return; }
        }
    }

    PASS();
    dl_close(db);
}

/* Proves the cross-check above is not a tautology: perturbing the expected set
 * by exactly one member MUST be visible to xc_same_set. */
static void t_search_crosscheck_detects_diff(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_search_xcheck_diff");
    uint32_t docs[XC_N_DOC];
    uint32_t lists[XC_N_TERM][XC_N_DOC];
    int list_n[XC_N_TERM];
    uint32_t got[XC_N_DOC];
    uint32_t exp[XC_N_DOC];
    uint32_t perturbed[XC_N_DOC];
    int scores[XC_N_DOC];
    uint32_t terms[2];
    const uint32_t *L[2];
    int ln[2];
    int ne, n, i, np = 0;
    uint32_t dropped;

    TEST("cross-check sees a one-element difference");
    if (xc_setup(db, docs, lists, list_n) != 0) {
        FAIL("setup: posting add failed");
        dl_close(db);
        return;
    }

    terms[0] = dl_intern_str(db, (char *)XC_TERM[0]);
    terms[1] = dl_intern_str(db, (char *)XC_TERM[1]);
    L[0] = lists[0];
    ln[0] = list_n[0];
    L[1] = lists[1];
    ln[1] = list_n[1];
    ne = xc_expected(docs, XC_N_DOC, L, ln, 2, exp);
    n = dl_search_top(db, terms, 2, got, scores, XC_N_DOC);
    if (n < 0) { FAIL("search returned -1"); dl_close(db); return; }
    if (ne < 3) { FAIL("expected set too small to perturb"); dl_close(db); return; }
    if (!xc_same_set(got, n, exp, ne)) {
        FAIL("cross-check disagrees with the engine on the true set");
        dl_close(db);
        return;
    }

    /* Drop the middle member — one document, nothing else changes. */
    dropped = exp[ne / 2];
    for (i = 0; i < ne; i++) {
        if (exp[i] == dropped) continue;
        perturbed[np++] = exp[i];
    }
    if (np != ne - 1) { FAIL("perturbation did not drop exactly one"); dl_close(db); return; }
    if (xc_same_set(got, n, perturbed, np)) {
        FAIL("comparator reported equality on a set that differs by one");
        dl_close(db);
        return;
    }
    /* And the difference must survive in the other direction too. */
    if (xc_same_set(perturbed, np, got, n)) {
        FAIL("comparator is asymmetric");
        dl_close(db);
        return;
    }

    PASS();
    dl_close(db);
}

/* A document matching only ONE of two terms must be ABSENT under AND (this is
 * the union/OR regression guard), in both query argument orders. */
static void t_search_and_partial_absent(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_search_partial_absent");
    uint32_t term1, term2, doc_a, doc_b, doc_ab;
    uint32_t results[10];
    int scores[10];
    int n, order;

    TEST("dl_search AND excludes single-term documents");
    aux_index_ensure_postings(db);

    term1 = dl_intern_str(db, "pa_hello");
    term2 = dl_intern_str(db, "pa_world");
    doc_a = dl_intern_str(db, "pa_doc_a");   /* term1 only */
    doc_b = dl_intern_str(db, "pa_doc_b");   /* term2 only */
    doc_ab = dl_intern_str(db, "pa_doc_ab"); /* both       */

    aux_index_add_posting(db, term1, doc_a);
    aux_index_add_posting(db, term2, doc_b);
    aux_index_add_posting(db, term1, doc_ab);
    aux_index_add_posting(db, term2, doc_ab);

    for (order = 0; order < 2; order++) {
        uint32_t terms[2];
        terms[0] = order == 0 ? term1 : term2;
        terms[1] = order == 0 ? term2 : term1;

        n = dl_search_top(db, terms, 2, results, scores, 10);
        if (n != 1) { FAIL("expected exactly 1 AND result"); dl_close(db); return; }
        if (results[0] != doc_ab) { FAIL("expected doc_ab"); dl_close(db); return; }
        if (xc_has(results, n, doc_a)) { FAIL("doc_a (one term) leaked into AND"); dl_close(db); return; }
        if (xc_has(results, n, doc_b)) { FAIL("doc_b (one term) leaked into AND"); dl_close(db); return; }
    }

    PASS();
    dl_close(db);
}

/* ─── Ranked (partial-match / OR) search ────────────────────────────────────
 *
 * dl_search_ranked is the entry point whose score CAN vary — that is the whole
 * point of it — so these tests assert the (obs_id, score) SEQUENCE, not just
 * membership.  The brute-force test re-derives each document's matched count
 * from the test's own dense posting lists with a linear scan, so the expected
 * side shares no code with the engine's group-count:
 * t_search_ranked_partial fails outright if the engine emits a constant score.
 */

typedef struct {
    uint32_t *ids;
    int *scores;
    int n;
    int cap;
} rk_sink;

static int rk_sink_cb(uint32_t obs_id, int score, void *user)
{
    rk_sink *s = (rk_sink *)user;
    if (s->n >= s->cap) return 1; /* stop: the test's buffer is fixed-size */
    s->ids[s->n] = obs_id;
    s->scores[s->n] = score;
    s->n++;
    return 0;
}

typedef struct {
    uint32_t obs_id;
    int score;
} rk_expect;

/* Brute-force ranked expectation: matched count per document (linear scan over
 * the test's own posting lists), inserted into (score desc, obs_id asc) order
 * by a plain insertion sort — an implementation the engine does not share. */
static int rk_expected(const uint32_t *docs, int n_docs,
                       const uint32_t *const *lists, const int *list_n, int n_terms,
                       int min_terms, rk_expect *out)
{
    int d, t, j, n = 0;
    for (d = 0; d < n_docs; d++) {
        int matched = 0;
        for (t = 0; t < n_terms; t++)
            if (xc_has(lists[t], list_n[t], docs[d])) matched++;
        if (matched < min_terms) continue;
        j = n;
        while (j > 0 && (out[j - 1].score < matched ||
                         (out[j - 1].score == matched && out[j - 1].obs_id > docs[d]))) {
            out[j] = out[j - 1];
            j--;
        }
        out[j].obs_id = docs[d];
        out[j].score = matched;
        n++;
    }
    return n;
}

/* A 3-term query whose documents match 3, 2 and 1 terms: with min_terms=2 only
 * the first two are returned and the 3-term document ranks FIRST.  Under
 * dl_search's AND semantics the same query returns exactly one document with a
 * score that cannot differ from any other result. */
static void t_search_ranked_partial(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_search_ranked_partial");
    uint32_t a, b, c;
    uint32_t terms[3];
    uint32_t ids[8];
    int scores[8];
    rk_sink sink;
    int n;

    TEST("dl_search_ranked: partial match, ranking varies, 3-term doc first");
    if (aux_index_ensure_postings(db) != 0) { FAIL("ensure postings failed"); dl_close(db); return; }

    a = dl_intern_str(db, "rk_alpha");
    b = dl_intern_str(db, "rk_beta");
    c = dl_intern_str(db, "rk_gamma");
    if (a == 0 || b == 0 || c == 0) { FAIL("intern failed"); dl_close(db); return; }

    /* obs 501 matches all 3 terms, 502 matches {a,b}, 503 matches {a} only.
     * Postings go in SCRAMBLED obs order (503 first) so a tie-break that leaked
     * posting/insertion order would be visible. */
    aux_index_add_posting(db, a, 503);
    aux_index_add_posting(db, a, 502);
    aux_index_add_posting(db, a, 501);
    aux_index_add_posting(db, b, 502);
    aux_index_add_posting(db, b, 501);
    aux_index_add_posting(db, c, 501);

    terms[0] = a; terms[1] = b; terms[2] = c;

    /* min_terms=2: 501 (3 terms) then 502 (2 terms); 503 (1 term) is out. */
    n = dl_search_ranked_top(db, terms, 3, 2, ids, scores, 8);
    if (n != 2) { FAIL("min_terms=2 did not return exactly 2 results"); dl_close(db); return; }
    if (ids[0] != 501 || scores[0] != 3) { FAIL("3-term doc is not first with score 3"); dl_close(db); return; }
    if (ids[1] != 502 || scores[1] != 2) { FAIL("2-term doc is not second with score 2"); dl_close(db); return; }

    /* min_terms=1 is OR: all three, ranked 3, 2, 1 — the score VARIES. */
    n = dl_search_ranked_top(db, terms, 3, 1, ids, scores, 8);
    if (n != 3) { FAIL("min_terms=1 did not return all 3 documents"); dl_close(db); return; }
    if (ids[0] != 501 || scores[0] != 3 ||
        ids[1] != 502 || scores[1] != 2 ||
        ids[2] != 503 || scores[2] != 1) {
        FAIL("OR ranking is not (501,3),(502,2),(503,1)");
        dl_close(db);
        return;
    }
    if (scores[0] == scores[1] || scores[1] == scores[2]) {
        FAIL("score did not vary across results");
        dl_close(db);
        return;
    }

    /* Tie-break: {a,b} with min_terms=1 gives 501 and 502 both at score 2, and
     * 503 at 1 — equal scores must come out obs_id ASCENDING. */
    {
        uint32_t ab[2];
        ab[0] = b; ab[1] = a; /* query order is not the tie-break order */
        n = dl_search_ranked_top(db, ab, 2, 1, ids, scores, 8);
        if (n != 3) { FAIL("{a,b} min_terms=1 did not return 3"); dl_close(db); return; }
        if (ids[0] != 501 || scores[0] != 2 || ids[1] != 502 || scores[1] != 2 ||
            ids[2] != 503 || scores[2] != 1) {
            FAIL("equal scores not tie-broken by obs_id ascending");
            dl_close(db);
            return;
        }
    }

    /* The AND tier is untouched: the same 3 terms still return exactly 501. */
    n = dl_search_top(db, terms, 3, ids, scores, 8);
    if (n != 1 || ids[0] != 501 || scores[0] != 3) {
        FAIL("dl_search_top AND semantics changed");
        dl_close(db);
        return;
    }

    /* Streaming form emits the same sequence. */
    sink.ids = ids;
    sink.scores = scores;
    sink.n = 0;
    sink.cap = 8;
    n = (int)dl_search_ranked(db, terms, 3, 1, rk_sink_cb, &sink);
    if (n != 3 || sink.n != 3) { FAIL("dl_search_ranked stream count wrong"); dl_close(db); return; }
    if (ids[0] != 501 || ids[1] != 502 || ids[2] != 503) {
        FAIL("dl_search_ranked stream order differs from _top");
        dl_close(db);
        return;
    }

    PASS();
    dl_close(db);
}

/* Brute-force cross-check of the WHOLE (obs_id, score) sequence, not just
 * membership: every threshold 1..n_terms on the shared 4-term corpus. */
static void t_search_ranked_bruteforce_crosscheck(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_search_ranked_xcheck");
    uint32_t docs[XC_N_DOC];
    uint32_t lists[XC_N_TERM][XC_N_DOC];
    int list_n[XC_N_TERM];
    uint32_t terms[XC_N_TERM];
    const uint32_t *L[XC_N_TERM];
    int ln[XC_N_TERM];
    uint32_t got[XC_N_DOC];
    int scores[XC_N_DOC];
    rk_expect exp[XC_N_DOC];
    int t, min_terms, n, ne;

    TEST("dl_search_ranked brute-force cross-check (score sequence, all thresholds)");
    if (xc_setup(db, docs, lists, list_n) != 0) {
        FAIL("setup: posting add failed");
        dl_close(db);
        return;
    }
    for (t = 0; t < XC_N_TERM; t++) {
        terms[t] = dl_intern_str(db, (char *)XC_TERM[t]); /* same sym, re-interned */
        if (terms[t] == 0) { FAIL("intern failed"); dl_close(db); return; }
        L[t] = lists[t];
        ln[t] = list_n[t];
    }

    for (min_terms = 1; min_terms <= XC_N_TERM; min_terms++) {
        int i, distinct_scores = 1;
        ne = rk_expected(docs, XC_N_DOC, L, ln, XC_N_TERM, min_terms, exp);
        n = dl_search_ranked_top(db, terms, XC_N_TERM, min_terms, got, scores, XC_N_DOC);
        if (n < 0) { FAIL("ranked search returned -1"); dl_close(db); return; }
        if (n != ne) { FAIL("result count disagrees with brute force"); dl_close(db); return; }
        for (i = 0; i < n; i++) {
            if (got[i] != exp[i].obs_id) {
                FAIL("(obs_id) sequence disagrees with brute force");
                dl_close(db);
                return;
            }
            if (scores[i] != exp[i].score) {
                FAIL("(score) sequence disagrees with brute force");
                dl_close(db);
                return;
            }
            if (i > 0 && scores[i] == scores[i - 1] && got[i] < got[i - 1]) {
                FAIL("equal scores are not obs_id ascending");
                dl_close(db);
                return;
            }
        }
        for (i = 1; i < n; i++)
            if (scores[i] != scores[0]) distinct_scores++;
        /* Instrument check: for min_terms=1 the corpus matches 1..4 terms, so a
         * single-score (i.e. dead-ranking) result would be a bug in the
         * expectation as much as in the engine. */
        if (min_terms == 1 && distinct_scores < 2) {
            FAIL("fixture does not exercise a varying score");
            dl_close(db);
            return;
        }
    }

    PASS();
    dl_close(db);
}

/* min_terms == n_terms must reproduce dl_search's result set EXACTLY — same
 * ids, same scores, same order — proving the AND path is preserved. */
static void t_search_ranked_and_equivalence(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_search_ranked_and_eq");
    uint32_t docs[XC_N_DOC];
    uint32_t lists[XC_N_TERM][XC_N_DOC];
    int list_n[XC_N_TERM];
    uint32_t and_ids[XC_N_DOC], rk_ids[XC_N_DOC];
    int and_scores[XC_N_DOC], rk_scores[XC_N_DOC];
    int qi, t, n_and, n_rk;

    TEST("dl_search_ranked min_terms==n_terms is byte-identical to dl_search");
    if (xc_setup(db, docs, lists, list_n) != 0) {
        FAIL("setup: posting add failed");
        dl_close(db);
        return;
    }

    for (qi = -1; qi < XC_N_QUERY; qi++) {
        uint32_t terms[3];
        int n_terms = 0;
        if (qi < 0) {
            terms[0] = dl_intern_str(db, (char *)XC_TERM[0]); /* single term */
            n_terms = 1;
        } else {
            for (t = 0; t < 3 && XC_QUERY[qi][t] >= 0; t++) {
                terms[t] = dl_intern_str(db, (char *)XC_TERM[XC_QUERY[qi][t]]);
                n_terms++;
            }
        }
        n_and = dl_search_top(db, terms, n_terms, and_ids, and_scores, XC_N_DOC);
        n_rk = dl_search_ranked_top(db, terms, n_terms, n_terms, rk_ids, rk_scores, XC_N_DOC);
        if (n_and < 0 || n_rk < 0) { FAIL("search returned -1"); dl_close(db); return; }
        if (n_and != n_rk) { FAIL("result counts differ from dl_search"); dl_close(db); return; }
        for (t = 0; t < n_and; t++) {
            if (and_ids[t] != rk_ids[t]) { FAIL("id order differs from dl_search"); dl_close(db); return; }
            if (and_scores[t] != rk_scores[t]) { FAIL("score differs from dl_search"); dl_close(db); return; }
        }
    }

    PASS();
    dl_close(db);
}

/* Contract edge cases: min_terms > n_terms is REJECTED (-1) — defined and
 * documented, never undefined and never silently emptied; min_terms <= 0
 * likewise.  A wholly absent term under min_terms=1 is a valid EMPTY result. */
static void t_search_ranked_contract(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_search_ranked_contract");
    uint32_t a, b, absent;
    uint32_t terms[3];
    uint32_t ids[8];
    int scores[8];
    rk_sink sink;
    int n;

    TEST("dl_search_ranked contract: bad thresholds rejected, empty is not an error");
    if (aux_index_ensure_postings(db) != 0) { FAIL("ensure postings failed"); dl_close(db); return; }

    a = dl_intern_str(db, "rk_c_a");
    b = dl_intern_str(db, "rk_c_b");
    absent = dl_intern_str(db, "rk_c_absent"); /* interned, never posted */
    if (a == 0 || b == 0 || absent == 0) { FAIL("intern failed"); dl_close(db); return; }
    aux_index_add_posting(db, a, 602);
    aux_index_add_posting(db, a, 601);
    aux_index_add_posting(db, b, 601);

    terms[0] = a;

    sink.ids = ids; sink.scores = scores; sink.n = 0; sink.cap = 8;

    /* min_terms > n_terms: rejected. */
    if (dl_search_ranked_top(db, terms, 1, 2, ids, scores, 8) != -1) {
        FAIL("min_terms > n_terms was not rejected");
        dl_close(db);
        return;
    }
    if (dl_search_ranked(db, terms, 1, 2, rk_sink_cb, &sink) != -1) {
        FAIL("min_terms > n_terms was not rejected (stream form)");
        dl_close(db);
        return;
    }
    /* min_terms <= 0 and n_terms <= 0: rejected. */
    if (dl_search_ranked_top(db, terms, 1, 0, ids, scores, 8) != -1) { FAIL("min_terms=0 accepted"); dl_close(db); return; }
    if (dl_search_ranked_top(db, terms, 1, -3, ids, scores, 8) != -1) { FAIL("min_terms<0 accepted"); dl_close(db); return; }
    if (dl_search_ranked_top(db, terms, 0, 0, ids, scores, 8) != -1) { FAIL("n_terms=0 accepted"); dl_close(db); return; }
    if (dl_search_ranked_top(db, terms, 1, 1, ids, scores, 0) != -1) { FAIL("limit=0 accepted"); dl_close(db); return; }

    /* NULL arguments: rejected on both forms. */
    sink.n = 0;
    if (dl_search_ranked(NULL, terms, 1, 1, rk_sink_cb, &sink) != -1) { FAIL("NULL db accepted"); dl_close(db); return; }
    if (dl_search_ranked(db, NULL, 1, 1, rk_sink_cb, &sink) != -1) { FAIL("NULL terms accepted"); dl_close(db); return; }
    if (dl_search_ranked(db, terms, 1, 1, NULL, &sink) != -1) { FAIL("NULL cb accepted"); dl_close(db); return; }
    if (dl_search_ranked_top(NULL, terms, 1, 1, ids, scores, 8) != -1) { FAIL("NULL db accepted (_top)"); dl_close(db); return; }
    if (dl_search_ranked_top(db, terms, 1, 1, NULL, scores, 8) != -1) { FAIL("NULL out buffer accepted"); dl_close(db); return; }

    /* An absent term is a valid empty result, not an error. */
    terms[0] = absent;
    if (dl_search_ranked_top(db, terms, 1, 1, ids, scores, 8) != 0) {
        FAIL("absent term did not yield 0 results");
        dl_close(db);
        return;
    }

    /* A repeated term sym counts ONCE: {a,a,b} with min_terms=2 keeps only the
     * document matching both distinct terms, at score 2 (not 3). */
    terms[0] = a; terms[1] = a; terms[2] = b;
    n = dl_search_ranked_top(db, terms, 3, 2, ids, scores, 8);
    if (n != 1 || ids[0] != 601 || scores[0] != 2) {
        FAIL("repeated term sym inflated the score");
        dl_close(db);
        return;
    }
    /* ... and {a,a} with min_terms=1 scores every matching document 1. */
    terms[0] = a; terms[1] = a;
    n = dl_search_ranked_top(db, terms, 2, 1, ids, scores, 8);
    if (n != 2 || ids[0] != 601 || scores[0] != 1 || ids[1] != 602 || scores[1] != 1) {
        FAIL("repeated term sym inflated the score (min_terms=1)");
        dl_close(db);
        return;
    }

    PASS();
    dl_close(db);
}

/* Duplicate sym_ids must never make a threshold SILENTLY EMPTY and must never
 * change a document's score.  The threshold is range-checked against the number
 * of DISTINCT query terms — a query cannot match more distinct terms than it
 * names — so min_terms above that is the documented caller-bug rejection (-1),
 * not 0 results.  Pre-fix this suite fails on {a,a,a} min_terms=2 (0 results,
 * exit 0) and on min_terms=3 (documents returned at score 3). */
static void t_search_ranked_duplicate_syms(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_search_ranked_dup");
    uint32_t a, b;
    uint32_t terms[3];
    uint32_t ids[8];
    int scores[8];
    rk_sink sink;
    int n, mt;

    TEST("dl_search_ranked: duplicate syms rejected above the DISTINCT count");
    if (aux_index_ensure_postings(db) != 0) { FAIL("ensure postings failed"); dl_close(db); return; }

    a = dl_intern_str(db, "rk_d_a");
    b = dl_intern_str(db, "rk_d_b");
    if (a == 0 || b == 0) { FAIL("intern failed"); dl_close(db); return; }
    aux_index_add_posting(db, a, 602);
    aux_index_add_posting(db, a, 601);
    aux_index_add_posting(db, b, 601);

    /* {a,a,a} names ONE distinct term: min_terms=1 is the only reachable
     * threshold; 2 and 3 are caller bugs, not empty results. */
    terms[0] = a; terms[1] = a; terms[2] = a;
    n = dl_search_ranked_top(db, terms, 3, 1, ids, scores, 8);
    if (n != 2 || ids[0] != 601 || scores[0] != 1 || ids[1] != 602 || scores[1] != 1) {
        FAIL("{a,a,a} min_terms=1 is not the 2 matching documents at score 1");
        dl_close(db);
        return;
    }
    if (dl_search_ranked_top(db, terms, 3, 2, ids, scores, 8) != -1) {
        FAIL("{a,a,a} min_terms=2 was not rejected (silently empty)");
        dl_close(db);
        return;
    }
    sink.ids = ids; sink.scores = scores; sink.n = 0; sink.cap = 8;
    if (dl_search_ranked(db, terms, 3, 2, rk_sink_cb, &sink) != -1 || sink.n != 0) {
        FAIL("{a,a,a} min_terms=2 was not rejected (stream form)");
        dl_close(db);
        return;
    }
    if (dl_search_ranked_top(db, terms, 3, 3, ids, scores, 8) != -1) {
        FAIL("{a,a,a} min_terms=3 was not rejected");
        dl_close(db);
        return;
    }

    /* {a,a,b} names two distinct terms: 1..2 are reachable, 3 is rejected — and
     * document 601 scores 2 at EVERY reachable threshold, i.e. score is a
     * property of (db, doc, query), not of the caller's min_terms sweep. */
    terms[0] = a; terms[1] = a; terms[2] = b;
    for (mt = 1; mt <= 2; mt++) {
        n = dl_search_ranked_top(db, terms, 3, mt, ids, scores, 8);
        if (n < 0) { FAIL("{a,a,b}: a reachable threshold was rejected"); dl_close(db); return; }
        if (ids[0] != 601 || scores[0] != 2) {
            FAIL("{a,a,b}: document 601's score depends on min_terms");
            dl_close(db);
            return;
        }
        if (mt == 1 && (n != 2 || ids[1] != 602 || scores[1] != 1)) {
            FAIL("{a,a,b} min_terms=1 is not (601,2),(602,1)");
            dl_close(db);
            return;
        }
        if (mt == 2 && n != 1) { FAIL("{a,a,b} min_terms=2 returned extra documents"); dl_close(db); return; }
    }
    if (dl_search_ranked_top(db, terms, 3, 3, ids, scores, 8) != -1) {
        FAIL("{a,a,b} min_terms=3 was not rejected");
        dl_close(db);
        return;
    }

    /* dl_search's AND semantics are untouched: it still scores n_terms verbatim,
     * duplicate syms included. */
    n = dl_search_top(db, terms, 3, ids, scores, 8);
    if (n != 1 || ids[0] != 601 || scores[0] != 3) {
        FAIL("dl_search AND semantics changed on a duplicate-sym query");
        dl_close(db);
        return;
    }

    /* Tie-break PIN on the delegated AND branch (min_terms == n_distinct).  Not
     * a pre-fix failure — the all-equal sort happened to come out ascending —
     * but the order must come from the comparator, not from sort stability. */
    terms[0] = a;
    n = dl_search_ranked_top(db, terms, 1, 1, ids, scores, 8);
    if (n != 2 || ids[0] != 601 || ids[1] != 602 || scores[0] != scores[1]) {
        FAIL("AND-branch equal scores are not obs_id ascending");
        dl_close(db);
        return;
    }

    PASS();
    dl_close(db);
}

/* ─── Integration test ──────────────────────────────────────────────────── */

static void t_integration_observation_indexing(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_integration");

    TEST("integration: observation indexing and search");
    
    /* Declare observation relation */
    if (dl_declare_relation(db, "observation", 2) != 0) {
        FAIL("failed to declare observation");
        dl_close(db);
        return;
    }
    
    /* Ensure postings */
    aux_index_ensure_postings(db);
    
    /* Add some observations and index them */
    uint32_t obs1 = dl_intern_str(db, "The quick brown fox");
    uint32_t obs2 = dl_intern_str(db, "The lazy dog");
    uint32_t obs3 = dl_intern_str(db, "Quick and lazy");
    uint32_t entity1 = dl_intern_str(db, "doc1");
    uint32_t entity2 = dl_intern_str(db, "doc2");
    uint32_t entity3 = dl_intern_str(db, "doc3");
    
    uint32_t cols[2];
    cols[0] = entity1; cols[1] = obs1;
    dl_add_fact(db, "observation", cols, 2);
    cols[0] = entity2; cols[1] = obs2;
    dl_add_fact(db, "observation", cols, 2);
    cols[0] = entity3; cols[1] = obs3;
    dl_add_fact(db, "observation", cols, 2);
    
    /* Index the observations */
    /* For obs1: "The quick brown fox" -> the, quick, brown, fox */
    char **tokens = tokenize(dl_intern_str_of(db, obs1), NULL);
    size_t n_tokens = 0;
    while (tokens[n_tokens]) n_tokens++;
    size_t i;
    for (i = 0; i < n_tokens; i++) {
        uint32_t term = dl_intern_str(db, tokens[i]);
        aux_index_add_posting(db, term, obs1);
    }
    token_free(tokens);
    
    /* For obs2: "The lazy dog" -> the, lazy, dog */
    tokens = tokenize(dl_intern_str_of(db, obs2), NULL);
    n_tokens = 0;
    while (tokens[n_tokens]) n_tokens++;
    for (i = 0; i < n_tokens; i++) {
        uint32_t term = dl_intern_str(db, tokens[i]);
        aux_index_add_posting(db, term, obs2);
    }
    token_free(tokens);
    
    /* For obs3: "Quick and lazy" -> quick, and, lazy */
    tokens = tokenize(dl_intern_str_of(db, obs3), NULL);
    n_tokens = 0;
    while (tokens[n_tokens]) n_tokens++;
    for (i = 0; i < n_tokens; i++) {
        uint32_t term = dl_intern_str(db, tokens[i]);
        aux_index_add_posting(db, term, obs3);
    }
    token_free(tokens);
    
    /* Search for "quick lazy" - should match obs1 (has quick) and obs3 (has both) */
    /* Actually obs1 has quick but not lazy, obs2 has lazy but not quick, obs3 has both */
    uint32_t term_quick = dl_intern_str(db, "quick");
    uint32_t term_lazy = dl_intern_str(db, "lazy");
    uint32_t terms[2] = {term_quick, term_lazy};
    
    uint32_t results[10];
    int scores[10];
    int n = dl_search_top(db, terms, 2, results, scores, 10);
    if (n != 1) { FAIL("expected 1 result (obs3 has both)"); dl_close(db); return; }
    if (results[0] != obs3) { FAIL("expected obs3"); dl_close(db); return; }
    
    PASS();
    dl_close(db);
}

static void t_index_observations(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_index_observations");

    TEST("dl_index_observations end-to-end");
    
    /* Declare observation relation */
    if (dl_declare_relation(db, "observation", 2) != 0) {
        FAIL("failed to declare observation");
        dl_close(db);
        return;
    }
    
    /* Add some observations */
    uint32_t obs1 = dl_intern_str(db, "The quick brown fox");
    uint32_t obs2 = dl_intern_str(db, "The lazy dog");
    uint32_t obs3 = dl_intern_str(db, "Quick and lazy");
    uint32_t entity1 = dl_intern_str(db, "doc1");
    uint32_t entity2 = dl_intern_str(db, "doc2");
    uint32_t entity3 = dl_intern_str(db, "doc3");
    
    uint32_t cols[2];
    cols[0] = entity1; cols[1] = obs1;
    dl_add_fact(db, "observation", cols, 2);
    cols[0] = entity2; cols[1] = obs2;
    dl_add_fact(db, "observation", cols, 2);
    cols[0] = entity3; cols[1] = obs3;
    dl_add_fact(db, "observation", cols, 2);
    
    /* Call dl_index_observations - should automatically populate postings */
    long n_postings = dl_index_observations(db);
    if (n_postings < 0) { FAIL("dl_index_observations failed"); dl_close(db); return; }
    /* obs1: "The quick brown fox" -> 4 tokens (the, quick, brown, fox)
       obs2: "The lazy dog" -> 3 tokens (the, lazy, dog)
       obs3: "Quick and lazy" -> 3 tokens (quick, and, lazy)
       Total unique postings: 4 + 3 + 3 = 10 (all distinct term/obs_id pairs)
       But "the" appears in obs1 and obs2, "quick" in obs1 and obs3, "lazy" in obs2 and obs3
       So unique postings: the->obs1, the->obs2, quick->obs1, quick->obs3, brown->obs1, fox->obs1, lazy->obs2, lazy->obs3, and->obs3, dog->obs2 = 10
    */
    if (n_postings != 10) { 
        FAIL("expected 10 postings");
        dl_close(db); 
        return; 
    }
    
    /* Now search for "quick lazy" - should match obs3 (has both) */
    uint32_t term_quick = dl_intern_str(db, "quick");
    uint32_t term_lazy = dl_intern_str(db, "lazy");
    uint32_t terms[2] = {term_quick, term_lazy};
    
    uint32_t results[10];
    int scores[10];
    int n = dl_search_top(db, terms, 2, results, scores, 10);
    if (n != 1) { FAIL("expected 1 result (obs3 has both)"); dl_close(db); return; }
    if (results[0] != obs3) { FAIL("expected obs3"); dl_close(db); return; }
    
    PASS();
    dl_close(db);
}

static void t_index_observations_no_relation(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_index_no_rel");

    TEST("dl_index_observations with no observation relation");
    
    /* Don't declare observation relation - should return 0 */
    long n_postings = dl_index_observations(db);
    if (n_postings != 0) { FAIL("expected 0 postings when no observation relation"); dl_close(db); return; }
    
    PASS();
    dl_close(db);
}

/* ─── Version-aware search tests ──────────────────────────────────────────── */

static void t_search_version_basic(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_search_version_basic");

    TEST("dl_search_version basic time-travel");
    
    aux_index_ensure_postings(db);
    
    /* Build v1: doc1 + doc2 */
    uint32_t term1 = dl_intern_str(db, "hello");
    uint32_t term2 = dl_intern_str(db, "world");
    uint32_t obs1 = dl_intern_str(db, "doc1");
    uint32_t obs2 = dl_intern_str(db, "doc2");
    
    aux_index_add_posting(db, term1, obs1);
    aux_index_add_posting(db, term2, obs1);
    aux_index_add_posting(db, term1, obs2);
    
    /* Publish v1 */
    if (dl_publish_snapshot(db) != 0) {
        FAIL("failed to publish v1");
        dl_close(db);
        return;
    }
    
    /* Get version list - should have at least 1 version */
    uint32_t versions[10];
    long n_versions = dl_snapshot_versions(db, versions, 10);
    if (n_versions < 1) { FAIL("expected at least 1 version"); dl_close(db); return; }
    uint32_t v1 = versions[0];
    
    /* Search as-of v1 for "hello world" - should return obs1 (has both terms) */
    uint32_t terms[2] = {term1, term2};
    uint32_t results[10];
    int scores[10];
    int n = dl_search_top_version(db, v1, terms, 2, results, scores, 10);
    if (n != 1) { FAIL("expected 1 result as-of v1"); dl_close(db); return; }
    if (results[0] != obs1) { FAIL("expected obs1 as-of v1"); dl_close(db); return; }
    
    /* Add doc3 with both terms */
    uint32_t obs3 = dl_intern_str(db, "doc3");
    aux_index_add_posting(db, term1, obs3);
    aux_index_add_posting(db, term2, obs3);
    
    /* Publish v2 */
    if (dl_publish_snapshot(db) != 0) {
        FAIL("failed to publish v2");
        dl_close(db);
        return;
    }
    
    /* Get version list again */
    n_versions = dl_snapshot_versions(db, versions, 10);
    if (n_versions < 2) { FAIL("expected at least 2 versions"); dl_close(db); return; }
    uint32_t v2 = versions[1];
    
    /* Search as-of v1 again - should still return only obs1 */
    n = dl_search_top_version(db, v1, terms, 2, results, scores, 10);
    if (n != 1) { FAIL("expected 1 result as-of v1 (after v2)"); dl_close(db); return; }
    if (results[0] != obs1) { FAIL("expected obs1 as-of v1 (after v2)"); dl_close(db); return; }
    
    /* Search as-of v2 - should return obs1 and obs3 */
    n = dl_search_top_version(db, v2, terms, 2, results, scores, 10);
    if (n != 2) { FAIL("expected 2 results as-of v2"); dl_close(db); return; }
    
    PASS();
    dl_close(db);
}

static void t_search_version_top_limit(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_search_version_top");

    TEST("dl_search_top_version with limit");
    
    aux_index_ensure_postings(db);
    
    /* Build v1 with multiple docs */
    uint32_t term = dl_intern_str(db, "test");
    int i;
    for (i = 0; i < 5; i++) {
        char name[16];
        snprintf(name, sizeof(name), "obs%d", i);
        uint32_t obs = dl_intern_str(db, name);
        aux_index_add_posting(db, term, obs);
    }
    
    if (dl_publish_snapshot(db) != 0) {
        FAIL("failed to publish");
        dl_close(db);
        return;
    }
    
    uint32_t versions[10];
    long n_versions = dl_snapshot_versions(db, versions, 10);
    if (n_versions < 1) { FAIL("expected at least 1 version"); dl_close(db); return; }
    
    /* Search with limit 3 */
    uint32_t results[10];
    int scores[10];
    int n = dl_search_top_version(db, versions[0], &term, 1, results, scores, 3);
    if (n != 3) { FAIL("expected 3 results (limited)"); dl_close(db); return; }
    
    PASS();
    dl_close(db);
}

static void t_search_version_no_postings(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_search_version_no_postings");

    TEST("dl_search_version when __postings__ doesn't exist in that version");
    
    /* Publish a snapshot WITHOUT any postings */
    if (dl_publish_snapshot(db) != 0) {
        FAIL("failed to publish");
        dl_close(db);
        return;
    }
    
    uint32_t versions[10];
    long n_versions = dl_snapshot_versions(db, versions, 10);
    if (n_versions < 1) { FAIL("expected at least 1 version"); dl_close(db); return; }
    
    /* Now add postings to live db */
    aux_index_ensure_postings(db);
    uint32_t term = dl_intern_str(db, "test");
    uint32_t obs = dl_intern_str(db, "doc1");
    aux_index_add_posting(db, term, obs);
    
    /* Search as-of v1 (which has no __postings__) - should return -1 */
    uint32_t results[10];
    int scores[10];
    int n = dl_search_top_version(db, versions[0], &term, 1, results, scores, 10);
    if (n != -1) { FAIL("expected -1 when __postings__ absent from version"); dl_close(db); return; }
    
    PASS();
    dl_close(db);
}

static void t_search_version_nonexistent(void)
{
    char dir[512];
    dl_db *db = fresh_db(dir, sizeof(dir), "t_search_version_nonexistent");

    TEST("dl_search_version with nonexistent version");
    
    aux_index_ensure_postings(db);
    uint32_t term = dl_intern_str(db, "test");
    uint32_t obs = dl_intern_str(db, "doc1");
    aux_index_add_posting(db, term, obs);
    
    /* Search with a version that doesn't exist - should return -1 */
    uint32_t results[10];
    int scores[10];
    int n = dl_search_top_version(db, 999999, &term, 1, results, scores, 10);
    if (n != -1) { FAIL("expected -1 for nonexistent version"); dl_close(db); return; }
    
    PASS();
    dl_close(db);
}

int main(void)
{
    printf("=== test_search ===\n");
    
    /* Tokenizer tests */
    t_tokenize_basic();
    t_tokenize_lowercase();
    t_tokenize_punctuation();
    t_tokenize_empty();
    t_tokenize_only_punctuation();
    
    /* aux_index tests */
    t_aux_index_ensure();
    t_aux_index_add();
    
    /* dl_search tests */
    t_search_single_term();
    t_search_and();
    t_search_no_match();
    t_search_top_limit();
    t_search_empty_terms();
    t_search_bruteforce_crosscheck();
    t_search_crosscheck_detects_diff();
    t_search_and_partial_absent();

    /* Ranked (partial-match / OR) search */
    t_search_ranked_partial();
    t_search_ranked_bruteforce_crosscheck();
    t_search_ranked_and_equivalence();
    t_search_ranked_contract();
    t_search_ranked_duplicate_syms();
    
    /* Integration test */
    t_integration_observation_indexing();
    
    /* dl_index_observations tests */
    t_index_observations();
    t_index_observations_no_relation();
    
    /* Version-aware search tests */
    t_search_version_basic();
    t_search_version_top_limit();
    t_search_version_no_postings();
    t_search_version_nonexistent();
    
    printf("\n");
    if (tests_failed == 0)
        printf("All %d tests passed.\n", tests_run);
    else
        printf("%d/%d tests passed, %d failed.\n",
               tests_run - tests_failed, tests_run, tests_failed);
    
    return tests_failed != 0;
}
