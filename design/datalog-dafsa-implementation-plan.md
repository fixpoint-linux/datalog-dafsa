# Implementation Plan: DAFSA-Backed Deductive Database (Datalog VM)

**Status:** M0-M3 complete (2026-08-11). 62 tests green (M0=9, M1=18, M2=18, M3=17 + smoke). Next: M4.
**Goal:** Turn the architecture into a concrete, executable build plan with file layout, per-milestone tasks, and verification for each step.
**Repos involved:** this project (new, e.g. `~/projects/datalog-dafsa/`), consuming the proven jing-meta DAFSA C engine (`indexer/dafsa/dafsa.c` + `dafsa_core.c` + `dafsa_persist.c` + `dafsa_view.c` + `dafsa_wal.c` + `dafsa.h`).

---

## 0. Working principles

- **Reuse, don't re-port.** The DAFSA engine in `jing-meta/indexer/dafsa/` is correct, WAL+mmap-capable, and measured (~5.8 B/key). Copy it into this project's `vendor/` and treat as frozen (no edits unless a blocker is proven).
- **Byte-for-byte deterministic facts.** Fixed-width u32-BE column encoding (per architecture §2) is the linchpin — validate it early (M0) before building anything on top.
- **Vertical slice first.** M0+M1 = a working non-recursive Datalog evaluator end-to-end (load facts → query), then layer recursion/negation/aggregates.
- **Test at each milestone.** Every milestone has a deliverable + a concrete test. Use property-based tests for the stratification checker (M2) — silent-wrong-answer bugs are the classic Datalog footgun.
- **Read-heavy contract.** Lifecycle is load → declare → compile → publish-snapshot → serve-reads. Single writer, multi reader (mmap RO). Confirm this framing with the user before M0 (architecture §7, §10-risk-8).

---

## 1. Project scaffolding

```
~/projects/datalog-dafsa/
  Makefile                  # builds libdatalog.so + dl CLI
  CMakeLists.txt            # (optional alternative)
  vendor/                   # frozen dafsa.c/.h + dafsa_core/persist/view/wal + crc32
  src/
    dl.h                    # public C API (dl_open, dl_load, dl_declare, dl_compile, dl_publish, dl_query)
    intern.c/.h             # term interner: symbol DAFSA (str->u32) + reverse array (u32->str)
    relation.c/.h           # per-relation DAFSA, fixed-width encoding, prefix/exact primitives
    parser.c/.h             # Datalog v1 parser (facts, rules, negation, aggregates)
    compiler.c/.h           # rules -> bytecode + stratification pass
    vm.c/.h                 # bytecode interpreter (binding-table, semi-naive fixpoint)
    snapshot.c/.h           # publish snapshot, mmap RO views, hot-relation cache
    regexwalk.c/.h          # regex->DFA + automaton intersection (M5)
    permindex.c/.h          # permutation indices (M6)
  pydl/                     # Python ctypes shim: parser/REPL/fact-load/tests
    __init__.py
    dl.py                   # ctypes bindings to libdatalog.so
  dl/                       # CLI entry (dl load/lookup/prefix/query/compile)
  tests/                    # C + Python test corpora (incl. known-bad Datalog programs)
  README.md
```

**Copy step (M0 start):** `cp jing-meta/indexer/dafsa/{dafsa.c,dafsa_core.c,dafsa_persist.c,dafsa_view.c,dafsa_wal.c,dafsa_crc32.c,dafsa.h,dafsa_internal.h} vendor/`. Add a `vendor/README` noting the source revision and the "treat as frozen" rule.

---

## 2. Milestones with concrete tasks

### M0 — Fact store + interner (~1 wk)
**Goal:** a queryable fact store using exact + prefix DAFSA primitives. No rules.

Tasks:
1. Vendor dafsa.c into `vendor/`; wire a Makefile that builds `libdatalog.so` from vendor + `relation.c` + `intern.c`.
2. `intern.c`: symbol DAFSA (`add_symbol(str) -> u32`, `symbol_str(u32) -> str`) + reverse array. Ints stored raw, strings interned.
3. `relation.c`: fixed-width u32-BE encoding. Implement `rel_add(rel, cols[], arity)`, `rel_exact(rel, prefix_len, key)`, `rel_prefix(rel, prefix_len, key, cb)`.
4. Public API `dl.h`: `dl_open(path)`, `dl_load_facts(rel, csv)`, `dl_lookup(rel, cols)`, `dl_prefix(rel, prefix_cols)`.
5. CLI: `dl load facts.csv`, `dl lookup edge A B`, `dl prefix edge A`.

