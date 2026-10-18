# Search: how to improve it

**Status: items 1–4 BUILT and verified; item 5 NOT BUILT (gated).** Written
2026-08-11 against `ef7e0e4`; the plan was executed in order and the results are
folded in below (each ranked item states its measured outcome). Current HEAD:
`fb04704`, engine 100% Zig, 42 C oracle suites green in ~26 s, one submodule
(`vendor/dafsa`).

| # | item | status | measured result |
|---|---|---|---|
| 1 | full-text `IntSet` → O(1) membership | **DONE** `4416161` | 20k × 3 terms **306 → 3.74 ms**; linear, not quadratic |
| 2 | hybrid hot loop + lexical gate | **DONE** `e08ee86` | `dl_prefix` calls **100 → 1**; recall hole closed via `dl_vector_search_gated` |
| 3 | `lev_compile` Levenshtein over symbols | **DONE** `07127a4` | k=1 10–28 µs end-to-end; k=2 58–145 µs; zero storage |
| 3b | review fixes (5 findings) | **DONE** `fb04704` | 2 measured defects incl. a regression; OOM leak proved closed with an interposer |
| 4 | `dl_search_ranked` (OR / `min_terms`) | **DONE** `f23899f` | score now varies with matched-term count |
| 5 | rev-gated columnar vector arena | **DONE** `$(git rev-parse --short HEAD)` | repeated queries **76.47 → 4.09 ms** at 10k (19×); +3.9 MB |

**Provenance.** Synthesised by the orchestrator from the planner pass recorded in
the memory node `handoff-search-plan` (rev 18), whose numbers come from throwaway
harnesses in `/tmp` (`sbench.c`, `pbench.c`, `tbench*.c`, `lev_probe.py`) run
against the built `zig-out/lib/libdatalog.so`. The headline vector figures were
independently reproduced by a separate bench. Claims are marked **MEASURED**,
**PROJECTION**, **INTERPRETATION** or **DESIGN-VALIDATED**; a projection is not a
result.

The question this plan answers: *can DAFSA properties be leveraged to improve
search?* The short answer is that two of the three biggest wins are not DAFSA
tricks at all — they are defects in the code that already exists.

---

## 1. Where search stands (measured baseline)

Two search tiers, both in-engine, both exact.

**Vector tier — scan-and-rank** (`zig/src/vector.zig`). `__vec_q__` is arity-3
(`entity_sym`, `chunk_idx`, `packed_4xint8`), 96 rows per 384-dim vector. A query
enumerates the relation, takes an integer dot, keeps top-k, emits in exact cosine
order.

| vectors | rows | query | per vector | bare `dl_prefix` enum of the same rows |
|---|---|---|---|---|
| 1,000 | 96,000 | 4.1 ms | 4.1 µs | 3.5 ms |
| 5,000 | 480,000 | 26.4 ms | 5.3 µs | 22.6 ms |
| 10,000 | 960,000 | 86.5 ms | 8.7 µs | 61.3 ms |

**MEASURED** (orchestrator bench, independently reproduced by the planner at
94.1 ms / 98 ns per row for 10k). Version reads are 1.6–2× the LIVE cost.
**INTERPRETATION:** 70–90% of a query is DAFSA row materialisation at ~50–100 ns
per row; the int8 MACs are free (an empty callback measures the same 95.8 ms as
one doing the real 4-lane arithmetic).

**Full-text tier** (`zig/src/index.zig`, 910 lines). `__postings__(term_sym,
obs_id)`, set-semantic, ranked by distinct matched terms.

**MEASURED:** `dl_search_top` with one term over 20k postings = 51 ms; two terms
= 203 ms; three terms = 306 ms — exactly quadratic in postings. At 10k documents
× 2 terms (97 ms) a *lexical* query already costs more than the entire 10k-vector
*semantic* scan (86.5 ms).

---

## 2. Direction 1 — Levenshtein DFS over `symbols.dafsa` (owner-named)

**Mechanism.** The interner forward map is keyed `utf8_bytes \0 sym_id_u32BE`. A
Levenshtein automaton for `(term, k)` is an ordinary DFA whose states are
edit-distance DP rows, and its layout is *exactly* `regexwalk.zig`'s `regex_dfa`
(`extern struct`, ~regexwalk.zig:62-90). The product DFS that already exists —
`symProdDfs` (:1490) reached through `symbols_dfa_walk` (:1595) — therefore walks
it **verbatim**. The only new engine code is the constructor
`lev_compile(pattern, k) -> *regex_dfa` (~130 lines: DP-row determinisation plus a
state hash map mirroring `dsmFindOrAdd`). The prune is sound: a state is dead
when `min(row) > k`, and `min_j DP[i][j]` is non-decreasing in `i`.

