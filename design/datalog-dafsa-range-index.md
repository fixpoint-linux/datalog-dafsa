# Range path index & join/range workloads

_Design note. Status: **Tier 2 (order-statistics) implemented** — commit `20e93bb` (2026-08-16),
**prefix-bound rank/select implemented** — commit `1b46922` (2026-08-16),
**permuted order-statistics (`dl_*_perm`) implemented** — commit `3a9d448` (2026-08-16),
**`range(X,Rel,Lo,Hi)` / `OP_RANGE` implemented** — commit `2724e31` (2026-08-16),
**snapshot (`dafsa_view`) rank/select implemented** — commit `0df6004` (2026-08-16).
The full feature remains partly proposed; this doc records the options, the recommended path, and
the deferred follow-ups._

## TL;DR

The DAFSA is already a sorted-enumeration structure for **fixed-width big-endian
keys**: every prefix-bound enumeration walks the minimal DAG in byte/lex order.
So the missing piece for join-heavy / range-query workloads is **not** "turn the
DAFSA into a B-tree" — it's a **compact seek/order-statistics layer on top of the
DAFSA's existing lex order**. This doc lays out the options and a recommended path.

## The key realization

Because keys are fixed-width u32-BE (columns at byte offsets `4*i`, no
separators), the DAFSA's prefix-walk (DFS over the minimized DAG) enumerates
tuples in **byte/lexicographic order**. Therefore:

> Any prefix-bound enumeration is already a **sorted stream**.
> "Everything with `cols[0..k-1]` bound to P, in order" is free.

This is what makes range scans and merge joins *almost* free — the blocker is
the **callback-based API** (`dl_prefix(..., cb)` is fire-and-forget), not the
automaton.

## Where things stand today

- **Join-heavy is mostly solved** by permutation indices (M6, `src/permindex.h`):
  a perm-π index re-encodes relation facts in column order `π(0)..π(a-1)`, so any
  join on `{π(0)..π(k-1)}` becomes a leading-k prefix lookup (`dl_prefix`).
  The remaining gap is **automatic perm-index selection** (a planner/cost pass;
  arity ≤ 8 caps candidate combinatorics at 2^8).
- **Range on leading columns**: the automaton already enumerates in order, but
  the API can't express "seek to a bound, then give me the next tuple" — so
  bounded range scans and merge joins are awkward.

## The proposed compact range path index

The DAFSA already holds the keys and enumerates them sorted. A range index does
**not** need to duplicate the keys — it needs only to get *near* an arbitrary
value, then let the DAFSA's own DFS walk forward. Three tiers, all "still compact":

### Tier 1 — Sparse checkpoint index (seek-to-range + bounded scan)

Sample every Nth key in lex order (or every Nth reachable state) into
`(key_suffix, state_id)` pairs — a small sorted array of checkpoints.

- To scan `[lo, hi)`: binary-search checkpoints for the largest ≤ `lo`, walk the
  DAFSA forward from that state to the `lo` bound (≤ N keys between checkpoints),
  then DFS-enumerate through `hi` with early exit.
- **Cost:** ~1/N the DAFSA size. No B-tree, no duplicated keys, no payloads —
  just a sorted `u32` array of checkpoint state-ids + sampled key bytes.

### Tier 2 — Per-state subtree path-counts: rank/select (recommended sweet spot)

Because the DAFSA is an automaton, each state has a **count of final paths
reachable from it** (how many complete keys pass through that state). Store that
count per state (one `u32` per state — already tiny):

- **`rank(k)`** = walk from the root down transitions, using subtree counts to
  binary-search which branch holds the k-th key → O(depth) ≈ O(arity) ≤ 8.
- **`range_count(lo, hi)`** = `rank(hi) − rank(lo)`, in O(depth).
- **k-th element / random access** = rank-guided descent (order statistics).
- **Compacts with permutation indices** for order-by on any column prefix
  (build counts on the permuted relations too).

**The only cost:** maintain subtree counts incrementally on `add`/`delete`, at
the mutation points your engine already touches (`clone_state` / realloc), under
the existing **collect-then-mutate** discipline. Fixed arity (≤ 8) makes the
descent trivially cheap.

