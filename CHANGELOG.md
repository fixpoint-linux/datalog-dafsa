# Changelog

All notable changes to this project are documented in this file.

## [Unreleased]

### Added
- **v2 DAFSA order-statistics — automatic perm-index selection**: a compile-time
  cost gate (`emit_nonleading_join`, src/compiler.c) that wires up the
  previously-dead `OP_HASH_JOIN` as a slot-free fallback for non-leading joins.
  A perm index is now declared only when the relation is large enough
  (`rel_count_subtree` ≥ `g_perm_card_threshold`, default 4) or already
  declared (reuse/dedup); small relations hash-join without declaring a perm,
  and perm-cap exhaustion (MAX_PERMS 64) degrades to hash-join instead of a
  hard compile error. Recursive body atoms always use `OP_LOOKUP_PERM` (the
  semi-naive override can't hash-join a recursive atom without a stale-DAFSA
  mis-eval). `OP_HASH_JOIN`'s `imm` now carries a packed 3-bit-per-column
  permutation, unpacked into a frame-local `vm_frame.perm_storage` (survives
  backtrack); pack/unpack symmetry verified over all 46,233 perms of arity
  1–8. New `dl_db_find_perm`/`dl_db_perm_count`. Semantics unchanged — the
  oracle test (T4) asserts `g_perm_select=1` vs `0` produce byte-identical
  query results. Tests: test_m14_permsel.c (9/9) — small→hash, large→perm,
  dedup, oracle (TC/3-col/IDB/negation), cap-exhaustion graceful, recursive
  non-leading == leading equivalent.
- **v2 DAFSA order-statistics — pull-based sorted iterator + merge-join**:
  resumable pull cursor replacing the callback fire-and-forget `rel_prefix`
  enumeration.  `dl_iter_open` / `dl_iter_seek` / `dl_iter_next` /
  `dl_iter_arity` / `dl_iter_close` (new `src/iter.c`) walk the minimized DAFSA
  with an explicit stack, yielding ONE lex-sorted (u32BE) tuple per
  `dl_iter_next` (end `0` vs error `-1` never conflated; absent prefix ⇒ valid
  empty iterator; variadic rejected loudly).  `dl_merge_join` is an
  O(n+m+out) equi-join on the first J columns — one-token lookahead +
  right-run buffering, duplicate-preserving, sorted output, drains both
  iterators, early-stop returns partial count.  Snapshot routing: owns an mmap
  `dafsa_view` (manifest_find_rel_ex + `dafsa_view_open`, not the LRU vcache);
  live path borrows `rel->d` via the new `rel_dafsa` accessor.  Tests:
  test_m13_iter.c (6/6) — pull==dl_prefix sorted, prefix bound k∈{0,1,2,arity},
  re-seek, snapshot publish/mutate-live, error matrix, merge-join vs
  brute-force with duplicates.
- **v2 DAFSA order-statistics — snapshot (mmap `dafsa_view`) rank/select**:
  `dl_rank` / `dl_select` / `dl_range_count` / `dl_count` now route to the
  published mmap snapshot view when `db->snap_version > 0` (exclusive with the
  in-memory `rel->d` path, mirroring `dl_query`/`dl_query_bound`/`dl_pattern`),
  so order-statistics work over a published snapshot.  New additive
  `vendor/dafsa_view_rank.c` walks the view's CSR + `final_bits` with the same
  distinct-key subtree-count recurrence (u64, state-0 sink, OOM-degrade);
  `src/snapshot.c` `view_rank`/`view_select`/`view_range_count`/`view_count`
  wrappers; `src/dl.c` `snapshot_view_open_rel` + `snap_version>0` routing.
  Routing lives only at the public `dl_*` layer (internal VM-at-publish
  evaluation of `rel_*` is untouched).  Tests: test_m12_snap_rank.c (6/6) —
  publish rank/select/range/count over arities 1-4 + suffix-sharing
  cross-product, snapshot-vs-live divergence + re-publish, empty snapshot,
  rejections, no-publish in-memory routing.  `dl_*_bound`/`dl_*_perm` remain
  in-memory-only (deferred).
