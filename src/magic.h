/*
 * magic.h — Magic-sets transformation (M8 v2 multi-predicate slice)
 *
 * Pure AST→AST adornment rewrite.  magic_transform() takes the retained
 * AST (db->ast_rules) plus a leading-prefix-bound goal and synthesizes an
 * adorned + magic program that, when compiled and run against a scoped
 * clone of the EDB, materializes only the reachable IDB slice.
 *
 * Multi-predicate slice (conservative): the goal IDB predicate's dependency
 * closure is an ACYCLIC DAG of IDB predicates (per-predicate self-recursion
 * through ONE consistent adornment allowed).  Each reachable predicate gets
 * exactly one adornment (worklist propagation from the goal's bound args);
 * adorned + magic rules are synthesized for every reachable predicate.
 * Negation, aggregates, k==0, cross-predicate mutual recursion, multiple
 * distinct adornments for one predicate, and recursive calls needing a
 * different adornment are all REJECTED with a clear reason (never silently
 * mis-evaluated).
 *
 * No dl_db access: the transform works purely on the AST + an interner
 * (the interner is reserved for future adornment decisions; constants are
 * already interned and are copied opaquely as tokens).
 */
#ifndef MAGIC_H
#define MAGIC_H

#include "parser.h"
#include "intern.h"
#include <stddef.h>
#include <stdint.h>

/* A predicate adornment: e.g. pred="tc", arity=2, adorn="bf" */
typedef struct {
    char    pred[64];
    uint8_t arity;
    char    adorn[9];
} magic_adornment;

/* One relation to declare in the scoped-eval clone (name + arity). */
typedef struct {
    char    name[64];
    uint8_t arity;
} magic_decl;

/* Output of the transformation: a fresh program + the decls it needs. */
typedef struct {
    rule       **rules;          /* owned: modified + magic rules */
    int          n_rules;
    magic_decl  *decls;          /* owned: relations to pre-declare */
    int          n_decls;
    char         adorned_goal[80]; /* e.g. "tc__bf" */
    uint8_t      goal_arity;
} magic_program;

/*
 * Transform the retained AST into an adorned + magic program for an
 * ARBITRARY goal adornment (non-leading slice).
 *
 *   ast_rules/n_ast : retained AST (db->ast_rules) — read-only.
 *   goal_pred       : predicate to bind (must be a rule head).
 *   goal_arity      : arity of goal_pred.
 *   adorn           : NUL-terminated string of goal_arity chars, each 'b'
 *                     (bound) or 'f' (free).  Copied verbatim as the goal's
 *                     adornment; propagated through the worklist.
 *   vals/nvals      : the bound values (packed in left-to-right bound-position
 *                     order).  Unused by the transform (the caller seeds the
 *                     magic relation); validated only for count == 'b' count.
 *   ir              : interner (unused; kept for plan parity).
 *   out             : receives the synthesized program (caller must free via
 *                     magic_program_free).  Zero-initialised on entry.
 *   reject_reason/sz: on reject, receives a human-readable reason.
 *
 * Returns 0 on success, -1 on reject/error (reject_reason set).
 */
int  magic_transform_adorn(const rule *const *ast_rules, int n_ast,
                           const char *goal_pred, uint8_t goal_arity,
                           const char *adorn, const uint32_t *vals,
                           uint8_t nvals,
                           interner *ir, magic_program *out,
                           char *reject_reason, size_t reject_sz);

/*
 * Leading-prefix shorthand for magic_transform_adorn: synthesizes
 * adorn = "b"*k + "f"*(goal_arity-k) and routes to magic_transform_adorn.
 */
int  magic_transform(const rule *const *ast_rules, int n_ast,
                     const char *goal_pred, uint8_t goal_arity,
                     const uint32_t *leading, uint8_t k,
                     interner *ir, magic_program *out,
                     char *reject_reason, size_t reject_sz);

/* Free everything owned by a magic_program. */
void magic_program_free(magic_program *p);

#endif /* MAGIC_H */
