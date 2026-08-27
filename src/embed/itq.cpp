/*
 * itq.cpp — dependency-free port of scripts/embed.py's ITQ math.
 *
 * Ported pieces (byte-identical semantics where bit-exactness is possible):
 *   - fit_itq_basis : PCA (top-c eigenvectors of X^T X via cyclic Jacobi,
 *                     the numpy svd replacement) + 50 ITQ iterations with
 *                     R := V U^T from a one-sided-Jacobi SVD of
 *                     B^T X_pca; R init = QR of MT19937(42) randn (the SAME
 *                     stream numpy's legacy RandomState produces).
 *   - itq_encode    : y = v @ B, sign bits, MSB-first u32 packing.
 *   - quantize_int8_global / corpus_global_scale : global-scale int8
 *                     quantization with numpy float32 + truncate semantics.
 *
 * Matrix convention: ROW-MAJOR throughout (A[i*ncols + j]).
 *
 * NOTE on bit-parity with embed.py: the MT19937 randn stream is identical
 * (verified), and all arithmetic is float64 like numpy's LAPACK path, but
 * the SIGN of each eigenvector / singular vector is mathematically
 * arbitrary and Jacobi chooses different signs than LAPACK.  The fitted
 * basis is therefore an equally-good rotation of the same PCA subspace,
 * not the bit-identical matrix; correctness is gated by the S4 oracle
 * (int8-vs-float recall >= 99%), not by a CSV diff.
 */
#include "itq.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

/* ── numpy legacy RandomState: MT19937 + polar gauss ───────────────────── */

struct itq_rng {
    uint32_t mt[624];
    int      mti;       /* index into mt; == 624 means "generate next block" */
    int      has_gauss; /* cached second polar value present? */
    double   gauss;
};

static struct itq_rng g_rng;

void itq_rng_seed(uint32_t seed) {
    g_rng.mt[0] = seed;
    for (int i = 1; i < 624; i++)
        g_rng.mt[i] = (uint32_t)1812433253u *
                      (g_rng.mt[i - 1] ^ (g_rng.mt[i - 1] >> 30)) + (uint32_t)i;
    g_rng.mti = 624;
    g_rng.has_gauss = 0;
    g_rng.gauss = 0.0;
}

static uint32_t itq_rng_int32(void) {
    if (g_rng.mti >= 624) {
        /* twist */
        static const uint32_t MATRIX_A[2] = {0u, 0x9908B0DFu};
        static const uint32_t UPPER = 0x80000000u, LOWER = 0x7FFFFFFFu;
        for (int i = 0; i < 624; i++) {
            uint32_t y = (g_rng.mt[i] & UPPER) | (g_rng.mt[(i + 1) % 624] & LOWER);
            g_rng.mt[i] = g_rng.mt[(i + 397) % 624] ^ (y >> 1) ^ MATRIX_A[y & 1u];
        }
        g_rng.mti = 0;
    }
    uint32_t y = g_rng.mt[g_rng.mti++];
    y ^= (y >> 11);
    y ^= (y << 7) & 0x9D2C5680u;
    y ^= (y << 15) & 0xEFC60000u;
    y ^= (y >> 18);
    return y;
}

/* numpy rk_double: 53-bit uniform in [0,1). */
static double itq_rng_double(void) {
    uint32_t a = itq_rng_int32() >> 5;
    uint32_t b = itq_rng_int32() >> 6;
    return (a * 67108864.0 + b) * (1.0 / 9007199254740992.0);
}

/* numpy rk_gauss: Marsaglia polar method with one cached value. */
double itq_rng_gauss(void) {
    if (g_rng.has_gauss) {
        g_rng.has_gauss = 0;
        return g_rng.gauss;
    }
    double x1, x2, r2;
    do {
        x1 = 2.0 * itq_rng_double() - 1.0;
        x2 = 2.0 * itq_rng_double() - 1.0;
        r2 = x1 * x1 + x2 * x2;
    } while (r2 >= 1.0 || r2 == 0.0);
    double f = std::sqrt(-2.0 * std::log(r2) / r2);
    g_rng.gauss = f * x1;
    g_rng.has_gauss = 1;
    return f * x2;
}

