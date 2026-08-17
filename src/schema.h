/*
 * schema.h — Typed relation schema for the Dhall-driven rule typechecker
 *
 * A `dl_schema` is a set of relation definitions (arity + per-column type)
 * used by the `dlp` tool to typecheck Datalog rules against a Dhall schema
 * before they are compiled.  This header is SELF-CONTAINED: it does NOT
 * include dl_internal.h (which is an internal header, not a public one).
 *
 * S2 scope: schema structs + builder + lookup + the guarded dl_typecheck_rules
 * call site in dl_load_rules.  The real rule typechecker body lands in S3
 * (src/typecheck.c).
 *
 * Capacity caps are defined HERE with distinct names (DL_SCHEMA_*) so they
 * never collide with dl_internal.h's MAX_RELS / compiler.h's MAX_ARITY.
 */
#ifndef DL_SCHEMA_H
#define DL_SCHEMA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Capacity caps (self-contained; distinct from internal MAX_RELS/ARITY) */
#define DL_SCHEMA_MAX_RELS  64
#define DL_SCHEMA_MAX_ARITY 8
#define DL_SCHEMA_NAME_MAX  64

/* Column type of a relation.  DLT_NATURAL -> raw u32; DLT_TEXT -> interned. */
typedef enum { DLT_NATURAL = 1, DLT_TEXT = 2 } dl_coltype;

/* A single relation definition: fixed name, fixed arity (1..8), and the
 * per-column type for each of the arity columns.  `is_idb` marks derived
 * (rule-head) relations; loading data into an IDB is an error. */
typedef struct {
    char        name[DL_SCHEMA_NAME_MAX];
    uint8_t     arity;
    uint8_t     is_idb;
    dl_coltype  cols[DL_SCHEMA_MAX_ARITY];
} dl_reldef;

/* A complete schema: an ordered array of relation definitions. */
typedef struct dl_schema {
    int        n_rels;
    dl_reldef  rels[DL_SCHEMA_MAX_RELS];
} dl_schema;

/* Add a relation definition to the schema.  `cols` must hold at least
 * `arity` dl_coltype values.
 *
 * Returns 0 on success, -1 on error:
 *   - NULL s / name / cols
 *   - arity < 1 or arity > DL_SCHEMA_MAX_ARITY
 *   - duplicate name (case-sensitive) already in the schema
 *   - schema full (n_rels == DL_SCHEMA_MAX_RELS)
 * The name is copied (strncpy, NUL-terminated); the caller's buffer is not
 * retained. */
int dl_schema_add(dl_schema *s, const char *name, uint8_t arity,
                  const dl_coltype *cols, int is_idb);

/* Look up a relation definition by name.  Returns a pointer to the entry in
 * the schema (valid while the schema outlives the call), or NULL if not
 * found / NULL s / NULL name. */
const dl_reldef *dl_schema_find(const dl_schema *s, const char *name);

/* Rule typechecker hook, invoked by dl_load_rules when a schema is attached.
 * `rules` is the parser's rule** (opaque here to keep this header free of
 * parser.h).  Returns 0 if the rules typecheck against `schema`, -1 on
 * failure with a human-readable diagnostic written into errbuf (errcap bytes).
 *
 * Implemented in src/typecheck.c (S3). */
int dl_typecheck_rules(const dl_schema *schema, void *rules, int n_rules,
                       char *errbuf, size_t errcap);

#ifdef __cplusplus
}
#endif

#endif /* DL_SCHEMA_H */
