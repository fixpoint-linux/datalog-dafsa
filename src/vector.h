/*
 * vector.h — Vector tier (semantic half) query path (S2)
 *
 * SCAN-AND-RANK over __vec_q__.  Every live entity in the corpus that has
 * stored int8 chunks is scored (integer dot <q,v> + integer norm |v|), the
 * whole candidate set is ranked by EXACT cosine order via a signed int64
 * cross-multiply (dot_a*|b| vs dot_b*|a|, integer isqrt — no float, no
 * sqrtf), and the top k are emitted.  Recall is 100% by construction and the
 * cost is O(N*D) int8 multiply-accumulates for N live vectors.
 *
 * SUPERSEDED (2026-08-11): the previous query path was MIH candidate
 * retrieval over 16 per-band postings __sig0__..__sig15__ plus a __itq_basis__
 * relation that was stored but never read.  Those relations are no longer
 * part of the read path — see the dated SUPERSEDED notes in
 * docs/datalog-dafsa-vector-search.md and docs/vector-search.html.  The
 * on-disk facts are untouched history; the engine simply stops consulting
 * them.
 *
 * Storage layout (single source of truth — an external encoder MUST emit
 * exactly these constants; the engine never embeds):
 *   - __vec_q__   (arity-3):  entity_sym_id, chunk_idx, packed_4x_int8_u32
 *                             (96 chunks, little-endian pack4).
 * __vec_q__ is the ONLY relation the query path reads (besides the per-corpus
 * liveness relation below).
 *
 * LIVE vs VERSION read discipline (the load-bearing invariant, F1/F2):
 *   - dl_prefix / dl_lookup are LIVE-ONLY (dl.c) — they never route to the
 *     snapshot view.  Version reads MUST use dl_query_bound_version.
 *   - LIVE search reads BOTH __vec_q__ and the corpus liveness relation via
 *     dl_prefix.
 *   - VERSION search reads BOTH via dl_query_bound_version.
 *   dl_vector_search / dl_vector_search_version are a single structure with
 *   two modes differing ONLY in the read primitive — mirroring dl_search /
 *   dl_search_version in index.c — which keeps the invariant auditable.
 *
 * dl_vector_rerank is the LIVE-only "rank exactly this candidate set" form of
 * the same ranking (it is what the search body runs internally, exposed for
 * callers that already hold a candidate set).
 */
#ifndef VECTOR_H
#define VECTOR_H

#include "dl.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Compile-time layout constants (shared verbatim with the encoder) ─── */

#define VEC_D          384           /* embedding dim (bge-small)           */
#define VEC_IVEC_WORDS (VEC_D / 4)   /* 96 u32, 4 int8 packed each          */
#define VEC_ENTITY_REL "entity"      /* arity-2, name sym-id in col 0       */

/* ─── Corpus descriptor ────────────────────────────────────────────────────
 * The vector tier is shared by multiple corpora.  Each corpus keeps SEPARATE
 * liveness / vec relations so entity-name vectors and observation-content
 * vectors can never collide (the interner is shared across all relations, so
 * the __vec_q__/__vec_obs__ relations are keyed by sym id regardless of which
 * corpus produced it).  ENTITY is the historical path (byte-compatible);
 * OBSERVATION_CONTENT indexes observation CONTENT text (col 1 of
 * `observation`). */
struct dl_vec_corpus {
    const char *filter_rel;   /* liveness relation                  */
    uint8_t     filter_col;   /* which col holds the sym            */
    const char *vec_rel;      /* e.g. "__vec_q__" / "__vec_obs__"   */
};
#define DL_VEC_CORPUS_ENTITY              ((struct dl_vec_corpus){"entity",0,"__vec_q__"})
#define DL_VEC_CORPUS_OBSERVATION_CONTENT ((struct dl_vec_corpus){"observation",1,"__vec_obs__"})

/* ─── Callback ──────────────────────────────────────────────────────────── */

