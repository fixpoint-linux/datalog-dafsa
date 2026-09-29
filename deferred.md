# Deferred work

Tracking file for work explicitly scoped out or deferred from a shipped slice,
so it is not lost. Each entry names the feature, the deferred item, and why it
was parked. Ties to the search-stack thesis (`docs/datalog-dafsa-search-stack.md`).

---

## Full-text search tier (`dl_search` / `aux_index`) — shipped `27bbee7`

Deferred from the initial implementation. See `docs/datalog-dafsa-fulltext.md`.

### Ranked: next slice (small)

- **MCP / embed wiring** — nothing in `jing-memory` calls `dl_search` yet.
  Wiring the memory MCP's search onto `dl_search` is the integration step.
  (This is stack **Idea 3** — "one embed pass emits all auxiliaries
  atomically"; the bulk `dl_publish_snapshot` wiring.)
- **Snapshot parity for the index** — `dl_search` reads the live postings
  relation only; it does not route through the snapshot/`dl_query_version`
  view. As-of search over an old snapshot version is not implemented.
- **IDF / positional ranking** — current ranking is distinct-matched-terms
  count per obs_id. True term-frequency requires a separate count relation
  (the store is SET-semantic, so tf-from-duplicate-keys is impossible);
  IDF and positional ranking are later refinements.
- **Non-ASCII tokenization** — `tokenize()` handles ASCII alphanumerics only;
  non-ASCII text (e.g. CJK) is not tokenized.

### Parked: follow-on tiers (stack Ideas 2 & 4, and sibling index types)

- **Trigram / MIH / regex tiers reusing `aux_index`** — the `aux_index`
  token→sym_id primitive is now a reusable codepath; building the trigram
  (fuzzy entity-name lookup), MIH vector postings, and regex-on-symbols
  tiers on it is the natural extension but was out of scope for the fulltext
  slice.
- **Unified time-travel across every tier + kill the archiver's JSONL**
  (stack Idea 2) — snapshot retention as the archive; deferred until the
  index tiers exist and snapshots are exercised.
- **Search-as-Datalog rules** (stack Idea 4) — the biggest leap; partly
  aspirational (in-VM float/int8 vector math does not exist).

---

## Other deferred / parked items (from the non-CAS open-work survey, 2026-08-19)

- **Pointwise / stratified negation over recursion** — verdict 2026-08-18:
  not tractable in the least-fixpoint semi-naive engine. Not a bug; parked by
  design.
- **Trace / JIT** — rejected; the interpreter is not the bottleneck.
- **Top-down / QSQ magic** — deferred pending workload signal; architecture
  of record only (`design/datalog-dafsa-topdown-magic.md`).
- **IVM / DRed fallback gaps** — not planned except recursion+deletion via
  DRed-over-recursion (low value for the current workload).

---

## Column-kind check: permissive-by-design remainder (S3, 2026-08-20)

The compile-time column-kind check (`checkAllRuleConstKinds` /
`checkRuleVarKinds` / `propagateHeadKinds` in `zig/src/compiler.zig`) rejects
constant/column, join, equality, ordered-comparison, arithmetic and
!= kind mismatches, and now also types aggregate-RESULT and
arithmetic-RESULT head columns (S3c: `c(N):-e(X),N=count()` feeds INT into
the head; `min`/`max` feed the source column's kind — mirroring the typed
path's `typeAggregate`). What deliberately stays permissive:

- **`X != foo` on an int column** — collision-prone BY DOCUMENTED CONTRACT
  (`docs/language.html` comparisons section, suite-pinned `test_m9_arith`
  T8d): the symbol constant is interned and its id compared against raw
  ints, so a small id can equal a raw value. Order-dependent by design.
- **populate-after-load (R2)** — a relation whose kind is established by
  facts loaded AFTER the rules are compiled is not re-checked; the check
  runs at `dl_load_rules` time only.
- **variadic relations** — skipped as kind sources (no fixed columns).
- **mixed-head columns** — a head column derived from int in one rule and
  symbols in another folds to MIXED, which contributes no definite site and
  is documented permissive (S1/S2 rule); only per-rule downstream USES of a
  mixed column are checked.
- **string/list/range-produced variables** have no relational kind source
  (RK1) and stay permissive in comparisons, except where the comparison
  operand's kind IS known (e.g. `length` over a sym var's result compared
  with an int constant is fine; an ordered cmp over a KNOWN-sym var is loud).

The historical 32-distinct-var truncation of the per-rule kind tables is
GONE (S3c): both tables are `MAX_VARS`(64) sized, matching the loud
`exceeds the maximum of 64 distinct variables` rejection `compile_one`
already issues, so a wide rule can no longer silently evade the check
(MEASURED pre-fix: a 4-relation arity-8 program with 32 filler vars plus the
cross-kind join loaded rc=0 while the same join with 24 fillers was loud;
post-fix both are loud, and a 33-var all-INT join still loads).

