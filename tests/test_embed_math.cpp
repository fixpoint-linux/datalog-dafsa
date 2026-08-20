/*
 * test_embed_math.cpp — offline verification of the dl-embed math core
 * (src/embed/itq.cpp) and WordPiece tokenizer (src/embed/tokenizer.cpp).
 *
 * No ggml / no model needed: this builds with plain g++ and runs anywhere.
 * Goldens were generated with the project venv numpy (2.5.2) — see
 * handoff-ggml-c-embed.  In particular the MT19937 stream goldens pin the
 * ITQ R-init to numpy's legacy RandomState, and the SVD/eigen goldens pin
 * the Jacobi implementations against LAPACK.
 *
 * Build (also wired into the Makefile):
 *   g++ -O2 -Wall -Wextra -Werror -std=c++17 \
 *       tests/test_embed_math.cpp src/embed/itq.cpp src/embed/tokenizer.cpp \
 *       -o tests/test_embed_math -lm
 */
#include "../src/embed/itq.h"
#include "../src/embed/tokenizer.h"
#include "../src/embed/vec_bits.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int tests_run = 0, tests_failed = 0;
#define CHECK(cond, msg) do { \
    tests_run++; \
    if (!(cond)) { printf("FAIL: %s\n", msg); tests_failed++; } \
} while (0)

static int near_d(double a, double b, double tol) { return std::fabs(a - b) < tol; }

/* ── 1. MT19937 + polar gauss parity with numpy ─────────────────────────── */
static void t_rng(void) {
    itq_rng_seed(42u);
    static const double gold8[8] = {
        0.49671415301123267, -0.13826430117118466, 0.64768853810069249,
        1.5230298564080254, -0.23415337472333597, -0.23413695694918055,
        1.5792128155073915, 0.76743472915290878
    };
    int ok = 1;
    for (int i = 0; i < 8; i++) {
        double g = itq_rng_gauss();
        if (!near_d(g, gold8[i], 1e-12)) { ok = 0;
            printf("  rng mismatch at %d: %.17g vs %.17g\n", i, g, gold8[i]); }
    }
    CHECK(ok, "MT19937 randn first-8 mismatch vs numpy");

    /* full 256x256 stream (65536 draws) — sum + last element goldens */
    itq_rng_seed(42u);
    double sum = 0.0, last = 0.0;
    for (int i = 0; i < 256 * 256; i++) last = itq_rng_gauss(), sum += last;
    CHECK(near_d(sum, 125.63392254307826, 1e-9), "randn(256x256) sum mismatch");
    CHECK(near_d(last, 0.31540017059692155, 1e-15), "randn(256x256) last mismatch");
}

/* ── shared LCG matrix (identical formula to the python generator) ─────── */
static uint32_t lcg_state;
static double lcg(void) {
    lcg_state = lcg_state * 1664525u + 1013904223u;
    return ((lcg_state >> 8) / (double)0xFFFFFF) - 0.5;
}

/* ── 2. Jacobi eigen ────────────────────────────────────────────────────── */
static void t_jacobi(void) {
    {   /* A = [2 1; 1 2] -> eig {3, 1} */
        double A[4] = {2, 1, 1, 2}, A0[4] = {2, 1, 1, 2}, V[4], eig[2];
        itq_jacobi_eigh(A, 2, V, eig, 100);
        int ok = (near_d(eig[0], 3.0, 1e-10) && near_d(eig[1], 1.0, 1e-10)) ||
                 (near_d(eig[0], 1.0, 1e-10) && near_d(eig[1], 3.0, 1e-10));
        for (int i = 0; i < 2; i++)
            for (int j = 0; j < 2; j++) {
                double s = 0;
                for (int k = 0; k < 2; k++) s += V[i * 2 + k] * eig[k] * V[j * 2 + k];
                ok = ok && near_d(s, A0[i * 2 + j], 1e-10);
            }
        CHECK(ok, "jacobi 2x2 eigen reconstruction");
    }
    {   /* 64x64 PSD, eigenvalues vs LAPACK goldens */
        lcg_state = 0xDEADBEEFu;
        int n = 64;
        std::vector<double> M(n * n), C(n * n);
        for (int i = 0; i < n * n; i++) M[i] = lcg();
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++) {
                double s = 0;
                for (int k = 0; k < n; k++) s += M[k * n + i] * M[k * n + j];
                C[i * n + j] = s;
            }
        std::vector<double> W(n * 32);
        itq_top_c_eigenvectors(C.data(), n, 32, W.data());
        /* C destroyed; recompute the eigen goldens check via sorted diag:
         * top_c_eigenvectors sorted desc internally — verify via W^T C W. */
        /* simpler: recompute C, project: eig_j = |C w_j| consistency and the
         * known LAPACK top-8 values. */
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++) {
                double s = 0;
                for (int k = 0; k < n; k++) s += M[k * n + i] * M[k * n + j];
                C[i * n + j] = s;
            }
        static const double gold[8] = {
            20.852374902511688, 19.107080234738383, 16.709304209738633,
            15.661659993548223, 14.306503209041633, 14.136760260459377,
            13.236726694915085, 12.790487832473653
        };
        int ok = 1;
        for (int j = 0; j < 8; j++) {
            /* lambda_j = w_j^T C w_j */
            double lam = 0;
            for (int i = 0; i < n; i++)
                for (int k = 0; k < n; k++)
                    lam += W[i * 32 + j] * C[i * n + k] * W[k * 32 + j];
            if (!near_d(lam, gold[j], 1e-8)) { ok = 0;
                printf("  eig %d: %.17g vs %.17g\n", j, lam, gold[j]); }
        }
        CHECK(ok, "jacobi 64x64 top eigenvectors vs LAPACK");
    }
}

