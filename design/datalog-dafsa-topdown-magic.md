# Design: Top-Down / QSQ Evaluation for Magic-Sets (DEFERRED — architecture of record)

**Status:** DEFERRED (2026-08-14). Architecture-of-record, not implemented. Magic-sets (M8, forward-chaining) is complete and correct; this document records the analysis of the one architectural change that could make magic *fast*, and why it is deferred pending a workload signal.
**Author:** advisor-offpeak (GLM-5.2 deep-think), routed tier=hard, analyzed against `main @ a991b2b` + fix `d4d0f27`.
**Grounded in:** measured profiling of `dl_query_magic` (the fixpoint is ~99.9% of cost; forward-chaining magic materializes the *entire* adorned closure on reachability-heavy workloads), the existing magic-sets slices (M8 v1–v5), and the `vm_override` / `exec_rule` / `magic_transform_adorn` / `eval_db_clone` seams in the code.

---

## 1. The problem (measured)

`dl_query_magic` re-runs a scoped semi-naive fixpoint on every call. Profiling shows the fixpoint is ~99.9% of the cost, and — critically — forward-chaining magic-sets materializes the **entire transitive closure** on reachability-heavy workloads:

| chain TC, single source | full materialize | forward magic | `dl_query_bound` (published) |
|---|---|---|---|
| N=1200 | ~1s | ~1.2s (719,400 adorned-closure tuples) | ~µs |
| N=10000 | ~110s | ~131s | ~µs |

The structural cause: the adorned rule `tc__bf(X,Y):-magic_tc__bf(X), edge(X,Z), tc__bf(Z,Y)`, evaluated bottom-up, computes `tc__bf(Z,*)` for **every magic-seeded node Z**, so it does the *same O(N²)* closure work as full materialization — just seeded from the bound nodes. magic/full-materialize ratio measures **0.8–1.3×**, and magic is ~2,000,000× slower per query than `dl_query_bound` on a hot mmap scan. **No forward-chaining tweak (R1/R2 from the fixpoint-opt plan, ~40%+~13%) fixes this — it only moves magic to ~0.5× of full materialize, never to a different complexity class.**

---

## 2. The only architectural fix: top-down / QSQ evaluation

Goal-driven (top-down) evaluation with **memoized subqueries** — the QSQ/SLG family — computes **only the reachable answer** instead of the full closure. For `tc(1,?)` on a chain it recurses on demand and memoizes each distinct subquery `tc(Z,?)` once, giving **O(N)** work per source query instead of O(N²).

### 2.1 It is genuinely feasible in this engine (validated against the code)

- **`exec_rule` (vm.c:364–992)** is already a goal-driven body walker with backtracking, and the **`vm_override`** mechanism (`vm.c:250–264 find_ov`) already redirects a body atom from its DAFSA to an arbitrary `tuple_set` — exactly the seam a QSQ driver needs: recursive IDB body atoms get overrides filled by a memoized subquery. **`exec_rule` is reused unchanged.**
- **`magic_transform_adorn` (magic.c)** already computes the adornment closure to a fixpoint and synthesizes the adorned rules `P__alpha(...) :- magic_P__alpha(bound head), body'`. Those are exactly the rule shapes top-down needs. **~80% of the machinery (parser AST retention, magic_transform_adorn, compile_rules, exec_rule, tuple_set, eval_db_clone) is reused**; the new code is one driver function (`eval_topdown`, ~150–300 lines) plus cycle detection + memo plumbing.
- **`dl_query_magic_adorn` (dl.c:1174–1369)** keeps steps 1–7 and 9; only step 8 (`vm_execute`) is replaced. The clone-and-scope pattern (`eval_db_clone`) is preserved → db byte-identical before/after, no mutation.

### 2.2 Quantified win (algorithmic complexity)

| workload | full materialize | forward magic | **top-down QSQ** | `dl_query_bound` (published) |
|---|---|---|---|---|
| chain TC, N=1000, source query | ~1s | ~1.2s (O(N²)) | **~2ms (O(N))** | ~20µs |
| chain TC, N=10000, source query | ~110s | ~131s | **~20ms** | ~70µs |

- **~600–1000× over forward magic** on reachability queries (O(N²) → O(N)).
- **~5000–10000× vs full materialization** in the **no-publish** regime.

