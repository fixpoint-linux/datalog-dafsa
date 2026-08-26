/*
 * dl_driver.h — drive the ./dl CLI from dl-embed via fork + execv (no
 * shell, no popen — the encode path is injection-free by construction).
 */
#ifndef EMBED_DL_DRIVER_H
#define EMBED_DL_DRIVER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Run `<dl> argv...`, capture stdout into out (NUL-terminated).
 * Returns the child's exit status (0 = ok), or 127 on exec failure,
 * or -1 on other failure. */
int dld_run(const char *dl_path, char *const argv[], char *out, size_t out_cap);

/* `dl -d db prefix <rel> [--raw] [leading...]` -> out lines.
 * Returns 0 on success; lines are NUL-separated in out, count in *n_lines. */
int dld_prefix(const char *dl_path, const char *db, const char *rel,
               int raw, const char *leading, char *out, size_t out_cap,
               int *n_lines);

/* `dl -d db load <csv> --rel <rel>`. */
int dld_load(const char *dl_path, const char *db, const char *csv,
             const char *rel);

/* `dl -d db publish`. */
int dld_publish(const char *dl_path, const char *db);

/* Parse <db>/symbols.array into parallel arrays (malloc'd; caller frees).
 * Line N (1-based) = interned string, sym_id = N.  Returns count or -1. */
int dld_symbols_array(const char *db, char ***names_out, uint32_t **ids_out);

/* `dl -d db prefix --raw <rel>` (k=0 full scan): collect every tuple as raw
 * u32 columns (RAW so content with spaces/unicode never breaks parsing).
 * On success sets *n_rows_out to the tuple count and *cols_out to a malloc'd
 * uint32_t[arity * n_rows] row-major buffer (caller frees).  Returns 0, or -1
 * on any failure (incl. the 16 MiB output-truncation guard). */
int dld_prefix_raw(const char *dl_path, const char *db, const char *rel,
                   uint32_t **cols_out, int *n_rows_out, uint8_t *arity_out);

#ifdef __cplusplus
}
#endif

#endif /* EMBED_DL_DRIVER_H */