/* ── 3. Gram-Schmidt QR ─────────────────────────────────────────────────── */
static void t_qr(void) {
    lcg_state = 0xDEADBEEFu;
    int n = 64;
    std::vector<double> A(n * n), Q(n * n), R(n * n);
    for (int i = 0; i < n * n; i++) A[i] = lcg();
    int rc = itq_qr_gramschmidt(A.data(), n, n, Q.data(), R.data());
    double maxerr = 0;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            double s = 0;
            for (int k = 0; k < n; k++) s += Q[k * n + i] * Q[k * n + j];
            double e = std::fabs(s - (i == j ? 1.0 : 0.0));
            if (e > maxerr) maxerr = e;
        }
    CHECK(rc == 0 && maxerr < 1e-10, "qr_gramschmidt 64x64 orthonormality");
    /* Q R == A */
    double rec = 0;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            double s = 0;
            for (int k = 0; k < n; k++) s += Q[i * n + k] * R[k * n + j];
            double e = std::fabs(s - A[i * n + j]);
            if (e > rec) rec = e;
        }
    CHECK(rec < 1e-10, "qr_gramschmidt Q*R == A");
}

/* ── 4. One-sided Jacobi SVD ────────────────────────────────────────────── */
static void t_svd(void) {
    lcg_state = 0xDEADBEEFu;
    int m = 64, n = 64;
    std::vector<double> M(m * n), U(m * n), S(n), V(n * n);
    for (int i = 0; i < m * n; i++) M[i] = lcg();
    int rc = itq_svd_onesided(M.data(), m, n, U.data(), S.data(), V.data());
    CHECK(rc == 0, "svd_onesided rc");

    static const double gold[8] = {
        4.566440068862363, 4.3711646313926913, 4.0877015803185355,
        3.957481521567503, 3.7823938463678828, 3.7598883308496482,
        3.6382312591306079, 3.5763791511071137
    };
    int ok = 1;
    for (int j = 0; j < 8; j++)
        if (!near_d(S[j], gold[j], 1e-9)) { ok = 0;
            printf("  svd %d: %.17g vs %.17g\n", j, S[j], gold[j]); }
    CHECK(ok, "svd_onesided singular values vs LAPACK (desc order)");

    /* U, V orthonormal */
    double err = 0;
    for (int a = 0; a < n; a++)
        for (int b = 0; b < n; b++) {
            double su = 0, sv = 0;
            for (int k = 0; k < m; k++) su += U[k * n + a] * U[k * n + b];
            for (int k = 0; k < n; k++) sv += V[k * n + a] * V[k * n + b];
            err = std::max(err, std::fabs(su - (a == b ? 1.0 : 0.0)));
            err = std::max(err, std::fabs(sv - (a == b ? 1.0 : 0.0)));
        }
    CHECK(err < 1e-10, "svd_onesided U,V orthonormal");

    /* M == U diag(S) V^T */
    double rec = 0;
    for (int i = 0; i < m; i++)
        for (int j = 0; j < n; j++) {
            double s = 0;
            for (int k = 0; k < n; k++) s += U[i * n + k] * S[k] * V[j * n + k];
            rec = std::max(rec, std::fabs(s - M[i * n + j]));
        }
    CHECK(rec < 1e-9, "svd_onesided reconstruction M == U S V^T");
}