/* ── Cyclic Jacobi symmetric eigen-decomposition (Golub & Van Loan 8.4.2) */

int itq_jacobi_eigh(double *A, int n, double *V, double *eig, int max_sweeps) {
    int sweep, p, q, changed = 1;
    for (int i = 0; i < n * n; i++) V[i] = 0.0;
    for (int i = 0; i < n; i++) V[i * n + i] = 1.0;

    for (sweep = 0; sweep < max_sweeps && changed; sweep++) {
        double off = 0.0;
        for (p = 0; p < n; p++)
            for (q = p + 1; q < n; q++) off += A[p * n + q] * A[p * n + q];
        if (off < 1e-30) break;
        double thresh = (sweep < 4) ? 0.2 * off / ((double)n * n) : 0.0;
        changed = 0;

        for (p = 0; p < n - 1; p++) {
            for (q = p + 1; q < n; q++) {
                double apq = A[p * n + q];
                double aqq = A[q * n + q];
                double app = A[p * n + p];
                if (std::fabs(apq) < thresh && sweep >= 4) continue;
                if (std::fabs(apq) < 1e-300) continue;
                changed = 1;
                double a = (aqq - app) / (2.0 * apq);
                double t = (a >= 0 ? 1.0 : -1.0) / (std::fabs(a) + std::sqrt(a * a + 1.0));
                double c = 1.0 / std::sqrt(t * t + 1.0);
                double s = t * c;
                for (int k = 0; k < n; k++) {
                    double akp = A[k * n + p], akq = A[k * n + q];
                    A[k * n + p] = c * akp - s * akq;
                    A[k * n + q] = s * akp + c * akq;
                }
                for (int k = 0; k < n; k++) {
                    double apk = A[p * n + k], aqk = A[q * n + k];
                    A[p * n + k] = c * apk - s * aqk;
                    A[q * n + k] = s * apk + c * aqk;
                }
                for (int k = 0; k < n; k++) {
                    double vkp = V[k * n + p], vkq = V[k * n + q];
                    V[k * n + p] = c * vkp - s * vkq;
                    V[k * n + q] = s * vkp + c * vkq;
                }
            }
        }
    }
    for (int i = 0; i < n; i++) eig[i] = A[i * n + i];
    return sweep;
}

/* ── Gram-Schmidt QR with one re-orthogonalization pass ────────────────── */

int itq_qr_gramschmidt(const double *A, int m, int n, double *Q, double *R) {
    std::memset(R, 0, sizeof(double) * n * n);
    std::memcpy(Q, A, sizeof(double) * m * n);
    for (int j = 0; j < n; j++) {
        for (int pass = 0; pass < 2; pass++) {
            for (int i = 0; i < j; i++) {
                double d = 0.0;
                for (int k = 0; k < m; k++) d += Q[k * n + i] * Q[k * n + j];
                for (int k = 0; k < m; k++) Q[k * n + j] -= d * Q[k * n + i];
            }
        }
        double nrm = 0.0;
        for (int k = 0; k < m; k++) nrm += Q[k * n + j] * Q[k * n + j];
        nrm = std::sqrt(nrm);
        if (nrm < 1e-12) return -1;   /* rank deficient */
        for (int k = 0; k < m; k++) Q[k * n + j] /= nrm;
        for (int i = 0; i <= j; i++) {
            double d = 0.0;
            for (int k = 0; k < m; k++) d += Q[k * n + i] * A[k * n + j];
            R[i * n + j] = d;
        }
    }
    return 0;
}

