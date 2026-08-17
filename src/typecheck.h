/*
 * typecheck.h — per-rule Datalog typechecker (S3 of the Dhall-schema feature)
 *
 * The rule typechecker body lives in src/typecheck.c.  Its public entry point
 * is dl_typecheck_rules() (schema.h: takes a `srcname` used in diagnostics;
 * NULL => `<input>`), which is DECLARED in schema.h (kept there so the
 * dl_load_rules schema hook and this module share one signature).  This header
 * exists so the typecheck module has a home header and so any file that wants
 * the typechecker can include it and get the schema types transitively.
 */
#ifndef DL_TYPECHECK_H
#define DL_TYPECHECK_H

/* dl_typecheck_rules / dl_schema / dl_colspec are declared in schema.h. */
#include "schema.h"

#endif /* DL_TYPECHECK_H */
