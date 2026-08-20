/*
 * test_bert_load.cpp — bert_load contract test on a SYNTHETIC GGUF.
 *
 * Needs vendor/ggml (cmake-built static libs) but NOT the 67 MB model:
 * a tiny 2-layer BERT-layout GGUF is written in-process via gguf's writer
 * API, then loaded through bert_load / driven through bert_embed_ids.
 *
 * Pins the load contract against the real CompendiumLabs bge GGUF layout
 * (verified via `dl-embed dump-tensors` on bge-small-en-v1.5-f16.gguf):
 *   token_embd / position_embd / token_types / token_embd_norm
 *   blk.N.attn_{q,k,v,output}[.weight,.bias]
 *   blk.N.attn_output_norm.{weight,bias}
 *   blk.N.ffn_{up,down}[.weight,.bias]
 *   blk.N.layer_output_norm.{weight,bias}
 *   — and NO output_norm (HF BERT has no separate final encoder LayerNorm;
 *     the last block's post-FFN layer_output_norm IS the final norm).
 *
 * Gates:
 *   1. no-output_norm GGUF loads (regression: bert_load used to die with
 *      "required GGUF tensor 'output_norm.weight' missing") and forwards
 *      to a finite, L2-normalized embedding.
 *   2. GGUF WITH output_norm also loads, and the final norm is APPLIED
 *      when present (embedding differs from the no-output_norm run).
 *   3. a GGUF missing a genuinely required tensor (token_embd.weight)
 *      still fails to load with a descriptive error.
 *
 * Build (wired into the Makefile alongside dl-embed):
 *   g++ -O2 -Wall -Wextra -std=c++17 -Ivendor/ggml/include -Isrc \
 *       tests/test_bert_load.cpp src/embed/bert.o src/embed/tokenizer.o \
 *       -o tests/test_bert_load -Lbuild-tmp/ggml/src \
 *       -lggml -lggml-cpu -lggml-base -lpthread -lm
 */
#include "../src/embed/bert.h"

#include "ggml.h"
#include "gguf.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/* tiny deterministic xorshift32 fill — same stream for both variants so
 * the only difference between GGUF A and GGUF B is output_norm itself */
static unsigned rng_state = 42u;
static float frnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return ((float)(rng_state % 20001u) - 10000.0f) / 10000.0f;
}

static void die(const char *msg) {
    fprintf(stderr, "test_bert_load: FAIL: %s\n", msg);
    exit(1);
}

/* model geometry (kept tiny but structurally faithful to bge-small) */
enum { N_VOCAB = 32, N_EMBD = 16, N_HEAD = 4, N_FF = 32, N_POS = 16, N_LAYER = 2 };

static ggml_tensor *mk_f32(ggml_context *c, const char *name,
                           int64_t ne0, int64_t ne1) {
    ggml_tensor *t = (ne1 > 1)
        ? ggml_new_tensor_2d(c, GGML_TYPE_F32, ne0, ne1)
        : ggml_new_tensor_1d(c, GGML_TYPE_F32, ne0);
    ggml_set_name(t, name);
    int64_t n = ne0 * (ne1 > 1 ? ne1 : 1);
    float *p = (float *)t->data;
    for (int64_t i = 0; i < n; i++) p[i] = frnd();
    return t;
}

static void add_kv_and_write(gguf_context *g, const char *path) {
    static const char *toks[N_VOCAB] = {
        "[PAD]", "[CLS]", "[SEP]", "[UNK]",
        "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l",
        "m", "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x",
        "y", "z", "hello", "world",
    };
    int32_t types[N_VOCAB] = {0};
    gguf_set_arr_str(g, "tokenizer.ggml.tokens", toks, N_VOCAB);
    gguf_set_arr_data(g, "tokenizer.ggml.token_type", GGUF_TYPE_INT32,
                      types, N_VOCAB);
    gguf_set_val_i32(g, "bert.attention.head_count", N_HEAD);
    gguf_set_val_f32(g, "attention.layer_norm_epsilon", 1e-5f);
    if (!gguf_write_to_file(g, path, /*only_meta=*/false))
        die("gguf_write_to_file failed");
}