/* ── 5. ITQ fit determinism + orthonormal basis ─────────────────────────── */
static void t_fit(void) {
    int n = 128, d = 48, c = 32;
    lcg_state = 777u;
    std::vector<float> X(n * d);
    for (int i = 0; i < n * d; i++) {
        double v = lcg() * 3.0;               /* spread for full-rank */
        X[i] = (float)v;
    }
    std::vector<float> B1(d * c), B2(d * c);
    itq_fit_basis(X.data(), n, d, c, B1.data());
    itq_fit_basis(X.data(), n, d, c, B2.data());
    int same = 1;
    for (int i = 0; i < d * c; i++)
        if (std::memcmp(&B1[i], &B2[i], sizeof(float)) != 0) { same = 0; break; }
    CHECK(same, "itq_fit_basis bit-deterministic");

    /* B^T B == I (W_pca orthonormal cols, R orthogonal) */
    double err = 0;
    for (int a = 0; a < c; a++)
        for (int b = 0; b < c; b++) {
            double s = 0;
            for (int k = 0; k < d; k++) s += (double)B1[k * c + a] * B1[k * c + b];
            err = std::max(err, std::fabs(s - (a == b ? 1.0 : 0.0)));
        }
    CHECK(err < 1e-3, "itq_fit_basis B^T B == I (float32 precision)");
}