**DESIGN-VALIDATED.** A Python prototype re-implementing the same product DFS and
prune over a minimal hash-consed DAFSA matched brute-force edit distance
**13/13**, including adversarial vocabularies: 15k random 8-char names (exact), and
a 384-term vocabulary consisting of *all* 1-edit variants of the query (384/384).

**Cost model (MEASURED on the prototype, 16,687-term vocabulary).** Automaton
states: k=1 → 16–234; k=2 → 33–2,006. Product-DFS visits of (dafsa node,
automaton state) pairs: **69–723** natural, **170–2,519** random, **1,449** for the
all-variants case (output-proportional). Brute-force baseline on the same
vocabularies: 9,027–960,000 DP cells. **INTERPRETATION:** 2–3 orders of magnitude
less work than scanning N names. **PROJECTION:** the automaton *build* dominates at
k≥1 (k=1/len≤14 ≈ 0.3–1 ms; k=2/len12 ≈ 3–6 ms), removable by building over byte
equivalence classes (distinct pattern bytes + 1) and expanding into the 256-wide
table, ~20× cheaper, same struct, no walker change.

**Work items (~250 engine lines + a suite).**

1. `regexwalk.zig`: `pub fn lev_compile(pattern, k) ?*regex_dfa` + tests: k=0 ==
   exact match; a symbol at distance k+1 is **absent** (the can-fail case); a
   transposition is distance 2, not 1; empty/garbage input; state-cap refusal.
2. `parser.zig`: a `~~` token mirroring `TOK_TILDE`, encoding `k` in the spare high
   bits of `atom.pattern_col` so the pinned 80-byte atom layout and `src/parser.h`
   are untouched. Grammar proposed: `atom ~~ k [col] 'term'` (k optional, default 1).
3. `compiler.zig` (~:1971-2010): branch to `lev_compile` where the M5 loop calls
   `regex_compile`.
4. `vm.zig`: **no change** — `OP_WALK` already filters a relation column by the
   `sym_set` the walker emits, and the automaton *is* a `regex_dfa`.
5. `dl.h` + `dl.zig`: `dl_fuzzy_lookup(db, str, k, cb, user)` (~25 lines) for
   CLI/python/tests.
6. `dl_cli.zig`: `dl similar '<name>' [--k N]` (~30 lines) — the surface
   `docs/datalog-dafsa-trigram.md` already promised as `dl_similar`.
7. Docs: mark the trigram doc **SUPERSEDED**; fix the stale status in
   `datalog-dafsa-regex-on-symbols.md`.
8. New oracle suite `tests/test_fuzzy.c`.

**Time travel is free (a finding, not a gap).** `vm.zig:696` resolves the pattern
set against the LIVE interner DAFSA. That stays correct for a version read because
sym-ids are append-only and never reused: the live symbol set is a superset of any
snapshot's, and rows still come from the version's relation view, so a superset
filter can only be applied to rows that exist in that version. **Same argument
covers the list-term vector layout** in §3: `terms.bin` is db-global and
append-only, so a handle read as-of a version is exact. No `*_version` entry point
and no snapshot work are needed for either.

**Measure first (cheap, before any wiring).** (a) the real db's symbol count and
its non-ASCII share — the walk cost is vocabulary-shaped and 16,707 is a
docs-derived synthetic; (b) time `lev_compile` in Zig for k=1 vs k=2 (a 40-line
test, no parser wiring) to confirm or refute the build projections; (c) confirm a
real db's symbol DAFSA has the NUL + 4-byte `u32BE` payload shape `readSymId`
assumes.

**Risk that would sink it: UTF-8.** The walk consumes *bytes*, so a
codepoint-aware distance needs the DP to advance only on a codepoint's first byte
(continuation bytes cost 0) — a different automaton. v1 must be documented as
**byte distance**: correct for ASCII, over-counting for non-ASCII (é = 2 bytes).
If measurement (a) shows a materially non-ASCII symbol space, ship ASCII-only with
a documented limit — do not silently ship a wrong-looking distance. Secondary risk:
`~~` is a user-visible grammar addition; the zero-parser-change alternative is a
`lev:k:` prefix inside the existing pattern string (stringly-typed but keeps
`parser.h` pinned).

**Honest ceiling:** k=1 is the sweet spot; k≥3 must be refused loudly (state
explosion).

---

## 3. Direction 2 — vector enumeration (premise refuted)

**OUTCOME (built and superseded):** this direction was NOT built — the premise
was refuted, and the measured 2.4x did not justify a cross-repo encoder-contract
change. The vector tier stays scan-and-rank. Kept below as the record of why. The cost is not the row count; it is **DAFSA key bytes
walked** (~8–9 bytes/row today, ~28 for arity-8). All three layouts below store
the same 384 int8 lanes, **MEASURED** at 10k vectors:

| layout | rows | query | ns/row | speedup |
|---|---|---|---|---|
| (a) today: arity-3 × 96 | 960,000 | 94.1 ms | 98 | — |
| (b) arity-8 × 16 (6 payload cols = 24 int8) | 160,000 | 41.2 ms | 257 | **2.4×, not 8×** |
| (c) arity-2 header + one 96-node cons list | 10,000 | 651 ns/vector (at 2k) | — | **9.7×** through ABI calls, 20–40× projected direct |

Note for (b): the `chunk_idx` column **cannot** be dropped — set semantics would
silently collapse two identical payload chunks.

**Blocker found for (c), MEASURED.** `dl_add_fact` (dl.zig:2127; termstore
durability rule at :2203) calls `term_save` — a full `terms.bin` rewrite, one
`write()` syscall per node — whenever the termstore is dirty. Interleaving 96
`dl_term_cons` with one `dl_add_fact` per vector therefore costs **53 ms/vector**
(2000 vectors = 105 s), i.e. O(N²). Fixable by caller ordering (cons all lists,
then add all facts: exactly one save) or a ~5-line saved-watermark check in
`dl_add_fact`. It must be stated, or the first implementer will measure a 100×
regression and blame the design.

**Also MEASURED:** the bulk loader is fine — `dl load` of 200k arity-3 facts =
0.76 s (3.8 µs/fact) — while the per-fact path is quadratic in the sorted-array
overlay (192k rows = 3.2 s, 960k rows = 127 s; `REL_OVERLAY_FLUSH_AT` is 1<<20 so a
960k-row load never flushes). **Any encoder must use `dl_load_facts`, never
`dl_add_fact`, for a `__vec_q__` rebuild.**

**What I would actually build (only if latency binds):** a lazily-materialised,
rev-gated **columnar arena** per vector relation (~120 lines in `vector.zig` plus
a tail field on `Relation`, which already carries non-C fields), invalidated by
`rel_rev`. LIVE mode only; VERSION mode keeps the current scan (the existing
LIVE/VERSION discipline). **PROJECTION:** first query unchanged (~94 ms),
subsequent queries ~1–3 ms, i.e. 30–80× for a repeated-query loop, 0× for a
one-shot CLI. Price it against a real workload.

---

## 4. Direction 3 — full-text: two measured defects

**(1) The tier is O(N²) per term.** `intSetAdd` (index.zig:160) calls
`intSetContains` (:152), a **linear scan**, for every posting — while the same
file already contains a correct open-addressing `U64Set` (:183-275) used
elsewhere. The linear scan is not an accident: its own comment reads *"Linear
search; small sets expected (postings per term)"*. The measured 20k postings are
what invalidated that assumption. Replacing it makes the cost O(sum postings): **PROJECTION ~1–3 ms at
20k postings × 3 terms, ~100×**. Invisible to the oracle (`test_search.c` asserts
sets and counts, not time) — which means the new test must be a brute-force
**cross-check** (build the expected intersection in the test, compare), not a
timing assert.

**(2) The ranking is structurally dead, not merely weak.** index.zig:482 assigns
`score = n_terms` to every result; **MEASURED** identical across all results in
every case tried. Under pure AND semantics "distinct matched terms" == n_terms by
definition, so no score can vary — IDF, tf and positions are all moot until
retrieval admits **partial** matches.

**Honest recommendation:** fix the data structure first, then add a *separate*
`dl_search_ranked(..., min_terms)` entry point whose score = number of query terms
matched, with IDF available free from the per-term set sizes the code already
enumerates. **Do not change `dl_search`'s AND semantics** — `test_search.c`
`t_search_and` pins it to exactly 1 result.

---

## 5. Direction 4 — hybrid (`vhybrid`): a hot loop and a recall hole

**Hot loop, MEASURED.** `dl_cli.zig:333` `obsToEntity` calls
`dl_prefix(db, "observation", NULL, 0, ...)` — a **full enumeration of the
observation relation** — *once per lexical hit*, inside the loop at :1317, for up
to k·10 = 100 hits. At 10k observations that is 100 × ~0.66 ms ≈ **66 ms per
`vhybrid` call**, the same order as the entire 10k-vector scan, for a mapping one
pre-built hash map answers in a single pass.

**Recall hole.** The vector half is truncated at `k*10` **before** the
intersection (:1257), so an entity present in both tiers but ranking below k·10
semantically is **silently missed**. The docs' own "lexical gate → semantic rank"
design is both correct and cheaper: pass the lexical entity set into the scan as a
second filter test alongside the existing liveness bsearch
(`vector.zig:255-300` already does one membership test per row, so the marginal
cost is ~0), and return the top-k **of the intersection**.