- **v2 DAFSA order-statistics — `range(X, Rel, Lo, Hi)` predicate / `OP_RANGE`**:
  a reserved builtin (the `member(X,L)` analog for relations) that is a
  GENERATOR (X unbound → bind X to each DISTINCT leading-column value of Rel in
  the half-open range `[Lo,Hi)`, lex order) or FILTER (X bound). Lowered to a
  new `OP_RANGE` opcode. Enumeration reuses the shipped rank/select via a new
  `rel_range_each` (bounded scan; does NOT pull in the deferred pull-iterator).
  Range is on the leading column (col0). Two correctness gates: (1) `range`
  programs excluded from IVM/DRed/agg/magic/topdown (7 sites); (2) recursive-
  staleness — `range` over a recursive-SCC relation is REJECTED (OP_RANGE reads
  `rel->d` directly, which the recursive fixpoint never updates). Tests:
  test_m11_range.c (6/6) — generator/filter/arity-2 dedup/edge cases/random
  oracle/reject matrix.
- **v2 stratification correctness fixes** (found by deep-review of the range
  feature; both are PRE-EXISTING silent-wrong-answer bugs, now closed):
  - `range`'s target-Rel dependency was invisible to `compute_strata`
    (`ba->pred`="range", real read target in `args[1]`) — a range rule over a
    non-recursive IDB silently mis-evaluated. Fixed: strict (is_neg=1) edge
    Rel→head for range atoms, so the range rule lands in a strictly higher
    stratum than its target.
  - `eval_stratum_recursive` ran non-recursive rules in a SINGLE parse-order
    pass (no fixpoint) — a non-recursive chain sharing a recursive stratum was
    silently mis-ordered (reproducible WITHOUT range: `q:-p, p:-e` in a
    recursive stratum → q empty). Fixed: non-recursive rules in a recursive
    stratum now loop to a fixpoint (mirrors eval_nonrecursive).
  - `compute_strata` strict-cycle: a strict edge on a cycle ping-ponged to the
    10000-iteration cap and exited with changed=1, and the post-hoc check
    caught it only for SOME rule orders — a strict cycle could be silently
    mis-evaluated. Fixed: exiting the cap with changed still set is now a loud
    "unstratifiable program" error (both fixpoint loops).
- **v2 DAFSA order-statistics — permuted rank/select** (`dl_rank_perm` /
  `dl_select_perm` / `dl_range_count_perm`): rank/select/range_count over a
  PERMUTED view of a relation (order-by on a non-leading column).  The
  permuted relation is just the base relation re-encoded via a declared
  permutation (perm[j] = original column at permuted position j), so its
  DAFSA subtree counts give order statistics for free.  Rank/range_count
  FORWARD-map the original-order input to permuted order (pcols[j] =
  cols[perm[j]]); select INVERSE-maps its permuted result back to original
  order (cols_out[perm[j]] = pcols_out[j]); a select→rank round-trip is the
  identity.  The permuted DAFSA is built on demand (permindex_build) if NULL
  or dirty — never silently mis-evaluated.  Implements deferred follow-up #2
  of the range-index design.  Rejects bad perm_id / wrong-rel perm /
  arity-mismatch / variadic / NULL.
- **v2 DAFSA order-statistics — prefix-bound rank/select** (`dl_rank_bound` /
  `dl_select_bound` / `dl_range_count_bound`): restrict rank/select/range_count
  to tuples whose leading `k` columns equal a bound, applied to the remaining
  suffix columns.  Implements deferred follow-up #1 of the range-index design.
  The DAFSA `*_n` primitives are generalized to start-state (`_from`) forms
  (the shipped `*_n` are unchanged thin wrappers); `rel_prefix_state` walks the
  leading bound, then the `_from` variant runs on the encoded suffix key.
  Absent prefix → 0 / -1; rejects variadic/arity-mismatch/NULL.
- **v2 top-down STAGE C verification (tests only — no engine change)**: the
  negation + aggregate correctness matrix for `dl_query_topdown` (T16-T23 in
  `tests/test_topdown.c`), mirroring the forward-magic suite.  Covers negated
  EDB, negated non-closure IDB (empty `!tc` and non-empty `!bad`, incl. the
  auto-compile guard path), aggregates over EDB (group-by count/sum/min/max +
  global k=0), mixed negation+recursion, aggregate head → downstream rule,
  non-leading fb-adorn negation, a 40-graph property, and REJECT parity
  (negated closure IDB / negated closure const / aggregate over a closure or
  non-recursive IDB).  Every accepted case asserts
  `topdown == bound == magic` byte-for-byte.  This confirms the previously
  `deferred` STAGE C was already correct through the shared
  `magic_transform_adorn` + `dl_compile` + `vm_exec_rule` path.
