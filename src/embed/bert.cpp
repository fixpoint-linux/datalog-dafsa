/*
 * bert.cpp — bge-small-en-v1.5 BERT forward pass on ggml (CPU).
 *
 * Load pattern follows ggml's own GGUF handling (gguf.cpp): init with
 * no_alloc=false so gguf reads the tensor-data blob into a ggml_context
 * and wires every tensor's ->data at the right offset itself — no manual
 * pointer arithmetic on our side (gguf_context is opaque in gguf.h).
 *
 * Graphs run on the CPU backend (ggml-backend API, as in ggml's
 * examples/magika + yolo): weights live in a plain ggml_context, and
 * ggml_backend_cpu_graph_compute is a direct ggml_graph_compute over those
 * pointers, so no backend-buffer allocation is needed for CPU-only compute.
 *
 * The attention op sequence is derived from ggml's shape algebra and
 * matches llama.cpp's build_attn:
 *   Q/K/V {n_embd, n} -> reshape_3d(head_dim, n_head, n)
 *   Qp = permute(0,2,1,3) -> {head_dim, n, n_head}
 *   Kp = permute(0,2,1,3) -> {head_dim, n, n_head}
 *   kq = mul_mat(Kp, Qp)  -> {n_kv, n_q, n_head}      (contract ne0 = head_dim)
 *   kq = soft_max_ext(kq, NULL, 1/sqrt(head_dim), 0)  (softmax over keys)
 *   Vp = permute(1,2,0,3) -> {n, head_dim, n_head}
 *   o  = mul_mat(Vp, kq)  -> {n_q, head_dim, n_head}   (contract ne0 = keys)
 *   op = permute(0,2,1,3) -> {head_dim, n_head, n}     (token-major buffer)
 *   attn = cont + reshape_2d(n_embd, n)
 * NB ggml_permute MOVE semantics: ne_out[axis_i] = ne_in[i] (ggml.c), i.e.
 * the args say WHERE each input dim lands — NOT ne_out[i] = ne_in[axis_i].
 * ggml aborts on any ne0 mismatch in mul_mat, so a wrong dance fails loudly
 * at graph-build time rather than silently corrupting embeddings.
 */
#include "bert.h"

#include "ggml.h"
#include "gguf.h"
#include "ggml-backend.h"
#include "ggml-cpu.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/* worker threads for the CPU backend forward pass */
#define BERT_N_THREADS 4

/* ── helpers ────────────────────────────────────────────────────────────── */

/* no_alloc=false load: gguf already pointed each tensor's ->data into the
 * blob inside the ggml_context, so lookup is a plain name query. */
static ggml_tensor *find_tensor_or_null(ggml_context *ctx, const char *name) {
    return ggml_get_tensor(ctx, name);
}

static ggml_tensor *require_tensor(ggml_context *ctx, const char *name,
                                   char *err, size_t errlen, const char *why) {
    ggml_tensor *t = find_tensor_or_null(ctx, name);
    if (!t) {
        snprintf(err, errlen, "required GGUF tensor '%s' missing (%s)", name, why);
        return NULL;
    }
    return t;
}

/* try primary name, then fallback; NULL if neither exists */
static ggml_tensor *find2(ggml_context *ctx, const char *a, const char *b) {
    ggml_tensor *t = find_tensor_or_null(ctx, a);
    if (t) return t;
    if (b) return find_tensor_or_null(ctx, b);
    return NULL;
}

static int kv_f32(gguf_context *gf, const char *key, float *out) {
    int64_t id = gguf_find_key(gf, key);
    if (id < 0) return -1;
    if (gguf_get_kv_type(gf, id) != GGUF_TYPE_FLOAT32) return -1;
    *out = gguf_get_val_f32(gf, id);
    return 0;
}

static int kv_i32(gguf_context *gf, const char *key, int32_t *out) {
    int64_t id = gguf_find_key(gf, key);
    if (id < 0) return -1;
    enum gguf_type t = gguf_get_kv_type(gf, id);
    if (t == GGUF_TYPE_INT32) { *out = gguf_get_val_i32(gf, id); return 0; }
    if (t == GGUF_TYPE_UINT32) { *out = (int32_t)gguf_get_val_u32(gf, id); return 0; }
    return -1;
}

