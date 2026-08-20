/*
 * regexwalk.h — Regex → DFA compiler + automaton-intersection walkers (M5)
 *
 * Full-key byte matching with implicit ^...$. Keys are length-prefixed byte
 * strings (DAFSA key = 4*arity+1 bytes: u32BE cols + trailing \0).
 *
 * Regex subset: literals (incl. \\, \xHH, \0), ., [abc]/[a-z]/[^abc],
 * *, +, ?, |, ().  NO backrefs, {n,m}, lookaround.
 *
 * Two walkers:
 *   regex_dfa_walk       — in-memory (iterates via trans_arr_c)
 *   regex_dfa_walk_view  — mmap view (iterates via view_edge_next)
 */

#ifndef REGEXWALK_H
#define REGEXWALK_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Forward declarations (DAFSA types from vendor/) ─────────────────── */

typedef struct dafsa     dafsa;
typedef struct dafsa_view dafsa_view;

/* ─── Regex DFA ───────────────────────────────────────────────────────── */

#define REGEX_DFA_MAX_STATES 50000
#define REGEX_DFA_ABORT_EARLY 8192

typedef struct regex_dfa {
    uint32_t  n_states;          /* number of states (0 = error / empty) */
    uint32_t *trans;             /* trans[s * 256 + byte]; UINT32_MAX = dead */
    uint8_t  *accept;            /* accept[s] = 1 if accepting state */
    char     *errmsg;            /* NULL on success, error string on failure */
} regex_dfa;

/* Compile a regex pattern string into a DFA.
 * Returns a regex_dfa with n_states > 0 on success, or one with
 * errmsg != NULL on failure (n_states == 0).
 * Caller must free with regex_dfa_free().
 *
 * Full-key match semantics (implicit ^...$).  The DFA matches a complete
 * key: walk the DFA byte-by-byte; accept at end iff in an accepting state.
 *
 * Errors:
 *   - NULL / empty pattern
 *   - Unescaped NUL in pattern
 *   - State cap exceeded (REGEX_DFA_MAX_STATES; aborts early via
 *     REGEX_DFA_ABORT_EARLY for pathological patterns)
 *   - Memory allocation failure
 *   - Syntax errors (unbalanced brackets, etc.)
 */
regex_dfa *regex_compile(const char *pattern);

/* Free a regex DFA. NULL-safe. */
void regex_dfa_free(regex_dfa *dfa);

/* ─── Walkers ─────────────────────────────────────────────────────────── */

/* Callback type: receives (key_bytes, key_len, user).
 * key_bytes is a complete DAFSA key that matched the regex.
 * Return non-zero to stop the walk early. */
typedef int (*regex_walk_cb)(const unsigned char *key_bytes, size_t key_len,
                             void *user);

/* Walk an in-memory DAFSA, emitting keys whose full byte sequence matches
 * the compiled regex DFA.  Product DFS with visited hash set.
 * Returns the number of matching keys, or -1 on error. */
long regex_dfa_walk(const dafsa *d, const regex_dfa *dfa,
                    regex_walk_cb cb, void *user);

/* Walk a mmap'd DAFSA view, same semantics as regex_dfa_walk.
 * Returns the number of matching keys, or -1 on error. */
long regex_dfa_walk_view(const dafsa_view *v, const regex_dfa *dfa,
                         regex_walk_cb cb, void *user);

/* ─── Symbol DAFSA walkers (M5-symbols: match string content, emit sym_ids) ─── */

/* Callback type for symbol walks: receives a matched sym_id. */
typedef int (*sym_walk_cb)(uint32_t sym_id, void *user);

/* Open-addressing u32 hash set for sym_ids. */
#define SYMSET_INIT_CAP 64

typedef struct sym_set sym_set;
struct sym_set {
    uint32_t *keys;
    int       cap;
    int       used;
};

/* Initialize a sym_set. Returns 0 on success, -1 on OOM. */
int symset_init(sym_set *s);

/* Free a sym_set. NULL-safe. */
void symset_free(sym_set *s);

/* Add a sym_id to the set. Returns 0 on success, -1 on OOM. */
int symset_add(sym_set *s, uint32_t sym_id);

/* Check if sym_id is in the set. Returns 1 if present, 0 otherwise. */
int symset_contains(const sym_set *s, uint32_t sym_id);

/* Walk the symbols DAFSA (keys: utf8_str, NUL, sym_id_u32BE), matching the
 * regex against the string portion (before NUL), emitting matched sym_ids.
 * Uses product DFS with visited set. Returns number of matched sym_ids, or -1.
 * The symbols DAFSA must have the structure: string bytes -> 0x00 -> 4 u32BE bytes.
 */
long symbols_dfa_walk(const dafsa *d, const regex_dfa *dfa,
                      sym_walk_cb cb, void *user);

/* Same as symbols_dfa_walk but for a mmap'd dafsa_view. */
long symbols_dfa_walk_view(const dafsa_view *v, const regex_dfa *dfa,
                           sym_walk_cb cb, void *user);

#ifdef __cplusplus
}
#endif

#endif /* REGEXWALK_H */
