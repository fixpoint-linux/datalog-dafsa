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
#include "vrelation.h"
#include "snapshot.h"
#include "permindex.h"
#include "termstore.h"
#include "compiler.h"   /* compiled_rule (anonymous-struct typedef — cannot
                           be forward-declared; compiler.h includes dl.h but
                           NOT dl_internal.h, so no cycle) */

#include <stddef.h>
#include <stdint.h>

/* ─── Constants ────────────────────────────────────────────────────────── */

#define MAX_RELS 64  /* enough for M0 */

/* ─── rel_entry kinds (v2 variable arity) ──────────────────────────────── */

#define RELK_FIXED    0  /* rel_entry.rel is the single fixed-width relation */
#define RELK_VARIADIC 1  /* rel_entry.vrel holds one variant per arity 1..8  */

/* ─── Transaction buffer (CAS optimistic-concurrency, Slice 2) ─────────── */

/* Buffered operation kinds inside an open transaction (db->txn). */
#define TXN_ADD 1
#define TXN_DEL 2
#define TXN_CAS 3

/* A single buffered txn operation.  For TXN_ADD/TXN_DEL, `rel_id`/`arity`/
 * `cols` name the fact; for TXN_CAS, `entity_sym`/`expected`/`next` name the
 * compare-and-swap on the internal "rev" relation. */
typedef struct txn_op {
    int      kind;         /* TXN_ADD / TXN_DEL / TXN_CAS */
    int      rel_id;       /* TXN_ADD/TXN_DEL: find_rel index of the relation */
    uint8_t  arity;        /* TXN_ADD/TXN_DEL: fact arity (1..8) */
    uint32_t cols[8];      /* TXN_ADD/TXN_DEL: fact columns (interned/raw) */
    uint32_t entity_sym;   /* TXN_CAS: interned entity symbol */
    uint32_t expected;     /* TXN_CAS: expected current revision */
    uint32_t next;         /* TXN_CAS: new revision */
} txn_op;

typedef struct txn {
    txn_op  *ops;          /* dynamically grown buffer */
    size_t   nops;         /* number of buffered operations */
    size_t   cap;          /* allocated capacity of ops */
} txn;

/* ─── Authoritative dl_db layout ───────────────────────────────────────── */

/* Kind-tagged entry: exactly one of rel / vrel is non-NULL, per `kind`.
 * The FIXED path is byte-identical to v1 (same relation, same files); a
 * variadic relation is a NEW kind with its own <name>.<a>.* files — a strict
 * on-disk superset, no migration. */
typedef struct {
    char      *name;
    uint8_t    kind;    /* RELK_FIXED or RELK_VARIADIC */
    uint8_t    arity;   /* RELK_FIXED: the fixed arity; RELK_VARIADIC: 0 */
    relation  *rel;     /* RELK_FIXED only (NULL for variadic) */
    vrelation *vrel;    /* RELK_VARIADIC only (NULL for fixed) */
} rel_entry;

struct dl_db {
    char      *dir;
    interner  *ir;
    termstore *terms;      /* v2-lists: hash-consed list term store (terms.bin) */
    rel_entry  rels[MAX_RELS];
    size_t     nrels;
    int        lock_fd;    /* M7: fcntl lock file descriptor, or -1 */
    int        rev_rel_id; /* CAS: cached index of the internal "rev" relation
                              (arity-2 entity→revision), or -1 if unknown */
    txn       *txn;        /* CAS Slice 2: active transaction buffer, or NULL
                              (NULL = no transaction open) */

    /* Dhall typed schema (dl_attach_schema).  Borrowed pointer, never owned;
     * the caller retains the dl_schema.  NULL = no schema attached (the
     * default, since dl_open2 calloc's the struct).  When non-NULL,
     * dl_load_rules typechecks parsed rules against it before compiling. */
    const struct dl_schema *schema;

    /* M1: compiled rules */
    compiled_rule **crules;
    int             n_crules;

    /* M4: snapshot support */
    int              fixpoint_dirty;  /* 1 if rules loaded / facts changed
                                         since last compile/publish */
    uint32_t         snap_version;    /* current snapshot version, 0=none */
    unsigned         snapshot_retain; /* opt-in retention: keep N newest
                                         versions (0=keep-all) */
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

/* ─── rel_entry dispatch helpers (defined in dl.c) ─────────────────────── */

/* 1 if the entry is a variadic relation, else 0. */
int db_entry_is_variadic(const rel_entry *e);

/* 1 if ANY relation in the db is variadic (the incremental-maintenance and
 * magic-sets exclusion test: such programs route to the full fixpoint). */
int db_has_variadic(const dl_db *db);

/* 1 if ANY compiled rule uses a list-CONSTRUCTION/DECOMPOSITION builtin
 * (OP_LIST_CONS/CAR/CDR/APPEND).  Such programs are excluded from the
 * incremental-maintenance classes (IVM/DRed/aggregates) and magic-sets
 * queries and always route to the full fixpoint — never silently
 * mis-evaluated.  List VALUES as pure data (a fact column holding a list
 * handle, with no list builtin) do NOT set this flag. */
int db_has_list_builtin(const dl_db *db);
/* 1 if ANY compiled rule emits OP_RANGE.  Such programs are excluded from
 * the incremental-maintenance classes (IVM/DRed/aggregates) and magic/
 * topdown queries and always route to the full fixpoint. */
int db_has_range_builtin(const dl_db *db);

/* LIVE-mode iterator constructor (defined in iter.c) for the VM's OP_RANGE
 * lazy generator: opens over an already-resolved relation, borrowing rel->d
 * and NEVER routing to the snapshot view (see iter.c). */
dl_iter *dl_iter_open_live(relation *rel, const uint32_t *leading, uint8_t k);

/* READ resolution: for a fixed entry, the relation iff arity matches (else
 * NULL); for a variadic entry, variant[arity] WITHOUT materializing it
 * (NULL = absent variant, which callers treat as an EMPTY relation). */
relation *db_entry_variant_ro(const rel_entry *e, uint8_t arity);

/* WRITE resolution: like db_entry_variant_ro, but a variadic entry with no
 * variant[arity] gets a fresh IN-MEMORY variant (rel_create — no WAL; the
 * durable open is dl.c's variadic_open_variant / dl_ensure_variant).  Used
 * by OP_PROJECT to commit derived tuples of a new arity. */
relation *db_entry_variant_rw(rel_entry *e, uint8_t arity);

/* (db, rel_id, arity)-flavored wrappers — the VM/permindex call shape. */
relation *db_rel_at_arity_ro(const dl_db *db, int rel_id, uint8_t arity);
relation *db_rel_at_arity_rw(dl_db *db, int rel_id, uint8_t arity);

/* Get-or-open (durable, WAL-backed) variant `arity` of relation rel_id.
 * For a fixed relation this just returns the fixed relation iff the arity
 * matches.  On a filesystem-less eval clone (dir==NULL) the variant is
 * created in memory instead.  Returns the relation or NULL on error. */
relation *dl_ensure_variant(dl_db *db, int rel_id, uint8_t arity);

#endif /* DL_INTERNAL_H */
