/*
 * dafsa_internal.h — Shared internals of the incremental-minimal DAFSA.
 *
 * Included by every implementation TU.  Not part of the public API.
 */
#ifndef DAFSA_INTERNAL_H
#define DAFSA_INTERNAL_H

#include "dafsa.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>
#include <assert.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

/* ─── Tunables ──────────────────────────────────────────────────────────── */

#define DAFSA_MAX_STATES_HARD  100000000
#define MAX_WORD_LEN           4096
#define ALPHABET_SZ            256
#define FNV_OFFSET             14695981039346656037ULL
#define FNV_PRIME              1099511628211ULL
#define DAFSA_PDWG_VERSION     4   /* v4: adds trailing CRC32 */
#define DAFSA_INLINE_N          4

/* Hint to prefetch a cache line for reading. No-op on compilers without
 * __builtin_prefetch (MSVC etc.). */
#if defined(__GNUC__) || defined(__clang__)
#define DAFSA_PREFETCH(ptr) __builtin_prefetch((const void *)(ptr), 0, 1)
#else
#define DAFSA_PREFETCH(ptr) ((void)0)
#endif

/* ─── Data structures ──────────────────────────────────────────────────── */

typedef struct {
    unsigned char  sym;
    unsigned int   target;         /* state index */
} Edge;

/* Linked-list node for incoming-edges tracking.
 * Stored as a singly-linked list threaded through a flat array so we
 * don't need malloc/free per edge. */
typedef struct inode {
    unsigned int   parent;         /* which state points here */
    unsigned char  sym;            /* via which character */
    unsigned int   next;           /* index of next inode in parent list, 0=end */
} Inode;

typedef struct TransHeap { uint32_t cap; Edge edges[]; } TransHeap;

typedef struct {
    unsigned int   refcount;       /* @0  number of incoming transitions */
    unsigned char  is_final;       /* @4 */
    unsigned char  _pad0[3];
    unsigned int   ntrans;         /* @8  live transition count */
    unsigned int   in_head;        /* @12 index of first Inode, 0=none */

    uint64_t       sig;            /* @16 cached FNV-1a signature, 0=invalid;
                                    *     also reused as free-list next pointer */

    TransHeap     *trans_heap;     /* @24 NULL => all ≤4 edges inline in trans[] */
    Edge           trans[DAFSA_INLINE_N]; /* @32 4 × 8B = 32B inline edges */
} State;

_Static_assert(sizeof(State) == 64, "State must be one cache line");
_Static_assert(offsetof(State, trans) + DAFSA_INLINE_N * sizeof(Edge) <= 64,
               "inline edges fit in cache line");

struct dafsa {
    unsigned int   nstates;        /* state 0 = implicit dead/sink state, unused */
    unsigned int   initial;        /* initial state id */

    /* Heap arrays with doubling growth */
    State         *states;
    size_t         states_cap;     /* capacity (allocated count) */

    Inode         *inodes;
    size_t         inodes_cap;
    unsigned int   inodes_used;    /* index 0 = sentinel */

    /* Register: open-addressing hash table sig -> state_id */
    uint64_t      *reg_keys;
    uint32_t      *reg_vals;
    size_t         reg_cap;        /* capacity (prime) */
    size_t         reg_used;       /* count of occupied slots */
    uint64_t       reg_probes;

    /* Per-handle scratch for add/delete path traversal.
     * NOT reentrant: a single dafsa must not be mutated concurrently. */
    unsigned int  *spath;
    unsigned char *schars;
    unsigned int  *sparents;
    size_t         scratch_cap;    /* entries (all three share this cap) */

    /* Orphan-state free-list. Freed slots are chained via their `sig` field
     * (reused as a `next` pointer).  0 = empty list. */
    unsigned int   free_head;

