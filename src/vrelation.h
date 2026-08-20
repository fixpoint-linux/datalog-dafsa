/*
 * vrelation.h — Variable-arity relation = family of fixed-width DAFSA variants
 *
 * A variadic relation R is a set of fixed-width sub-relations R_a, one per
 * arity a in [1..MAX_VAR_ARITY] that actually occurs.  Each R_a is an
 * ordinary `relation` (relation.c) keyed 4*a+1 bytes — the fixed-width key
 * encoding is UNCHANGED.  This is the whole trick (v2 variable-arity plan):
 * byte-prefix lookup, exact lookup, perm indices, WAL, snapshot all stay
 * per-variant and therefore verbatim the current engine.  Variable arity is
 * purely a dispatch/loop over variants.
 *
 * On-disk naming (owned by dl.c, which knows db->dir):
 *   variant a of R  →  <R>.<a>.dafsa  (+ .wal, + .base.dafsa when a rule head)
 * Predicate names cannot contain '.', so variant file names can never
 * collide with a user relation name (parser.c is_pred_char excludes '.').
 *
 * NOT length-tagged: a leading length byte would break the "bind k leading
 * columns = byte-prefix at k*4" linchpin.  NOT sentinel-padded: there is no
 * safe reserved u32 (ints span the full range; sym_ids share the namespace).
 */

#ifndef VRELATION_H
#define VRELATION_H

#include "relation.h"
#include <stdint.h>

struct sym_set;

/* Per-variant arity cap UNCHANGED (<=8, 33-byte key).  Raising the arity cap
 * itself is a separate item — variable arity does not change key width. */

/* Forward declaration for sym_set (from regexwalk.h) */
struct sym_set;
#define MAX_VAR_ARITY 8

typedef struct vrelation vrelation;

/* Visitor over the PRESENT variants.  Called in ascending arity order. */
typedef void (*vrel_iter_cb)(relation *variant, uint8_t arity, void *user);

vrelation *vrel_create(void);
void       vrel_free(vrelation *v);

/* Get-or-create the arity-a variant as a fresh IN-MEMORY relation
 * (rel_create: EDB-aliased base==view, no WAL — the durable open with WAL
 * lives in dl.c, which knows the file paths).  NULL on bad arity or OOM. */
relation *vrel_variant(vrelation *v, uint8_t arity);

/* Peek at variant a without materializing it: NULL if not present.
 * This is the READ path used by the VM (an absent variant reads as empty). */
relation *vrel_variant_or_null(const vrelation *v, uint8_t arity);

/* Attach an already-opened (path-backed, possibly WAL'd) relation as the
 * arity-a variant.  Fails (-1) on bad arity, NULL args, or if the variant
 * already exists (the caller keeps ownership on failure). */
int vrel_attach(vrelation *v, uint8_t arity, relation *r);

/* Iterate present variants (ascending arity). */
void vrel_foreach(vrelation *v, vrel_iter_cb cb, void *user);

/* ─── Base/view partition (rule heads) ─────────────────────────────────── */

/* 1 if ANY present variant is split (base != view), else 0. */
int vrel_any_idb(const vrelation *v);

/* rel_reset_view on every present variant: first call SPLITs base off the
 * view, subsequent calls reset view = copy of base.  Returns 0 on success,
 * -1 on the first failure (earlier variants stay reset). */
int vrel_reset_views(vrelation *v);

/* ─── Fact operations (route by arity) ─────────────────────────────────── */

/* Exact membership against variant[arity] (VIEW).  0 for absent variant. */
int vrel_exact(const vrelation *v, const uint32_t *cols, uint8_t arity);

/* Exact membership against variant[arity] (BASE only). */
int vrel_exact_base(const vrelation *v, const uint32_t *cols, uint8_t arity);

/* ─── Prefix enumeration with a VARIABLE tail ──────────────────────────── */

/* Fan out over every present variant a >= max(k,1), doing a per-variant
 * rel_prefix(leading, k) — each of which is the EXISTING fixed-width
 * byte-prefix walk.  The cb receives the per-tuple arity (its own parameter),
 * so tuples of different arity are naturally disambiguated.  Returns the
 * summed count, or -1 on error.  A cb early-stop (non-zero return) aborts
 * the remaining variants and returns the count so far. */
long vrel_prefix(const vrelation *v, const uint32_t *leading, uint8_t k,
                 rel_enum_cb cb, void *user);

/* Like vrel_prefix but enumerates BASE only (durable EDB facts). */
long vrel_prefix_base(const vrelation *v, const uint32_t *leading, uint8_t k,
                      rel_enum_cb cb, void *user);

/* Regex pattern walk fanned out over present variants.
 * DEPRECATED: kept for backward compatibility with raw-DAFSA tests. */
long vrel_pattern(const vrelation *v, const struct regex_dfa *dfa,
                  rel_enum_cb cb, void *user);

/* Filter tuples by column string-content regex match across variants. */
long vrel_filter_col(const vrelation *v, uint8_t col,
                     const struct sym_set *set,
                     rel_enum_cb cb, void *user);

/* ─── Stats ────────────────────────────────────────────────────────────── */

/* Total fact count across variants (VIEW; compiler join-order hint). */
uint64_t vrel_count(const vrelation *v);

#endif /* VRELATION_H */
