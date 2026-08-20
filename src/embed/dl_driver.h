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

#ifdef __cplusplus
}
#endif

#endif /* EMBED_DL_DRIVER_H */
