/*
 * index.c — Auxiliary postings index primitive + tokenizer + dl_search
 *
 * Implements the unified aux_index abstraction and the full-text search tier
 * as its first concrete consumer.
 *
 * Design notes:
 * - The postings relation is arity-2: (term_sym, obs_id)
 * - Tokenizer: split on non-alphanumeric, lowercase
 * - SET-semantic store: DAFSA collapses duplicate keys, so tf-from-duplicates
 *   is IMPOSSIBLE.  Ranking uses distinct matched terms per obs_id.
 */

#include "index.h"
#include "dl.h"

#include <stdlib.h>
#include <string.h>

/* ─── Tokenizer ───────────────────────────────────────────────────────────── */

/* Check if a byte is alphanumeric (ASCII). */
static int is_alnum_ascii(unsigned char c)
{
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}

/* Lowercase an ASCII character. */
static unsigned char to_lower_ascii(unsigned char c)
{
    if (c >= 'A' && c <= 'Z')
        return (unsigned char)(c - 'A' + 'a');
    return c;
}

char **tokenize(const char *text, size_t *n_out)
{
    if (!text) {
        if (n_out) *n_out = 0;
        return NULL;
    }

    /* Count tokens first to allocate the array. */
    size_t n = 0;
    const char *p = text;
    while (*p) {
        /* Skip non-alphanumeric */
        while (*p && !is_alnum_ascii((unsigned char)*p))
            p++;
        if (!*p) break;
        n++;
        /* Skip alphanumeric */
        while (*p && is_alnum_ascii((unsigned char)*p))
            p++;
    }

    if (n == 0) {
        if (n_out) *n_out = 0;
        return NULL;
    }

    /* Allocate token array (+ NULL terminator) */
    char **tokens = calloc(n + 1, sizeof(*tokens));
    if (!tokens) return NULL;

    /* Extract tokens */
    p = text;
    size_t i = 0;
    while (*p && i < n) {
        /* Skip non-alphanumeric */
        while (*p && !is_alnum_ascii((unsigned char)*p))
            p++;
        if (!*p) break;

        /* Start of token */
        const char *start = p;
        while (*p && is_alnum_ascii((unsigned char)*p))
            p++;

        /* Allocate and copy, lowercasing */
        size_t len = (size_t)(p - start);
        char *tok = malloc(len + 1);
        if (!tok) {
            token_free(tokens);
            return NULL;
        }
        size_t j;
        for (j = 0; j < len; j++)
            tok[j] = (char)to_lower_ascii((unsigned char)start[j]);
        tok[len] = '\0';
        tokens[i++] = tok;
    }

    if (n_out) *n_out = n;
    return tokens;
}

void token_free(char **tokens)
{
    if (!tokens) return;
    size_t i = 0;
    while (tokens[i]) {
        free(tokens[i]);
        i++;
    }
    free(tokens);
}

/* ─── Postings index ─────────────────────────────────────────────────────── */

static const char POSTINGS_REL_NAME[] = "__postings__";

int aux_index_ensure_postings(dl_db *db)
{
    if (!db) return -1;
    /* dl_declare_relation is idempotent - just call it */
    return dl_declare_relation(db, POSTINGS_REL_NAME, 2);
}

int aux_index_add_posting(dl_db *db, uint32_t term_sym, uint32_t obs_id)
{
    if (!db) return -1;
    if (term_sym == 0 || obs_id == 0) return -1;  /* invalid sym_ids */

    /* Ensure the relation exists */
    if (aux_index_ensure_postings(db) != 0)
        return -1;

    uint32_t cols[2] = {term_sym, obs_id};
    return dl_add_fact(db, POSTINGS_REL_NAME, cols, 2);
}

/* ─── Set operations for obs_id sets ──────────────────────────────────────── */

/* Simple dynamic int set (obs_id is u32). */
typedef struct {
    uint32_t *data;
    size_t   count;
    size_t   cap;
} int_set;

static int int_set_init(int_set *s)
{
    s->data = NULL;
    s->count = 0;
    s->cap = 0;
    return 0;
}

static void int_set_free(int_set *s)
{
    free(s->data);
    s->data = NULL;
    s->count = 0;
    s->cap = 0;
}

/* Linear search; small sets expected (postings per term). */
static int int_set_contains(const int_set *s, uint32_t val)
{
    size_t i;
    for (i = 0; i < s->count; i++)
        if (s->data[i] == val)
            return 1;
    return 0;
}

static int int_set_add(int_set *s, uint32_t val)
{
    if (int_set_contains(s, val))
        return 0;  /* already present */
    if (s->count >= s->cap) {
        size_t new_cap = s->cap ? s->cap * 2 : 8;
        uint32_t *new_data = realloc(s->data, new_cap * sizeof(*new_data));
        if (!new_data) return -1;
        s->data = new_data;
        s->cap = new_cap;
    }
    s->data[s->count++] = val;
    return 1;
}

