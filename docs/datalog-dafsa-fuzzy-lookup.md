# Fuzzy entity-name lookup: Levenshtein over the symbol table

**Status:** IMPLEMENTED. `lev_compile` (zig/src/regexwalk.zig), the `~~` atom
operator (zig/src/parser.zig + compiler.zig), `dl_fuzzy_lookup` (src/dl.h +
zig/src/dl.zig), `dl similar` (zig/src/dl_cli.zig), and the oracle suite
tests/test_fuzzy.c. Nothing was added to the on-disk format, and no relation,
index or build step was introduced.

**Two mechanisms, two callers (2026-10-18).** `dl_fuzzy_lookup` / `dl similar`
no longer walk any index: they run a **banded linear scan** over the interner's
symbol table (§ *Why the scan, not the walk*). The Levenshtein automaton below
is still the mechanism for the `~~` rule operator, whose walk is a *query* over
a relation and is untouched. Both were measured to agree id-for-id and
distance-for-distance (a Zig unit test compares them on the same corpus).

## The Levenshtein automaton (`~~` rules, and the walk the scan replaced)

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
column by the `sym_set` the matcher emits — the walk then, the scan
(`symbolsScanTable`) since `3a588a0` — and the automaton *is* a `regex_dfa`.

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

## Why the scan, not the walk (MEASURED 2026-10-18, commit 3dd4ae0)

The hypothesis that motivated this rewrite was that the symbol product DFS
(`symProdDfs`) explores an unbounded depth and that a depth bound of
`|needle| + k` would prune it. **That hypothesis is refuted, and the proposed
bound would have pruned nothing:**

1. The automaton is *already* depth-bounded by its own dead-state prune. A row
   with `min_j dp[j] > k` is `DFA_DEAD`, and `min_j dp[i][j]` is non-decreasing in
   `i`, so no state survives past depth `|needle| + k`. MEASURED over a battery
   of needles (lengths 1-20, `k = 0/1/2`) on a 166,743-symbol store: the walk
   costs **0-2 ms** in every case.
2. The cost was never the walk. `dl_fuzzy_lookup` called `intern_fwd()`, which
   **materializes** the whole symbol DAFSA from `symbols.array` on first use
   (symbols.dafsa is no longer persisted — it is a derived cache). MEASURED:
   **10.3-19 s** for 166,743 symbols (≈60 µs/symbol), against 0.37 s to open the
   store: the command was 96 % index build.
3. The earlier "10-28 µs" figure was an *in-process, warm-DAFSA* measurement of
   `dl_fuzzy_lookup`, while the 13.6-14.1 s figure was a *cold CLI run*. The two
   were not comparable; that comparison is what made an index-build cost look
   like a walk cost.

A scan needs no index at all, so the cliff disappears. On a warm DAFSA it is the
slower of the two (see the measured table) — but a warm symbol DAFSA is exactly
what a fresh process does not have, and every other consumer that builds it
(`dl pattern` in-memory, `~` rules, `~~` rules) pays the same 10-19 s. Fuzzy
lookup was the only consumer that paid it for a query that costs 15 ms without
it.

**The band is exact, not heuristic:** every edit costs at least one byte of
length, so a name whose length is outside `[m-k, m+k]` cannot be within distance
`k` of a needle of length `m`. The scan skips those before any DP, which also
bounds the DP itself to `O((m+k)²)` per surviving candidate. A match at exactly
`len-k` (deletion) and exactly `len+k` (insertion) is *inside* the band and is
covered by tests/test_fuzzy.c, the Zig unit test, and the CLI battery.

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
These are WARM (DAFSA already built) numbers — the condition under which the walk
is the cheaper mechanism.

The banded scan of `dl_fuzzy_lookup` (MEASURED 2026-10-18, in-process, ReleaseFast):

| symbol table | store bytes | scan, k=1 | scan, k=2 | DAFSA build it replaces |
|---|---|---|---|---|
| 6,885 symbols | 0.2 MB | 0.18 ms | 0.11–0.27 ms | ≈12 ms |
| 166,743 symbols | 44 MB | 15.7 ms | 15.1–16.7 ms | 10.3–19 s |