/* ── One-sided (Hestenes) Jacobi SVD ─────────────────────────────────────
 * Orthogonalizes the COLUMNS of a copy W of M by Givens rotations applied
 * on the right; V accumulates the rotations.  On convergence W = U diag(S):
 * U_j = W_j / S_j with S_j = |W_j|.  Singular values are then sorted
 * descending (numpy convention) with U/V columns permuted to match.       */

int itq_svd_onesided(const double *M, int m, int n,
                     double *U, double *S, double *V) {
    double *W = (double *)std::malloc(sizeof(double) * m * n);
    if (!W) return -1;

    std::memcpy(W, M, sizeof(double) * m * n);
    std::memset(V, 0, sizeof(double) * n * n);
    for (int i = 0; i < n; i++) V[i * n + i] = 1.0;

    /* One-sided Jacobi converges quadratically; for the 256x256 ITQ update a
     * relative tolerance of 1e-10 is far tighter than needed (feeds int8
     * quantization) and avoids grinding through the full 60-sweep cap. */
    const double tol = 1e-10;
    for (int sweep = 0; sweep < 60; sweep++) {
        int changed = 0;
        for (int p = 0; p < n - 1; p++) {
            for (int q = p + 1; q < n; q++) {
                double alpha = 0.0, beta = 0.0, gamma = 0.0;
                for (int k = 0; k < m; k++) {
                    double wp = W[k * n + p], wq = W[k * n + q];
                    alpha += wp * wq;
                    beta  += wp * wp;
                    gamma += wq * wq;
                }
                if (beta <= 0.0 || gamma <= 0.0) continue;
                if (std::fabs(alpha) <= tol * std::sqrt(beta * gamma)) continue;
                changed = 1;
                /* zeroes W_p . W_q under the update J = [[c,s],[-s,c]]:
                 * matches the symmetric prototype's a = (aqq-app)/(2*apq) */
                double zeta = (gamma - beta) / (2.0 * alpha);
                double t    = (zeta >= 0 ? 1.0 : -1.0) /
                              (std::fabs(zeta) + std::sqrt(1.0 + zeta * zeta));
                double c = 1.0 / std::sqrt(1.0 + t * t);
                double s = c * t;
                for (int k = 0; k < m; k++) {
                    double wp = W[k * n + p], wq = W[k * n + q];
                    W[k * n + p] = c * wp - s * wq;
                    W[k * n + q] = s * wp + c * wq;
                }
                for (int k = 0; k < n; k++) {
                    double vp = V[k * n + p], vq = V[k * n + q];
                    V[k * n + p] = c * vp - s * vq;
                    V[k * n + q] = s * vp + c * vq;
                }
            }
        }
        if (!changed) break;
    }

    /* singular values + U; then selection-sort descending (n <= 256). */
    for (int j = 0; j < n; j++) {
        double nrm = 0.0;
        for (int k = 0; k < m; k++) nrm += W[k * n + j] * W[k * n + j];
        S[j] = std::sqrt(nrm);
    }
    for (int i = 0; i < n; i++) {
        int bi = i;
        for (int j = i + 1; j < n; j++) if (S[j] > S[bi]) bi = j;
        if (bi != i) {
            double ts = S[i]; S[i] = S[bi]; S[bi] = ts;
            for (int k = 0; k < m; k++) {
                double tw = W[k * n + i]; W[k * n + i] = W[k * n + bi]; W[k * n + bi] = tw;
            }
            for (int k = 0; k < n; k++) {
                double tv = V[k * n + i]; V[k * n + i] = V[k * n + bi]; V[k * n + bi] = tv;
            }
        }
    }
    for (int j = 0; j < n; j++) {
        double s = S[j] > 1e-300 ? S[j] : 1.0;
        for (int k = 0; k < m; k++) U[k * n + j] = W[k * n + j] / s;
    }
    std::free(W);
    return 0;
}

/* ── Top-c eigenvectors of symmetric C (columns of V), eigenvalue desc ─── */

