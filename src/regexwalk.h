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

#ifdef __cplusplus
}
#endif

#endif /* REGEXWALK_H */
