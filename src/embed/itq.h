/*
 * itq.h — ITQ basis fit + encode + int8 quantization (vector tier, S3 port
 * of scripts/embed.py's numpy math to dependency-free C++).
 *
 * Everything float64 here is OFFLINE / build-time only (the pipeline runs
 * once per corpus); the query-time hot path is float32 matmul + sign.
 * This file is pure C++ with C linkage so both the C++ dl-embed tool and C
 * tests can link it.  It deliberately has NO ggml dependency.
 */
#ifndef ITQ_H
#define ITQ_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── numpy-compatible MT19937 + polar gauss ──────────────────────────────
 * Reproduces np.random.seed(42); np.random.randn(...) exactly (legacy
 * RandomState: init_genrand + tempered MT19937, rk_double = (a>>5,b>>6)/2^53,
 * standard_normal = polar method with one cached value).  Verified against
 * numpy 2.5.2 in tests/test_embed_math.c. */
void   itq_rng_seed(uint32_t seed);
double itq_rng_gauss(void);

/* ── Linear algebra (row-major, float64) ───────────────────────────────── */

/* Cyclic Jacobi eigen-decomposition of a symmetric matrix.  A (n*n) is
 * OVERWRITTEN (diagonal becomes eigenvalues), V (n*n) receives eigenvectors
 * as COLUMNS, eig[n] the eigenvalues (UNORDERED — caller sorts).
 * Returns the number of sweeps used. */
int itq_jacobi_eigh(double *A, int n, double *V, double *eig, int max_sweeps);

/* Gram-Schmidt QR with one re-orthogonalization pass.  A (m*n) input,
 * Q (m*n) orthonormal columns, R (n*n) upper-triangular.  Returns 0, or -1
 * if rank-deficient. */
int itq_qr_gramschmidt(const double *A, int m, int n, double *Q, double *R);

/* One-sided (Hestenes) Jacobi SVD of a general m*n matrix M (m >= n).
 * U (m*n) left singular vectors as columns, S[n] singular values DESCENDING,
 * V (n*n) right singular vectors as columns, so that M = U diag(S) V^T.
 * Returns 0 on success, -1 on failure. */
int itq_svd_onesided(const double *M, int m, int n,
                     double *U, double *S, double *V);

/* Top-c eigenvectors (columns) of symmetric C (n*n) by eigenvalue
 * descending -> W (n*c) row-major.  (== np.linalg.svd(X).Vt[:c].T of X=chol
 * factor when C = X^T X.) */
void itq_top_c_eigenvectors(double *C, int n, int c, double *W);

/* ── ITQ pipeline (ports scripts/embed.py) ─────────────────────────────── */

/* fit_itq_basis: PCA via eigen of X^T X (no centering — embeddings are
 * unit-norm and encode projects the same uncentered vectors), then 50
 * ITQ rotation iterations seeded with MT19937(42) randn QR (same stream as
 * embed.py).  X is (n*d) float32 row-major, B out is (d*c) float32
 * row-major.  Deterministic.  Requires n >= c for a full-rank basis. */
void itq_fit_basis(const float *X, int n, int d, int c, float *B);

/* itq_encode: project v (d floats) on B (d*c) and pack the sign bits
 * MSB-first into sig (c/32 u32 words).  (embed.py itq_encode; y>=0 -> 1.) */
void itq_encode(const float *v, const float *B, int d, int c, uint32_t *sig);

/* GLOBAL corpus int8 scale: max over rows of max|v|. (embed.py
 * corpus_global_scale.) */
float itq_corpus_global_scale(const float *X, int n, int d);

/* Quantize with the GLOBAL scale, matching numpy float32 semantics:
 * scaled = (float32)(v / gscale) * 127.0f, clip to [-127,127], then
 * TRUNCATE toward zero (numpy .astype(np.int8)). */
void itq_quantize_int8_global(const float *v, int d, float gscale, int8_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ITQ_H */
