# Timestamps (`created_at` / `updated_at`)

**Status:** Design/plan — not yet implemented. Tier-1 gap (#3) for the jing-memory use case. The memory graph records `created_at`/`updated_at` per entity/observation/relation; datalog-dafsa today has no temporal field.

## TL;DR

Give datalog-dafsa a time capability so the graph can record when entities/observations/relations were created and last updated. The leanest design is **not** a timestamp column in every relation (costly, breaks the encoding thesis) but a **parallel time relation** plus reuse of the existing **publish-once snapshot versioning** for as-of semantics. Time resolution as u32 epoch seconds, with a permutation index for recency ordering.

## Motivation

`jing_meta/schema.py` gives every row a `created_at TEXT` (and entities an `updated_at TEXT`); `memory/server.py recent()` filters on `datetime(updated_at) >= cutoff` and orders by `updated_at DESC`. The **archiver** (`memory/archiver.py`) deletes observations older than a cutoff and writes them to a JSONL archive. So "when did this happen / what changed recently" is a core graph query.

datalog-dafsa facts are bare u32 tuples with **no time field**. There's no way to ask "what changed in the last 24h."

## Design decision: parallel time relation + snapshot-as-of, not per-relation columns

Recommendation: **a separate `at(thing_sym_id, epoch_u32)` relation** rather than adding a column to every relation.

- **Per-relation timestamp columns** would change the fixed-width u32-BE encoding thesis and inflate every key by 4 bytes for the common case where time is rarely queried. Rejected.
- **A parallel `at(entity_or_obs_sym_id, epoch_u32)` relation** keeps the core encoding intact; time is a *property* you can look up, prefix-enum, or perm-index on demand. `created_at` and `updated_at` are just two rows of `at`.
- **Existing snapshot versioning** (`dl_publish_snapshot`, `dl_query_version`) already gives as-of / time-travel reads "for free" — a snapshot IS a point in time. Use it where "state as of then" is the question; use `at()` where a wall-clock timestamp is the question. These are complementary, not competing.

### Resolution / encoding

- u32 epoch seconds (Unix time). Fine for "what changed in the last day/week"; the graph's microsecond ISO strings are higher resolution than needed for recency/archival — note the lossy truncation as an accepted trade-off.
- Seconds fit a u32 key column directly (no interning needed).

### `updated_at` semantic

- `updated_at` is a **separate `at(id, epoch)` row** written on every mutation (bump alongside the revision bump in the CAS plan). It is *not* derived from the revision counter — the revision is a logical ordering, the timestamp is wall-clock. They move together in the same txn (see CAS plan `handoff-t1-cas-ctx`).

### Ordering / query surface

- A **permutation index** on `at` ordered by `(epoch, thing)` turns "most recent N" into `dl_select_perm(at, perm, n-1, ...)` descending — see the `recent` plan (`handoff-t3-recent-ctx`) which consumes this.
- Range queries ("between t0 and t1") reuse the order-statistics `dl_range_count_perm` / `dl_rank_perm` machinery that already exists (`design/datalog-dafsa-range-index.md`).

## Files that change

| File | Change |
|---|---|
| `src/dl.h` / `src/dl.c` | `dl_add_timestamp(db, thing, at, kind)` / `dl_get_timestamp(db, thing, kind)` (kind = created/updated); wire into txn (CAS plan) so a revision bump also writes `at(id, updated)`. Intern `thing` via existing interner. |
| `src/permindex.h/.c` | verify an `at(thing, epoch)` perm index on `(epoch, thing)` works; add test. |
| `src/dl_cli.c` | `dl at <thing>` get; `dl recent` hooks into `recent` plan. |
| `docs/` | this doc. |
| `tests/` | `test_time.c`: set/get created/updated; ordering by epoch via perm; range via rank; updated-at-bump-on-mutation. |

## Honest ceiling

- u32 epoch seconds is lossy vs the graph's microsecond ISO strings (sub-second recency is invisible). Acceptable for the use case (hours-scale windows).
- Time is a property you must explicitly write (`at` rows), not a hidden field — the graph adapter must maintain it.
- No wall-clock in the DAFSA encoding itself; time-travel *within* a snapshot is via versioning, wall-clock via `at()`.

## Concrete next slice (if pursued)

1. `at(thing, epoch)` relation + `dl_add_timestamp`/`dl_get_timestamp`.
2. Perm index on `(epoch, thing)` for recency ordering.
3. Bump `at(id, updated)` inside the CAS txn commit.
4. Range query via order-statistics; wire `recent` and archiver cutoff.

## References

- `jing_meta/schema.py` (`created_at`, `updated_at`).
- `memory/server.py` `recent()` (~line 952, `datetime(updated_at) >= cutoff`).
- `memory/archiver.py` (oldest-by-created_at → archive).
- `design/datalog-dafsa-range-index.md` (rank/select/range + perm indices).
- `src/dl.h` (`dl_publish_snapshot`, `dl_query_version`, `dl_db_declare_perm`).
- CAS plan `docs/datalog-dafsa-cas.md` (updated-at bumps with revision).
