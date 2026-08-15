# Changelog

All notable changes to this project are documented in this file.

## [Unreleased]

### Added
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