/* Callback context for collecting obs_ids */
typedef struct {
    int_set *set;
    int error;
} collect_ctx;

/* Callback for dl_prefix to collect obs_ids */
static int collect_obs_id_cb(const uint32_t *cols, uint8_t arity, void *user)
{
    collect_ctx *c = (collect_ctx *)user;
    (void)arity;  /* arity should be 2 */
    if (int_set_add(c->set, cols[1]) < 0) {
        c->error = 1;
        return 1;  /* stop */
    }
    return 0;
}

/* Collect all obs_ids for a single term via dl_prefix on the postings relation. */
static int collect_term_obs_ids(dl_db *db, uint32_t term_sym, int_set *set)
{
    int_set_free(set);
    int_set_init(set);

    collect_ctx ctx = {set, 0};

    long n = dl_prefix(db, POSTINGS_REL_NAME, &term_sym, 1,
                   collect_obs_id_cb, &ctx);
    if (n < 0 || ctx.error) return -1;

    return 0;
}

/* Intersect multiple obs_id sets using smallest-first strategy.
 * Returns a new set containing the intersection, or NULL on OOM. */
static int_set *int_set_intersect(int_set **sets, int n_sets)
{
    if (n_sets == 0) return NULL;
    if (n_sets == 1) {
        int_set *result = malloc(sizeof(*result));
        if (!result) return NULL;
        *result = *sets[0];
        sets[0]->data = NULL;  /* steal the data */
        sets[0]->count = 0;
        sets[0]->cap = 0;
        return result;
    }

    /* Find the smallest set to iterate over */
    int smallest_idx = 0;
    size_t smallest_count = sets[0]->count;
    int i;
    for (i = 1; i < n_sets; i++) {
        if (sets[i]->count < smallest_count) {
            smallest_count = sets[i]->count;
            smallest_idx = i;
        }
    }

    /* For each obs_id in the smallest set, check if it's in all other sets */
    int_set *result = malloc(sizeof(*result));
    if (!result) return NULL;
    int_set_init(result);

    size_t j;
    for (j = 0; j < sets[smallest_idx]->count; j++) {
        uint32_t obs_id = sets[smallest_idx]->data[j];
        int in_all = 1;
        for (i = 0; i < n_sets; i++) {
            if (i == smallest_idx) continue;
            if (!int_set_contains(sets[i], obs_id)) {
                in_all = 0;
                break;
            }
        }
        if (in_all) {
            if (int_set_add(result, obs_id) < 0) {
                int_set_free(result);
                free(result);
                return NULL;
            }
        }
    }

    return result;
}

/* Sort results by score descending. */
typedef struct {
    uint32_t obs_id;
    int score;
} scored_result;

static int scored_result_cmp(const void *a, const void *b)
{
    const scored_result *ra = (const scored_result *)a;
    const scored_result *rb = (const scored_result *)b;
    if (ra->score != rb->score)
        return rb->score - ra->score;  /* descending */
    return 0;  /* arbitrary order for ties */
}

/* Callback context for dl_search_top */
typedef struct {
    uint32_t *obs_ids;
    int *scores;
    int cap;
    int count;
} top_ctx;

/* Callback for dl_search to collect top results */
static int top_search_cb(uint32_t obs_id, int score, void *user)
{
    top_ctx *c = (top_ctx *)user;
    if (c->count >= c->cap)
        return 1;  /* stop */
    c->obs_ids[c->count] = obs_id;
    c->scores[c->count] = score;
    c->count++;
    return 0;
}

