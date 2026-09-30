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
`!=` kind mismatches, and types aggregate-RESULT, arithmetic-RESULT and
(G3, below) string/list/range-RESULT head columns (S3c: `c(N):-e(X),N=count()`
feeds INT into the head; `min`/`max` feed the source column's kind — mirroring
the typed path's `typeAggregate`).

Three follow-on slices closed most of what this section used to list as
permissive (G1/G2/G3, 2026-09-29; see the result nodes for the asymmetries):

- **G1 — populate-after-load.** A fact-load that NEWLY establishes a column
  kind on a db that already holds compiled rules RE-RUNS
  `checkAllRuleConstKinds` over the resident AST and FAILS THE LOAD loudly;
  the newly-recorded kind is rolled back on rejection. MEASURED: declare
  `e/1 f/1` empty, `dl_load_rules("q(X):-e(X),f(X).")` rc=0, load int `e`
  rc=2, then load sym `f` -> rc=-1,
  `variable X joins integer and symbol columns (e/0 and f/0)`; a retry with
  int `f` then loads rc=2 (the rejected load left `f` UNKNOWN). The re-check
  fires once per column-kind establishment (a `0 -> definite` transition),
  never per query and never on an idempotent re-load, so a relation filled
  over several same-kind loads is unaffected.
- **G2 — mixed-head across rules.** A head column that is definitely INT in
  one rule and definitely SYM in another is a loud error naming both rules.
  MEASURED: `p(X):-e(X).` (e int) rc=0, then `p(X):-f(X).` (f sym) -> rc=-1,
  `head column p/0 holds integers in rule 1 and symbols in rule 2`.
- **G3 — builtin result kinds.** The RESULT var of `length`/`concat`/
  `lower`/`upper`/`cdr`/`cons`/`append`/`range` now carries a kind
  (documented in `docs/language.html`), so the R1 join and R2
  ordered-comparison checks see it. MEASURED: `N=concat(S,S),N>2` -> rc=-1
  (ordered cmp over a symbol result); `N=length(S),f(N)` with f sym -> rc=-1
  (join); the controls `N=length(S),N>2` and `N=length(S),M=N+1` still load.
  Review fix: `car` returns the head ELEMENT, not a handle (test_lists T4/T9
  pin H==X/H==1/H==7), so NO single kind is sound for it and it stays
  UNKNOWN (a `car` result in a comparison is unchecked — see below);
  arithmetic over a true handle (`M=L+1` over `L=cons(..)`) is now loud too.

What deliberately stays permissive (each MEASURED against the shipped code):

- **`X != foo` on an int column** — collision-prone BY DOCUMENTED CONTRACT
  (`docs/language.html` comparisons section, suite-pinned `test_m9_arith`
  T8d): the symbol constant is interned and its id compared against raw
  ints, so a small id can equal a raw value. Order-dependent by design.
- **variadic relations** — kind-untracked: the variadic add path
  (`dlAddFactVariadic`) never calls the kind-recording helpers, so a variadic
  column can silently hold mixed int/sym. MEASURED: variadic `v`,
  `dl_add_fact(v,(1,5))` rc=1 then `dl_add_fact(v,(a,7))` rc=1, no conflict.
  Deliberately NOT fixed — per-arity variant kind tracking is an
  architectural add with no measured silent-wrong in any suite.
- **the raw-u32 `dl_add_fact` / `dl_txn_add_fact` symbol gap** — this path
  records INT only when a fact is a column's first row and its value does not
  resolve as a live sym id; a symbol-definite first fact is ambiguous to the
  raw-u32 API and neither records a kind nor triggers the G1 re-check.
  MEASURED: the CSV path rejects `q(X):-e(X). p(X):-e(X),!q(3).` after a sym
  `e` load (rc=-1), while the same conflict via `dl_add_fact` with the
  interned id stays rc=1. The documented floor of `kindNoteRaw` (a
  loud-but-wrong reject is worse than the gap).
- (CLOSED by the review fixes — kept for the record) a relation holding
  recorded INT FACTS whose column a rule then derives SYM into: the G2 head
  fold now seeds the column's schema-or-recorded kind, so
  `p` with recorded INT facts then `dl_load_rules("p(X):-f(X).")` (f sym)
  is LOUD (`head column p/0 holds integers in its recorded facts but
  symbols in rule 1`). The matching-kind control still loads.  The seed
  binds only where the relation actually HOLDS ROWS: a FRESH program
  deriving symbols into an EMPTY relation whose INT kind was persisted by
  a PREVIOUS session's rules LOADS (MEASURED at HEAD a89c8d5: rc=0) —
  deriving into an empty column collides with nothing, and the
  rows-arrive-later case is G1's populate-after-load recheck.
- **`sum` over a symbol source** — `count`/`sum` are typed INT by
  construction regardless of the source operand's kind, so summing a symbol
  column's ids is not rejected. MEASURED: `n(S):-s(Y),S=sum(Y).` (s sym)
  rc=0.
- **a `car` result in any comparison or join** — `car` returns the head
  ELEMENT (an int, a sym id, or a nested-list handle), so its kind is
  data-dependent and unchecked. MEASURED: `H=car(L),H>5` over
  `L=cons(X,[8])` with X int loads and answers the element (the review's
  false-positive control). This is the price of soundness: no single kind
  fits every element.
- **`member(X, L)` as a generator over a NON-literal or mixed L** — X's kind
  is data-dependent. When L is a constant literal of one kind (all ints or
  all syms) X IS kinded (an ordered cmp over a sym-element literal is loud);
  over a var list, a mixed/nested literal, or a `|` tail it stays unchecked.
- **a list result joined against an int/sym column, or in a `!=`** — a term
  handle is `>= 0x80000000` and never collides with a small raw int or sym
  id, so the join and `!=` checks stay INT-vs-SYM only. MEASURED:
  `T=cdr(L),e(T)` (e int) rc=0 and `T=cdr(L),T!=1` rc=0. Only an ordered
  comparison over a list result is loud.
- **a list-derived head in a downstream ordered comparison** — list results
  are not fed to the head-kind fixpoint (`KIND_LIST` has no `kindJoin`
  meaning and is never recorded), so such a head propagates as UNKNOWN and a
  later rule's ordered cmp over it stays permissive. MEASURED:
  `lh(L):-p(X),L=cons(X,[7]).` rc=0 then `r(T):-lh(T),T>2.` rc=0. The
  same-rule producer case (a list result compared in its own rule) IS loud
  (`T=cdr(L),T>2` -> rc=-1).
- **`range` over a relation whose leading column is UNKNOWN** — `range` is
  data-dependent: its result takes the named relation's `col0` kind when that
  is known (MEASURED: `range(X,rr,0,5),X>2` over a sym `rr` -> rc=-1; over an
  int `ri` -> rc=0), and stays UNKNOWN when the relation is fresh / empty.

The historical 32-distinct-var truncation of the per-rule kind tables is
GONE (S3c): both tables are `MAX_VARS`(64) sized, matching the loud
`exceeds the maximum of 64 distinct variables` rejection `compile_one`
already issues, so a wide rule can no longer silently evade the check
(MEASURED pre-fix: a 4-relation arity-8 program with 32 filler vars plus the
cross-kind join loaded rc=0 while the same join with 24 fillers was loud;
post-fix both are loud, and a 33-var all-INT join still loads).

