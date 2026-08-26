/*
 * dl-embed.cpp — standalone C++ embed tool for the vector tier
 * (replaces scripts/embed.py; see docs/vector-storage scope).
 *
 * Subcommands:
 *   pipeline --db DIR        walk entities -> embed -> fit ITQ -> emit
 *                            __sig0..15__ / __vec_q__ / __itq_basis__ CSVs
 *                            -> dl load x18 -> dl publish (+ metadata + .npy)
 *   encode --db DIR QUERY    embed QUERY, ITQ-encode + int8-quantize with the
 *                            stored basis/scale; prints "sig_hex ivec_hex"
 *                            (parsed by dl vsearch/vhybrid)
 *   embed QUERY              debug: print the 384 float32 dims (unnormalized)
 *   self-test                offline math/tokenizer checks + (if the model
 *                            is present) the golden-embedding cosine gate
 *   dump-tensors [PATH]      list GGUF tensors + key metadata
 *   fetch-model              download bge-small-en-v1.5-f16.gguf via curl
 *
 * Model path resolution: $DL_EMBED_MODEL, else
 * $XDG_CACHE_HOME|~/.cache)/datalog-dafsa/models/bge-small-en-v1.5-f16.gguf.
 * The model is NEVER committed; `make fetch-model` or `dl-embed fetch-model`
 * downloads it once (~67 MB).
 */
#include "bert.h"
#include "csv_emit.h"
#include "dl_driver.h"
#include "itq.h"
#include "tokenizer.h"
#include "vec_bits.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

#define VEC_D_ 384
#define VEC_C_ 256
#define VEC_M_ 16
#define VEC_SIG_WORDS_ 8
#define VEC_IVEC_WORDS_ 96
#define MODEL_NAME "bge-small-en-v1.5-f16.gguf"
#define MODEL_URL "https://huggingface.co/CompendiumLabs/bge-small-en-v1.5-gguf/resolve/main/bge-small-en-v1.5-f16.gguf"
#define GOLDEN_PATH "tests/data/bge_golden.txt"

static char g_repo_dir[4096];       /* dir of this binary (repo root)  */
static char g_dl_path[4096];        /* repo-root/dl                    */
static char g_model_path[4096];

static void die(const char *msg) { fprintf(stderr, "dl-embed: %s\n", msg); exit(1); }

/* snprintf that dies on truncation instead of silently cutting a path. */
static void pathf(char *dst, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(dst, cap, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap) die("path too long");
}

static int file_exists(const char *p);   /* forward decl (defined below) */

static void resolve_paths(const char *argv0) {
    snprintf(g_repo_dir, sizeof g_repo_dir, "%s", argv0);
    char *slash = strrchr(g_repo_dir, '/');
    if (slash) *slash = '\0';
    else snprintf(g_repo_dir, sizeof g_repo_dir, ".");
    pathf(g_dl_path, sizeof g_dl_path, "%s/dl", g_repo_dir);

    const char *env = getenv("DL_EMBED_MODEL");
    if (env && *env) {
        snprintf(g_model_path, sizeof g_model_path, "%s", env);
        return;
    }
    /* Prefer the repo-local, git-lfs-tracked model (models/<MODEL_NAME>);
     * fall back to the per-user cache path (for dev/fetch-on-demand). */
    pathf(g_model_path, sizeof g_model_path, "%s/models/" MODEL_NAME, g_repo_dir);
    if (file_exists(g_model_path)) return;

    const char *cache = getenv("XDG_CACHE_HOME");
    char base[4096];
    if (cache && *cache) snprintf(base, sizeof base, "%s/datalog-dafsa/models", cache);
    else {
        const char *home = getenv("HOME");
        if (!home || !*home) home = "/tmp";
        snprintf(base, sizeof base, "%s/.cache/datalog-dafsa/models", home);
    }
    pathf(g_model_path, sizeof g_model_path, "%s/" MODEL_NAME, base);
}

static int file_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

/* ── self-test (offline parts always; model part if available) ─────────── */