long dl_search(dl_db *db, const uint32_t *terms, int n_terms,
              dl_search_cb cb, void *user)
{
    if (!db || !terms || n_terms <= 0 || !cb)
        return -1;

    /* Collect obs_id sets for each term */
    int_set **sets = calloc((size_t)n_terms, sizeof(*sets));
    if (!sets) return -1;

    int i;
    int collect_err = 0;
    int all_empty = 1;
    for (i = 0; i < n_terms; i++) {
        sets[i] = malloc(sizeof(**sets));
        if (!sets[i]) {
            for (int j = 0; j < i; j++) {
                int_set_free(sets[j]);
                free(sets[j]);
            }
            free(sets);
            return -1;
        }
        int_set_init(sets[i]);

        /* collect_term_obs_ids returns 0 for an ABSENT term (empty set, valid)
         * and -1 for a genuine error (missing postings relation / OOM).  A
         * genuine error must NOT be treated as "0 results". */
        if (collect_term_obs_ids(db, terms[i], sets[i]) < 0) {
            collect_err = 1;
            break;
        }
        if (sets[i]->count > 0)
            all_empty = 0;
    }

    if (collect_err) {
        for (int j = 0; j <= i; j++) {
            int_set_free(sets[j]);
            free(sets[j]);
        }
        free(sets);
        return -1;
    }

    /* If any term has no postings, the AND is empty */
    if (all_empty) {
        for (i = 0; i < n_terms; i++) {
            int_set_free(sets[i]);
            free(sets[i]);
        }
        free(sets);
        return 0;
    }

    /* Intersect all sets */
    int_set *intersection = int_set_intersect(sets, n_terms);

    /* Free the individual sets */
    for (i = 0; i < n_terms; i++) {
        int_set_free(sets[i]);
        free(sets[i]);
    }
    free(sets);

    if (!intersection)
        return -1;

    /* Score each result: for AND search, all results match all terms.
     * Since the relation store is SET-semantic (DAFSA collapses duplicates),
     * we cannot compute tf from duplicate keys.  We rank by the number of
     * distinct matched terms per obs_id.  For AND search, this is always n_terms.
     */

    /* Build scored results */
    if (intersection->count == 0) {
        int_set_free(intersection);
        free(intersection);
        return 0;  /* disjoint terms -> no matches, not an error */
    }

    scored_result *results = malloc(intersection->count * sizeof(*results));
    if (!results) {
        int_set_free(intersection);
        free(intersection);
        return -1;
    }

    size_t k;
    for (k = 0; k < intersection->count; k++) {
        results[k].obs_id = intersection->data[k];
        results[k].score = n_terms;  /* All matched all terms */
    }

    /* Sort by score descending */
    qsort(results, intersection->count, sizeof(*results), scored_result_cmp);

    /* Emit via callback.  Count the emission BEFORE invoking the cb so a
     * callback that stops early still counts the result it consumed (mirrors
     * dl_prefix's count-before-callback semantics). */
    long emitted = 0;
    for (k = 0; k < intersection->count; k++) {
        emitted++;
        if (cb(results[k].obs_id, results[k].score, user) != 0)
            break;
    }

    free(results);
    int_set_free(intersection);
    free(intersection);

    return emitted;
}

int dl_search_top(dl_db *db, const uint32_t *terms, int n_terms,
                  uint32_t *obs_ids_out, int *scores_out, int limit)
{
    if (!db || !terms || n_terms <= 0 || !obs_ids_out || !scores_out || limit <= 0)
        return -1;

    top_ctx ctx = {obs_ids_out, scores_out, limit, 0};

    long n = dl_search(db, terms, n_terms, top_search_cb, &ctx);
    if (n < 0) return -1;
    return ctx.count;
}

/* ─── Observation indexing ──────────────────────────────────────────────────── */

/* Callback context for collecting observation tuples */
typedef struct {
    dl_db *db;
    long postings_added;
    int error;
} index_obs_ctx;

/* Callback for dl_prefix to process each observation tuple */
static int index_obs_cb(const uint32_t *cols, uint8_t arity, void *user)
{
    index_obs_ctx *ctx = (index_obs_ctx *)user;

    if (ctx->error) return 1;  /* stop on error */

    /* The observation relation must be arity-2 (entity, content).  A fixed
     * relation of any other arity, or a variadic variant of a different
     * arity, would make cols[1] (the content column) read out of bounds. */
    if (arity != 2) {
        ctx->error = 1;
        return 1;
    }

    /* cols[0] = entity_sym, cols[1] = content_sym */
    const char *content = dl_intern_str_of(ctx->db, cols[1]);
    if (!content) {
        ctx->error = 1;
        return 1;
    }

    /* Tokenize the content */
    char **tokens = tokenize(content, NULL);
    if (!tokens) {
        /* No tokens to index for this observation */
        return 0;
    }

    /* For each token, intern and add posting */
    size_t i = 0;
    while (tokens[i]) {
        uint32_t term_sym = dl_intern_str(ctx->db, tokens[i]);
        if (term_sym == 0) {
            ctx->error = 1;
            token_free(tokens);
            return 1;
        }
        int rc = aux_index_add_posting(ctx->db, term_sym, cols[1]);
        if (rc < 0) {
            ctx->error = 1;
            token_free(tokens);
            return 1;
        }
        if (rc == 1) {
            ctx->postings_added++;
        }
        i++;
    }

    token_free(tokens);
    return 0;
}

long dl_index_observations(dl_db *db)
{
    if (!db) return -1;

    /* Ensure postings relation exists */
    if (aux_index_ensure_postings(db) != 0)
        return -1;

    /* Check if observation relation exists and is arity-2 */
    /* We use dl_prefix with k=0 to enumerate all observation tuples */
    index_obs_ctx ctx = {db, 0, 0};

    long n = dl_prefix(db, "observation", NULL, 0, index_obs_cb, &ctx);
    if (n < 0) {
        /* Relation doesn't exist (or is variadic and enumerable as empty):
         * nothing to index.  A variadic relation that holds non-arity-2 facts
         * is caught by the arity check in index_obs_cb, not here. */
        return 0;
    }

    if (ctx.error)
        return -1;

    return ctx.postings_added;
}