CLI end-to-end (`dl similar '<word>' --k N`, wall clock, store open included —
open alone is 0.37 s on the 166,743-symbol store and 5 ms on the 6,885-symbol one):

| store | before | after | speed-up |
|---|---|---|---|
| 166,743 symbols | 11.19–16.29 s | 0.39–0.47 s | 25–37× |
| 6,885 symbols | 15.4–18.5 ms | 3.2–4.6 ms | 4.5–5.5× |
| 10-symbol synthetic | 2.6–3.4 ms | 2.5–3.8 ms | unchanged (±0.4 ms) |

**INTERPRETATION:** the scan is `O(N)` in the symbol table with a band that keeps
the DP per candidate at `O((m+k)²)`, and it is the *whole* cost — there is no
index to build. The trade is explicit: on a warm DAFSA the walk wins by 10–20×
(tens of µs vs a fraction of a ms); when the DAFSA would have to be materialized,
the scan wins by 500–1000×. A separate process that already built the DAFSA for a
pattern query can still use it — `~~` rule evaluation does, and pays nothing.

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

**Time travel is free:** both mechanisms resolve against the LIVE symbol space
(the scan reads the interner's symbol table directly; the `~~` walk — now the
differential-test arm only, `dl.zig`, since the `~~` atom itself scans
`symbolsScanTable` as of `3a588a0` — reads the live forward DAFSA). Rows still
come from the version's relation view, so the filter is only ever applied to
rows that exist in that version.

> **Correction (2026-10-19, unit `fix-idreuse`).** The original justification
> here ("sym-ids are append-only and never reused, so the live symbol set is a
> superset of any snapshot's") is **MEASURED FALSE across a publish**, and a
> superset is the wrong predicate anyway: what a version-scoped filter needs is
> that the live map sends every id the version's rows reference to the SAME
> string. `dl similar` itself has no `--version` form, so it is unaffected;
> `dl search --version N` is: its terms are interned LIVE and then looked up in
> that version's postings, so a db-root `symbols.array` that diverges from the
> snapshot's selects another term's postings — MEASURED on a truncated root
> array: `search "blue" --version 1` returned the "red fox" observation (id 2)
> where the intact store returns id 4. `fix-idreuse` closes the publish path
> that produced that state (publish now persists the db-root array), so what
> remains is a legacy-store / restored-or-edited-root exposure — reported, not
> fixed, and not resolved by the argument above.

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

Plus a Zig unit test in zig/src/regexwalk.zig: an **exhaustive** agreement check
(every string over `{a,b,c}` up to length 4 × every pattern over `{a,b}` up to
length 4 × k ∈ {0,1,2}) against `lev_distance_cap`.

And a Zig unit test in zig/src/dl.zig (`dl_fuzzy_lookup: scan agrees with the lev
automaton walk`): the banded scan and the Levenshtein-automaton product walk are
run over the same corpus and compared as `(sym_id, distance)` SETS for 5 needles ×
3 values of k, plus explicit assertions that the `len-m±1` neighbours are present
(the edges a length/depth bound could cut) and the `len-m±2` ones only at k=2.
The two mechanisms are independent code paths, so this is a differential, not a
tautology.

CLI differential (2026-10-18): `dl similar` output captured for 52 invocations —
a 10-symbol synthetic store with hand-built `len-k`/`len+k`/transposition/no-
neighbour cases, the 6,885-symbol store, and the 166,743-symbol store, over
`k ∈ {0,1,2}` and needle lengths 1-20 — **before and after the change, byte
identical stdout, stderr and exit codes** (including the `--k 3` refusal).

**Asymmetry proof (the tests can fail):** flipping the accept test from
`<= k` to `< k` fails 7 of the 8 C cases (including the brute-force oracle, the
VM operator and `dl_fuzzy_lookup`); the unmutated tree passes all 8.
