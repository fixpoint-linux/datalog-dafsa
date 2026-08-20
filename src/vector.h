/*
 * vector.h — Vector tier (semantic half) query path (S2)
 *
 * Implements MIH (Multi-Index Hashing) candidate retrieval over the per-band
 * DAFSA postings __sig0__..__sig15__ plus integer int8 cosine re-ranking over
 * __vec_q__, for the LIVE relation store and for published snapshot VERSIONs.
 *
 * Storage layout (single source of truth — embed.py MUST emit exactly these
 * constants; see docs/datalog-dafsa-vector-storage-scope.md):
 *   - __sig{j}__  (arity-2):  band_value_u32 -> entity_sym_id, for j in 0..15.
 *   - __vec_q__   (arity-3):  entity_sym_id, chunk_idx, packed_4x_int8_u32
 *                             (96 chunks, little-endian pack4).
 *   - __itq_basis__(arity-3): dim_i, dim_j, float32_bits_u32 (not read here).
 *
 * LIVE vs VERSION read discipline (the load-bearing invariant, F1/F2):
 *   - dl_prefix / dl_lookup are LIVE-ONLY (dl.c) — they never route to the
 *     snapshot view.  Version reads MUST use dl_query_bound_version.
 *   - LIVE search reads BOTH sig_j and entity via dl_prefix.
 *   - VERSION search reads BOTH via dl_query_bound_version.
 *   dl_vector_search / dl_vector_search_version are a single structure with
 *   two modes differing ONLY in the read primitive — mirroring dl_search /
 *   dl_search_version in index.c — which keeps the invariant auditable.
 *
 * int8 re-rank (dl_vector_rerank) is pure integer arithmetic (F5): integer
 * dot + integer norm, ranked by exact cosine order.  No float, no sqrtf.
 */
#ifndef VECTOR_H
#define VECTOR_H

#include "dl.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Compile-time layout constants (shared verbatim with embed.py) ────── */

#define VEC_D          384    /* embedding dim (bge-small)                 */
#define VEC_C          256    /* ITQ bit-code length (c bits)              */
#define VEC_M          16     /* MIH bands == __sig0__..__sig15__          */
#define VEC_W          16     /* bits/band = ceil(VEC_C/VEC_M)             */
#define VEC_SIG_WORDS  (VEC_C / 32)  /* 8 u32, MSB-first                    */
#define VEC_IVEC_WORDS (VEC_D / 4)   /* 96 u32, 4 int8 packed each          */
#define VEC_ENTITY_REL "entity"      /* arity-2, name sym-id in col 0       */

/* ─── Callback ──────────────────────────────────────────────────────────── */

/* Callback for dl_vector_search / dl_vector_search_version / dl_vector_rerank.
 * entity_sym is the matched entity's name sym-id.  score semantics:
 *   - search/_version: number of bands (1..VEC_M) in which the entity matched
 *     a query variant within the pigeonhole budget (coarse relevance).
 *   - rerank: the signed integer dot product <q, v> (the int8 cosine
 *     numerator; the emitted order is exact cosine order).
 * Return non-zero to stop early. */
typedef int (*dl_vec_cb)(uint32_t entity_sym, int score, void *user);

/* ─── Query path ────────────────────────────────────────────────────────── */

/* MIH candidate retrieval over the LIVE relations.  q_sig is a PRE-ENCODED
 * c-bit signature (VEC_SIG_WORDS u32, MSB-first) — C never embeds.
 * k caps the number of candidates emitted; r is the Hamming radius
 * (pigeonhole budget per band is floor(r/VEC_M)).  Enumerates each query
 * band's variants within the budget, unions the per-band postings, drops
 * candidates whose entity is no longer live, sorts by band-match count
 * descending, and emits at most k via cb.
 * Returns the number emitted, or -1 on error (NULL db/q_sig/cb, k <= 0,
 * r < 0, or a __sig{j}__ / entity relation missing). */
long dl_vector_search(dl_db *db, const uint32_t *q_sig,
                      int k, int r, dl_vec_cb cb, void *user);

/* Same structure, but reads BOTH sig_j and entity as-of a published snapshot
 * `version` via dl_query_bound_version.
 * Error contract (mirrors dl_search_version): returns -1 on version == 0, a
 * nonexistent version, or a required relation absent from that version;
 * returns 0 for a valid-but-empty vector index. */
long dl_vector_search_version(dl_db *db, uint32_t version, const uint32_t *q_sig,
                              int k, int r, dl_vec_cb cb, void *user);

/* int8 cosine re-rank of a candidate set over the LIVE __vec_q__ relation.
 * q_int8 is the pre-quantized int8 query vector (VEC_IVEC_WORDS u32, 4 int8
 * packed little-endian).  cand_syms[0..n_cand-1] are entity sym-ids (from
 * dl_vector_search).  For each candidate the stored int8 vector is unpacked,
 * the integer dot <q,v> and norm |v| are computed, and the set is ranked by
 * exact cosine order via a SIGNED int64 cross-multiply dot_a*|b| vs
 * dot_b*|a| (integer isqrt for |v|) — no float, no sqrtf, no overflow.
 * Emits at most k via cb, highest cosine first.
 * Returns the number emitted, or -1 on error (NULL db/q_int8/cand_syms/cb,
 * n_cand <= 0, k <= 0, OOM, or __vec_q__ missing). */
long dl_vector_rerank(dl_db *db, const uint32_t *q_int8,
                      const uint32_t *cand_syms, int n_cand,
                      int k, dl_vec_cb cb, void *user);

#ifdef __cplusplus
}
#endif

#endif /* VECTOR_H */
