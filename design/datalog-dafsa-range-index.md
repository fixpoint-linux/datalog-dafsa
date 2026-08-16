# Range path index & join/range workloads

_Design note. Status: proposal / assessment, not yet implemented. Author-date: 2026-08-16._

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

### Tier 3 — Succinct sorted array (only if true B-tree point-seek is needed)

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

## References

- `design/datalog-dafsa-architecture.md` — the DAFSA-as-fact-store linchpin (§0).
- `src/permindex.h` / M6 — permutation indices for join acceleration.
- `vendor/dafsa.h`, `vendor/dafsa_persist.c` — the automaton (tracks
  `n_final`, `n_states`, `n_trans`; PDWG v4 canonical save).
- `src/relation.c` — `rel_open_writable` / `rel_add`/`rel_lookup`/`rel_delete`
  DAFSA mappings; `struct relation` holds `dafsa *d` / `*base` / `*wal`.