void itq_top_c_eigenvectors(double *C, int n, int c, double *W) {
    double *V   = (double *)std::malloc(sizeof(double) * n * n);
    double *eig = (double *)std::malloc(sizeof(double) * n);
    if (!V || !eig) { std::free(V); std::free(eig); return; }
    itq_jacobi_eigh(C, n, V, eig, 100);
    for (int i = 0; i < n; i++) {              /* selection sort desc */
        int bi = i;
        for (int j = i + 1; j < n; j++) if (eig[j] > eig[bi]) bi = j;
        if (bi != i) {
            double t = eig[i]; eig[i] = eig[bi]; eig[bi] = t;
            for (int k = 0; k < n; k++) {
                double v = V[k * n + i]; V[k * n + i] = V[k * n + bi]; V[k * n + bi] = v;
            }
        }
    }
    for (int j = 0; j < c; j++)
        for (int k = 0; k < n; k++)
            W[k * c + j] = V[k * n + j];
    std::free(V);
    std::free(eig);
}

/* ── ITQ fit (port of embed.py fit_itq_basis) ──────────────────────────── */

void itq_fit_basis(const float *X, int n, int d, int c, float *B) {
    /* 1. C = X^T X (d x d), float64. */
    double *C = (double *)std::calloc((size_t)d * d, sizeof(double));
    /* 2. W_pca (d x c) = top-c eigenvectors of C. */
    double *W = (double *)std::malloc(sizeof(double) * d * c);
    /* 3. X_pca (n x c) = X @ W_pca. */
    double *Xp = (double *)std::malloc(sizeof(double) * n * c);
    /* 4. R (c x c). */
    double *R  = (double *)std::malloc(sizeof(double) * c * c);
    double *R0 = (double *)std::malloc(sizeof(double) * c * c);
    double *QR = (double *)std::malloc(sizeof(double) * c * c);
    double *RR = (double *)std::malloc(sizeof(double) * c * c);
    /* per-iteration buffers */
    double *Z  = (double *)std::malloc(sizeof(double) * n * c);   /* X_pca @ R */
    double *Bs = (double *)std::malloc(sizeof(double) * n * c);   /* sign matrix */
    double *Mm = (double *)std::malloc(sizeof(double) * c * c);   /* Bs^T X_pca */
    double *U  = (double *)std::malloc(sizeof(double) * c * c);
    double *Sv = (double *)std::malloc(sizeof(double) * c);
    double *Vv = (double *)std::malloc(sizeof(double) * c * c);
    if (!C || !W || !Xp || !R || !R0 || !QR || !RR || !Z || !Bs || !Mm ||
        !U || !Sv || !Vv) {
        std::free(C); std::free(W); std::free(Xp); std::free(R); std::free(R0);
        std::free(QR); std::free(RR); std::free(Z); std::free(Bs); std::free(Mm);
        std::free(U); std::free(Sv); std::free(Vv);
        return;
    }

    for (int k = 0; k < n; k++)
        for (int i = 0; i < d; i++) {
            double x = (double)X[k * d + i];
            for (int j = i; j < d; j++)
                C[i * d + j] += x * (double)X[k * d + j];
        }
    for (int i = 0; i < d; i++)
        for (int j = 0; j < i; j++) C[i * d + j] = C[j * d + i];

    itq_top_c_eigenvectors(C, d, c, W);          /* C destroyed */

    for (int k = 0; k < n; k++)
        for (int j = 0; j < c; j++) {
            double s = 0.0;
            for (int i = 0; i < d; i++) s += (double)X[k * d + i] * W[i * c + j];
            Xp[k * c + j] = s;
        }

    /* R init: MT19937(42) randn(c,c), then QR.  Same stream as embed.py. */
    itq_rng_seed(42u);
    for (int i = 0; i < c * c; i++) R0[i] = itq_rng_gauss();
    itq_qr_gramschmidt(R0, c, c, QR, RR);
    std::memcpy(R, QR, sizeof(double) * c * c);

    /* ITQ maximizes sum(singular values of Bs^T X_pca)^2 — i.e. tr((Bs^T Xp)^T
     * (Bs^T Xp)) = tr(Bs^T Xp Xp^T Bs).  Sum of squared singular values from
     * the SVD IS that objective; when it stops improving we've converged and
     * can stop far before the 50-iteration cap (which is just an upper bound). */
    double prev_obj = -1.0;
    for (int it = 0; it < 50; it++) {
        /* Z = X_pca @ R ; Bs = sign(Z) (zeros -> +1, matching np.where). */
        for (int k = 0; k < n; k++)
            for (int j = 0; j < c; j++) {
                double s = 0.0;
                for (int i = 0; i < c; i++) s += Xp[k * c + i] * R[i * c + j];
                Z[k * c + j] = s;
                Bs[k * c + j] = (s >= 0.0) ? 1.0 : -1.0;
            }
        /* M = Bs^T @ X_pca. */
        for (int i = 0; i < c; i++)
            for (int j = 0; j < c; j++) {
                double s = 0.0;
                for (int k = 0; k < n; k++) s += Bs[k * c + i] * Xp[k * c + j];
                Mm[i * c + j] = s;
            }
        /* SVD(M) = U S V^T ; R = V @ U^T. */
        itq_svd_onesided(Mm, c, c, U, Sv, Vv);
        double obj = 0.0;
        for (int j = 0; j < c; j++) obj += Sv[j] * Sv[j];
        /* converged once the objective stops increasing: stop when the gain
         * is < 0.1% relative (measured: the ITQ objective plateaus to ~1e-3
         * relative gain by ~iteration 12 on real embeddings, vs the 50-cap). */
        if (prev_obj >= 0.0 && obj <= prev_obj * 1.001) break;
        prev_obj = obj;
        for (int i = 0; i < c; i++)
            for (int j = 0; j < c; j++) {
                double s = 0.0;
                for (int k = 0; k < c; k++) s += Vv[i * c + k] * U[j * c + k];
                R[i * c + j] = s;
            }
    }

    /* B = W_pca @ R -> float32. */
    for (int i = 0; i < d; i++)
        for (int j = 0; j < c; j++) {
            double s = 0.0;
            for (int k = 0; k < c; k++) s += W[i * c + k] * R[k * c + j];
            B[i * c + j] = (float)s;
        }

    std::free(C); std::free(W); std::free(Xp); std::free(R); std::free(R0);
    std::free(QR); std::free(RR); std::free(Z); std::free(Bs); std::free(Mm);
    std::free(U); std::free(Sv); std::free(Vv);
}