### ~~Tier 3 — Succinct sorted array (only if true B-tree point-seek is needed)~~ *(out-of-scope / overkill — see deferred item 7)*

Elias-Fano or another succinct sorted-array encoding of the keys: ~`log(U/n)`
bits/key. Gives genuine point-seek / rank-select at B-tree-like cost when a
sparse checkpoint or subtree-count approach isn't enough.

## The honest ceiling

The DAFSA (minimized DAG) cannot give true B-tree semantics: no O(log n) random
access to an arbitrary mid-key, no point-seek to a value that is not a prefix
boundary, no cheap "k-th element" without an order-statistics layer. For genuine
OLTP point-updates/concurrency or arbitrary value-range B-tree seeks, add a
**complementary** sorted structure (a real B-tree or sorted-run file) beside the
DAFSA rather than bending the DAFSA into a B-tree.

## Recommended order of work

1. **Pull-based sorted iterator + merge-join operator** — the biggest lever,
   pure additive, fits the C / `-Werror` idiom. Replaces the callback fire-
   and-forget `dl_prefix` with a resumable cursor: `iter_seek` (walk to the
   lower-bound state, O(prefix-length)) + `iter_next` (resume DFS). Unlocks
   bounded range scans with early termination, O(n+m) merge joins, and order-by
   (via perms + cursor). Prefix seeks on the DAG are exact (land on the bound),
   not B-tree-comparison.
2. **Automatic perm-index selection** — planner/cost pass; perms already exist.
3. **Range-scan API** over the iterator — `seek(lower, upper)` + early exit;
   nearly free once the cursor exists.
4. **Order-statistics** via Tier-2 subtree path-counts (`rank`/`select`/
   `range_count`) + an `OP_RANGE` VM opcode or range-aware aggregate.
5. Only if B-tree access patterns are truly required, add a **sorted-run / B-tree
   companion** side index — don't rewrite the DAFSA core.

## Concrete next slice (if pursued)

- API: `dl_iter_open`, `iter_seek(db, rel, prefix, k)`, `iter_next`,
  `iter_close`; plus (Tier 2) `dl_rank`, `dl_select`, `dl_range_count`.
- VM: an `OP_RANGE` opcode or a range-aware aggregate consuming the iterator.
- Update-on-mutation for subtree path-counts at the `add`/`delete` points.
- Test: merge-join over two sorted iterators; range-count over a permuted
  relation; order-by on a non-leading column prefix.

## Implementation status & deferred follow-ups

### Implemented (commit `20e93bb`, 2026-08-16) — Tier 2 order-statistics

Per-state **distinct-subtree-key counts** on the DAFSA, giving `rank` / `select` /
`range_count` / `count`:

- `vendor/dafsa_rank.c`: `dafsa_ensure_subtree` (lazy one-time bottom-up DFS,
  memoized) + `dafsa_rank_n` / `dafsa_select_n` / `dafsa_range_count_n`.
  Count recurrence = **DISTINCT complete keys reachable** from a state:
  `count(s) = is_final(s) + Σ count(target)` over outgoing transitions —
  correct on a minimized DAG (disjoint symbols per state ⇒ disjoint key sets;
  a state shared by N parents is counted N times at the root, which is correct).
  Counts are **`uint64_t`** (not the u32 suggested below) — a cross-product
  relation exceeds 2³² keys with few states; u32 would silently mis-evaluate.
- `struct dafsa` (vendor/dafsa_internal.h) gains `subtree` / `subtree_cap` /
  `subtree_valid`; coarse invalidation at the top of `dafsa_add_n` and
  `dafsa_delete_n`; freed in `dafsa_free`. OOM degrades to 0/-1, never aborts.
- Public API (src/dl.h): `dl_rank`, `dl_select`, `dl_range_count`, `dl_count`.
  Reject NULL / unknown rel / arity-mismatch / variadic (UINT64_MAX/-1).