static void self_test_rng_and_math(void) {
    /* MT19937 numpy parity (goldens from numpy 2.5.2; see test_embed_math) */
    itq_rng_seed(42u);
    static const double g8[4] = {0.49671415301123267, -0.13826430117118466,
                                 0.64768853810069249, 1.5230298564080254};
    for (int i = 0; i < 4; i++) {
        double v = itq_rng_gauss();
        if (std::fabs(v - g8[i]) > 1e-12)
            die("self-test: MT19937 stream mismatch (itq.cpp regression)");
    }
    /* band/pack byte-identity invariants */
    uint32_t sig[VEC_SIG_WORDS_];
    for (int w = 0; w < VEC_SIG_WORDS_; w++) sig[w] = 0xA5A5F0F0u * (uint32_t)(w + 1);
    uint32_t recon[VEC_SIG_WORDS_] = {0};
    for (int j = 0; j < VEC_M_; j++)
        vec_band_set(recon, j, vec_band_slice(sig, j));
    if (memcmp(sig, recon, sizeof sig) != 0)
        die("self-test: band_slice/band_set round-trip failed");
    if (vec_pack4_le(1, 2, 3, 4) != 0x04030201u)
        die("self-test: pack4_le golden failed");
    printf("  offline math checks OK (full suite: tests/test_embed_math)\n");
}

static void self_test_model(void) {
    if (!file_exists(g_model_path)) {
        printf("  model not present at %s — skipping forward-pass gates\n"
               "  (fetch with: make fetch-model  or  dl-embed fetch-model)\n",
               g_model_path);
        return;
    }
    bert_model m;
    char err[512];
    if (bert_load(g_model_path, &m, err, sizeof err) != 0)
        die(err);
    printf("  model: %d layers, n_embd=%d, n_head=%d, vocab=%d, pos=%d, eps=%g\n",
           m.n_layer, m.n_embd, m.n_head, m.n_vocab, m.n_pos, m.eps);

    /* golden-embedding cosine gate */
    char golden_path[4200];
    snprintf(golden_path, sizeof golden_path, "%s/" GOLDEN_PATH, g_repo_dir);
    if (!file_exists(golden_path)) {
        printf("  %s not found — skipping golden-embedding gate\n"
               "  (generate once on a fastembed host: scripts/gen_golden.py)\n",
               golden_path);
    } else {
        FILE *f = fopen(golden_path, "r");
        if (!f) die("cannot open golden fixture");
        char text[1024];
        float ref[VEC_D_];
        int cases = 0, bad = 0;
        while (fgets(text, sizeof text, f)) {
            size_t tl = strlen(text);
            while (tl && (text[tl-1] == '\n' || text[tl-1] == '\r')) text[--tl] = '\0';
            if (tl == 0) continue;
            int nread = 0;
            for (int i = 0; i < VEC_D_; i++) {
                double v;
                if (fscanf(f, "%lf,", &v) != 1) { nread = -1; break; }
                ref[i] = (float)v;
                nread++;
            }
            fgetc(f);      /* trailing newline */
            if (nread != VEC_D_) die("malformed golden fixture line");
            float out[VEC_D_];
            if (bert_embed(&m, text, out, err, sizeof err) != 0) { bad++; continue; }
            double dot = 0, na = 0, nb = 0;
            for (int i = 0; i < VEC_D_; i++) {
                dot += (double)out[i] * ref[i];
                na += (double)out[i] * out[i];
                nb += (double)ref[i] * ref[i];
            }
            double cos = dot / (std::sqrt(na) * std::sqrt(nb) + 1e-30);
            cases++;
            printf("  golden [%s] cosine = %.6f %s\n", text, cos,
                   cos >= 0.9999 ? "OK" : "FAIL");
            if (cos < 0.9999) bad++;
        }
        fclose(f);
        if (cases == 0) die("golden fixture empty");
        if (bad) die("golden-embedding gate FAILED");
    }

    /* ITQ full-size fit sanity (one fit, orthonormal basis) */
    {
        int n = 300, d = VEC_D_, c = VEC_C_;
        float *X = (float *)malloc(sizeof(float) * n * d);
        if (!X) die("OOM");
        itq_rng_seed(1234u);
        for (int i = 0; i < n * d; i++) X[i] = (float)itq_rng_gauss();
        float *B = (float *)malloc(sizeof(float) * d * c);
        if (!B) die("OOM");
        itq_fit_basis(X, n, d, c, B);
        double err_max = 0;
        for (int a = 0; a < c; a++)
            for (int b = 0; b < c; b++) {
                double s = 0;
                for (int k = 0; k < d; k++) s += (double)B[k * c + a] * B[k * c + b];
                double want = (a == b) ? 1.0 : 0.0;
                if (std::fabs(s - want) > err_max) err_max = std::fabs(s - want);
            }
        printf("  full-size ITQ fit: max |B^T B - I| = %.2e %s\n", err_max,
               err_max < 1e-3 ? "OK" : "FAIL");
        if (err_max >= 1e-3) die("ITQ fit not orthonormal");
        free(X); free(B);
    }
    bert_free(&m);
}

