# Fuzzy entity-name lookup: Levenshtein over `symbols.dafsa`

**Status:** IMPLEMENTED. `lev_compile` (zig/src/regexwalk.zig), the `~~` atom
operator (zig/src/parser.zig + compiler.zig), `dl_fuzzy_lookup` (src/dl.h +
zig/src/dl.zig), `dl similar` (zig/src/dl_cli.zig), and the oracle suite
tests/test_fuzzy.c. Nothing was added to the on-disk format, and no relation,
index or build step was introduced.

## Why this is cheap

A Levenshtein automaton for `(term, k)` is an **ordinary DFA**: its states are
edit-distance DP rows, its start row is `[0,1,...,m]`, a transition on byte `c`
is the classic recurrence

```
dp'[j] = min(dp[j] + 1, dp'[j-1] + 1, dp[j-1] + (term[j-1] != c))
```

and a state accepts when `dp[m] <= k`. That layout is *exactly* the `regex_dfa`
extern struct (`n_states`, `trans[s*256+b]`, `accept[s]`, `errmsg`), so the
product DFS that already exists — `symProdDfs` in zig/src/regexwalk.zig, reached
through `symbols_dfa_walk` — walks it **verbatim**. The only new engine code is
the constructor. `vm.zig` (`OP_WALK`) needed no change: it filters a relation
column by the `sym_set` the walker emits, and the automaton *is* a `regex_dfa`.

**The prune is sound.** A state whose row has `min_j dp[j] > k` can never lead to
an accepting state, because `min_j dp[i][j]` is non-decreasing in the row index
`i` (`dp[i][j] >= min_j dp[i-1][j]`: term 1 is `dp[i-1][j]+1 > min`, term 3 is
`>= dp[i-1][j-1] >= min`, and `dp[i][0] = i > min`). Such rows become
`DFA_DEAD` and the DFS never descends into them. MEASURED: neutering the prune
(forcing every row alive) leaves every test result identical and only raises the
state count by one — it is a pure optimization, not part of the semantics.

Two construction economies, both behaviour-preserving:

- **Values clamped at `k+1`** (Ukkonen / Schulz-Mihov): a distance above the
  threshold cannot change an accept decision, so rows live in a bounded alphabet.
- **Byte equivalence classes**: the automaton is built over the distinct term
  bytes plus one "other" class, then expanded into the 256-wide table the walker
  indexes. A DP transition depends on a byte only through `(term[j] == c)`, which
  the class determines exactly. This is ~`nclass`× cheaper and is *load-bearing*:
  measured build time is comparable to the walk time, so the naive 256-wide build
  would double-plus the per-call cost. Same struct, same walker, no API change.

## Grammar: `atom ~~ k [col] 'term'`

`~~` (a new `TOK_TILDE2`) is the fuzzy form of the M5 `~` pattern operator:

```
q(X,Y) :- p(X,Y) ~~ 'alice'.       % k = 1 (default), column 0
q(X,Y) :- p(X,Y) ~~ 2 'alice'.     % k = 2
q(X,Y) :- p(X,Y) ~~ 2 1 'alice'.   % k = 2, column 1
q(X,Y) :- p(X,Y) ~~ 0 'alice'.     % k = 0 — exact match
```

`~~` and `~` share `atom.pattern_col`, so the **pinned 80-byte `atom` layout and
src/parser.h are untouched** (the compiler and `dl.zig`'s `astAtomClone` were
already field-wise, so the encoding flows through them unchanged):

| bits | meaning |
|---|---|
| 0..7 | column index (what `~` has always stored) |
| 8..15 | edit distance `k` |
| 24 | `PATTERN_FUZZY` flag — the pattern is an edit-distance term |

A `~` atom therefore encodes to exactly the value it always did. The
audit that matters: `test_m5.c` and `test_m5_review.c` pass **unchanged**.

**The alternative we did not build** is a `lev:k:` prefix inside the existing
pattern string (`p(X,Y) ~ 'lev:1:alice'`), which needs *no* parser change at all
— stringly-typed, but zero grammar surface. It remains the fallback if `~~` ever
has to be withdrawn: it is a 3-line change in the same compiler branch.

`k >= 3` is **refused loudly**, never degraded: `lev_compile` returns a
`regex_dfa` with `n_states == 0` and `errmsg = "lev: k must be 0..2 (k>=3
refused: automaton state explosion)"`, which the compiler turns into a rule
compile error and the rule does not load.

## Byte distance — the honest limit