- Tests: `tests/test_m10_rank.c` (6/6) — lex-order rank, select roundtrip,
  range_count, absent-key insertion position, interleaved add/delete
  (dirty/rebuild), empty relation, cross-product stress (distinct-key not
  path-count), rejection matrix. Reviewer: SAFE TO SHIP, `ESCALATE=none`,
  39,161 independent adversarial checks, 0 failures.

> **Note (do not conflate):** `dl_count` returns the *distinct* tuple count via
> the subtree array. The pre-existing `rel_count` returns `n_final` (the count of
> *final states*, which under-counts distinct tuples under suffix sharing) and is
> used only as a join-order hint. They are intentionally different.

### Deferred follow-ups (not yet implemented)

In rough priority order. ~~struck-through~~ items are implemented (commit noted).

1. ~~**Prefix-bound rank/select** (`dl_rank_bound` / `dl_select_bound` /
   `dl_range_count_bound`)~~ — **implemented** (commit `1b46922`). Walk a
   leading-column prefix to a state, then rank/select within that subtree.
   The DAFSA `*_n` primitives were generalized to start-state (`_from`) forms;
   `rel_prefix_state` + suffix-key encoding expose the bound.
2. ~~**`dl_rank_perm` / order-by on a non-leading column**~~ — **implemented**
   (`dl_rank_perm` / `dl_select_perm` / `dl_range_count_perm`). Perm relations
   are just relations with their own `dafsa`, so they get subtree counts for
   free; a thin wrapper over the perm relation's dafsa gives order-by on any
   column prefix. `dl_db_declare_perm` now validates the perm is a bijection.
3. ~~**`OP_RANGE` VM opcode / range-aware aggregate**~~ — **implemented** as the
   `range(X, Rel, Lo, Hi)` reserved builtin (a `member(X,L)` analog for
   relations) lowered to an `OP_RANGE` opcode: generator (bind X to distinct
   leading-column values in `[Lo,Hi)`) or filter.  **The generator is now a
   LAZY resumable generator driven by the #5 pull-iterator** (commit `2f78be3`,
   2026-08-17): an owned `dl_iter*` in `vm_frame`, opened first entry via the
   LIVE-only `dl_iter_open_live` (never the snapshot-aware open — `vm_execute`
   runs with `snap_version>0` on re-publish and must read the live `rel->d`),
   advanced per backtrack re-entry with skip-`<lo`/stop-`≥hi`/dedup-col0, and
   closed on pop/cleanup.  This replaced the earlier eager `rel_range_each`
   materialization and gives early termination under a short-circuiting
   consumer.  Two gates: `range` excluded from IVM/DRed/agg/magic/topdown, and
   `range` over a recursive-SCC relation rejected (stale-read).
4. ~~**Snapshot (mmap `dafsa_view`) rank/select**~~ — **implemented**.
   `dl_rank`/`dl_select`/`dl_range_count`/`dl_count` route to the published
   mmap view when `db->snap_version > 0` (exclusive with the in-memory path,
   mirroring `dl_query`/`dl_query_bound`/`dl_pattern`).  A view-level
   subtree-count walk (`vendor/dafsa_view_rank.c`) covers the CSR + `final_bits`
   layout.  `dl_*_bound`/`dl_*_perm` remain in-memory-only (deferred).
5. ~~**Pull-based sorted iterator + merge-join**~~ — **implemented** (commit
   `3a08ef2`). `dl_iter_open` / `dl_iter_seek` / `dl_iter_next` /
   `dl_iter_arity` / `dl_iter_close` (src/iter.c) replace the callback
   fire-and-forget `rel_prefix` with a **resumable explicit-stack DFS cursor**
   yielding one lex-sorted tuple per `dl_iter_next` (end `0` vs error `-1`
   never conflated; absent prefix ⇒ valid empty iterator; variadic rejected).
   `dl_merge_join` is an O(n+m+out) equi-join on the first J columns
   (one-token lookahead + right-run buffering, duplicate-preserving, sorted,
   drains both iterators).  Snapshot path owns an mmap `dafsa_view`; live path
   borrows `rel->d`.  **Wired into the VM** (commit `2f78be3`) as the lazy
   resumable driver behind `OP_RANGE`; a dedicated VM merge-join/order-by
   opcode is not yet added (future).