- **Roadmap: pointwise / stratified negation over recursive predicates
  (deferred).**  Negation or an aggregate reading a predicate *in the adorned
  closure* is currently REJECTED as unsound in both `dl_query_magic` and
  `dl_query_topdown` (a magic-seeded slice is not the full complement).  Making
  it work requires an SLG extension that evaluates the negated subquery to
  completion before the enclosing rule proceeds.  Recorded in
  `design/datalog-dafsa-topdown-magic.md` (STAGE C) and the architecture
  roadmap; do not silently relax the current REJECT.
- **v2 top-down / QSQ magic evaluation** (`dl_query_topdown` /
  `dl_query_topdown_adorn`): a goal-driven, memoized 4th per-query path that
  reuses the forward-magic adornment machinery (`magic_transform_adorn` +
  `compile_rules` + `exec_rule` via a new `vm_exec_rule` body-primitive
  wrapper) but drives the bytecode demand-first instead of by fixpoint.  The
  SLG-style driver (`src/topdown.{h,c}`) maintains a per-variant memo/bound-set
  and uses a worklist (no C recursion), so a chain N=10000 single-source query
  terminates without stack overflow.  Rejects the same program classes as
  forward magic (negation-on-closure, aggregates, cross-predicate mutual
  recursion, closure blow-up).  Correctness oracle: byte-for-byte
  `top_down == filter(full_materialize)` and `== dl_query_magic_adorn`.
  Honest benchmark (vs `dl_query_magic_adorn`, opt-in via `RUN_BENCH=1`):
  chain TC ~0.9x (the plan's material correction — not the originally-advertised
  600x), but ~5x faster on dense graphs.  `make test` stays fast (benchmark is
  gated off the default path).
- **v2 Datalog lists — Phase 2 (patterns, member, list-length)**:
  - **`[X|Xs]` pattern destructuring** in positive body atoms (`p([a, X|Xs])`):
    lowered inline during the relational phase to the existing
    `OP_LIST_CAR`/`OP_LIST_CDR`/`OP_EQ`/`OP_EQ_CONST` opcodes — no new
    opcode. Each variable element binds (bind-or-filter), each constant
    element filters, a `|Tail` binds the remaining sublist, and an absent
    tail requires `cdr^n == NIL`. Pattern-bound vars are visible to
    subsequent relational atoms (unlike post-join builtins).
  - **`[X|Xs] = L` assignment form** (sugar over `X = car(L), Xs = cdr(L)`):
    a body atom whose LHS is a list pattern destructures the RHS list value
    via the same `emit_pattern` lowering, binding the pattern vars. The RHS
    may be a bound variable or a constant list literal; a lone `[X|_] = [1,2]`
    with a constant RHS drives a rule (no positive relational atom needed).
    Rejected loudly: a negated assignment form, and an assignment form with an
    unbound RHS list var (never silently destructure the UNBOUND sentinel).
    Nested-tail sugar `[a|[b]]` remains unsupported.
  - **`member(X, L)`** filter atom → new `OP_LIST_MEMBER` opcode: `X` bound
    → linear-scan membership test; `X` unbound → a generator `vm_frame`
    (mirroring `OP_MAT_JOIN`) enumerating `L`'s elements.  A lone
    `member(X, L)` with a bound-or-constant `L` may drive a rule with no
    positive relational atom (`r(X) :- member(X, [1,2,3]).`).
  - **`length`** runtime dispatch: `OP_STR_LEN` now dispatches a list handle
    to `term_length` before the `strlen` path (`length([])` == 0,
    `length("s")` unchanged).
  - **Loud compile-time rejects** (never silently mis-evaluated): list
    patterns in rule heads/facts, negated atoms, and list-builtin / member /
    length operands; `member`/`length` operands that are ungrounded or
    malformed. `member` and `length` are reserved predicate names — and, like
    the list/string builtins, are now rejected loudly as rule HEADS too
    (`member(1,2).` reports "reserved builtin predicate name" rather than the
    misleading "rule member has no body").
  - **`_` is a plain named var, not anonymous**: `[X|_]` binds the tail to a
    variable literally named `_`, so two `_`s in one rule unify.  This is
    consistent with the dialect's existing `dummy(_)` semantics (there is no
    separate anonymous-var token); use distinct names for distinct tails.
  - **Gating**: `OP_LIST_MEMBER` joins the `db_has_list_builtin` exclusion
    (IVM/DRed/agg/magic-sets route to the full fixpoint); `length` stays
    un-flagged (a pure read, preserving string-IVM eligibility).
  - Tests `tests/test_lists` T9 (patterns + car/cdr equivalence), T10
    (member generator/filter), T11 (length dispatch), plus expanded T8
    reject cases.