static void write_model(const char *path, bool with_out_norm,
                        bool drop_token_embd) {
    rng_state = 42u;                       /* identical weights each call */

    struct ggml_init_params ip;
    ip.mem_size   = 8u * 1024u * 1024u;
    ip.mem_buffer = NULL;
    ip.no_alloc   = false;
    ggml_context *c = ggml_init(ip);
    gguf_context *g = gguf_init_empty();
    if (!c || !g) die("ggml_init/gguf_init_empty failed");

    if (!drop_token_embd)
        gguf_add_tensor(g, mk_f32(c, "token_embd.weight", N_EMBD, N_VOCAB));
    gguf_add_tensor(g, mk_f32(c, "position_embd.weight", N_EMBD, N_POS));
    gguf_add_tensor(g, mk_f32(c, "token_types.weight", N_EMBD, 2));
    gguf_add_tensor(g, mk_f32(c, "token_embd_norm.weight", N_EMBD, 1));
    gguf_add_tensor(g, mk_f32(c, "token_embd_norm.bias", N_EMBD, 1));

    for (int l = 0; l < N_LAYER; l++) {
        char n[64];
        snprintf(n, sizeof n, "blk.%d.attn_q.weight", l);
        gguf_add_tensor(g, mk_f32(c, n, N_EMBD, N_EMBD));
        snprintf(n, sizeof n, "blk.%d.attn_q.bias", l);
        gguf_add_tensor(g, mk_f32(c, n, N_EMBD, 1));
        snprintf(n, sizeof n, "blk.%d.attn_k.weight", l);
        gguf_add_tensor(g, mk_f32(c, n, N_EMBD, N_EMBD));
        snprintf(n, sizeof n, "blk.%d.attn_k.bias", l);
        gguf_add_tensor(g, mk_f32(c, n, N_EMBD, 1));
        snprintf(n, sizeof n, "blk.%d.attn_v.weight", l);
        gguf_add_tensor(g, mk_f32(c, n, N_EMBD, N_EMBD));
        snprintf(n, sizeof n, "blk.%d.attn_v.bias", l);
        gguf_add_tensor(g, mk_f32(c, n, N_EMBD, 1));
        snprintf(n, sizeof n, "blk.%d.attn_output.weight", l);
        gguf_add_tensor(g, mk_f32(c, n, N_EMBD, N_EMBD));
        snprintf(n, sizeof n, "blk.%d.attn_output.bias", l);
        gguf_add_tensor(g, mk_f32(c, n, N_EMBD, 1));
        snprintf(n, sizeof n, "blk.%d.attn_output_norm.weight", l);
        gguf_add_tensor(g, mk_f32(c, n, N_EMBD, 1));
        snprintf(n, sizeof n, "blk.%d.attn_output_norm.bias", l);
        gguf_add_tensor(g, mk_f32(c, n, N_EMBD, 1));
        /* ggml mul_mat convention: weights are {ne0=input, ne1=output} */
        snprintf(n, sizeof n, "blk.%d.ffn_up.weight", l);
        gguf_add_tensor(g, mk_f32(c, n, N_EMBD, N_FF));
        snprintf(n, sizeof n, "blk.%d.ffn_up.bias", l);
        gguf_add_tensor(g, mk_f32(c, n, N_FF, 1));
        snprintf(n, sizeof n, "blk.%d.ffn_down.weight", l);
        gguf_add_tensor(g, mk_f32(c, n, N_FF, N_EMBD));
        snprintf(n, sizeof n, "blk.%d.ffn_down.bias", l);
        gguf_add_tensor(g, mk_f32(c, n, N_EMBD, 1));
        snprintf(n, sizeof n, "blk.%d.layer_output_norm.weight", l);
        gguf_add_tensor(g, mk_f32(c, n, N_EMBD, 1));
        snprintf(n, sizeof n, "blk.%d.layer_output_norm.bias", l);
        gguf_add_tensor(g, mk_f32(c, n, N_EMBD, 1));
    }

    if (with_out_norm) {
        gguf_add_tensor(g, mk_f32(c, "output_norm.weight", N_EMBD, 1));
        gguf_add_tensor(g, mk_f32(c, "output_norm.bias", N_EMBD, 1));
    }

    add_kv_and_write(g, path);
    gguf_free(g);
    ggml_free(c);
}

