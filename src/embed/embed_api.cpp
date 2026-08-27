/*
 * embed_api.cpp — implementation of dl_embed_encode_query (see embed_api.h).
 * Reuses bert/itq/csv_emit exactly as dl-embed's cmd_encode does, but reads
 * the CORPUS-SPECIFIC basis/metadata files (itq_basis<suffix>.npy /
 * vector_metadata<suffix>.txt) and resolves the model WITHOUT the repo-local
 * models/ dir (a .so has no argv0).
 */
#include "embed_api.h"

#include "bert.h"
#include "csv_emit.h"
#include "itq.h"
#include "remote_embed.h"
#include "vec_bits.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>

#include <dlfcn.h>   /* dladdr: locate a models/ dir next to this .so */

#define VEC_D_ 384
#define VEC_C_ 256
#define VEC_SIG_WORDS_ (VEC_C_ / 32)   /* 8 */
#define VEC_IVEC_WORDS_ (VEC_D_ / 4)   /* 96 */
#define MODEL_NAME "bge-small-en-v1.5-f16.gguf"

static void errset(char *err, size_t errlen, const char *msg) {
    if (err && errlen) { snprintf(err, errlen, "%s", msg); }
}

static int file_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static int resolve_model(char *out, size_t cap, char *err, size_t errlen) {
    const char *env = getenv("DL_EMBED_MODEL");
    if (env && *env) {
        snprintf(out, cap, "%s", env);
        if (file_exists(out)) return 0;
        errset(err, errlen, "model missing: $DL_EMBED_MODEL path does not exist");
        return -1;
    }
    /* Self-contained deploy: a models/<MODEL_NAME> dir next to this .so
     * (the binary's config folder).  A .so has no argv0, so use dladdr. */
    Dl_info di;
    if (dladdr((void*)&resolve_model, &di) && di.dli_fname && di.dli_fname[0]) {
        char sopath[4096];
        snprintf(sopath, sizeof sopath, "%s", di.dli_fname);
        char *slash = strrchr(sopath, '/');
        if (slash) {
            *slash = '\0';
            snprintf(out, cap, "%s/models/" MODEL_NAME, sopath);
            if (file_exists(out)) return 0;
        }
    }
    const char *cache = getenv("XDG_CACHE_HOME");
    char base[4096];
    if (cache && *cache) snprintf(base, sizeof base, "%s/datalog-dafsa/models", cache);
    else {
        const char *home = getenv("HOME");
        if (!home || !*home) home = "/tmp";
        snprintf(base, sizeof base, "%s/.cache/datalog-dafsa/models", home);
    }
    snprintf(out, cap, "%s/" MODEL_NAME, base);
    if (file_exists(out)) return 0;
    errset(err, errlen, "model missing: not at $DL_EMBED_MODEL or ~/.cache/datalog-dafsa/models/"
                        MODEL_NAME " (run: make fetch-model)");
    return -1;
}

int dl_embed_encode_query(const char *db_dir, const char *corpus_suffix,
                          const char *query,
                          uint32_t *q_sig, uint32_t *q_ivec,
                          char *err, size_t errlen) {
    const char *suffix = corpus_suffix ? corpus_suffix : "";
    char path[4096];
    float *B;
    double gscale;
    bert_model m;
    float v[VEC_D_];
    int8_t q[VEC_D_];

    if (!db_dir || !query || !q_sig || !q_ivec) {
        errset(err, errlen, "null argument to dl_embed_encode_query");
        return -1;
    }

    /* basis + qscale for THIS corpus. */
    B = (float *)malloc(sizeof(float) * VEC_D_ * VEC_C_);
    if (!B) { errset(err, errlen, "OOM"); return -1; }
    snprintf(path, sizeof path, "%s/itq_basis%s.npy", db_dir, suffix);
    if (npy_read_basis(path, B, VEC_D_, VEC_C_) != 0) {
        errset(err, errlen, "no ITQ basis: run the pipeline first (dl-embed pipeline --db)");
        free(B);
        return -1;
    }
    snprintf(path, sizeof path, "%s/vector_metadata%s.txt", db_dir, suffix);
    if (meta_read_qscale(path, &gscale) != 0) {
        errset(err, errlen, "no qscale in vector_metadata.txt (run the pipeline first)");
        free(B);
        return -1;
    }

    /* model (env -> cache; never repo-local).  When DL_EMBED_API_URL is set,
     * route the query through the remote OpenAI-compatible endpoint instead. */
    if (remote_embed_configured()) {
        const char *q = query;
        if (remote_embed(&q, 1, v, err, errlen) != 0) { free(B); return -1; }
    } else {
        char mp[4096];
        if (resolve_model(mp, sizeof mp, err, errlen) != 0) { free(B); return -1; }
        if (bert_load(mp, &m, err, errlen) != 0) { free(B); return -1; }
        /* encode (identical to dl-embed cmd_encode). */
        if (bert_embed(&m, query, v, err, errlen) != 0) { bert_free(&m); free(B); return -1; }
        bert_free(&m);
    }
    itq_encode(v, B, VEC_D_, VEC_C_, q_sig);
    itq_quantize_int8_global(v, VEC_D_, (float)gscale, q);
    for (int w = 0; w < VEC_IVEC_WORDS_; w++)
        q_ivec[w] = vec_pack4_le(q[w * 4], q[w * 4 + 1], q[w * 4 + 2], q[w * 4 + 3]);

    free(B);
    return 0;
}
