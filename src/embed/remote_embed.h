/*
 * remote_embed.h — OpenAI-compatible /v1/embeddings client for the embedder.
 *
 * When DL_EMBED_API_URL is set, corpus build (dl-embed pipeline) and query
 * encode (dl-embed encode / embed, and libembed.so's dl_embed_encode_query)
 * route embeddings through a remote OpenAI-compatible endpoint instead of the
 * local CPU ggml bert model.  The remote model is the same bge-small-en-v1.5
 * GGUF (384-dim) served on ollama (babylon P40), so the vector space is
 * identical and the existing 384-dim vector tier stays byte-compatible.
 *
 * Unset -> remote_embed_configured() is false and callers keep the local path.
 */
#ifndef REMOTE_EMBED_H
#define REMOTE_EMBED_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* True iff DL_EMBED_API_URL is set and non-empty. */
int remote_embed_configured(void);

/* Embed n texts into out (row-major n*384 floats).  Single-query callers pass
 * n=1.  Returns 0 on success, -1 on error (err set, errlen-cap). */
int remote_embed(const char *const *texts, int n, float *out,
                 char *err, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif /* REMOTE_EMBED_H */
