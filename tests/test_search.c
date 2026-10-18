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