/* Callback for dl_vector_search / dl_vector_search_version / dl_vector_rerank.
 * entity_sym is the matched entity's name sym-id.  score semantics:
 *   - search/_version: the signed integer dot product <q, v> (the int8 cosine
 *     numerator; the emitted order is exact cosine order).
 *   - rerank: the same quantity (they share one ranking path).
 * Return non-zero to stop early. */
typedef int (*dl_vec_cb)(uint32_t entity_sym, int score, void *user);

/* ─── Query path ────────────────────────────────────────────────────────── */

/* Scan-and-rank semantic search over the LIVE relations.  q_int8 is a
 * PRE-QUANTIZED int8 query vector (VEC_IVEC_WORDS u32, 4 int8 packed
 * little-endian) — C never embeds.
 * Scores every live entity in the corpus that has __vec_q__ chunks on record,
 * drops entities that are not live (the corpus liveness relation), keeps the
 * top k by exact cosine order, and emits them via cb.
 * Returns the number emitted, or -1 on error (NULL db/q_int8/cb, k <= 0, a
 * missing liveness relation, a missing __vec_q__, or OOM). */
long dl_vector_search(dl_db *db, const uint32_t *q_int8,
                      int k, dl_vec_cb cb, void *user);

/* Same structure, but reads BOTH __vec_q__ and the liveness relation as-of a
 * published snapshot `version` via dl_query_bound_version.
 * Error contract (mirrors dl_search_version): returns -1 on version == 0, a
 * nonexistent version, or a required relation absent from that version;
 * returns 0 for a valid-but-empty vector index. */
long dl_vector_search_version(dl_db *db, uint32_t version,
                              const uint32_t *q_int8,
                              int k, dl_vec_cb cb, void *user);

/* Corpus-parameterized forms.  `corpus` selects the liveness relation/col and
 * the vec relation (see the corpus descriptors above).  Semantics otherwise
 * identical to the entity wrappers. */
long dl_vector_search_corpus(dl_db *db, const struct dl_vec_corpus *corpus,
                             const uint32_t *q_int8, int k,
                             dl_vec_cb cb, void *user);
long dl_vector_search_corpus_version(dl_db *db, uint32_t version,
                                     const struct dl_vec_corpus *corpus,
                                     const uint32_t *q_int8, int k,
                                     dl_vec_cb cb, void *user);

/* Exact ranking of an EXPLICIT candidate set over the LIVE __vec_q__ relation.
 * q_int8 is the pre-quantized int8 query vector (VEC_IVEC_WORDS u32, 4 int8
 * packed little-endian).  cand_syms[0..n_cand-1] are entity sym-ids.  For each
 * candidate present in __vec_q__ the stored int8 vector is scored (integer dot
 * + integer norm) and the set is ranked by exact cosine order via a signed
 * int64 cross-multiply (integer isqrt for |v|) — no float, no sqrtf, no
 * overflow.  Liveness is the CALLER's concern here (unlike the search forms).
 * Emits at most k via cb, highest cosine first.
 * Returns the number emitted, or -1 on error (NULL db/q_int8/cand_syms/cb,
 * n_cand <= 0, k <= 0, OOM, or __vec_q__ missing). */
long dl_vector_rerank(dl_db *db, const uint32_t *q_int8,
                      const uint32_t *cand_syms, int n_cand,
                      int k, dl_vec_cb cb, void *user);

/* Corpus-parameterized rerank: loads stored vectors from corpus->vec_rel
 * instead of the hardcoded "__vec_q__".  Identical semantics otherwise. */
long dl_vector_rerank_corpus(dl_db *db, const struct dl_vec_corpus *corpus,
                             const uint32_t *q_int8,
                             const uint32_t *cand_syms, int n_cand,
                             int k, dl_vec_cb cb, void *user);

#ifdef __cplusplus
}
#endif

#endif /* VECTOR_H */