6. ~~**Automatic perm-index selection**~~ — **implemented** (commit `b553f2c`).
   A compile-time cost gate in `emit_nonleading_join` (src/compiler.c) wires up
   the previously-dead `OP_HASH_JOIN` as a **slot-free fallback**: a perm is
   declared only when the relation is large enough (`rel_count_subtree` ≥ 4,
   tunable `g_perm_card_threshold`) or already declared (reuse). Recursive
   body atoms always use `OP_LOOKUP_PERM` (the semi-naive override can't
   hash-join a recursive atom). Perm-cap exhaustion degrades to hash-join
   instead of a hard compile error. `OP_HASH_JOIN`'s `imm` carries a packed
   3-bit-per-column permutation, unpacked into a frame-local
   `vm_frame.perm_storage`; pack/unpack symmetry verified over all 46,233
   perms of arity 1–8. Semantics unchanged — an oracle test asserts
   `g_perm_select=1` vs `0` produce byte-identical query results.
7. ~~**Tier 1 sparse checkpoint index** and **Tier 3 succinct sorted array
   (Elias-Fano)**~~ — **struck as out-of-scope / overkill** (2026-08-16).
   Tier 1 (seek-to-range + bounded scan) is **subsumed by the pull-based
   iterator** (#5: `dl_iter_seek` walks to the lower-bound state via
   O(prefix-length) `trans_find` and resumes DFS, so the explicit checkpoint
   array is redundant). Tier 3 (Elias-Fano) is **overkill**: it would store the
   keys a second time, undermining the DAFSA's shared-suffix compression; Tier 2
   rank/select is already effectively O(1) for arity ≤ 8; and if genuine OLTP
   point-seek is ever truly needed, a real **B-tree companion** (a complementary
   sorted structure) beats Elias-Fano anyway. Deferred as an "only if we ever
   need a B-tree companion" note, not a range-index slice.
8. **Fine-grained invalidation** — currently coarse (rebuilds on any add/delete,
   including duplicate-add / absent-delete / bad-arg). A precision improvement
   would track only real structural changes; deferred because a DAFSA merge
   restructures the DAG wholesale (incremental maintenance is infeasible in the
   general case).  **Parked (future):** low value for the read-heavy +
   batch-write workload — subtree counts build lazily and once, so a coarse
   rebuild between mutation batches is amortized and nearly invisible. Revisit
   only if OLTP-style point-updates interleaved with rank/select ever appear.

## Parked / future follow-ups (only if a use case appears)

These are **not** planned work — recorded so a future session can pick them up
without re-deriving why they were set aside.

- **Snapshot `dl_*_bound` / `dl_*_perm`** (order-statistics over a published
  snapshot for a prefixed or permuted relation). Plain snapshot rank/select is
  covered (#4); the bound/perm variants need (a) a stable snapshot
  `perm_id`→name mapping across reopen and (b) a view `_from` variant. Niche —
  build only if a read path needs rank/select on a permuted/prefixed relation
  from a snapshot.
- **VM merge-join / order-by opcode** over the #5 iterator (currently the
  iterator only drives `OP_RANGE`). Cheap to add once a concrete rule shape
  justifies it.
- **Tier 3 / B-tree companion** (see struck #7) — a real complementary sorted
  structure beside the DAFSA if genuine OLTP point-seek is ever required.

## References

- `design/datalog-dafsa-architecture.md` — the DAFSA-as-fact-store linchpin (§0).
- `src/permindex.h` / M6 — permutation indices for join acceleration.
- `vendor/dafsa.h`, `vendor/dafsa_persist.c` — the automaton (tracks
  `n_final`, `n_states`, `n_trans`; PDWG v4 canonical save).
- `src/relation.c` — `rel_open_writable` / `rel_add`/`rel_lookup`/`rel_delete`
  DAFSA mappings; `struct relation` holds `dafsa *d` / `*base` / `*wal`.
