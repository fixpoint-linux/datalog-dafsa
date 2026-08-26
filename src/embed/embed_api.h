/*
 * embed_api.h — extern-C encoder entrypoint exported from libembed.so.
 *
 * Lets a C or Zig host (fx-agent-memory vsearch) encode a query string into
 * the vector-tier signature + int8 vector IN-PROCESS, without shelling out to
 * the dl-embed binary.  The model path comes from the DL_EMBED_MODEL env var
 * or the per-user cache — a .so has no argv0, so the repo-local models/ dir
 * (which dl-embed resolves next to its binary) is NOT used here.
 */
#ifndef EMBED_EMBED_API_H
#define EMBED_EMBED_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Encode a query against the corpus identified by `corpus_suffix` ("" for
 * entity, "_obs" for observation content) in db_dir.
 *
 * Reads the stored ITQ basis (itq_basis<suffix>.npy) and qscale
 * (vector_metadata<suffix>.txt) for that corpus.  On success fills q_sig
 * (VEC_SIG_WORDS=8 u32) and q_ivec (VEC_IVEC_WORDS=96 u32, 4 int8 packed
 * little-endian) and returns 0.  On any failure returns -1 and writes a
 * message into err (errlen bytes). */
int dl_embed_encode_query(const char *db_dir, const char *corpus_suffix,
                          const char *query,
                          uint32_t *q_sig, uint32_t *q_ivec,
                          char *err, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif /* EMBED_EMBED_API_H */