/* LayerNorm: (x - mean)/sqrt(var+eps) * w + b   (ggml_norm + mul + add). */
static ggml_tensor *build_norm(ggml_context *ctx, ggml_tensor *x,
                               ggml_tensor *w, ggml_tensor *b, float eps) {
    x = ggml_norm(ctx, x, eps);
    x = ggml_mul(ctx, x, w);
    if (b) x = ggml_add(ctx, x, b);
    return x;
}

/* ── load ───────────────────────────────────────────────────────────────── */

int bert_load(const char *path, bert_model *m, char *err, size_t errlen) {
    memset(m, 0, sizeof *m);
    m->cls_id = 101; m->sep_id = 102; m->unk_id = 100; m->pad_id = 0;
    m->max_len = 512;

    ggml_context *meta = NULL;
    struct gguf_init_params ip;
    ip.no_alloc = false;   /* gguf reads the blob + wires t->data itself */
    ip.ctx = &meta;
    gguf_context *gf = gguf_init_from_file(path, ip);
    if (!gf) { snprintf(err, errlen, "cannot open GGUF '%s'", path); return -1; }

    /* embeddings + norms */
    m->gf = gf;
    m->wctx = meta;
    m->tok_embd = require_tensor(meta, "token_embd.weight", err, errlen,
                                 "token embeddings");
    if (!m->tok_embd) return -1;
    m->pos_embd = find2(meta, "position_embd.weight", "pos_embd.weight");
    if (!m->pos_embd) {
        snprintf(err, errlen, "position embeddings missing (position_embd.weight / pos_embd.weight)");
        return -1;
    }
    m->tok_types = find_tensor_or_null(meta, "token_types.weight");
    m->embd_norm_w = require_tensor(meta, "token_embd_norm.weight", err, errlen,
                                    "BertEmbeddings.LayerNorm — REQUIRED for bge");
    if (!m->embd_norm_w) return -1;
    m->embd_norm_b = find_tensor_or_null(meta, "token_embd_norm.bias");
    /* Final encoder LayerNorm is OPTIONAL by presence: HF BERT has no
     * separate final norm — the last block's post-FFN layer_output_norm IS
     * the final norm, and the CompendiumLabs bge GGUF ships it that way (no
     * output_norm tensor at all).  Conversions that DO add output_norm
     * still get it applied (see bert_embed_ids). */
    m->out_norm_w = find_tensor_or_null(meta, "output_norm.weight");
    m->out_norm_b = m->out_norm_w
        ? find_tensor_or_null(meta, "output_norm.bias") : NULL;

    m->n_embd  = (int)m->tok_embd->ne[0];
    m->n_vocab = (int)m->tok_embd->ne[1];
    m->n_pos   = (int)m->pos_embd->ne[1];

    if (m->n_embd <= 0 || m->n_vocab <= 0 || m->n_pos <= 0) {
        snprintf(err, errlen, "degenerate model geometry (n_embd=%d n_vocab=%d n_pos=%d)",
                 m->n_embd, m->n_vocab, m->n_pos);
        return -1;
    }

    /* layer count: highest N with blk.N.attn_q.weight present */
    char name[64];
    int n_layer = 0;
    for (int l = 0; l < 128; l++) {
        snprintf(name, sizeof name, "blk.%d.attn_q.weight", l);
        if (gguf_find_tensor(gf, name) >= 0) n_layer = l + 1;
    }
    if (n_layer == 0) { snprintf(err, errlen, "no blk.N.attn_q tensors (not a BERT GGUF?)"); return -1; }
    m->n_layer = n_layer;

    int32_t heads = 0;
    if (kv_i32(gf, "bert.attention.head_count", &heads) != 0 &&
        kv_i32(gf, "attention.head_count", &heads) != 0)
        heads = 0;
    if (heads <= 0) heads = m->n_embd / 32;    /* bge-small: 384/32 = 12 */
    m->n_head = heads;

    float eps = 0.0f;
    if (kv_f32(gf, "attention.layer_norm_epsilon", &eps) != 0 &&
        kv_f32(gf, "bert.layer_norm_epsilon", &eps) != 0)
        eps = 1e-12f;                          /* bge-small config value */
    m->eps = eps;

    /* per-layer tensors */
    m->wq = (ggml_tensor **)calloc(n_layer, sizeof(void *));
    m->bq = (ggml_tensor **)calloc(n_layer, sizeof(void *));
    m->wk = (ggml_tensor **)calloc(n_layer, sizeof(void *));
    m->bk = (ggml_tensor **)calloc(n_layer, sizeof(void *));
    m->wv = (ggml_tensor **)calloc(n_layer, sizeof(void *));
    m->bv = (ggml_tensor **)calloc(n_layer, sizeof(void *));
    m->wo = (ggml_tensor **)calloc(n_layer, sizeof(void *));
    m->bo = (ggml_tensor **)calloc(n_layer, sizeof(void *));
    m->attn_norm_w = (ggml_tensor **)calloc(n_layer, sizeof(void *));
    m->attn_norm_b = (ggml_tensor **)calloc(n_layer, sizeof(void *));
    m->up_w = (ggml_tensor **)calloc(n_layer, sizeof(void *));
    m->up_b = (ggml_tensor **)calloc(n_layer, sizeof(void *));
    m->down_w = (ggml_tensor **)calloc(n_layer, sizeof(void *));
    m->down_b = (ggml_tensor **)calloc(n_layer, sizeof(void *));
    m->ffn_norm_w = (ggml_tensor **)calloc(n_layer, sizeof(void *));
    m->ffn_norm_b = (ggml_tensor **)calloc(n_layer, sizeof(void *));
    if (!m->wq || !m->bq || !m->wk || !m->bk || !m->wv || !m->bv || !m->wo ||
        !m->bo || !m->attn_norm_w || !m->attn_norm_b || !m->up_w || !m->up_b ||
        !m->down_w || !m->down_b || !m->ffn_norm_w || !m->ffn_norm_b) {
        snprintf(err, errlen, "OOM allocating layer tables");
        return -1;
    }

    for (int l = 0; l < n_layer; l++) {
        char fb[64];
        snprintf(name,  sizeof name,  "blk.%d.attn_q.weight", l);
        snprintf(fb,    sizeof fb,    "blk.%d.attn_q.bias", l);
        m->wq[l] = require_tensor(meta, name, err, errlen, "attn q");
        if (!m->wq[l]) return -1;
        m->bq[l] = find_tensor_or_null(meta, fb);

        snprintf(name, sizeof name, "blk.%d.attn_k.weight", l);
        snprintf(fb,   sizeof fb,   "blk.%d.attn_k.bias", l);
        m->wk[l] = require_tensor(meta, name, err, errlen, "attn k");
        if (!m->wk[l]) return -1;
        m->bk[l] = find_tensor_or_null(meta, fb);

        snprintf(name, sizeof name, "blk.%d.attn_v.weight", l);
        snprintf(fb,   sizeof fb,   "blk.%d.attn_v.bias", l);
        m->wv[l] = require_tensor(meta, name, err, errlen, "attn v");
        if (!m->wv[l]) return -1;
        m->bv[l] = find_tensor_or_null(meta, fb);

        snprintf(name, sizeof name, "blk.%d.attn_output.weight", l);
        snprintf(fb,   sizeof fb,   "blk.%d.attn_output.bias", l);
        m->wo[l] = require_tensor(meta, name, err, errlen, "attn out proj");
        if (!m->wo[l]) return -1;
        m->bo[l] = find_tensor_or_null(meta, fb);

        snprintf(name, sizeof name, "blk.%d.attn_output_norm.weight", l);
        snprintf(fb,   sizeof fb,   "blk.%d.attn_output_norm.bias", l);
        m->attn_norm_w[l] = require_tensor(meta, name, err, errlen,
                                           "post-attn LayerNorm");
        if (!m->attn_norm_w[l]) return -1;
        m->attn_norm_b[l] = find_tensor_or_null(meta, fb);

        snprintf(name, sizeof name, "blk.%d.ffn_up.weight", l);
        snprintf(fb,   sizeof fb,   "blk.%d.ffn_up.bias", l);
        m->up_w[l] = require_tensor(meta, name, err, errlen, "ffn up");
        if (!m->up_w[l]) return -1;
        m->up_b[l] = find_tensor_or_null(meta, fb);

        snprintf(name, sizeof name, "blk.%d.ffn_down.weight", l);
        snprintf(fb,   sizeof fb,   "blk.%d.ffn_down.bias", l);
        m->down_w[l] = require_tensor(meta, name, err, errlen, "ffn down");
        if (!m->down_w[l]) return -1;
        m->down_b[l] = find_tensor_or_null(meta, fb);

        /* post-FFN LN: current name layer_output_norm, older conversions
         * use output_norm — accept either (by presence). */
        snprintf(name, sizeof name, "blk.%d.layer_output_norm.weight", l);
        snprintf(fb,   sizeof fb,   "blk.%d.layer_output_norm.bias", l);
        m->ffn_norm_w[l] = find2(meta, name, nullptr);
        if (m->ffn_norm_w[l]) {
            m->ffn_norm_b[l] = find_tensor_or_null(meta, fb);
        } else {
            snprintf(name, sizeof name, "blk.%d.output_norm.weight", l);
            snprintf(fb,   sizeof fb,   "blk.%d.output_norm.bias", l);
            m->ffn_norm_w[l] = require_tensor(meta, name, err, errlen,
                                              "post-FFN LayerNorm");
            if (!m->ffn_norm_w[l]) return -1;
            m->ffn_norm_b[l] = find_tensor_or_null(meta, fb);
        }
    }

    /* tokenizer vocab from metadata: tokenizer.ggml.tokens [+ token_type] */
    int64_t kid = gguf_find_key(gf, "tokenizer.ggml.tokens");
    size_t ntok = 0;
    if (kid >= 0 && gguf_get_kv_type(gf, kid) == GGUF_TYPE_ARRAY &&
        gguf_get_arr_type(gf, kid) == GGUF_TYPE_STRING) {
        ntok = gguf_get_arr_n(gf, kid);
    }
    if (ntok == 0 || ntok > (size_t)1 << 24) {
        snprintf(err, errlen, "tokenizer.ggml.tokens missing — not a tokenizer GGUF");
        return -1;
    }
    /* string arrays are element-accessed via gguf_get_arr_str (the backing
     * store is not a char** array); we own strdup copies. */
    m->vocab_storage = (char **)calloc(ntok, sizeof(char *));
    if (!m->vocab_storage) { snprintf(err, errlen, "OOM vocab"); return -1; }
    for (size_t i = 0; i < ntok; i++) {
        const char *s = gguf_get_arr_str(gf, kid, i);
        m->vocab_storage[i] = strdup(s ? s : "");
    }
    const int32_t *types = NULL;
    int64_t tid = gguf_find_key(gf, "tokenizer.ggml.token_type");
    if (tid >= 0 && gguf_get_kv_type(gf, tid) == GGUF_TYPE_ARRAY &&
        gguf_get_arr_type(gf, tid) == GGUF_TYPE_INT32) {
        if (gguf_get_arr_n(gf, tid) == ntok)
            types = (const int32_t *)gguf_get_arr_data(gf, tid);
    }
    if (types) {
        m->types_storage = (int32_t *)malloc(sizeof(int32_t) * ntok);
        if (m->types_storage) memcpy(m->types_storage, types, sizeof(int32_t) * ntok);
    }
    m->vocab.texts = (const char *const *)m->vocab_storage;
    m->vocab.types = m->types_storage;
    m->vocab.n = (int)ntok;
    if (wp_init(&m->vocab) != 0) { snprintf(err, errlen, "OOM vocab index"); return -1; }

    /* tokenizer vocab and the token-embedding table must agree: a mismatched
     * GGUF would otherwise hand get_rows row ids >= token_embd rows and die
     * inside a GGML_ASSERT abort instead of a clean load error. */
    if (m->n_vocab != m->vocab.n) {
        snprintf(err, errlen,
                 "vocab mismatch: tokenizer.ggml.tokens has %d entries but "
                 "token_embd.weight has %d rows",
                 m->vocab.n, m->n_vocab);
        return -1;
    }

    /* special ids by exact text (robust across vocab layouts) */
    int id;
    if ((id = wp_find_exact(&m->vocab, "[CLS]")) >= 0) m->cls_id = id;
    if ((id = wp_find_exact(&m->vocab, "[SEP]")) >= 0) m->sep_id = id;
    if ((id = wp_find_exact(&m->vocab, "[UNK]")) >= 0) m->unk_id = id;
    if ((id = wp_find_exact(&m->vocab, "[PAD]")) >= 0) m->pad_id = id;

    int32_t ctx = 0;
    if (kv_i32(gf, "bert.context_length", &ctx) == 0 && ctx > 0 && ctx < m->n_pos)
        m->max_len = ctx;
    else
        m->max_len = m->n_pos;   /* always clamp to the position table */

    /* CPU compute backend — created last so no failure path above has to
     * clean it up.  Owns the worker threads reused across forward passes. */
    m->backend = ggml_backend_cpu_init();
    if (!m->backend) {
        snprintf(err, errlen, "ggml_backend_cpu_init failed");
        return -1;
    }
    ggml_backend_cpu_set_n_threads(m->backend, BERT_N_THREADS);

    return 0;
}

