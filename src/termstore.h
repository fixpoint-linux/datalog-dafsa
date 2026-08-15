/*
 * termstore.h — Hash-consed LIST term store (v2 Datalog lists)
 *
 * A list is a structural, first-class column value: a DAG of cons cells
 * interned into a canonical handle.  Equal lists share ONE handle, so list
 * equality is plain u32== (the OP_EQ / OP_EQ_CONST bytecode compare handles
 * directly — no deep-equality opcode).
 *
 * Handles are allocated from a RESERVED HIGH RANGE [TERM_BASE, ...):
 *     handle = TERM_BASE + node_index
 * This is disjoint from sym_ids (1..N, small) and from 0 (intern_str's
 * invalid sentinel) and from 0xFFFFFFFF (the VM binding UNBOUND sentinel).
 * The empty list NIL is the store's node 0 (handle == TERM_BASE), so
 * is_list([]) is true and car([])/cdr([]) must backtrack (handled by the VM).
 *
 * is_list(v) is an EXACT index-range test (v >= TERM_BASE &&
 * v - TERM_BASE < node_count) — NOT the reverse-map heuristic used for
 * sym_ids.  The int-vs-handle collision is closed at the PARSER boundary:
 * Datalog-source integer literals >= TERM_BASE are rejected (a 31-bit
 * literal cap, user-accepted).  Bulk CSV data keeps legacy raw-u32 semantics,
 * so a high-bit CSV int carries the same documented residual alias risk as the
 * pre-existing int-vs-symbol B6 collision.  Note the sharpest edge is the
 * value TERM_BASE itself (0x80000000): NIL is always node 0, so a CSV int
 * exactly 0x80000000 is treated as the empty list EVEN when the store holds no
 * other nodes (term_is_list is a pure range test: v-TERM_BASE==0 < count==1).
 * For any store with N nodes, CSV ints in [TERM_BASE, TERM_BASE+N) alias list
 * handles.  This is inherent to a tagless u32 column namespace and accepted;
 * list-consuming queries on such legacy data are outside the supported use.
 *
 * A node's tail is always a list (term_cons rejects a non-list tail), so the
 * DAG is well-founded (acyclic): every cons node references only strictly
 * earlier nodes, and car/cdr/append traversal terminates.
 */
#ifndef TERMSTORE_H
#define TERMSTORE_H

#include <stdint.h>

#define TERM_BASE 0x80000000UL
#define TERM_NIL  ((uint32_t)TERM_BASE)   /* empty list, node 0 */

/* Opaque handle */
typedef struct termstore termstore;

/* ─── Lifecycle ───────────────────────────────────────────────────────── */

/* Create a term store with NIL pre-allocated as node 0.  Returns NULL on OOM. */
termstore *term_create(void);
void       term_free(termstore *t);

/* ─── Core ops ────────────────────────────────────────────────────────── */

/* 1 if v is a list handle in this store (EXACT index-range test). */
int term_is_list(const termstore *t, uint32_t v);

/* head / tail of a cons cell.  Precondition: h is a list handle and
 * h != TERM_NIL; returns 0 (garbage) otherwise — the caller (VM) performs the
 * definitive is_list / non-NIL check before calling. */
uint32_t term_car(const termstore *t, uint32_t h);
uint32_t term_cdr(const termstore *t, uint32_t h);

/* Intern the cons cell (head, tail).  head is any u32 (int / sym_id / nested
 * list); tail MUST be a list.  Returns the canonical handle, or 0 on OOM or
 * a non-list tail.  Hash-consing makes equal cells share one handle. */
uint32_t term_cons(termstore *t, uint32_t head, uint32_t tail);

/* append(a, b): the list a followed by the list b.  Returns the canonical
 * handle, or 0 on OOM or a non-list operand.  append(NIL, b) == b. */
uint32_t term_append(termstore *t, uint32_t a, uint32_t b);

/* Number of elements in the list h (0 for NIL; 0 if h is not a list). */
uint32_t term_length(const termstore *t, uint32_t h);

/* Number of nodes in the store (NIL inclusive). */
uint32_t term_node_count(const termstore *t);

/* ─── Persistence ─────────────────────────────────────────────────────── */

/* 1 if new nodes were interned since the last term_save. */
int  term_is_dirty(const termstore *t);
void term_clear_dirty(termstore *t);

/* Save the store to `path` atomically (tmp + fsync + rename + dir-fsync).
 * Format: a count header line followed by one "head tail" decimal line per
 * node in handle order (node 0 = NIL, "0 0").  Handles are IMPLICIT
 * (TERM_BASE + row index), so reload is stable and the hash-cons table is
 * rebuilt.  Returns 0 on success, -1 on error. */
int term_save(termstore *t, const char *path);

/* Load the store from `path`.  Returns an EMPTY store (NIL only) if the file
 * does not exist (backward-compat: old DBs have no terms.bin).  Returns NULL
 * only on OOM or a corrupt file. */
termstore *term_load(const char *path);

#endif /* TERMSTORE_H */