**Verification:**
- Unit: encode/decode round-trips for mixed string/int facts; arity ≤ 8.
- Byte-stability: same facts → same DAFSA bytes (deterministic encoding).
- CLI: `dl lookup edge A B` returns the row; `dl prefix edge A` enumerates all `edge(A, *)`.
- **Encoding validation (the linchpin):** confirm `rel_prefix` with k bound leading columns == `dafsa_prefix_enum(prefix=k*4 bytes)` (i.e. exactly the two DAFSA primitives; architecture §0).

**Deliverable:** fact store queryable via exact + prefix. **Exit:** the two-primitive query path works and is byte-deterministic.

---

### M1 — Rule parser + compiler + non-recursive VM (~1–2 wk)
**Goal:** `tc(X,Y) :- edge(X,Y).` works; the 2-body recursive rule is evaluated as a single step (no fixpoint yet).

Tasks:
1. `parser.c`: Datalog v1 subset — facts, rules with conjunction, variables, constants, `:-` head/body, `,` body sep. No negation/aggregates/recursion-yet (parse them but reject at compile).
2. `compiler.c`: rules → bytecode; resolve relation/column refs; assign binding-table slots; reject unknown predicates, arity mismatch, ungrounded heads.
3. `vm.c`: instructions `OPEN_REL`, `SCAN`, `LOOKUP`, `EQ`, `EQ_CONST`, `NESTED_LOOP`, `PROJECT`, `HALT`. Hard-coded index-nested-loop join (prefix-bound, per architecture §5).
4. CLI `dl query 'tc(X,Y):-edge(X,Y).'` + a `.dl` file runner.

**Verification:**
- Known-good: a small `edge` + one non-recursive rule returns correct tuples.
- Known-bad: ungrounded head, arity mismatch, undefined predicate → clear compile error.
- Join correctness: a 2-relation body join (e.g. `path(X,Z):-edge(X,Y),edge(Y,Z).` non-recursive) returns the transitive pair set.
- Property test: random small fact sets → VM result == naive Python set-comprehension result.

**Deliverable:** non-recursive Datalog evaluator. **Exit:** join + projection correct on the non-recursive subset.

---

### M2 — Semi-naive fixpoint + stratified negation (~1 wk)
**Goal:** recursion reaches fixpoint; `!atom` works.

Tasks:
1. `vm.c`: `DELTA_NEW`, `DIFF`, `FIXPOINT`, `STRATUM` instructions; delta-relation loop.
2. `compiler.c`: stratification pass — assign strata; **reject un-stratifiable rulesets with a clear error** (negation through recursion cycle).
3. Recursive `tc/2` reaches fixpoint; `!edge(X,Y)` in a body works.

**Verification (this is where silent-wrong-answer bugs live):**
- Recursion: transitive closure on a small graph → correct fixpoint (matches Python).
- Negation: `path(X,Y) :- edge(X,Y), !blocked(X,Y).` correct.
- **Property tests of known-bad programs:** negation cycle, clique, negation-in-recursion variants → all REJECTED at compile with clear errors. Never silently accepted.
- Stratifier unit tests: correct stratum assignment on a mixed ruleset.

**Deliverable:** full Datalog recursion + stratified negation. **Exit:** fixpoint correct; un-stratifiable programs rejected.

---

### M3 — Aggregates + equality + disjunction (~1 wk)
**Goal:** count/sum/min/max grouped; equality; disjunction via multiple same-head rules.