- **v2 variable-arity relations**: a relation declared variadic
  (`dl_declare_relation_variadic`, or `dl_declare_relation` with arity 0)
  holds facts of ANY arity 1..8. Storage is PER-ARITY-VARIANT DAFSAs: each
  arity a is an ordinary fixed-width relation keyed 4·a+1 bytes under
  `<name>.<a>.dafsa` (+ `.wal`, `.base.dafsa`), so the fixed-width key
  encoding — the byte-prefix-lookup linchpin — is UNCHANGED.
  - `dl_add_fact`/`dl_delete_fact` route to the fact's own arity variant
    (WAL-durable); `dl_load_facts` takes variable-width CSV rows (row field
    count = arity); `dl_prefix`/`dl_query`/`dl_query_bound`/`dl_pattern` fan
    out over variants a ≥ k (the callback's arity parameter disambiguates
    tuples); snapshot publish writes a `name:*:edb|idb` manifest marker plus
    ordinary per-variant lines, and the mmap query path fans out over them.
  - Compiler: a variadic atom accepts any syntactic arity 1..8 (each atom's
    nargs is static); rule heads materialize their variant at compile time;
    permutation indices are per-variant. AGGREGATES over a variadic relation
    and RECURSIVE variadic heads are compile errors (never mis-evaluated).
  - Gating: a program containing a variadic relation is excluded from IVM /
    DRed / aggregate maintenance and magic-sets queries — every publish runs
    the full fixpoint (the correctness floor).
  - Backward-compatible strict superset: fixed relations keep their exact
    v1 files and `name:arity:edb|idb` lines; older binaries skip the new
    `name:*:...` marker (arity parse rejects it). No migration.
  - 10-test suite `tests/test_vararity` (single-variant≡fixed equivalence,
    variable-tail prefixes, fixed-arity-equivalent oracle incl. cross-arity
    joins, randomized property tests vs a brute-force model, gating via the
    `vm_*_runs` counters, compile-error, persistence, snapshot, magic
    rejection, fixed coexistence).

- **v2 IVM (incremental view maintenance)**, Slices 0-5: the fixpoint is now
  incrementally maintained instead of fully re-evaluated on every change.
  - **Slice 0 — deletion-correctness**: a base/view partition inside each
    relation (`base` = durable EDB facts, `view` = base ∪ derived) fixes a
    pre-existing silent wrong answer where `dl_delete_fact` + re-publish left
    stale derived tuples (recursive `tc` returned 6 rows after deleting
    `edge(2,3)`; an aggregate kept a stale `(1,15)`). `vm_execute` now resets
    every rule-head view to a copy of base before re-evaluating. IDB base
    persists to `<name>.base.dafsa`; rels.txt + snapshot manifest gain a
    backward-compatible `edb|idb` flag.
  - **Slice 1 — insert-only IVM**: `dl_add_fact` captures a +delta; the publish
    dispatch propagates eligible deltas through dependent rules (semi-naive
    single-step via the existing `exec_rule` override) instead of a full
    fixpoint. Anything outside the eligible class (recursion, negation,
    aggregates, OP_WALK/OP_LOOKUP_PERM) falls back to a full re-eval.
  - **Slice 2 — recursive insert IVM**: the semi-naive recursive fixpoint is
    seeded from the current view + the new base delta (never reset on the IVM
    path). `vm_ivm_eligible` accepts recursive rules under a correctness
    restriction.
  - **Slice 3 — DRed deletion**: on delete, over-delete + re-derive for the
    monotone + stratified-negation cases, with a mandatory full-re-eval
    fallback for recursion/aggregates/anything unproven. Recursion+deletion
    falls back to full re-eval.
  - **Slice 4 — aggregates under change**: count/sum/min/max via affected-group
    re-scan (uniformly correct incl. delete-an-extremum); non-tractable shapes
    (shared head, repeated-variable anchor, derived anchor, etc.) fall back.
  - **Slice 5 — bulk-load IVM + persistence**: `dl_load_facts` is a batched
    delta through the existing dispatch; `write_rels_txt` hardened to atomic
    (tmp+fsync+rename).
  - Correctness is guaranteed throughout by a full-re-eval fallback (never
    silently mis-evaluate) and by equivalence-oracle property tests comparing
    IVM-maintained views to full re-eval byte-for-byte over random
    add/delete/bulk sequences. 25 IVM tests; full suite 306 tests green.
