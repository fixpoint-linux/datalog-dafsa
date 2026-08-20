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