    /* Tier-2 order-statistics: per-state DISTINCT-complete-keys-reachable
     * subtree counts (u64 — cross-product relations can exceed 2^32 keys
     * with few states).  Built lazily on first rank/select by
     * dafsa_ensure_subtree (vendor/dafsa_rank.c), invalidated coarsely at the
     * top of dafsa_add_n / dafsa_delete_n.  subtree_valid is 0 until a build
     * completes; 0 also means "OOM'd last build, rebuild on next use". */
    uint64_t       *subtree;
    size_t          subtree_cap;
    int             subtree_valid;
};

/* ─── Write-ahead log (M5) ─────────────────────────────────────────────── */

struct dafsa_wal { int fd; uint64_t size; };

/* ─── WAL overlay for layered read view ────────────────────────────────── */

struct wal_slot { uint8_t payload[8]; uint8_t state; };  /* state: 0=empty, ADD=1, DEL=2 */
struct wal_bucket {
    uint8_t *word;
    uint32_t word_len;
    struct wal_slot *slots;
    size_t slots_cap;    /* power of two */
    size_t slots_used;   /* occupied (non-zero state) */
};
struct wal_overlay {
    struct wal_bucket *buckets;
    size_t buckets_cap;
    size_t buckets_used;
    uint32_t *table;     /* outer hash index → bucket index (UINT32_MAX=empty) */
    size_t table_cap;    /* power of two */
};

/* Zero-copy search-only view: mmaps the on-disk PDWG v3 file and indexes
 * directly into the CSR — no State[]/Edge[] materialization. */
struct dafsa_view {
    uint8_t       *map;          /* mmap'd file, PROT_READ MAP_PRIVATE */
    size_t         map_len;
    uint32_t       n_states;
    uint32_t       initial;      /* == 1 */
    const uint8_t *final_bits;   /* points into map */
    const uint8_t *csr;          /* points into map: first CSR byte */
    uint64_t      *state_off;    /* n_states+2 byte offsets into csr;
                                  * state s spans [csr+off[s], csr+off[s+1])
                                  * off[0]=0; off[n_states+1]==total CSR bytes */
    struct wal_overlay *ov;      /* WAL overlay for layered read, or NULL */
};

/* ─── Transition accessors ─────────────────────────────────────────────── */

static inline Edge *trans_arr(State *s) {
    return s->trans_heap ? s->trans_heap->edges : s->trans;
}
static inline const Edge *trans_arr_c(const State *s) {
    return s->trans_heap ? s->trans_heap->edges : s->trans;
}

/* ─── Cross-file declarations (formerly static) ────────────────────────── */

/* dafsa.c */
int           is_prime(size_t n);
size_t        next_prime(size_t n);
#ifdef DAFSA_DEBUG
void          dafsa_check_invariants(const dafsa *d);
#endif

/* dafsa_state.c */
unsigned int  state_new(dafsa *d);
void          state_detach_from_children(dafsa *d, unsigned int sid);
void          state_free(dafsa *d, unsigned int id);
Inode        *inode_alloc(dafsa *d);
int           dafsa_ensure_scratch(dafsa *d, size_t len);
int           trans_find(const State *s, unsigned char c);
int           trans_reserve(State *s, unsigned int need);
void          trans_add(State *s, unsigned char c, unsigned int tgt);

/* dafsa_core.c */
void          incoming_add(dafsa *d, unsigned int src, unsigned char c,
                           unsigned int dst);
void          incoming_redirect(dafsa *d, unsigned int old_tgt,
                                unsigned int new_tgt);
void          incoming_redirect_one(dafsa *d, unsigned int parent,
                                     unsigned char sym,
                                     unsigned int old_tgt,
                                     unsigned int new_tgt);
uint64_t      sig_compute(const State *s);
unsigned int  reg_lookup(dafsa *d, uint64_t sig);
#ifdef DAFSA_DEBUG
unsigned int  reg_lookup_no_count(const dafsa *d, uint64_t sig);
#endif
void          reg_insert(dafsa *d, uint64_t sig, unsigned int id);
void          reg_grow(dafsa *d);
int           replace_or_register(dafsa *d, unsigned int sid,
                                  unsigned int parent);
