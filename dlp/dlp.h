/* dlp.h — shared header for the dlp (dl-project) tool.
 *
 * dlp links the datalog-dafsa engine (dl_schema, dl_schema_add) with the
 * dhall-c interpreter core to scaffold a project and load/walk a schema.dhall
 * into a typed dl_schema.  S4 scope: `dlp init` + `dlp schema`/`check-schema`.
 */
#ifndef DLP_H
#define DLP_H

#include "schema.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Parse + typecheck + normalize + walk a Dhall schema file into a dl_schema.
 * `path` is the schema.dhall file.  Returns 0 on success, -1 on failure with a
 * human-readable diagnostic written into errbuf (errcap bytes). */
int dlp_schema_load(dl_schema *s, const char *path, char *errbuf, size_t errcap);

/* Scaffold a new project directory: schema.dhall (worked-example template),
 * data/, rules/, .build/.  Returns 0 on success, -1 on failure with errbuf. */
int dlp_project_init(const char *dir, char *errbuf, size_t errcap);

#ifdef __cplusplus
}
#endif

#endif /* DLP_H */