/* ── 6. encode + band round-trip ────────────────────────────────────────── */
static void t_encode(void) {
    int d = 8, c = 32;                          /* small: 32-bit code = 1 word */
    std::vector<float> v(d), B(d * c, 0.0f);
    for (int i = 0; i < d; i++) v[i] = (float)(i + 1) * 0.125f;
    for (int j = 0; j < c; j++) B[(j % d) * c + j] = 1.0f;   /* permutation-ish */
    uint32_t sig[1];
    itq_encode(v.data(), B.data(), d, c, sig);
    /* v projected on basis columns: y_j = v[j % d] > 0 -> 1 for all j */
    CHECK(sig[0] == 0xFFFFFFFFu, "itq_encode all-positive projection bits");
    /* negative variant */
    for (int i = 0; i < d; i++) v[i] = -(float)(i + 1) * 0.125f;
    itq_encode(v.data(), B.data(), d, c, sig);
    CHECK(sig[0] == 0x00000000u, "itq_encode all-negative projection bits");
    /* mixed: v[0]>0 -> bit31=1; v[1]<0 -> bit30=0; v[2..]=0 -> 1; the basis
     * columns cycle every d=8, so the byte pattern 10111111 repeats:
     * 0xBFBFBFBF (MSB-first order, y>=0 -> 1) */
    v[0] = 1.0f; v[1] = -1.0f;
    for (int i = 2; i < d; i++) v[i] = 0.0f;
    itq_encode(v.data(), B.data(), d, c, sig);
    CHECK(sig[0] == 0xBFBFBFBFu, "itq_encode MSB-first bit order");

    /* band round-trip via vec_bits (byte-identity with vector.c formula) */
    uint32_t s8[8];
    for (int w = 0; w < 8; w++) s8[w] = (uint32_t)(0x01234567u * (w + 1));
    uint32_t recon[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    for (int j = 0; j < 16; j++) vec_band_set(recon, j, vec_band_slice(s8, j));
    CHECK(std::memcmp(s8, recon, sizeof s8) == 0, "vec band_slice/band_set round-trip");
    CHECK(vec_band_slice(s8, 0) == (s8[0] >> 16) && vec_band_slice(s8, 1) == (s8[0] & 0xFFFF),
          "vec band 0/1 = high/low half of sig[0]");
}

/* ── 7. quantization semantics ──────────────────────────────────────────── */
static void t_quant(void) {
    float v[6] = {0.5f, -0.5f, 0.004f, -0.004f, 2.0f, -2.0f};
    int8_t q[6];
    itq_quantize_int8_global(v, 6, 1.0f, q);
    /* v/1*127 = 63.5 -> 63 (truncate); -63.5 -> -63; 0.508->0; clip 127 */
    CHECK(q[0] == 63 && q[1] == -63, "quantize truncation toward zero");
    CHECK(q[2] == 0 && q[3] == 0, "quantize small -> 0 (truncate)");
    CHECK(q[4] == 127 && q[5] == -127, "quantize clip at +/-127");
    /* pack4 little-endian golden */
    CHECK(vec_pack4_le(1, 2, 3, 4) == 0x04030201u, "pack4_le golden");
    CHECK(vec_pack4_le(-1, -2, -3, -4) == 0xFCFDFEFFu, "pack4_le negative golden");
    int8_t un[4];
    vec_unpack4_le(0xFCFDFEFFu, un);
    CHECK(un[0] == -1 && un[1] == -2 && un[2] == -3 && un[3] == -4, "unpack4 round-trip");
    CHECK(vec_float32_bits(1.0f) == 0x3F800000u, "float32 bits golden 1.0f");
    CHECK(vec_float32_bits(-0.5f) == 0xBF000000u, "float32 bits golden -0.5f");
    CHECK(vec_bits_float32(0x3F800000u) == 1.0f, "bits float32 round-trip");
    /* corpus scale */
    float X[4] = {-3.0f, 0.5f, 2.9f, -2.95f};
    CHECK(itq_corpus_global_scale(X, 1, 4) == 3.0f, "corpus_global_scale = max|v|");
}

/* ── 8. tokenizer ───────────────────────────────────────────────────────── */
static void t_tokenizer(void) {
    /* mini vocab (wp_init sorts internally) */
    static const char *texts[] = {
        "##!", "##ing", "##s", "[CLS]", "[PAD]", "[SEP]", "[UNK]",
        "cafe", "hello", "play", "world", "zzzzqq", ",", "!"
    };
    static const int32_t types[] = {1,1,1, 0,0,0,0, 0,0,0,0,0, 0,0};
    wp_vocab v;
    v.texts = texts; v.types = types; v.n = 14;
    CHECK(wp_init(&v) == 0, "wp_init");
    CHECK(wp_find_exact(&v, "[CLS]") == 3 && wp_find_exact(&v, "hello") == 8,
          "wp_find_exact");

    int ids[64];
    int n = wp_tokenize(&v, "Playing worlds!", 3, 5, 6, ids, 64);
    /* [CLS] play(9) ##ing(1) world(10) ##s(2) !(13) [SEP](5) */
    int expect1[] = {3, 9, 1, 10, 2, 13, 5};
    int ok = (n == 7);
    for (int i = 0; i < n && i < 7; i++) if (ids[i] != expect1[i]) ok = 0;
    CHECK(ok, "wordpiece greedy longest-match + ## continuation");

    n = wp_tokenize(&v, "qwzxv", 3, 5, 6, ids, 64);
    CHECK(n == 3 && ids[0] == 3 && ids[1] == 6 && ids[2] == 5,
          "single [UNK] per word (HF semantics)");

    /* basic tokenizer preprocessing */
    char out[512];
    wp_basic_tokenize("Hello, World!", out, sizeof out);
    CHECK(std::string(out) == "hello , world !", "basic: lower + punct split");
    wp_basic_tokenize("foo_bar baz", out, sizeof out);
    CHECK(std::string(out) == "foo _ bar baz", "basic: underscore is punctuation");
    wp_basic_tokenize("a\tb" "\001" "c", out, sizeof out);
    /* HF _clean_text DROPS control chars without inserting a separator:
     * "b\x01c" glues to "bc" — verified against transformers. */
    CHECK(std::string(out) == "a bc", "basic: whitespace fold + control strip");
    wp_basic_tokenize("Caf\u00e9 NA\u00cfVE", out, sizeof out);
    CHECK(std::string(out) == "cafe naive", "basic: accent strip");
    wp_basic_tokenize("x\u4e2dy", out, sizeof out);
    CHECK(std::string(out) == "x \xe4\xb8\xad y", "basic: CJK space padding");
    wp_basic_tokenize("  multiple   spaces  ", out, sizeof out);
    CHECK(std::string(out) == "multiple spaces", "basic: whitespace collapse");

    wp_free(&v);
}

int main(void) {
    t_rng();
    t_jacobi();
    t_qr();
    t_svd();
    t_fit();
    t_encode();
    t_quant();
    t_tokenizer();
    printf("test_embed_math: %d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