/* ── Encode + quantize (hot path, float32) ─────────────────────────────── */

void itq_encode(const float *v, const float *B, int d, int c, uint32_t *sig) {
    int words = c / 32;
    for (int w = 0; w < words; w++) {
        uint32_t word = 0;
        for (int b = 0; b < 32; b++) {
            int j = w * 32 + b;
            double s = 0.0;
            for (int i = 0; i < d; i++) s += (double)v[i] * (double)B[i * c + j];
            word = (word << 1) | (s >= 0.0 ? 1u : 0u);
        }
        sig[w] = word;
    }
}

float itq_corpus_global_scale(const float *X, int n, int d) {
    float g = 0.0f;
    for (int k = 0; k < n; k++)
        for (int i = 0; i < d; i++) {
            float a = std::fabs(X[k * d + i]);
            if (a > g) g = a;
        }
    return g;
}

void itq_quantize_int8_global(const float *v, int d, float gscale, int8_t *out) {
    float gs = (gscale < 1e-12f) ? 1e-12f : gscale;
    for (int i = 0; i < d; i++) {
        float scaled = (v[i] / gs) * 127.0f;
        if (scaled > 127.0f) scaled = 127.0f;
        if (scaled < -127.0f) scaled = -127.0f;
        /* numpy .astype(np.int8) truncates toward zero, as does C's cast. */
        out[i] = (int8_t)scaled;
    }
}
