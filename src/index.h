/*
 * index.h — Auxiliary postings index primitive for full-text search
 *
 * Provides a unified abstraction over dl_declare_relation + dl_add_fact + dl_prefix
 * for building token->sym_id postings DAFSAs.  Reusable by full-text, trigram,
 * MIH, and regex tiers.
 *
 * The relation store is SET-semantic (DAFSA collapses duplicate keys), so
 * tf-from-duplicate-keys is IMPOSSIBLE.  Ranking uses DISTINCT matched terms
 * per obs_id (more matched -> higher) and/or inverse doc-frequency.
 */
#ifndef INDEX_H
#define INDEX_H

#include "dl.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Tokenizer ───────────────────────────────────────────────────────────── */

/* Tokenize a string: split on non-alphanumeric, lowercase each token.
 * Returns a NULL-terminated array of tokens (caller must free the array AND
 * each string via token_free).  Returns NULL on OOM.
 * *n_out receives the number of tokens (not counting the NULL terminator). */
char **tokenize(const char *text, size_t *n_out);

/* Free a token array returned by tokenize. */
void token_free(char **tokens);

/* ─── Postings index ─────────────────────────────────────────────────────── */

/* Ensure the postings relation (arity-2: term_sym, obs_id) exists.
 * Idempotent: returns 0 on success (relation exists or was created),
 * -1 on error. */
int aux_index_ensure_postings(dl_db *db);

/* Add a posting: term_sym -> obs_id.  Uses dl_add_fact under the hood.
 * Returns 1 if added, 0 if duplicate, -1 on error. */
int aux_index_add_posting(dl_db *db, uint32_t term_sym, uint32_t obs_id);

/* ─── Search callback ─────────────────────────────────────────────────────── */

/* Callback for dl_search: receives (obs_id, score).  score is the number of
 * distinct matched terms for this obs_id (higher = more terms matched).
 * Return non-zero to stop early. */
typedef int (*dl_search_cb)(uint32_t obs_id, int score, void *user);

/* Full-text search: find obs_ids whose content matches ALL of the given terms
 * (AND semantics).  terms[] are already-interned sym_ids.
 *
 * Ranking: score = number of distinct matched terms per obs_id (since the
 * relation store is SET-semantic, duplicate postings are impossible, so
 * tf-from-duplicates cannot work; we rank by how many distinct query terms
 * each obs_id matched).  For AND search, all results have the same score
 * (n_terms).
 *
 * Results are returned via cb in descending score order (highest score first).
 * If multiple obs_ids have the same score, order is arbitrary.
 *
 * Returns the number of results emitted, or -1 on error (NULL db, n_terms == 0,
 * or a term lookup failure). */
long dl_search(dl_db *db, const uint32_t *terms, int n_terms,
              dl_search_cb cb, void *user);

/* Convenience: dl_search with a --top N limit.  Collects up to limit results
 * and returns them sorted by score descending.  Caller provides buffers
 * `obs_ids_out` and `scores_out` of size at least `limit`.
 * Returns the number of results (<= limit), or -1 on error. */
int dl_search_top(dl_db *db, const uint32_t *terms, int n_terms,
                  uint32_t *obs_ids_out, int *scores_out, int limit);

/* Version-aware search: same AND-intersect + rank logic as dl_search,
 * but collects each term's obs_ids as-of a published snapshot `version` via
 * dl_query_bound_version on the "__postings__" relation.
 * Since the sym_id space is append-only/never-reused, pre-interned term sym_ids
 * are valid across versions — no re-interning needed.
 *
 * Returns the number of results emitted, or -1 on error (NULL db, n_terms == 0,
 * version == 0, nonexistent version, or __postings__ relation absent from that
 * version).  Note: -1 fires only when __postings__ was never DECLARED at the
 * time that version was published; a declared-but-empty __postings__ in the
 * version yields 0 results, not an error. */
long dl_search_version(dl_db *db, uint32_t version, const uint32_t *terms, int n_terms,
                       dl_search_cb cb, void *user);

/* Version-aware convenience: dl_search_version with a --top N limit.
 * Same contract as dl_search_top but for a specific snapshot version.
 * Returns the number of results (<= limit), or -1 on error. */
int dl_search_top_version(dl_db *db, uint32_t version, const uint32_t *terms, int n_terms,
                         uint32_t *obs_ids_out, int *scores_out, int limit);

/* ─── Ranked (partial-match / OR) search ─────────────────────────────────── */

/* Ranked full-text search: a document is a result iff it matches at least
 * `min_terms` of the query terms (terms[] are already-interned sym_ids).
 *
 * Score: the number of DISTINCT query terms the document matched (an honest
 * integer, 1..n_terms — NOT a scaled tf-idf; see the note below).  Results are
 * emitted in score-descending order with obs_id ASCENDING as the explicit
 * tie-break, so the order is fully deterministic.
 *
 * min_terms == n_terms is exactly dl_search's AND retrieval: it runs the same
 * intersect/rank/emit body, so the result set, the scores AND the order are
 * identical to dl_search's on the same fixture, and the AND path keeps its
 * current cost.  min_terms == 1 is pure OR.
 *
 * Ranking signals deliberately NOT in the score:
 *   - tf: IMPOSSIBLE.  The store is SET-semantic (DAFSA collapses duplicate
 *     keys), so duplicate postings cannot exist.
 *   - idf: available (each term's posting-set size is enumerated before the
 *     count) but NOT baked in — log-idf weights are fractional, and packing a
 *     fixed-point scale into the int score would be fake precision rather than
 *     a ranking.  A caller who wants rarity can rank on it as a separate axis.
 *   - positions: out of scope (3x storage for a signal that cannot vary within
 *     one term's own posting set).
 *
 * Returns the number of results emitted, or -1 on error: NULL db/terms/cb,
 * n_terms <= 0, min_terms <= 0, min_terms > n_terms (rejected as a caller bug
 * rather than silently emptied), or a per-term read failure.  0 is a valid
 * empty result.
 *
 * Duplicate sym_ids in terms[] are counted ONCE by the partial-match path (a
 * repeated term cannot raise a document's score or help it reach min_terms);
 * the min_terms == n_terms branch keeps dl_search's existing score of n_terms
 * verbatim. */
long dl_search_ranked(dl_db *db, const uint32_t *terms, int n_terms, int min_terms,
                      dl_search_cb cb, void *user);

/* Convenience: dl_search_ranked with a --top N limit.  Collects up to `limit`
 * results, already ranked (score desc, obs_id asc), into caller buffers.
 * Returns the number of results (<= limit), or -1 on error. */
int dl_search_ranked_top(dl_db *db, const uint32_t *terms, int n_terms, int min_terms,
                         uint32_t *obs_ids_out, int *scores_out, int limit);

/* Index all observations: walks the observation(entity, content) relation,
 * tokenizes each content string, interns each term, and adds postings.
 * Returns the number of postings added, or -1 on error.
 * Idempotent: safe to call repeatedly (dl_add_fact on existing fact is fine).
 * If the "observation" relation does not exist, returns 0; if it is a fixed
 * relation of arity != 2 (or a variadic relation holding non-arity-2 facts),
 * returns -1.
 */
long dl_index_observations(dl_db *db);

#ifdef __cplusplus
}
#endif

#endif /* INDEX_H */