- **M9 arithmetic + comparison builtins**: non-relational builtins extend the
  existing equality to a full comparison / arithmetic set:
  - **Comparisons** `<`, `<=`, `>`, `>=`, `!=` as infix body atoms (operands =
    variable or integer literal; `!=` also accepts a symbol constant).
  - **Arithmetic** `X = E` over `+ - * / %` via a precedence-climbing
    expression parser (`* / %` bind tighter, left-associative, parentheses).
  - Two generic VM opcodes: `OP_CMP` (filter; false → backtrack) and
    `OP_ARITH` (bind; result written via `b_try`, never overwrites).  An
    arithmetic result lowers to `OP_ARITH` into a fresh temp then `OP_EQ`
    reusing the existing bind-or-filter invariant, so both "bind a new var"
    and "filter a pre-bound var" work.  Division/modulo by zero backtracks
    (never crashes); integers wrap mod 2³² (matching the sum aggregate).
  - A builtin-safety pass rejects ungrounded operands, negated builtins, and
    division/modulo by a literal 0 loudly (never silently mis-evaluates).
    Arithmetic is allowed in recursive rules; aggregates remain banned there.
  - magic-sets integration: SIPS/`compute_betas` treat comparison as
    zero-propagation and arithmetic as result-var propagation, so
    `dl_query_magic` over an arithmetic rule matches full materialization.
  - Operands are interpreted as raw u32 (the documented B6 int/symbol
    limitation); arithmetic on a symbol-constant operand is rejected at parse
    time.  Suite: `tests/test_m9_arith.c` (11 tests incl. 200 seeded property
    cases against a C reference).
- **M9-strings string builtins**: `concat` (produces a new string value via
  runtime interning), `length` (byte length, UTF-8 counted per byte), `lower` /
  `upper` (ASCII case folding), and `prefix` / `suffix` / `contains` (pure
  filters).  Function-call syntax reusing M9's `X = name(args)` for producers
  and bare atoms for filters.  Three new VM opcodes
  (`OP_STR_FILTER`/`OP_STR_LEN`/`OP_STR_BIND`); only `OP_STR_BIND` interns
  (builds a heap buffer, calls `intern_str`, >4096-byte result backtracks).
  New symbols persist with the interner on `dl_close` / `dl_publish_snapshot`,
  identical to fact-load interning.  Suite: `tests/test_m9_str.c` (9 tests
  incl. empty-string-literal regression + 200 seeded property cases).
- **M9 compiler hardening (nits)**: (1) generated constant/temp slot names
  (`__kN`/`__tN`) no longer collide with a user variable literally named
  `__k0`/`__t0` — a new `v_fresh_name()` skips past names already in the var
  table, fixing a potential silent wrong answer (regression test T11).  (2)
  rules exceeding the 64-var/temp/constant budget now print a loud
  diagnostic instead of failing silently.  (3) a failed string-constant
  intern (OOM or >4096-byte key limit) prints a diagnostic (with the constant
  truncated) instead of a silent `goto fail`.