New test that can fail: a document in both tiers but ranked outside the vector
top-k·10 — old code returns nothing, gated code returns it.

---

## 6. Ranked sequence

| # | change | effort | measured/projected win |
|---|---|---|---|
| 1 | full-text `IntSet` → existing `U64Set` | ½ day | 306 ms → ~3 ms (**~100×**) at 20k × 3 terms |
| 2 | hybrid: one-shot obs→entity map + lexical gate inside the vector scan | ~1 h | ~66 ms at 10k obs; **fixes a recall hole** |
| 3 | `lev_compile` — Levenshtein over `symbols.dafsa` (owner's ask) | 1–2 days | exact typo-tolerant lookup, zero storage, sub-ms; **retires the trigram tier** |
| 4 | `OR`/ranked full-text entry point (after 1) | ½ day | makes the dead ranking live; IDF becomes meaningful |
| 5 | rev-gated columnar vector arena | ~1 day | **DONE** — 19–23× measured on repeated queries (workload confirmed: agent handoff lookups) |

**Order rationale:** 1 and 2 are strictly cheaper than 3 and each removes a
measured quadratic or hot-loop defect; 3 is the only one that adds a *new*
capability; doing 1–2 first leaves the search tiers honest before a new primitive
lands on top of them.

---

## 7. Do not build (each with the measurement that kills it)

- **The trigram index** (`docs/datalog-dafsa-trigram.md`) — its own doc calls
  itself the lowest-value item and names "Levenshtein over a candidate set" as the
  refinement. Direction 1 delivers exactly that: no relation, no build step, no
  incremental maintenance, 69–2,519 pair-visits measured. Supersede the doc.
- **Any approximate/bucket vector index** — rejected by the owner, and the 2.4×
  arity-8 measurement shows the cost is key bytes, which bucketing does not remove.
  Recall would be traded for ~2×.
- **A vector storage format change now** — 2.4× at the price of a documented
  external-encoder contract (`src/vector.h:19-22`) plus 4 oracle suites plus
  `python/dlb`, and it does not change the O(N) shape.
- **tf/positions in `__postings__`** — 3× storage and an encoder change for a
  ranking signal that provably cannot vary under AND. Revisit only for phrase
  search.
- **Moving search out of the engine** — no caller to break today and nothing to
  gain: every cost measured here is inside the engine and stays payable wherever
  the code lives.
- **Resurrecting MIH/ITQ.**

---

## 8. Verification plan (the gate, and what proves each mechanism)

Gate = `zig/run_zig_suites.sh` (41 C suites re-linked against the Zig-built `.so`);
the **exit code** decides, not the tail of stdout.

1. **Full-text** — existing `tests/test_search.c` unchanged, plus new cases that
   cross-check against a brute-force intersection built in the test (drop one
   posting from the expected set and assert inequality), plus a 2-term case where a
   document matches only one term and must be **absent** under AND.
2. **Hybrid** — a case where the joint-tier document ranks beyond the vector
   top-k·10: fails before, passes after (the asymmetry *is* the proof). Existing
   `test_vector_cli.c` golden output must be **re-derived**, not blessed.
3. **Levenshtein** — new `tests/test_fuzzy.c`: brute-force edit distance over a
   hand-built symbol set as the oracle; a distractor at distance k+1 must be
   absent; a transposition at k=1 must be absent; k=0 == exact; the `~~` operator
   through the VM joining entity rows; a non-ASCII symbol documenting byte-distance
   semantics. `test_m5.c`/`test_m5_review.c` must pass **unchanged**, proving the
   parser/compiler change is additive. The automaton must be freed by the existing
   `regex_dfa_free` — assert no leak on the error paths.
4. **Arena cache (if built)** — a mutation-after-query test: add a vector, assert
   the next query sees it. A cache whose invalidation is untested is worse than no
   cache.

---

## 9. Incidental findings (not this task, recorded so they are not re-discovered)

1. **MEASURED, bug-shaped:** `dl_add_fact` triggers a full termstore save whenever
   the termstore is dirty (`dl.zig:2203` + `termstore.zig:310`) — 53 ms per
   vector-as-list write, so any user of list values built *interleaved* with fact
   adds pays O(N²). Suggested fix: a saved-watermark skip.
2. **MEASURED:** the per-fact path is quadratic in the sorted-array overlay (see
   §3). Encoders must use `dl_load_facts`.
3. **Stale doc:** `docs/datalog-dafsa-regex-on-symbols.md` still says "Status:
   Design/plan — not yet implemented" while `vm.zig:696` wires
   `regexwalk.symbols_dfa_walk` and `test_m5.c` exercises it. One-line fix.
4. **Misleading test name:** `test_search.c`'s rank test — score is constant (see
   §4(2)); its assertions only check membership and counts, which is why a real
   ranking can be added without breaking the oracle.
