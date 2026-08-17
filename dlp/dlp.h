/* dlp.h — shared header for the dlp (dl-project) tool.
 *
 * dlp links the datalog-dafsa engine (dl_schema, dl_schema_add) with the
 * dhall-c interpreter core to scaffold a project and load/walk a schema.dhall
 * into a typed dl_schema.  S4 scope: `dlp init` + `dlp schema`/`check-schema`.
 */
#ifndef DLP_H
#define DLP_H

#include "schema.h"

/* dl_db is an opaque engine handle (defined in src/dl.h); forward-declared
 * here so the typed CSV loader can take one without dragging dl.h into dlp.h. */
typedef struct dl_db dl_db;

#ifdef __cplusplus
extern "C" {
#endif

/* Parse + typecheck + normalize + walk a Dhall schema file into a dl_schema.
 * `path` is the schema.dhall file.  Returns 0 on success, -1 on failure with a
 * human-readable diagnostic written into errbuf (errcap bytes). */
int dlp_schema_load(dl_schema *s, const char *path, char *errbuf, size_t errcap);

/* Return the name of column `col` (0-based) of relation `rel` in the schema
 * last loaded by dlp_schema_load (retained during the walk — dl_reldef stores
 * types only).  Returns NULL if the relation/column is out of range. */
const char *dlp_schema_colname(const dl_schema *s, const char *rel, int col);

/* Scaffold a new project directory: schema.dhall (worked-example template),
 * data/, rules/, .build/.  Returns 0 on success, -1 on failure with errbuf. */
int dlp_project_init(const char *dir, char *errbuf, size_t errcap);

/* Typed CSV loader (S5).  Loads `path` (CSV with a header row) into relation
 * `rel` of schema `s`, coercing each column against the schema column type.
 * Header columns are matched BY NAME (any order; unknown/missing/duplicate
 * headers are errors).  When db == NULL this is a DRY-RUN: it validates and
 * coerces only (used by `dlp check`, no writes).  When db != NULL the relation
 * must already be declared; validated facts are added via dl_add_fact.
 * Returns the number of facts loaded (or validated in a dry-run) on success,
 * -1 on failure with the first human-readable diagnostic in errbuf. */
int dlp_csv_load(dl_db *db, const dl_schema *s, const char *rel,
                 const char *path, char *errbuf, size_t errcap);

/* S5 workflow commands.  `dir` is a project directory (schema.dhall + data/ +
 * rules/).  Each returns 0 on success, -1 on failure with a diagnostic in
 * errbuf.  check does not write anything; build and query write under dir/.build. */
int dlp_project_check(const char *dir, char *errbuf, size_t errcap);
int dlp_project_build(const char *dir, char *errbuf, size_t errcap);
int dlp_project_query(const char *dir, const char *goal, char *errbuf, size_t errcap);

#ifdef __cplusplus
}
#endif

#endif /* DLP_H */
