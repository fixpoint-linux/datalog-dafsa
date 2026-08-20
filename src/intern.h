/*
 * intern.h — Term interner: string ↔ u32 symbol-id mapping
 *
 * Forward map:  DAFSA keyed  utf8_bytes \0 sym_id_u32BE
 *   → intern_str() uses dafsa_prefix_enum to check existence.
 *
 * Reverse map:  append-only flat array, indexed by sym_id → const char*
 *
 * Sym ids are 1-based (0 = invalid / not-found sentinel).
 */
#ifndef INTERN_H
#define INTERN_H

#include <stdint.h>
#include <stdio.h>

/* Forward declaration for dafsa */
typedef struct dafsa dafsa;

/* Opaque handle */
typedef struct interner interner;

/* Return the forward DAFSA (symbols DAFSA) from an interner. */
const dafsa *intern_fwd(const interner *ir);

/* ─── Lifecycle ───────────────────────────────────────────────────────── */

interner *intern_create(void);
void      intern_free(interner *ir);

/* ─── Core ops ────────────────────────────────────────────────────────── */

/* Intern a string, return its sym_id (1-based).  Returns 0 on OOM.
 * Idempotent: the same string always returns the same id. */
uint32_t    intern_str(interner *ir, const char *str);

/* NON-MUTATING lookup: return the sym_id for str if already interned, else 0.
 * Never grows the interner / never marks it dirty (unlike intern_str). */
uint32_t    intern_str_find(interner *ir, const char *str);

/* Look up a sym_id → string.  Returns NULL if id is out of range. */
const char *intern_str_of(interner *ir, uint32_t sym_id);

/* ─── Persistence ─────────────────────────────────────────────────────── */

/* Save forward DAFSA + reverse array to files. */
int  intern_save(interner *ir, const char *fwd_path, const char *rev_path);

/* Load from files (or return an empty interner if files don't exist). */
interner *intern_load(const char *fwd_path, const char *rev_path);

/* ─── Dirty tracking (M7: durability ordering invariant) ──────────────── */

/* Returns 1 if the interner has new syms not yet saved to disk. */
int  intern_is_dirty(interner *ir);

/* Clear the dirty flag (caller must have saved first). */
void intern_clear_dirty(interner *ir);

#endif /* INTERN_H */
