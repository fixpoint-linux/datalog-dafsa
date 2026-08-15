/*
 * dl_internal.h — Real dl_db struct shared by dl.c and internal consumers
 *
 * dl.h keeps dl_db opaque (typedef struct dl_db dl_db;).  The authoritative
 * layout lives here so every file that needs to reach into the handle
 * (dl.c, vm.c, compiler.c, permindex.c, dl_cli.c) uses the SAME struct.
 * This eliminates the hand-maintained struct dl_db_internal mirrors that
 * silently drifted when dl_db gained fields (M6 vcache, M7 lock_fd).
 *
 * Include-graph notes (cycle-free):
 *   - dl.h        → dl_fpoint enum, dl_db typedef.  dl.h does NOT include
 *                    this header, so there is no cycle.
 *   - intern.h    → interner (opaque pointer).
 *   - relation.h  → relation (opaque pointer) + rel_entry member.
 *   - snapshot.h  → view_cache_slot + DL_VIEW_CACHE_SZ (concrete array member).
 *   - permindex.h → perm_index_entry + MAX_PERMS (concrete array member).
 *   - compiled_rule is used ONLY as a pointer here, so we forward-declare it
 *     rather than include compiler.h (which would drag in parser.h/regexwalk.h).
 */
#ifndef DL_INTERNAL_H
#define DL_INTERNAL_H

#include "dl.h"
#include "intern.h"
#include "relation.h"
#include "snapshot.h"
#include "permindex.h"
#include "compiler.h"   /* compiled_rule (anonymous-struct typedef — cannot
                           be forward-declared; compiler.h includes dl.h but
                           NOT dl_internal.h, so no cycle) */

#include <stddef.h>
#include <stdint.h>

/* ─── Constants ────────────────────────────────────────────────────────── */

#define MAX_RELS 64  /* enough for M0 */

/* ─── Authoritative dl_db layout ───────────────────────────────────────── */

typedef struct {
    char     *name;
    relation *rel;
} rel_entry;

struct dl_db {
    char      *dir;
    interner  *ir;
    rel_entry  rels[MAX_RELS];
    size_t     nrels;
    int        lock_fd;    /* M7: fcntl lock file descriptor, or -1 */

    /* M1: compiled rules */
    compiled_rule **crules;
    int             n_crules;

    /* M4: snapshot support */
    int              fixpoint_dirty;  /* 1 if rules loaded / facts changed
                                         since last compile/publish */
    uint32_t         snap_version;    /* current snapshot version, 0=none */
    view_cache_slot  vcache[DL_VIEW_CACHE_SZ];
    int            (*fault_hook)(dl_fpoint, void *);
    void            *fault_user;

    /* M6: permutation indices */
    perm_index_entry  perms[MAX_PERMS];
    int               n_perms;

    /* M8: retained rule AST (deep-copied in dl_load_rules; freed in
     * dl_close).  Used by the magic-sets transform (dl_query_magic). */
    rule            **ast_rules;
    int               n_ast_rules;

    /* IVM Slice 1: insert-only incremental maintenance state.
     *   full_reeval_pending — 1 forces the next publish/compile to run the
     *     FULL fixpoint (the correct oracle) instead of delta propagation.
     *     Set on ANY change outside the IVM-insert class: bulk load, rule
     *     load, a base fact into a rule-head relation, or a rule that is
     *     recursive / negated / aggregate / pattern-walk / perm-join.
     *   delta_pending[i] — pending +delta tuple_set for relation i, captured
     *     at dl_add_fact after the in-memory base commit.  NULL when empty.
     *     Insert-only: only +tuples.
     * IVM Slice 3: deletion state.
     *   del_pending[i] — pending -delta tuple_set for relation i, captured at
     *     dl_delete_fact after the in-memory base commit.  NULL when empty.
     *     Consumed by vm_dred_delete (DRed over-delete + re-derive) at the
     *     next publish; a program outside the DRed class routes to the full
     *     re-eval fallback via the publish dispatch (never mis-evaluate). */
    int                full_reeval_pending;
    struct tuple_set  *delta_pending[MAX_RELS];  /* struct tuple_set forward-
                                                    declared in relation.h;
                                                    full def in tupleset.h */
    struct tuple_set  *del_pending[MAX_RELS];    /* pending -deltas (Slice 3) */
};

#endif /* DL_INTERNAL_H */