void bert_free(bert_model *m) {
    if (m->vocab_storage) {
        for (int i = 0; i < m->vocab.n; i++) free(m->vocab_storage[i]);
        free(m->vocab_storage);
    }
    free(m->types_storage);
    wp_free(&m->vocab);
    free(m->wq); free(m->bq); free(m->wk); free(m->bk); free(m->wv); free(m->bv);
    free(m->wo); free(m->bo);
    free(m->attn_norm_w); free(m->attn_norm_b);
    free(m->up_w); free(m->up_b); free(m->down_w); free(m->down_b);
    free(m->ffn_norm_w); free(m->ffn_norm_b);
    if (m->backend) ggml_backend_free(m->backend);
    if (m->wctx) ggml_free(m->wctx);
    if (m->gf) gguf_free(m->gf);
    memset(m, 0, sizeof *m);
}

/* ── forward ────────────────────────────────────────────────────────────── */

int bert_embed_ids(bert_model *m, const int *ids, int n_ids, float *out,
                   char *err, size_t errlen) {
    if (n_ids <= 0 || n_ids > m->max_len) {
        snprintf(err, errlen, "n_ids %d out of range (1..%d)", n_ids, m->max_len);
        return -1;
    }

    const int n = n_ids;
    const int n_embd = m->n_embd, n_head = m->n_head;
    const int head_dim = n_embd / n_head;
    if (head_dim * n_head != n_embd) {
        snprintf(err, errlen, "n_embd %d not divisible by n_head %d", n_embd, n_head);
        return -1;
    }

    /* compute ctx: intermediates + mul_mat work buffers scale ~O(n)+O(n^2).
     * Measured peak on bge-small f16 (12 layers, 384 embd, n_ff 1536):
     *   n=128 ->  98.5 MB,  n=256 -> 234.6 MB,  n=512 -> 620 MB
     * i.e. roughly 0.24MB + n*620KB + n^2*1.15KB; size with ~30% headroom. */
    struct ggml_init_params cp;
    cp.mem_size   = 32u * 1024u * 1024u
                  + (size_t)n * 704u * 1024u
                  + (size_t)n * (size_t)n * 1536u;
    cp.mem_buffer = NULL;
    cp.no_alloc   = false;
    ggml_context *ctx = ggml_init(cp);
    if (!ctx) { snprintf(err, errlen, "ggml_init failed"); return -1; }

    ggml_tensor *tok_ids = ggml_new_tensor_1d(ctx, GGML_TYPE_I32, n);
    memcpy(tok_ids->data, ids, sizeof(int) * n);
    ggml_tensor *pos_ids = ggml_new_tensor_1d(ctx, GGML_TYPE_I32, n);
    for (int i = 0; i < n; i++) ((int32_t *)pos_ids->data)[i] = i;
    ggml_tensor *tty_ids = ggml_new_tensor_1d(ctx, GGML_TYPE_I32, n);
    memset(tty_ids->data, 0, sizeof(int32_t) * n);

    /* embeddings: tok + pos + types -> LN */
    ggml_tensor *h = ggml_get_rows(ctx, m->tok_embd, tok_ids);
    h = ggml_add(ctx, h, ggml_get_rows(ctx, m->pos_embd, pos_ids));
    if (m->tok_types)
        h = ggml_add(ctx, h, ggml_get_rows(ctx, m->tok_types, tty_ids));
    h = build_norm(ctx, h, m->embd_norm_w, m->embd_norm_b, m->eps);

    const float kq_scale = 1.0f / sqrtf((float)head_dim);

    for (int l = 0; l < m->n_layer; l++) {
        /* ---- self-attention (POST-LN) ---- */
        ggml_tensor *Q = ggml_mul_mat(ctx, m->wq[l], h);
        if (m->bq[l]) Q = ggml_add(ctx, Q, m->bq[l]);
        ggml_tensor *K = ggml_mul_mat(ctx, m->wk[l], h);
        if (m->bk[l]) K = ggml_add(ctx, K, m->bk[l]);
        ggml_tensor *V = ggml_mul_mat(ctx, m->wv[l], h);
        if (m->bv[l]) V = ggml_add(ctx, V, m->bv[l]);

        Q = ggml_permute(ctx, ggml_reshape_3d(ctx, Q, head_dim, n_head, n), 0, 2, 1, 3);
        K = ggml_permute(ctx, ggml_reshape_3d(ctx, K, head_dim, n_head, n), 0, 2, 1, 3);
        ggml_tensor *kq = ggml_mul_mat(ctx, K, Q);
        /* single unpadded sequence: every position attends (no mask; the
         * padding mask in bert.cpp is all-valid here), no ALiBi. */
        kq = ggml_soft_max_ext(ctx, kq, NULL, kq_scale, 0.0f);

        /* cont() materializes the permuted view: mul_mat requires its first
         * operand non-transposed (row-major strides), which a permuted view
         * of {head_dim, n_head, n} is not (llama.cpp does the same for V). */
        ggml_tensor *Vp = ggml_cont(
            ctx, ggml_permute(ctx, ggml_reshape_3d(ctx, V, head_dim, n_head, n),
                              1, 2, 0, 3));
        ggml_tensor *o = ggml_mul_mat(ctx, Vp, kq);
        /* ggml_permute MOVE semantics: ne_out[axis_i] = ne_in[i], so
         * (0,2,1,3) turns {head_dim, n, n_head} into {head_dim, n_head, n} —
         * the token-major buffer reshape_2d below expects (matches
         * llama.cpp's KQV merge). */
        o = ggml_permute(ctx, o, 0, 2, 1, 3);
        o = ggml_cont(ctx, o);
        o = ggml_reshape_2d(ctx, o, n_embd, n);

        ggml_tensor *attn = ggml_mul_mat(ctx, m->wo[l], o);
        if (m->bo[l]) attn = ggml_add(ctx, attn, m->bo[l]);
        h = ggml_add(ctx, h, attn);
        h = build_norm(ctx, h, m->attn_norm_w[l], m->attn_norm_b[l], m->eps);

        /* ---- FFN: up -> exact-erf GELU -> down (no gate) ---- */
        ggml_tensor *ffn = ggml_mul_mat(ctx, m->up_w[l], h);
        if (m->up_b[l]) ffn = ggml_add(ctx, ffn, m->up_b[l]);
        ffn = ggml_gelu_erf(ctx, ffn);
        ffn = ggml_mul_mat(ctx, m->down_w[l], ffn);
        if (m->down_b[l]) ffn = ggml_add(ctx, ffn, m->down_b[l]);
        h = ggml_add(ctx, h, ffn);
        h = build_norm(ctx, h, m->ffn_norm_w[l], m->ffn_norm_b[l], m->eps);
    }

    /* Optional final norm: absent in HF-faithful BERT GGUFs (bge-small),
     * where the last block's post-FFN layer_output_norm already IS the final
     * encoder LayerNorm — applying another norm there would corrupt every
     * embedding.  Apply only when the GGUF actually ships output_norm. */
    if (m->out_norm_w)
        h = build_norm(ctx, h, m->out_norm_w, m->out_norm_b, m->eps);

    /* run on the CPU backend (weights + intermediates live in plain
     * ggml_contexts, which is fine for CPU-only compute — the CPU backend's
     * graph_compute is a direct ggml_graph_compute over those pointers). */
    ggml_cgraph *gcalc = ggml_new_graph(ctx);
    ggml_build_forward_expand(gcalc, h);
    if (ggml_backend_graph_compute(m->backend, gcalc) != GGML_STATUS_SUCCESS) {
        snprintf(err, errlen, "ggml_backend_graph_compute failed");
        ggml_free(ctx);
        return -1;
    }

    /* CLS pooling: token 0 is the first n_embd floats (ne0 contiguous). */
    const float *hidden = (const float *)h->data;
    double norm = 0.0;
    for (int i = 0; i < n_embd; i++) norm += (double)hidden[i] * (double)hidden[i];
    norm = sqrt(norm);
    if (norm < 1e-12) norm = 1e-12;
    for (int i = 0; i < n_embd; i++) out[i] = (float)((double)hidden[i] / norm);

    ggml_free(ctx);
    return 0;
}

