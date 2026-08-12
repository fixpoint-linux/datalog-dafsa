/*
 * relation.h — Per-relation DAFSA with fixed-width u32BE column encoding
 *
 * Each fact (v0,...,v_{a-1}) → key: 4*a bytes (columns as u32 BE) + 1 trailing \0
 * byte-for-byte deterministic: same facts in same order → identical DAFSA bytes.
 *
 * Exact lookup:  dafsa_lookup_n on the full 4*a+1 byte key.
 * Prefix enum:   custom DFS walker (uses dafsa_internal.h) that walks k*4
 *                prefix bytes, then DFS from that state — no W\0 requirement.
 */

#ifndef RELATION_H
#define RELATION_H

#include <stdint.h>
#include <stddef.h>

/* Opaque */
typedef struct relation relation;

/* Forward declaration — full definition in tupleset.h */
struct tuple_set;

/* ─── Lifecycle ───────────────────────────────────────────────────────── */

/* Create an empty relation of given arity (1-8). Returns NULL on error. */
relation *rel_create(uint8_t arity);

/* Load from a DAFSA file, validating arity if file exists.
 * If path doesn't exist, creates empty. Returns NULL on error. */
relation *rel_open(const char *path, uint8_t arity);

/* Save the relation DAFSA to path. Returns 0 on success, -1 on error. */
int rel_save(const relation *rel, const char *path);

void rel_free(relation *rel);

/* ─── Accessors ───────────────────────────────────────────────────────── */

uint8_t rel_arity(const relation *rel);

/* ─── Fact operations ─────────────────────────────────────────────────── */

/* Add a fact. cols has `arity` u32 values. Returns 1 if added, 0 if
 * duplicate, -1 on error.   DAFSA mapping: dafsa_add_n */
int rel_add(relation *rel, const uint32_t *cols);

/* Exact membership test. Returns 1 if present, 0 if absent.
 * DAFSA mapping: dafsa_lookup_n */
int rel_exact(const relation *rel, const uint32_t *cols);

/* Replace rel->d with a fresh minimal DAFSA bulk-built from a SORTED,
 * DEDUPLICATED tuple_set. Old DAFSA freed. Caller must have ts_sort()'d.
 * Returns 0 on success, -1 on error (rel->d unchanged on error). */
int rel_build_from_tupleset(relation *rel, const struct tuple_set *ts);

/* Callback for rel_prefix that sinks enumerated tuples into a tuple_set.
 * Usage: rel_prefix(rel, NULL, 0, ts_sink_cb, &ts) unions all facts. */
int ts_sink_cb(const uint32_t *cols, uint8_t arity, void *user);

/* ─── Prefix enumeration ──────────────────────────────────────────────── */

/* Callback type for prefix enumeration: receives the FULL tuple (arity cols). */
typedef int (*rel_enum_cb)(const uint32_t *cols, uint8_t arity, void *user);

/* Enumerate all tuples whose first k columns match `leading`.
 * For k=0, enumerates all tuples.  For k=arity, this is an exact lookup
 * (returns 0 or 1 tuples).  Returns count or -1 on error.
 *
 * DAFSA mapping: walks k*4 prefix bytes via trans_find, then DFS from
 * the resulting state (enum_dfs from dafsa_internal.h). */
long rel_prefix(const relation *rel,
                const uint32_t *leading, uint8_t k,
                rel_enum_cb cb, void *user);

/* ─── Regex pattern walk ──────────────────────────────────────────────── */

/* Forward declaration (full struct in regexwalk.h) */
struct regex_dfa;

/* Walk full keys of the relation matching the compiled regex pattern.
 * Enumerates complete tuples whose full DAFSA key (4*arity+1 bytes)
 * matches the regex DFA.  Returns count or -1 on error. */
long rel_pattern(const relation *rel, const struct regex_dfa *dfa,
                 rel_enum_cb cb, void *user);

#endif /* RELATION_H */
