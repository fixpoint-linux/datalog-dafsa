/*
 * dafsa.h — Public API for the Carrasco & Forcada Incremental Minimal DAFSA
 *
 * Deterministic Acyclic Finite State Automaton with incremental add/delete.
 * Opaque handle; all keys are length-delimited (may contain embedded NUL).
 */
#ifndef DAFSA_H
#define DAFSA_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dafsa dafsa;   /* opaque */

/* ─── Lifecycle ──────────────────────────────────────────────────────── */

dafsa *dafsa_create(void);                 /* empty DAFSA; NULL on OOM */
void   dafsa_free(dafsa *d);              /* NULL-safe */

/* Bulk-build a MINIMAL DAFSA from a SORTED, DEDUPLICATED key list in ~linear time.
 * Keys are length-delimited (may contain embedded NUL). Result, when saved via
 * dafsa_save, is byte-identical to building the same keys via dafsa_add_n in any
 * order (both yield the unique minimal DFA and dafsa_save canonicalizes).
 * PRE: keys[] are in unsigned-byte lexicographic order, no duplicates.
 * Returns new handle (free with dafsa_free) or NULL on OOM/bad args.
 * nkeys==0 => empty dafsa. */
dafsa *dafsa_build_sorted(const unsigned char *const *keys,
                          const size_t *lens, size_t nkeys);

/* ─── Persistence (M0 stubs — implemented in M1) ───────────────────── */

dafsa *dafsa_load(const char *path);            /* M0 stub: returns NULL */
dafsa *dafsa_load_readonly(const char *path);   /* fast: search-only, skips inode/register rebuild */
int    dafsa_save(const dafsa *d, const char *path); /* M0 stub: returns -1 */

/* ─── Length-delimited key ops (keys MAY contain NUL) ──────────────── */

int dafsa_add_n    (dafsa *d, const unsigned char *key, size_t len); /* 1=added, 0=dup, -1=err */
int dafsa_lookup_n (const dafsa *d, const unsigned char *key, size_t len); /* 1/0 */
int dafsa_delete_n (dafsa *d, const unsigned char *key, size_t len); /* 1=deleted, 0=absent, -1=err */

/* ─── NUL-terminated convenience (delegate to _n with strlen) ──────── */

int dafsa_add    (dafsa *d, const unsigned char *word);
int dafsa_lookup (const dafsa *d, const unsigned char *word);
int dafsa_delete (dafsa *d, const unsigned char *word);

/* ─── Prefix enumeration (M0 stub — implemented in M1) ─────────────── */

typedef int (*dafsa_enum_cb)(const unsigned char *payload, size_t payload_len, void *user);
long dafsa_prefix_enum(const dafsa *d, const unsigned char *prefix,
                       size_t prefix_len, dafsa_enum_cb cb, void *user);

/* ─── Zero-copy search-only view (M4) ─────────────────────────────────── */
typedef struct dafsa_view dafsa_view;   /* opaque */

dafsa_view *dafsa_view_open (const char *path);   /* NULL on any error */
void        dafsa_view_close(dafsa_view *v);      /* NULL-safe */
int  dafsa_view_lookup_n(const dafsa_view *v,
                         const unsigned char *key, size_t len);   /* 1/0 */
long dafsa_view_prefix_enum(const dafsa_view *v,
                            const unsigned char *prefix, size_t prefix_len,
                            dafsa_enum_cb cb, void *user);        /* count, or -1 */

/* ─── Statistics ───────────────────────────────────────────────────── */

typedef struct {
    uint32_t n_states_total;      /* live + orphans (excludes sink 0) */
    uint32_t n_states_reachable;  /* BFS from initial */
    uint32_t n_final;             /* final reachable states */
    uint32_t n_trans;             /* transitions from reachable states */
    uint64_t register_probes;
} dafsa_stats_out;

void dafsa_stats(const dafsa *d, dafsa_stats_out *out);

/* ─── Write-ahead log (M5) ──────────────────────────────────────────── */

#define DAFSA_WAL_MAGIC0 'D'   /* 'D','A','W','L' */
#define DAFSA_WAL_VERSION 1

#define DAFSA_WAL_OP_ADD 1
#define DAFSA_WAL_OP_DEL 2

typedef struct dafsa_wal dafsa_wal;   /* opaque */

dafsa_wal *dafsa_wal_open(const char *path);        /* back-compat: writer open (same as _rw) */
dafsa_wal *dafsa_wal_open_rw(const char *path);     /* writer: O_RDWR|O_CREAT|O_APPEND, may ftruncate */
dafsa_wal *dafsa_wal_open_ro(const char *path);     /* reader: O_RDONLY, never mutates, no O_CREAT */
int   dafsa_wal_append_add(dafsa_wal *w, const unsigned char *key, uint32_t key_len);
int   dafsa_wal_append_del(dafsa_wal *w, const unsigned char *key, uint32_t key_len);
int   dafsa_wal_sync(dafsa_wal *w);
uint64_t dafsa_wal_size(const dafsa_wal *w);
typedef int (*dafsa_wal_replay_cb)(uint8_t op, const unsigned char *key, uint32_t key_len, void *user);
int   dafsa_wal_replay(dafsa_wal *w, dafsa_wal_replay_cb cb, void *user);
void  dafsa_wal_close(dafsa_wal *w);

dafsa_view *dafsa_view_open_layered(const char *fst_path, const char *wal_path);

/* ─── ABI version probe ─────────────────────────────────────────────── */

#define DAFSA_ABI_VERSION 1
uint32_t dafsa_abi_version(void);

/* ─── Debug ────────────────────────────────────────────────────────── */

void dafsa_dot(const dafsa *d, FILE *f);

#ifdef __cplusplus
}
#endif

#endif /* DAFSA_H */
