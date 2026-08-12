# Changelog

All notable changes to this project are documented in this file.

## [Unreleased]

### Added
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