- M8 magic-sets: the full deferred magic-sets plan (slices 1–5), plus a
  correctness fix, are now implemented.  `dl_query_magic` /
  `dl_query_magic_adorn` are opt-in per-query paths that re-evaluate a scoped
  clone of the EDB seeded by the bound query arguments, coexisting with the
  publish-once model:
  - **First slice**: single goal predicate, leading-prefix-bound adornment,
    all-EDB + self-recursive-through-same-adornment bodies.
  - **Multi-predicate dependency closure**: a goal may depend on other IDB
    predicates; every predicate in the goal's dependency closure is adorned
    and magic-synthesized.
  - **Non-leading-prefix adornments (fb/bfb)**: a goal may be bound on an
    arbitrary subset of positions, not just a leading k-prefix.
  - **SIPS body reordering**: always-on deterministic greedy reordering of
    rule bodies to maximize binding propagation.
  - **Negation + aggregates**: supported under a conservative soundness
    boundary — a negated atom is allowed only on a predicate OUTSIDE the
    adorned closure (EDB, or an IDB not reached by positive references), and
    an aggregate only in a rule with no adorned-closure body atom.  Negation
    on an adorned-closure predicate and aggregates over a closure predicate
    are rejected with a clear reason (never a silent wrong answer).
    `dl_query_magic` materializes the source fixpoint (internal `dl_compile`)
    when a rule uses negation/aggregate and the fixpoint is dirty, so a
    negated non-closure IDB is evaluated against its full materialization —
    the no-mutation guarantee now holds only for all-positive programs.
  - **Adornment-closure fixpoint**: a predicate may carry multiple distinct
    adorned variants (e.g. `tc__bf` and `tc__bb`) discovered by a closure
    fixpoint, removing the last magic-sets reject.  `dl_query_magic_adorn`
    gained a `size_t src_nrels` parameter (tightened MAX_RELS budget).
  - Unsupported cases stay loud REJECTs (never silently mis-evaluated).
  M8 magic-sets suite now 43 tests; full M0–M8 suite green.
- Design note `design/datalog-dafsa-topdown-magic.md`: architecture-of-record
  for a DEFERRED top-down / QSQ evaluation strategy (the one architectural
  change that would make magic-sets compute O(N) per-source reachability
  instead of the O(N²) forward-chaining closure, ~600–1000× on reachability
  queries in the no-publish regime).  Deferred pending a real workload
  (selective queries on an unpublished db) — it cannot beat `dl_query_bound`
  on a published db (~100× constant-factor gap remains intrinsic).  §8
  documents that top-down and the publish-once model coexist orthogonally
  (top-down is an opt-in 4th query path) and can mutually accelerate
  (top-down reads a published snapshot for its EDB leaf lookups; top-down
  memo results can seed a later publish), with a proposed routing rule.
- Top-level `README.md` (intro, quickstart, how-it-works, API summary,
  milestone table, build/license notes).
- `CHANGELOG.md` and `LICENSE` (MIT).
- `tests/bench.c` demonstration benchmark (`make bench`): transitive closure,
  prefix-query throughput, and regex-walk workloads.
- Wired the previously-orphaned `tests/test_m4_review.c` (16 tests) and
  `tests/test_m5_review.c` (60 tests) into the Makefile build and `make test`;
  removed the stale `tests/test_review_adversarial` target (source did not exist).
  Completing `test_m5_review.c` (its `main()` had only run one case) surfaced and
  fixed real `regexwalk.c` bugs: an infinite loop on a trailing-backslash pattern,
  `regex_compile` dropping a lexer error on the "success" path, and character-class
  handling of metacharacters and descending ranges.
- `.gitignore` entries for the new test binaries and the bench binary.