/* ── fetch-model ────────────────────────────────────────────────────────── */

static int fetch_model(void) {
    if (file_exists(g_model_path)) {
        printf("model already present: %s\n", g_model_path);
        return 0;
    }
    char dir[4096], cmd[8192];
    snprintf(dir, sizeof dir, "%s", g_model_path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0';
    pathf(cmd, sizeof cmd, "mkdir -p '%s' && curl -L --fail --progress-bar -o '%s' '%s'",
          dir, g_model_path, MODEL_URL);
    printf("downloading %s\n  -> %s\n", MODEL_URL, g_model_path);
    fflush(stdout);
    int rc = system(cmd);
    if (rc != 0 || !file_exists(g_model_path)) {
        fprintf(stderr, "download failed (rc=%d).  Check network, or place the "
                        "model manually at\n  %s\n", rc, g_model_path);
        return 1;
    }
    struct stat st;
    if (stat(g_model_path, &st) == 0)
        printf("done (%lld bytes)\n", (long long)st.st_size);
    else
        printf("done\n");
    return 0;
}

/* ── pipeline ───────────────────────────────────────────────────────────── */

static int cmd_pipeline(const char *db, const char *rel, int col,
                        const char *suffix) {
    char err[512];
    if (!file_exists(g_dl_path))
        die("dl binary not found next to dl-embed (expected ./dl in repo root)");
    if (!file_exists(g_model_path)) {
        fprintf(stderr, "model missing: %s\n  run: make fetch-model (or dl-embed fetch-model)\n",
                g_model_path);
        return 1;
    }
    bert_model m;
    if (bert_load(g_model_path, &m, err, sizeof err) != 0) die(err);
    if (m.n_embd != VEC_D_)
        die("model n_embd != 384 (not bge-small-en-v1.5?)");

    /* RAW walk of the corpus liveness relation (spaces/unicode-safe). */
    uint32_t *rows; int n_rows = 0; uint8_t arity = 0;
    if (dld_prefix_raw(g_dl_path, db, rel, &rows, &n_rows, &arity) != 0)
        die("dl prefix raw failed for relation (is the DB published?)");
    if ((int)arity <= col)
        die("corpus relation arity too small for --col");
    if (n_rows <= 0)
        die("corpus relation is empty (nothing to embed)");

    /* dense sym -> text map over symbols.array (ids are 1-based sym ids). */
    char **names; uint32_t *ids;
    int n_syms = dld_symbols_array(db, &names, &ids);
    if (n_syms <= 0) die("cannot read symbols.array (publish first)");
    uint32_t max_id = 0;
    for (int k = 0; k < n_syms; k++) if (ids[k] > max_id) max_id = ids[k];
    std::vector<const char *> sym_text((size_t)max_id + 1, NULL);
    for (int k = 0; k < n_syms; k++) sym_text[ids[k]] = names[k];

    /* collect (text, sym) for each corpus row, dedup by sym. */
    std::vector<std::string> corpus_names;
    std::vector<uint32_t> corpus_syms;
    std::vector<int> seen((size_t)max_id + 1, 0);
    for (int r = 0; r < n_rows; r++) {
        uint32_t s = rows[(size_t)r * arity + (uint32_t)col];
        if (s > max_id) die("corpus sym out of symbols.array range (rebuild?)");
        if (seen[s]) continue;
        seen[s] = 1;
        const char *txt = sym_text[s];
        if (!txt || !*txt) {
            /* dead row: the observation references a sym whose string was
               removed from symbols.array.  Skip it — it can never be searched
               (its text is gone), so it must not enter the index. */
            fprintf(stderr, "  skip dead observation content sym=%u (no interned text)\n", s);
            continue;
        }
        corpus_names.push_back(txt);
        corpus_syms.push_back(s);
    }
    for (int k = 0; k < n_syms; k++) free(names[k]);
    free(names); free(ids);
    free(rows);
    if ((int)corpus_names.size() < VEC_C_)
        die("corpus too small for a rank-256 ITQ basis (need >= 256 entries; "
            "use the S3 synthetic path for smaller corpora)");
    printf("%zu corpus entries (%s col %d)\n", corpus_names.size(), rel, col);

    /* embed all corpus texts (L2-normalized) */
    size_t n_ent = corpus_names.size();
    float *X = (float *)malloc(sizeof(float) * n_ent * VEC_D_);
    if (!X) die("OOM embeddings");
    for (size_t k = 0; k < n_ent; k++) {
        if (bert_embed(&m, corpus_names[k].c_str(), X + k * VEC_D_, err, sizeof err) != 0)
            die(err);
        if ((k + 1) % 50 == 0 || k + 1 == n_ent)
            fprintf(stderr, "\rembedded %zu/%zu", k + 1, n_ent);
    }
    fprintf(stderr, "\n");

    /* fit basis + global scale */
    float *B = (float *)malloc(sizeof(float) * VEC_D_ * VEC_C_);
    if (!B) die("OOM basis");
    itq_fit_basis(X, (int)n_ent, VEC_D_, VEC_C_, B);
    float gscale = itq_corpus_global_scale(X, (int)n_ent, VEC_D_);
    printf("fitted ITQ basis; global int8 scale = %.9g\n", gscale);

    /* persist basis (.npy for the oracle) + metadata (corpus-namespaced) */
    char path[4096];
    snprintf(path, sizeof path, "%s/itq_basis%s.npy", db, suffix);
    if (npy_write_basis(path, B, VEC_D_, VEC_C_) != 0) die("cannot write itq_basis.npy");
    snprintf(path, sizeof path, "%s/vector_metadata%s.txt", db, suffix);
    csv_write_metadata(path, VEC_D_, VEC_C_, VEC_M_, (double)gscale);

    /* corpus-namespaced relations: __sig{j}__/__vec_q__ (entity, suffix "")
       or __obssig{j}__/__vec_obs__ (content, suffix "_obs"). */
    const char *sig_fmt = strcmp(suffix, "_obs") == 0 ? "__obssig%d__" : "__sig%d__";
    const char *vec_rel = strcmp(suffix, "_obs") == 0 ? "__vec_obs__" : "__vec_q__";

    /* encode corpus -> sig bands + packed int8 */
    size_t n_sig = n_ent, n_vecq = n_ent * VEC_IVEC_WORDS_;
    uint32_t *band_vals[VEC_M_];
    for (int j = 0; j < VEC_M_; j++)
        band_vals[j] = (uint32_t *)malloc(sizeof(uint32_t) * n_sig);
    uint32_t *vecq_syms = (uint32_t *)malloc(sizeof(uint32_t) * n_vecq);
    uint32_t *vecq_packed = (uint32_t *)malloc(sizeof(uint32_t) * n_vecq);
    if (!vecq_syms || !vecq_packed) die("OOM encodings");
    for (int j = 0; j < VEC_M_; j++) if (!band_vals[j]) die("OOM encodings");

    int8_t q[VEC_D_];
    uint32_t sig[VEC_SIG_WORDS_];
    for (size_t k = 0; k < n_ent; k++) {
        const float *v = X + k * VEC_D_;
        itq_encode(v, B, VEC_D_, VEC_C_, sig);
        for (int j = 0; j < VEC_M_; j++) band_vals[j][k] = vec_band_slice(sig, j);
        itq_quantize_int8_global(v, VEC_D_, gscale, q);
        for (int w = 0; w < VEC_IVEC_WORDS_; w++) {
            vecq_syms[k * VEC_IVEC_WORDS_ + w] = corpus_syms[k];
            vecq_packed[k * VEC_IVEC_WORDS_ + w] =
                vec_pack4_le(q[w * 4], q[w * 4 + 1], q[w * 4 + 2], q[w * 4 + 3]);
        }
    }

    /* write + load CSVs (corpus-namespaced csv names so bases don't clash) */
    char reln[32];
    for (int j = 0; j < VEC_M_; j++) {
        snprintf(path, sizeof path, "%s/sig_%d%s.csv", db, j, suffix);
        if (csv_write_sig(path, band_vals[j], corpus_syms.data(), n_sig) != 0) die("csv write failed");
        snprintf(reln, sizeof reln, sig_fmt, j);
        if (dld_load(g_dl_path, db, path, reln) != 0) die("dl load sig failed");
        free(band_vals[j]);
    }
    snprintf(path, sizeof path, "%s/vec_q%s.csv", db, suffix);
    if (csv_write_vecq(path, vecq_syms, vecq_packed, n_vecq) != 0) die("csv write failed");
    if (dld_load(g_dl_path, db, path, vec_rel) != 0) die("dl load vec failed");
    snprintf(path, sizeof path, "%s/itq_basis%s.csv", db, suffix);
    if (csv_write_basis(path, B, VEC_D_, VEC_C_) != 0) die("csv write failed");
    if (dld_load(g_dl_path, db, path, "__itq_basis__") != 0) die("dl load basis failed");

    /* publish + clean up */
    if (dld_publish(g_dl_path, db) != 0) die("dl publish failed");
    for (int j = 0; j < VEC_M_; j++) {
        snprintf(path, sizeof path, "%s/sig_%d%s.csv", db, j, suffix);
        remove(path);
    }
    snprintf(path, sizeof path, "%s/vec_q%s.csv", db, suffix); remove(path);
    snprintf(path, sizeof path, "%s/itq_basis%s.csv", db, suffix); remove(path);

    free(X); free(B); free(vecq_syms); free(vecq_packed);
    bert_free(&m);
    printf("published vector snapshot\n");
    return 0;
}

/* ── encode ─────────────────────────────────────────────────────────────── */

static int cmd_encode(const char *db, const char *query) {
    char err[512];
    /* basis: prefer the __itq_basis__ relation, fallback itq_basis.npy */
    float *B = (float *)malloc(sizeof(float) * VEC_D_ * VEC_C_);
    if (!B) die("OOM");
    static char out_buf[16 << 20];
    int n_lines = 0;
    int got = 0;
    if (dld_prefix(g_dl_path, db, "__itq_basis__", 1, NULL,
                   out_buf, sizeof out_buf, &n_lines) == 0 &&
        n_lines == VEC_D_ * VEC_C_) {
        int n = 0;
        for (char *p = out_buf; *p && n < VEC_D_ * VEC_C_; p += strlen(p) + 1) {
            unsigned i, j, bits;
            if (sscanf(p, "%u %u %u", &i, &j, &bits) == 3 &&
                i < (unsigned)VEC_D_ && j < (unsigned)VEC_C_) {
                B[i * VEC_C_ + j] = vec_bits_float32(bits);
                n++;
            }
        }
        got = (n == VEC_D_ * VEC_C_);
    }
    if (!got) {
        char path[4096];
        snprintf(path, sizeof path, "%s/itq_basis.npy", db);
        got = npy_read_basis(path, B, VEC_D_, VEC_C_) == 0;
    }
    if (!got)
        die("no ITQ basis: run 'dl-embed pipeline --db' first");

    double gscale = 0;
    char path[4096];
    snprintf(path, sizeof path, "%s/vector_metadata.txt", db);
    if (meta_read_qscale(path, &gscale) != 0)
        die("no qscale in vector_metadata.txt (run pipeline first)");

    if (!file_exists(g_model_path)) {
        fprintf(stderr, "model missing: %s\n", g_model_path);
        return 1;
    }
    bert_model m;
    if (bert_load(g_model_path, &m, err, sizeof err) != 0) die(err);
    float v[VEC_D_];
    if (bert_embed(&m, query, v, err, sizeof err) != 0) die(err);

    uint32_t sig[VEC_SIG_WORDS_];
    itq_encode(v, B, VEC_D_, VEC_C_, sig);
    int8_t q[VEC_D_];
    itq_quantize_int8_global(v, VEC_D_, (float)gscale, q);

    for (int w = 0; w < VEC_SIG_WORDS_; w++) printf("%08x", sig[w]);
    printf(" ");
    for (int w = 0; w < VEC_IVEC_WORDS_; w++)
        printf("%08x", vec_pack4_le(q[w * 4], q[w * 4 + 1], q[w * 4 + 2], q[w * 4 + 3]));
    printf("\n");
    bert_free(&m);
    return 0;
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "usage: dl-embed <command>\n"
            "  pipeline --db DIR [--rel REL --col N --suffix S]\n"
            "                      embed corpus + emit vector relations\n"
            "  encode --db DIR QRY   print 'sig_hex ivec_hex' for a query\n"
            "  embed QRY             print the raw 384-float embedding\n"
            "  tokenize QRY          print token ids + strings (debug)\n"
            "  self-test             math/tokenizer (+model if present)\n"
            "  dump-tensors [PATH]   list GGUF tensors\n"
            "  fetch-model           download the bge-small GGUF\n");
        return 1;
    }
    resolve_paths(argv[0]);
    const char *cmd = argv[1];

    if (strcmp(cmd, "fetch-model") == 0) return fetch_model();

    if (strcmp(cmd, "dump-tensors") == 0) {
        const char *p = argc > 2 ? argv[2] : g_model_path;
        bert_dump_tensors(p);
        return 0;
    }

    if (strcmp(cmd, "self-test") == 0) {
        printf("dl-embed self-test\n");
        self_test_rng_and_math();
        self_test_model();
        printf("self-test OK\n");
        return 0;
    }

    if (strcmp(cmd, "tokenize") == 0) {
        if (argc < 3) die("tokenize requires a query string");
        char err[512];
        if (!file_exists(g_model_path)) {
            fprintf(stderr, "model missing: %s\n", g_model_path);
            return 1;
        }
        bert_model m;
        if (bert_load(g_model_path, &m, err, sizeof err) != 0) die(err);
        int ids[1024];
        int cap = m.max_len < (int)(sizeof ids / sizeof ids[0])
                ? m.max_len : (int)(sizeof ids / sizeof ids[0]);
        int n = wp_tokenize(&m.vocab, argv[2], m.cls_id, m.sep_id, m.unk_id,
                            ids, cap);
        if (n < 1) die("tokenization produced no ids");
        printf("n=%d ids:", n);
        for (int i = 0; i < n; i++) printf(" %d", ids[i]);
        printf("\ntokens:");
        for (int i = 0; i < n; i++)
            printf(" [%s]", ids[i] >= 0 && ids[i] < m.vocab.n
                             ? m.vocab.texts[ids[i]] : "?");
        printf("\n");
        bert_free(&m);
        return 0;
    }

    if (strcmp(cmd, "embed") == 0) {
        if (argc < 3) die("embed requires a query string");
        char err[512];
        if (!file_exists(g_model_path)) {
            fprintf(stderr, "model missing: %s\n", g_model_path);
            return 1;
        }
        bert_model m;
        if (bert_load(g_model_path, &m, err, sizeof err) != 0) die(err);
        float v[VEC_D_];
        if (bert_embed(&m, argv[2], v, err, sizeof err) != 0) die(err);
        for (int i = 0; i < VEC_D_; i++)
            printf("%.9g%c", v[i], i + 1 == VEC_D_ ? '\n' : ' ');
        bert_free(&m);
        return 0;
    }

    /* --db is common to pipeline/encode */
    const char *db = NULL;
    for (int i = 2; i < argc - 1; i++)
        if (strcmp(argv[i], "--db") == 0) db = argv[i + 1];
    if (!db) die("missing --db DIR");

    if (strcmp(cmd, "pipeline") == 0) {
        /* --rel <rel> (default "entity"), --col <N> (default 0),
           --suffix <s> (default "" — the entity corpus). */
        const char *rel = "entity";
        int col = 0;
        const char *suffix = "";
        for (int i = 2; i < argc - 1; i++) {
            if (strcmp(argv[i], "--rel") == 0) rel = argv[i + 1];
            else if (strcmp(argv[i], "--col") == 0) col = atoi(argv[i + 1]);
            else if (strcmp(argv[i], "--suffix") == 0) suffix = argv[i + 1];
        }
        return cmd_pipeline(db, rel, col, suffix);
    }
    if (strcmp(cmd, "encode") == 0) {
        if (argc < 3) die("encode requires a query string");
        /* query = the non-flag positional after the command */
        const char *query = NULL;
        for (int i = 2; i < argc; i++)
            if (strcmp(argv[i], "--db") != 0 && (i == 2 || strcmp(argv[i - 1], "--db") != 0)) {
                query = argv[i];
                break;
            }
        if (!query) die("encode requires a query string");
        return cmd_encode(db, query);
    }

    fprintf(stderr, "dl-embed: unknown command '%s'\n", cmd);
    return 1;
}