The walk consumes **bytes**, so this is **byte edit distance**:

- correct for ASCII;
- **over-counts non-ASCII**: `café` vs `cafe` is distance **2**, not 1, because
  `é` is two bytes.

A codepoint-aware distance needs a DP that advances only on a lead byte
(continuation bytes cost 0) — a *different* automaton, not built. Do not describe
this as codepoint distance.

**MEASURED** on a real db built from a docs-derived vocabulary (6,885 interned
symbols — the repo ships no populated db): 187 symbols (**2.72 %**) contain
non-ASCII bytes, and every one of them is typographic (`—`, `→`, `≈`, `×`, `µs`)
rather than an entity name. So the over-counting is real but confined to a slice
of the symbol space where entity lookup does not live.

## Measured cost

`lev_compile` build (ReleaseFast, mean of 2,000 calls; also the automaton size):

| term (len) | k=0 | k=1 | k=2 |
|---|---|---|---|
| `alice` (5) | 6 states | 20 states, 5 µs | 58 states, 15 µs |
| `entity_name` (11) | 12 | 45, 18 µs | 150, 57 µs |
| `observation` (11) | 12 | 44, 17 µs | 154, 71 µs |
| `abcdefghijkl` (12) | 13 | 48, 22 µs | 170, 89 µs |
| `typographical` (13) | 14 | 52, 23 µs | 186, 95 µs |

Walk over a 6,885-symbol DAFSA: 0.3–0.5 µs (k=0), 5–7 µs (k=1), 35–55 µs (k=2).

End-to-end `dl_fuzzy_lookup` on that db (build + walk + per-hit distance, mean of
200 calls): **k=0 2–6 µs, k=1 10–28 µs, k=2 58–145 µs** — i.e. sub-millisecond,
exact, with **zero index storage**. For comparison, a brute-force scan is
`N × len²` DP cells (≈ 900 k cells for this vocabulary) with no prune.

**INTERPRETATION:** build is roughly half the k=1/k=2 cost at these term lengths,
which is why the equivalence-class optimization is in v1 rather than deferred.

## API

```c
typedef int (*dl_fuzzy_cb)(uint32_t sym_id, const char *name, size_t name_len,
                           uint32_t dist, void *user);
long dl_fuzzy_lookup(dl_db *db, const char *needle, uint32_t k,
                     dl_fuzzy_cb cb, void *user);   /* -1 on k>2 / bad args */
regex_dfa *lev_compile(const char *term, uint32_t k);
uint32_t lev_distance_cap(const uint8_t *a, size_t alen,
                          const uint8_t *b, size_t blen, uint32_t cap);
```

CLI: `dl similar '<name>' [--k N]` prints `distance<TAB>name`, nearest first.

**Time travel is free:** the walk resolves against the LIVE interner DAFSA.
Sym-ids are append-only and never reused, so the live symbol set is a superset of
any snapshot's; rows still come from the version's relation view, so a superset
filter can only be applied to rows that exist in that version. No version
entry point is needed.

## Verification

`tests/test_fuzzy.c` (in both `zig/run_zig_suites.sh` and `tests/run_all.sh`) —
each case named with what would make it fail:

1. `k=0` is exact match only;
2. a distractor at distance `k+1` is **absent** (the can-fail case for the prune);
3. a transposition is distance **2**, so absent at k=1 and present at k=2 (that
   is Damerau, not Levenshtein — the usual "nice-to-have" bug);
4. the whole vocabulary cross-checked against a full-matrix DP written in the
   test (both directions: every emitted id must be within k, every non-emitted
   one must not be);
5. `~~` through the VM joining entity **rows** — default k, explicit k, explicit
   column, out-of-range column rejected, `k=3` rejected;
6. a non-ASCII symbol documenting byte distance (`café` absent at k=1, present at
   k=2 with reported distance 2);
7. refusals are loud and the returned struct is always freeable, plus a
   200-iteration compile/free loop whose arena growth is asserted (the leak
   check: a missing `trans` free would show as ~20 MB).

Plus Zig unit tests in zig/src/regexwalk.zig: an **exhaustive** agreement check
(every string over `{a,b,c}` up to length 4 × every pattern over `{a,b}` up to
length 4 × k ∈ {0,1,2}) against `lev_distance_cap`.

**Asymmetry proof (the tests can fail):** flipping the accept test from
`<= k` to `< k` fails 7 of the 8 C cases (including the brute-force oracle, the
VM operator and `dl_fuzzy_lookup`); the unmutated tree passes all 8.