**STATUS: COMPLETE (2026-08-11).** Soufflé-style `Var=agg()` syntax (`N = count()/sum(X)/min(X)/max(X)`); group-by = head vars excluding the aggregate result var; equality `X=Y` (var-var only, constants stay in atom args); disjunction via multiple same-head rules. 17 M3 tests incl. 50-iteration property tests (count/sum/min/max vs C reference) + reviewer adversarial pass (38 inputs) = CLEAN. New opcodes OP_AGG_ACC=8, OP_AGG_EMIT=9. Rejections: aggregate in recursive rule, >1 aggregate/rule, negated aggregate, min/max without source var, constants in aggregate-rule head. sum wraps u32. Build switched to static linking for `dl` + test binaries (sandbox busybox sh can't exec dynamic ELF subprocesses).

Tasks:
1. `vm.c`: `AGG_*` instructions; grouped aggregates (count/sum/min/max). **min/max are free via big-endian encoding** (architecture §4.2).
2. Equality `X = Y` in bodies.
3. Disjunction: multiple rules for same head (already structural; test it).

**Verification:**
- Aggregate correctness vs Python (grouped count/sum/min/max on random sets).
- Equality binding correctness.
- Disjunction union correctness (no dup, no drop).

**Deliverable:** aggregates + equality + disjunction. **Exit:** grouped aggregates correct.

---

### M4 — Snapshot publish + query path (~1 wk)
**Goal:** full load → declare → compile → publish-snapshot → serve-reads lifecycle.

Tasks:
1. `snapshot.c`: `dl_publish_snapshot` — run fixpoint once, write materialized goal relations to disk atomically (versioned dirs + CURRENT pointer flip, tmp+fsync+rename+dir fsync, same pattern as `dafsa_save`). Permutation indices deferred to M6 (matches architecture §86/§320/§322-324).
2. mmap read-only views; hot-relation view cache (`dl_query` opens RO mmap per query).
3. `dl_open`/`dl_query` read path.

**Verification:**
- Lifecycle: load → compile → publish → query returns correct results.
- Snapshot atomicity: kill mid-publish → old snapshot intact (crash test).
- Read path: multiple concurrent read-only queries from the same snapshot are safe (mmap RO).

**Deliverable:** end-to-end usable system. **Exit:** load-compile-publish-query works; snapshot is atomic.

---

### M5 — Regex/pattern walker (~1 wk)
**Goal:** third fact-store primitive live.

Tasks:
1. `regexwalk.c`: regex → DFA compiler (borrow `rust-fst` `Automata` trait shape + `regex-automata` DFA builder; cap DFA states ~50k per architecture §10-risk-6).
2. `dafsa_view_automaton_walk`: automaton-intersection walk (FST/DAFSA × regex-DFA in lockstep).
3. `vm.c`: `WALK` instruction; expose `dl query` with a pattern predicate.

**Verification:**
- Pattern query over keys returns correct match set (matches Python `re`).
- DFA-state cap: pathological regex rejected, not OOM.
- **Exit:** regex/pattern walker correct + bounded.

---

### M6 — Permutation indices + hash-join fallback (~1 wk)
**Goal:** join strategy fully covers arbitrary access patterns.

Tasks:
1. `compiler.c`: observe access patterns; declare permutation indices for non-leading-column join keys (Soufflé-style index-building pass).
2. `permindex.c`: index builder runs at publish time; index update on fact load.
3. `vm.c`: `HASH_JOIN` for residual non-leading-column joins.

**Verification:**
- Non-leading-column join (e.g. join on 2nd column) is correct and indexed.
- Hash-join fallback correctness vs index-nested-loop.
- **Exit:** arbitrary join access patterns covered.

---

### M7 — Durability wiring (~1 wk)
**Goal:** crash-safe publish.

Tasks:
1. Wire WAL + compaction (already in dafsa.c) into the publish lifecycle.
2. Snapshot atomicity already in M4; add WAL-replay on open.
3. Enforce single-writer via `flock` on a lockfile in the DB dir (fixes the inherited jing-meta "no file locking" caveat, architecture §10-risk-7/9).

**Verification:**
- Crash mid-append → WAL replay recovers.
- Concurrent publish attempt → second writer refused.
- **Deliverable:** crash-safe, single-writer publish.

---

### M8+ — v2 (deferred, not scoped here)
magic-sets, bushy plans, IVM, time-travel, variable arity, trace compilation.

---

## 3. Cross-cutting tasks

- **Python shim (`pydl/dl.py`):** ctypes bindings tracked alongside each milestone so the CLI/REPL/tests are Python-driven from M0.
- **Test corpus:** a `tests/programs/` dir with known-good and **known-bad** Datalog programs (negation cycle, clique, ungrounded, arity mismatch, cartesian-product blowup). The known-bad corpus grows at M1–M2 and is the backbone of the property tests.
- **Performance baseline:** after M4, benchmark `dl query` on a synthetic graph (e.g. 1M edges) and compare against Python + (optionally) Soufflé to validate the read-heavy claim and the embeddability-vs-throughput tradeoff (architecture §11, §10-risk-8).

---

## 4. First milestone to start

**M0.** The whole design stands on the fixed-width encoding + the two-primitive query path. Validate it first: vendor dafsa.c, build the interner + relation layer, and confirm `rel_prefix(k bound cols)` == `dafsa_prefix_enum(k*4 bytes)`. If that holds, the rest of the plan is unblocked; if not, revisit architecture §0 before writing the VM.

---

## 5. Open decisions to confirm before M0

1. **Framing:** read-heavy + embed-in-a-larger-system is what justifies building this vs using Soufflé. Confirm this is the actual goal (architecture §7, §11).
2. **Project location/name:** e.g. `~/projects/datalog-dafsa/`. 
3. **Term interner scope:** strings interned, ints raw (architecture §3). Confirm u32 ids are acceptable (symbol space 4G, arity ≤ 8).
4. **Regex walker in v1.1 (M5)** vs deferring — it's a differentiator but not needed for core Datalog; keep as scoped M5 but movable.
5. **Int/symbol id collision (B6):** A raw integer column value and an interned sym_id share the u32 namespace. If a relation column mixes raw ints (e.g., 1) and interned symbols (e.g., 'a' → some sym_id), they collide. In M1, relations should keep each column type-homogeneous (all ints or all symbols) to avoid this. This is a known design-level limitation; do NOT change the u32 encoding in M1.