unsigned int  clone_state(dafsa *d, unsigned int sid);
int           confluence_path(dafsa *d, unsigned int *path,
                              unsigned int *parents,
                              unsigned int len);

/* dafsa_persist.c */
int           put_u8(FILE *f, uint8_t v, uint32_t *crc);
int           put_uvarint(FILE *f, uint32_t v, uint32_t *crc);
int           put_u16_le(FILE *f, uint16_t v, uint32_t *crc);
int           put_u32_le(FILE *f, uint32_t v, uint32_t *crc);
int           fsync_dir_of(const char *path);
int           mb_u8(const uint8_t **p, const uint8_t *end, uint8_t *out);
int           mb_u16(const uint8_t **p, const uint8_t *end, uint16_t *out);
int           mb_u32(const uint8_t **p, const uint8_t *end, uint32_t *out);
int           mb_uvarint(const uint8_t **p, const uint8_t *end, uint32_t *out);
int           mb_skipvarint(const uint8_t **p, const uint8_t *end);
dafsa        *dafsa_load_impl(const char *path, int mutable);

/* dafsa_rank.c — Tier-2 order-statistics (rank/select/range_count) */
uint64_t      dafsa_ensure_subtree(dafsa *d);
uint64_t      dafsa_rank_n(dafsa *d, const unsigned char *key, size_t len);
int           dafsa_select_n(dafsa *d, uint64_t k, unsigned char *key_out,
                             size_t key_cap);
uint64_t      dafsa_range_count_n(dafsa *d,
                                  const unsigned char *lo, size_t lo_len,
                                  const unsigned char *hi, size_t hi_len);

/* Prefix-bound (start-state) forms: rank/select/range_count within the subtree
 * rooted at state `s` (typically reached by walking a bound prefix). */
uint64_t      dafsa_rank_from(dafsa *d, unsigned int s,
                              const unsigned char *key, size_t len);
int           dafsa_select_from(dafsa *d, unsigned int s, uint64_t k,
                                unsigned char *key_out, size_t key_cap);
uint64_t      dafsa_range_count_from(dafsa *d, unsigned int s,
                                     const unsigned char *lo, size_t lo_len,
                                     const unsigned char *hi, size_t hi_len);

/* dafsa_crc32.c */
extern const uint32_t crc32_table[256];
uint32_t      crc32_init(void);
uint32_t      crc32_update(uint32_t crc, const uint8_t *data, size_t len);
uint32_t      crc32_finalize(uint32_t crc);
uint32_t      crc32_compute(const uint8_t *data, size_t len);

/* dafsa_view.c */
int           enum_dfs(const dafsa *d, unsigned int state, unsigned char *buf,
                       size_t depth, dafsa_enum_cb cb, void *user, long *count);
int           view_trans_find(const dafsa_view *v, uint32_t s,
                              unsigned char sym, uint32_t *target_out);
int           view_edge_next(const dafsa_view *v, uint32_t s,
                             const uint8_t **cursor,
                             unsigned char *sym_out, uint32_t *target_out);
int           view_enum_dfs(const dafsa_view *v, uint32_t state,
                             unsigned char *buf, size_t depth,
                             dafsa_enum_cb cb, void *user, long *count);

/* dafsa_view_rank.c — order-statistics over a zero-copy dafsa_view */
uint64_t      dafsa_view_subtree_counts(const dafsa_view *v,
                                        uint64_t **counts_out);
uint64_t      dafsa_view_rank_n(const dafsa_view *v,
                                const unsigned char *key, size_t len);
int           dafsa_view_select_n(const dafsa_view *v, uint64_t k,
                                  unsigned char *key_out, size_t key_cap);
uint64_t      dafsa_view_range_count_n(const dafsa_view *v,
                                       const unsigned char *lo, size_t lo_len,
                                       const unsigned char *hi, size_t hi_len);

#endif /* DAFSA_INTERNAL_H */