static double l2(const float *v, int n) {
    double s = 0;
    for (int i = 0; i < n; i++) s += (double)v[i] * v[i];
    return sqrt(s);
}

int main(void) {
    const char *path_a = "/tmp/test_bert_load_a.gguf";   /* bge layout    */
    const char *path_b = "/tmp/test_bert_load_b.gguf";   /* + output_norm */
    const char *path_x = "/tmp/test_bert_load_x.gguf";   /* missing embd  */

    write_model(path_a, false, false);
    write_model(path_b, true,  false);
    write_model(path_x, false, true);

    /* ── 1. bge layout (NO output_norm) loads + forwards ─────────────── */
    bert_model ma;
    char err[512];
    if (bert_load(path_a, &ma, err, sizeof err) != 0) {
        fprintf(stderr, "  load A failed: %s\n", err);
        die("no-output_norm GGUF must load (the real bge GGUF has no final norm)");
    }
    if (ma.out_norm_w != NULL) die("out_norm_w should be NULL for bge layout");
    if (ma.n_layer != N_LAYER || ma.n_embd != N_EMBD || ma.n_head != N_HEAD)
        die("geometry mismatch after load");

    int ids[3] = {1, 5, 7};
    float out_a[N_EMBD];
    if (bert_embed_ids(&ma, ids, 3, out_a, err, sizeof err) != 0) {
        fprintf(stderr, "  embed A failed: %s\n", err);
        die("forward pass failed on bge-layout GGUF");
    }
    for (int i = 0; i < N_EMBD; i++)
        if (!std::isfinite(out_a[i])) die("non-finite embedding value");
    double na = l2(out_a, N_EMBD);
    if (fabs(na - 1.0) > 1e-5) die("embedding not L2-normalized");

    /* ── 2. GGUF WITH output_norm: loads and APPLIES the final norm ──── */
    bert_model mb;
    if (bert_load(path_b, &mb, err, sizeof err) != 0) {
        fprintf(stderr, "  load B failed: %s\n", err);
        die("GGUF shipping output_norm must still load");
    }
    if (mb.out_norm_w == NULL) die("out_norm_w must be found when present");
    float out_b[N_EMBD];
    if (bert_embed_ids(&mb, ids, 3, out_b, err, sizeof err) != 0)
        die("forward pass failed on with-output_norm GGUF");
    double maxdiff = 0;
    for (int i = 0; i < N_EMBD; i++) {
        double d = fabs((double)out_a[i] - (double)out_b[i]);
        if (d > maxdiff) maxdiff = d;
    }
    if (maxdiff < 1e-6)
        die("final norm not applied when output_norm present (outputs identical)");

    /* ── 3. missing a genuinely required tensor still fails ───────────── */
    bert_model mx;
    if (bert_load(path_x, &mx, err, sizeof err) == 0)
        die("load must fail when token_embd.weight is missing");
    if (!strstr(err, "token_embd.weight"))
        die("load error does not name the missing tensor");

    bert_free(&ma);
    bert_free(&mb);
    remove(path_a); remove(path_b); remove(path_x);

    printf("test_bert_load: OK (bge layout loads w/o output_norm; "
           "optional final norm applied when present; "
           "missing-tensor error intact; A/B maxdiff %.2e)\n", maxdiff);
    return 0;
}
