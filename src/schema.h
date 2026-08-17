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
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Capacity caps (self-contained; distinct from internal MAX_RELS/ARITY) */
#define DL_SCHEMA_MAX_RELS  64
#define DL_SCHEMA_MAX_ARITY 8
#define DL_SCHEMA_NAME_MAX  64
#define DL_ENUM_MAX_VALUES  8
#define DL_ENUM_VALUE_MAX   32

/* Column type tag. Flat scalars are raw u32; parameterized carry a param. */
typedef enum {
    DLT_NATURAL   = 1,  /* raw u32 */
    DLT_TEXT      = 2,  /* interned sym_id */
    DLT_BOOL      = 3,  /* raw u32 0/1 */
    DLT_CHAR      = 4,  /* raw u32 Unicode scalar value */
    DLT_DATE      = 5,  /* raw u32 yyyymmdd */
    DLT_TIMESTAMP = 6,  /* raw u32 unix (epoch) seconds */
    DLT_SIGNED    = 7,  /* raw u32 zigzag(i32) */
    DLT_LIST      = 8,  /* term handle; param.elem = element dl_coltype */
    DLT_OPTIONAL  = 9,  /* param.elem; None = 0xFFFFFFFF sentinel */
    DLT_ENUM      = 10, /* param.evalues[0..n_evalues-1]; value = interned sym_id */
} dl_coltype;

typedef struct {
    dl_coltype tag;
    dl_coltype elem;                                    /* LIST/OPTIONAL element (flat scalar only, v1) */
    char       evalues[DL_ENUM_MAX_VALUES][DL_ENUM_VALUE_MAX]; /* ENUM value set (strings, interned at DATA-load) */
    uint8_t    n_evalues;                               /* ENUM cardinality */
} dl_colspec;

/* A single relation definition: fixed name, fixed arity (1..8), and the
 * per-column type for each of the arity columns.  `is_idb` marks derived
 * (rule-head) relations; loading data into an IDB is an error. */
typedef struct {
    char       name[DL_SCHEMA_NAME_MAX];
    uint8_t    arity;
    uint8_t    is_idb;
    dl_colspec cols[DL_SCHEMA_MAX_ARITY];
} dl_reldef;

/* A complete schema: an ordered array of relation definitions. */
typedef struct dl_schema {
    int        n_rels;
    dl_reldef  rels[DL_SCHEMA_MAX_RELS];
} dl_schema;

/* Structural type equality (tag + elem for LIST/OPTIONAL; tag + value set for
 * ENUM; tag alone for flat). Use in constrain_var and type_equality. */
static inline int dl_colspec_eq(dl_colspec a, dl_colspec b) {
    if (a.tag != b.tag) return 0;
    if (a.tag == DLT_LIST || a.tag == DLT_OPTIONAL) return a.elem == b.elem;
    if (a.tag == DLT_ENUM)
        return a.n_evalues == b.n_evalues &&
               memcmp(a.evalues, b.evalues,
                      (size_t)a.n_evalues * DL_ENUM_VALUE_MAX) == 0;
    return 1;
}

/* Add a relation definition to the schema.  `cols` must hold at least
 * `arity` dl_colspec values.
 *
 * Returns 0 on success, -1 on error:
 *   - NULL s / name / cols
 *   - arity < 1 or arity > DL_SCHEMA_MAX_ARITY
 *   - duplicate name (case-sensitive) already in the schema
 *   - schema full (n_rels == DL_SCHEMA_MAX_RELS)
 * The name is copied (strncpy, NUL-terminated); the caller's buffer is not
 * retained. */
int dl_schema_add(dl_schema *s, const char *name, uint8_t arity,
                  const dl_colspec *cols, int is_idb);

/* Look up a relation definition by name.  Returns a pointer to the entry in
 * the schema (valid while the schema outlives the call), or NULL if not
 * found / NULL s / NULL name. */
const dl_reldef *dl_schema_find(const dl_schema *s, const char *name);

/* Rule typechecker hook, invoked by dl_load_rules when a schema is attached.
 * `rules` is the parser's rule** (opaque here to keep this header free of
 * parser.h).  Returns 0 if the rules typecheck against `schema`, -1 on
 * failure with a human-readable diagnostic written into errbuf (errcap bytes).
 * `srcname` names the source file in diagnostics (NULL => the literal
 * `<input>`); the dlp tool passes the real rules-file path so errors show the
 * actual file.
 *
 * Implemented in src/typecheck.c (S3). */
int dl_typecheck_rules(const dl_schema *schema, void *rules, int n_rules,
                       const char *srcname, char *errbuf, size_t errcap);

#ifdef __cplusplus
}
#endif

#endif /* DL_SCHEMA_H */