int bert_embed(bert_model *m, const char *text, float *out,
               char *err, size_t errlen) {
    int ids[1024];
    /* never let GGUF metadata (max_len) grow past the physical buffer:
     * wp_tokenize truncates to whichever limit is smaller. */
    int cap = m->max_len < (int)(sizeof ids / sizeof ids[0])
            ? m->max_len : (int)(sizeof ids / sizeof ids[0]);
    int n = wp_tokenize(&m->vocab, text, m->cls_id, m->sep_id, m->unk_id,
                        ids, cap);
    if (n < 2) { snprintf(err, errlen, "tokenization produced %d ids", n); return -1; }
    return bert_embed_ids(m, ids, n, out, err, errlen);
}

/* ── dump ───────────────────────────────────────────────────────────────── */

void bert_dump_tensors(const char *path) {
    ggml_context *meta = NULL;
    struct gguf_init_params ip;
    ip.no_alloc = true;   /* dump reads names/dims only — skip the blob */
    ip.ctx = &meta;
    gguf_context *gf = gguf_init_from_file(path, ip);
    if (!gf) { fprintf(stderr, "cannot open GGUF '%s'\n", path); return; }
    int64_t n = gguf_get_n_tensors(gf);
    printf("GGUF %s: %lld tensors\n", path, (long long)n);
    for (int64_t i = 0; i < n; i++) {
        const char *name = gguf_get_tensor_name(gf, i);
        ggml_tensor *t = ggml_get_tensor(meta, name);
        if (!t) { printf("  %-40s <no meta>\n", name); continue; }
        printf("  %-40s %-8s %lld", name,
               ggml_type_name(t->type), (long long)t->ne[0]);
        for (int d = 1; d < GGML_MAX_DIMS && t->ne[d] > 1; d++)
            printf(" x %lld", (long long)t->ne[d]);
        printf("\n");
    }
    /* metadata of interest */
    const char *keys[] = {
        "general.architecture", "general.name",
        "bert.context_length", "bert.embedding_length", "bert.block_count",
        "bert.attention.head_count", "bert.feed_forward_length",
        "attention.layer_norm_epsilon", "tokenizer.ggml.model",
    };
    for (const char *k : keys) {
        int64_t id = gguf_find_key(gf, k);
        if (id < 0) continue;
        enum gguf_type t = gguf_get_kv_type(gf, id);
        if (t == GGUF_TYPE_STRING)
            printf("kv %-36s (string) %s\n", k, gguf_get_val_str(gf, id));
        else if (t == GGUF_TYPE_FLOAT32)
            printf("kv %-36s (f32) %g\n", k, gguf_get_val_f32(gf, id));
        else if (t == GGUF_TYPE_INT32)
            printf("kv %-36s (i32) %d\n", k, gguf_get_val_i32(gf, id));
        else if (t == GGUF_TYPE_UINT32)
            printf("kv %-36s (u32) %u\n", k, gguf_get_val_u32(gf, id));
    }
    ggml_free(meta);
    gguf_free(gf);
}