### 2.3 The honest caveat: it cannot beat the publish-once path

Top-down still pays ~1µs/tuple of bytecode dispatch + memo bookkeeping, whereas `dl_query_bound` on a *published* db walks the pre-materialized DAFSA prefix at memory bandwidth (~10ns/tuple). So **~100× gap to `dl_query_bound` remains** — top-down closes the *algorithmic* gap, not the *constant-factor* gap. It is a **no-publish / selective-query** feature: it wins only when the db is *not* published (or published infrequently) and the reachable slice is genuinely small relative to the closure.

### 2.4 Where top-down is neutral or worse

1. **Dense graphs / near-complete queries** — every subquery's answer is large; top-down ≈ forward magic with extra memo overhead.
2. **Multi-source / all-pairs queries** — loses its selectivity advantage, and per-tuple cost is higher.
3. **Published db** — `dl_query_bound` is strictly faster.
4. **Negation / aggregates** — top-down still needs the full materialization of the negated/aggregated predicate (same soundness boundary forward magic enforces).

---

## 3. Memoization & cycle-handling design (for when it is implemented)

- **Key:** `(adorned-variant-id, bound-args-tuple)` — for canonical `tc__bf`, `(tc__bf, Z)`.
- **Storage:** one `tuple_set` per adorned variant (the variant's adorned IDB), holding accumulated answers across all subqueries. Per-subquery answers are slices via `ts_prefix(variant_ts, bound_args, k)`.
- **Recursion:** SLG-style continuation. Each subquery has a state `{NEW, IN_PROGRESS, COMPLETE}`; an in-progress subquery returns its *partial* answer by reference (subsequent `ts_add`s are visible to the caller), and the driver loops the pending-subquery worklist until a full pass adds nothing — a fixpoint scoped to the goal's reachable subqueries.
- **Critical:** use a worklist, **not** C recursion, to avoid stack overflow on long chains (N=10000).
- **Overrides must be stable within one `exec_rule` call** (memo may grow during recursion, but a single firing's overrides are frozen) — borrow the same discipline the semi-naive loop already relies on.

---

## 4. Coexistence & interaction with the existing paths

Top-down is a **4th per-query path, opt-in**, coexisting with the existing three:
- `dl_query` (full materialization)
- `dl_query_bound` (snapshot prefix scan — the publish-once fast path)
- `dl_query_magic` / `dl_query_magic_adorn` (forward-chaining scoped re-eval)

Add `dl_query_topdown` (or an `eval_strategy=TOPDOWN` flag on `dl_query_magic_adorn`), using the **same clone-and-scope eval pattern**. **Do NOT replace forward magic** — it remains the conservative correctness oracle (property test: `top_down(Q) == filter(full_materialize, Q)`). Do not touch the publish path.

---

## 5. Verdict & decision

**Advisor recommendation: DEFER** until a real workload demonstrates the niche. Top-down is the right architecture *if* magic is to deliver its original promise, but the engine's value prop is publish-once + mmap scan, which top-down cannot beat. It becomes worth building under any of:

- **(a)** a user reports per-query latency > 1s on an **unpublished** db for a reachability-shaped query (TC, ancestor, graph-reach, path-existence);
- **(b)** an embedding use case with ad-hoc selective queries against a large/infrequently-published db (interactive exploration, debug, partial-state inspection);
- **(c)** the project commits to interactive (non-publish) Datalog as a first-class mode.

Absent (a)/(b)/(c), the publish-once + `dl_query_bound` path is the right strategic bet. The tactical magic-side spend is **R1 from the fixpoint-opt plan** (skip the post-fixpoint DAFSA bulk-build in the magic clone, ~40% speedup, ~1 day, low risk).

**Decision recorded:** DEFERRED, 2026-08-14. Implement only when a workload signal (a)/(b)/(c) arrives. This document is the architecture-of-record to pick up at that point.

---

## 6. Ranked implementation plan (if/when pursued)

**Stage A — PoC, canonical case** (~2–3 days, `pro-coder`):
- NEW `src/topdown.h` + `src/topdown.c` (recursive memoized driver, ~200–300 lines).
- MODIFIED `src/dl.c` (add `dl_query_topdown`, ~80 lines, mirroring `dl_query_magic_adorn` steps 1–7 + 9, replacing step 8 with `topdown_eval`); MODIFIED `src/dl.h`; MODIFIED `Makefile`; NEW `tests/test_topdown.c`.
- Reuse `magic_transform_adorn` **unchanged**.
- Expected win: chain N=1000 source query ~1.2s → ~2ms (600×).
- Risk: cycle bookkeeping (SLG continuation); override/memo stability; memo `tuple_set` sort invariant for `ts_prefix`; stack depth (worklist, not recursion).

**Stage B — generality** (~3–4 days): multi-predicate dependency closures + non-leading adornments + multi-variant predicates — mostly free from reusing the M8 v3/v5 adornment closure.

**Stage C — defer:** negation/aggregates (require full materialization of the negated/aggregated predicate).

**Correctness backstop (unchanged):** `top_down(Q) == filter(full_materialize(Q), Q)` byte-for-byte; property test over random graphs × random sources (200+ cases). Never silently mis-evaluate.

**IMPLEMENTER TIER:** `pro-coder` (the design is the hard part; implementation is multi-step and correctness-sensitive — cycle bookkeeping, override stability, memo sort invariants are exactly the silent-wrong-answer risks this project prioritizes).

---

## 7. Related / out of scope

- **IVM (incremental view maintenance)** is the *other* architectural direction that delivers per-query speedups, but on a **different workload** (snapshot churn, not selective queries). It does not help selective queries on a static db. If the project ever does both, IVM is the larger investment (~months) with the bigger payoff for write-heavy workloads. See architecture-design §3.2 DEFERRED / §7.
- **R1/R2 from the fixpoint-opt plan** are forward-chaining constant-factor optimizations (~40%/~13%), orthogonal to (and compatible with) top-down. R1 is recommended as the tactical magic-side spend now.
- Arithmetic/string/comparison builtins and trace compilation remain the bigger capability/throughput gaps; see architecture-design §3.2 / §10.

---

## 8. Coexistence & synergy with publish-once (added 2026-08-14)

Top-down and the publish-once model are **orthogonal by construction** — they live at different layers, serve different workloads, and **compose** (not just coexist). There is no conflict and no need to choose between them; the design is "both in one engine."

### 8.1 They coexist trivially — different layers, different regimes

| | publish-once (`dl_query_bound`) | top-down QSQ |
|---|---|---|
| optimizes | repeated reads of a large static snapshot | single selective queries on an unpublished db |
| key structure | pre-materialized mmap DAFSA, prefix scan | on-demand recursion + memoized subqueries |
| cost model | amortized (pay once to publish, then µs) | per-query (pay for what you touch) |
| wins when | read-heavy, closure ≈ answer | reachable slice ≪ closure |

Top-down uses the **same clone-and-scope eval pattern** (`eval_db_clone`) as forward magic, so it is a per-query path that never mutates the db and never touches the publish path. The engine ends up with four query entry points (`dl_query`, `dl_query_bound`, `dl_query_magic`, `dl_query_topdown`); the caller picks based on state and query shape.

### 8.2 The deeper synergy — each can help the other

1. **Top-down reads a published snapshot.** If the db is already published, top-down's EDB base-case subqueries can be served by the fast DAFSA / `dl_query_bound` path instead of the clone's in-memory relations. Publishing is not wasted on a top-down query — it accelerates the leaf lookups.
2. **Top-down can seed a publish.** Unioning the memoized answers of several top-down queries yields a partial materialization — useful if a reachable slice turns out to be queried often and is promoted to a published relation.
3. **A natural routing rule** (clean and honest):
   - db **published** + closure ≈ answer → `dl_query_bound` (fastest, always)
   - db **unpublished** + selective reachability → `dl_query_topdown`
   - db **unpublished** + need full answer → `dl_query` (materialize) or forward `dl_query_magic`
   - can be auto-routed on `fixpoint_dirty` + reachability heuristics.

### 8.3 Honest caveat

They serve **different regimes**. Top-down still cannot beat `dl_query_bound` on a *published* db (~100× intrinsic constant-factor gap). So the realistic design is: **publish-once remains the primary read path; top-down is the opt-in escape hatch for the regime publish-once is bad at** (ad-hoc selective queries when the db is not published, or publishing is too expensive). This is the "4th path, coexist, don't replace" decision — no replacement, no interference, only optional mutual acceleration.
