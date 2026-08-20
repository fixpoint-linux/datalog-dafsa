/*
 * bert.h — bge-small-en-v1.5 (BERT) GGUF loader + forward pass on ggml.
 *
 * Structure verified against ggml-org/llama.cpp src/models/bert.cpp
 * (LLM_ARCH_BERT, pinned a30273376): POST-LayerNorm, learned position
 * embeddings + token types, embeddings LayerNorm (token_embd_norm — REQUIRED),
 * per-layer attn_output_norm + layer_output_norm (accepting the older
 * blk.N.output_norm conversion name), padding-free single-sequence mask
 * (all positions attend), exact-erf GELU, FFN without gate, CLS pooling
 * (hidden[0] + L2 normalize, NO pooler layer).
 *
 * This module is the ONLY dl-embed module that includes ggml headers; it is
 * compiled only for the `dl-embed` target (vendor/ggml + cmake required).
 */
#ifndef EMBED_BERT_H
#define EMBED_BERT_H

#include "tokenizer.h"

#include <stdint.h>
#include <stddef.h>

struct ggml_context;
struct gguf_context;
struct ggml_tensor;

typedef struct {
    gguf_context *gf;        /* owns the mmapped tensor data            */
    ggml_context *wctx;      /* weight tensor structs (no_alloc)        */
    int   n_layer, n_embd, n_head, n_vocab, n_pos;
    float eps;

    /* embeddings */
    ggml_tensor *tok_embd, *pos_embd, *tok_types;
    ggml_tensor *embd_norm_w, *embd_norm_b;
    ggml_tensor *out_norm_w, *out_norm_b;      /* final norm */

    /* per layer */
    ggml_tensor **wq, **bq, **wk, **bk, **wv, **bv;
    ggml_tensor **wo, **bo;
    ggml_tensor **attn_norm_w, **attn_norm_b;  /* post-attn LN           */
    ggml_tensor **up_w, **up_b, **down_w, **down_b;
    ggml_tensor **ffn_norm_w, **ffn_norm_b;    /* post-FFN LN            */

    /* tokenizer vocab loaded from GGUF metadata */
    wp_vocab vocab;
    char   **vocab_storage;                    /* owning strings         */
    int32_t *types_storage;
    int      cls_id, sep_id, unk_id, pad_id;
    int      max_len;                          /* 512                    */
} bert_model;

/* Load a bge-small-en-v1.5 GGUF.  Returns 0 on success; on failure returns
 * -1 and writes a message into err. */
int  bert_load(const char *path, bert_model *m, char *err, size_t errlen);
void bert_free(bert_model *m);

/* Tokenize + forward pass.  out receives n_embd floats (CLS-pooled,
 * L2-normalized).  Returns 0 or -1 (err message). */
int  bert_embed(bert_model *m, const char *text, float *out,
                char *err, size_t errlen);

/* Raw ids path (used by --dump-tensors / tests). */
int  bert_embed_ids(bert_model *m, const int *ids, int n_ids, float *out,
                    char *err, size_t errlen);

/* Print all GGUF tensor names/types/dims to stdout. */
void bert_dump_tensors(const char *path);

#endif /* EMBED_BERT_H */
