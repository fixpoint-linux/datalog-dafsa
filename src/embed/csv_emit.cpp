/*
 * csv_emit.cpp — byte-identical CSV emission (see csv_emit.h).  Row order
 * and formats match scripts/embed.py exactly:
 *   sig_j   : (band_value, sym_id)     per entity, entity walk order
 *   vec_q   : (sym_id, chunk, packed)  entity-major then chunk
 *   basis   : (i, j, bits)             i-major j-minor
 */
#include "csv_emit.h"
#include "vec_bits.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int csv_write_sig(const char *path, const uint32_t *band_vals,
                  const uint32_t *sym_ids, size_t n) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    for (size_t k = 0; k < n; k++)
        fprintf(f, "%u,%u\n", band_vals[k], sym_ids[k]);
    fclose(f);
    return 0;
}

int csv_write_vecq(const char *path, const uint32_t *syms,
                   const uint32_t *packed, size_t n_rows) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    for (size_t k = 0; k < n_rows; k++)
        fprintf(f, "%u,%u,%u\n", syms[k], (uint32_t)(k % 96u), packed[k]);
    fclose(f);
    return 0;
}

int csv_write_basis(const char *path, const float *B, int d, int c) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    for (int i = 0; i < d; i++)
        for (int j = 0; j < c; j++)
            fprintf(f, "%d,%d,%u\n", i, j, vec_float32_bits(B[i * c + j]));
    fclose(f);
    return 0;
}

int csv_write_metadata(const char *path, int d, int c, int m, double qscale) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "D=%d\nc=%d\nm=%d\nqscale=%.17g\n", d, c, m, qscale);
    fclose(f);
    return 0;
}

int npy_write_basis(const char *path, const float *B, int d, int c) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    char header[256];
    int hl = snprintf(header, sizeof header,
                      "{'descr': '<f4', 'fortran_order': False, 'shape': (%d, %d), }",
                      d, c);
    /* pad header to 64-byte alignment (numpy v1.0) */
    int total = 10 + hl + 1;                 /* magic(6) + ver(2) + hlen(2) + dict + \n */
    int pad = (64 - (total % 64)) % 64;
    char magic[6] = {(char)0x93, 'N', 'U', 'M', 'P', 'Y'};
    char ver[2] = {1, 0};
    uint16_t hlen = (uint16_t)(hl + 1 + pad);
    fwrite(magic, 1, 6, f);
    fwrite(ver, 1, 2, f);
    fwrite(&hlen, 2, 1, f);                  /* little-endian host */
    fwrite(header, 1, (size_t)hl, f);
    fputc('\n', f);
    for (int p = 0; p < pad; p++) fputc(' ', f);
    fwrite(B, sizeof(float), (size_t)d * c, f);
    fclose(f);
    return 0;
}

int npy_read_basis(const char *path, float *B, int d, int c) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char magic[6];
    if (fread(magic, 1, 6, f) != 6) { fclose(f); return -1; }
    if (memcmp(magic, "\x93NUMPY", 6) != 0) { fclose(f); return -1; }
    char ver[2];
    if (fread(ver, 1, 2, f) != 2) { fclose(f); return -1; }
    uint16_t hlen;
    if (fread(&hlen, 2, 1, f) != 1) { fclose(f); return -1; }
    char *header = (char *)malloc(hlen + 1);
    if (!header) { fclose(f); return -1; }
    if (fread(header, 1, hlen, f) != (size_t)hlen) { free(header); fclose(f); return -1; }
    header[hlen] = '\0';
    /* sanity: '<f4' + fortran_order False + shape (d, c) */
    int ok = strstr(header, "'<f4'") != NULL &&
             strstr(header, "False") != NULL;
    free(header);
    if (!ok) { fclose(f); return -1; }
    size_t want = (size_t)d * c;
    if (fread(B, sizeof(float), want, f) != want) { fclose(f); return -1; }
    fclose(f);
    return 0;
}

int meta_read_qscale(const char *path, double *qscale) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "qscale=", 7) == 0) {
            *qscale = strtod(line + 7, NULL);
            fclose(f);
            return 0;
        }
    }
    fclose(f);
    return -1;
}