### Rejected
- **Trace/JIT compilation of hot rules** (2026-08-15): rejected. Measurement and
  code inspection show the bytecode interpreter is not the fixpoint bottleneck —
  the cost is dominated by C tuple/DAFSA operations that a native-compiled rule
  body would still call into. QBE (the candidate backend) was evaluated and
  found to emit assembly text rather than executable memory, so it does not
  solve the asm→executable half of a runtime JIT. Reconsidered with DynASM
  (LuaJIT's runtime assembler, which does emit executable memory): the verdict
  stands — the bottleneck is intrinsic memory-bound DAFSA data access, not
  dispatch; full trace specialization would reimplement vendored private structs
  in dual-arch asm for a ~3–15% win that interpreter-level optimization
  recovers at zero dependency cost. See design §10.8. If a dispatch-bound
  workload ever emerges, interpreter-level optimization comes first, not a JIT.

### Fixed
- **Silent wrong answer in the semi-naive fixpoint for multi-recursive-atom
  rules** (`dl_query_magic` returned a too-small result, e.g. `tc(1,?)` = 5
  tuples when it should be 6).  Root cause: a non-delta `OP_LOOKUP` recursive
  body atom reads the full-idb override via `ts_prefix` (binary search, needs
  sorted data), but `idb` was only sorted once at stratum start and grown
  unsorted via `ts_add`; with two `OP_LOOKUP` recursive atoms every delta
  firing leaves one on unsorted data.  Fixed in `eval_stratum_recursive`: (1)
  re-sort `idb` at the top of each iteration only when a stratum has a rule
  with ≥2 plain-`OP_LOOKUP` recursive atoms (`need_idb_sort`), and (2) freeze
  `idb` during the firing sub-loop (candidates go to `next_delta` only) and
  merge them into `idb` at rollover so the sort is never stale within an
  iteration.  Regression test T43.  Full M0–M8 suite green.
- Materialize goal-first non-recursive rule chains: `eval_nonrecursive` now
  evaluates to a fixpoint instead of a single emit-order pass, so a
  non-recursive dependency chain written goal-first is fully materialized.

## [0.1.0] — 2026-08-12

First milestone-complete release: M0–M7 of the DAFSA-backed Datalog engine.

### M0 — Fact store + interner
- Vendored the jing-meta DAFSA engine into `vendor/`.
- Fixed-width u32-BE fact encoding (the linchpin) with exact + prefix DAFSA
  primitives; symbol DAFSA interner (forward `str→sym` + reverse `sym→str`).
- `dl_open`/`dl_close`, `dl_declare_relation`, `dl_load_facts`, `dl_lookup`,
  `dl_prefix`; `dl` CLI load/lookup/prefix. 9 tests.

### M1 — Rule parser, compiler, non-recursive VM
- Datalog v1 parser (facts, conjunction, variables, constants, `:-`).
- Compiler: rules → bytecode, binding-table slot assignment, rejection of
  ungrounded heads / arity mismatches / unknown predicates.
- VM with index-nested-loop join primitives. 18 tests.

### M2 — Semi-naive fixpoint + stratified negation
- Delta-relation fixpoint loop; stratification pass that rejects
  un-stratifiable rulesets (negation through recursion) with clear errors.
- Recursive `tc/2` reaches fixpoint; `!atom` in bodies works. 18 tests.

### M3 — Aggregates + equality + disjunction
- Soufflé-style `Var=agg()` (count/sum/min/max), group-by via head vars;
  `min`/`max` fall out of the big-endian encoding.
- Body equality and disjunction via multiple same-head rules.
- Rejections: aggregate in recursive rule, >1 aggregate/rule, negated aggregate.
- Build switched to static linking. 17 tests.

### M4 — Snapshot publish + query path
- Atomic `dl_publish_snapshot` (versioned dirs + `CURRENT` flip, tmp+fsync+rename).
- mmap read-only views + hot-relation cache; `dl_query`/`dl_query_bound` read
  from snapshot when published, else fall back to the VM path.
- Full load → declare → compile → publish → serve-reads lifecycle. 8 tests
  (+16 adversarial review).

### M5 — Regex/pattern walker
- Regex → NFA → dense DFA compiler; full-key byte matching with implicit `^...$`.
- Automaton-intersection product walkers (in-memory + mmap) with cycle-safe
  visited set; DFA state cap (50k) with early-abort for pathological patterns.
- `dl_pattern`, CLI `dl pattern`, and `OP_WALK` VM instruction via `~ '<regex>'`.
- 25 tests (+60 adversarial review).

### M6 — Permutation indices + hash-join
- Per-column-prefix permuted DAFSA indices for non-leading-column joins;
  `OP_LOOKUP_PERM` / `OP_HASH_JOIN` opcodes.
- Dirty-tracked index rebuild before/after IDB materialization; snapshot
  persistence of permutation indices. 8 tests (+5 review +10 deep review).

### M7 — Durability wiring
- `fcntl` single-writer lock (`DL_E_LOCKED` on contention).
- Durable interner (atomic save, ordered before WAL records).
- Incremental `dl_add_fact`/`dl_delete_fact` with per-relation WAL, idempotent
  replay, and compaction at 25%. 14 tests.
