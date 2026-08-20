/*
 * csv_emit.h — emit the vector-tier CSVs + metadata byte-identically to
 * scripts/embed.py (values as bare decimal integers, no header, \n rows).
 */
#ifndef EMBED_CSV_EMIT_H
#define EMBED_CSV_EMIT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* __sig{j}__ CSV: rows "band_value,sym_id". */
int csv_write_sig(const char *path, const uint32_t *band_vals,
                  const uint32_t *sym_ids, size_t n);

/* __vec_q__ CSV: rows "sym_id,chunk_idx,packed_u32". */
int csv_write_vecq(const char *path, const uint32_t *syms,
                   const uint32_t *packed, size_t n_rows);

/* __itq_basis__ CSV: rows "i,j,float32_bits" in i-major j-minor order
 * (B is (d x c) row-major float32). */
int csv_write_basis(const char *path, const float *B, int d, int c);

/* vector_metadata.txt: D/c/m/qscale (qscale written %.17g so the exact
 * float32 value round-trips through Python float()). */
int csv_write_metadata(const char *path, int d, int c, int m, double qscale);

/* itq_basis.npy: numpy v1.0, '<f4', C-order (d, c).  Read back by the S4
 * oracle / encode fallback. */
int npy_write_basis(const char *path, const float *B, int d, int c);

/* Read itq_basis.npy; returns 0 and fills B (d*c floats) on success. */
int npy_read_basis(const char *path, float *B, int d, int c);

/* Read qscale from vector_metadata.txt; 0 on success. */
int meta_read_qscale(const char *path, double *qscale);

#ifdef __cplusplus
}
#endif

#endif /* EMBED_CSV_EMIT_H */
