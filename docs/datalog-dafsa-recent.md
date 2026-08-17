# `recent` — recency ordering

**Status:** Design/plan — not yet implemented. Tier-3 gap (#8) — depends on the timestamps plan (`docs/datalog-dafsa-timestamps.md`). The memory MCP `recent` returns entities/relations/observations created-or-updated in the last N hours, newest first; datalog-dafsa needs a recency query.

## TL;DR

Add a `recent` query to datalog-dafsa that returns the N most-recently-updated entities (and their window-scoped observations), matching `memory/server.py recent()`. **Depends on timestamps** (`at(thing, epoch)` from `docs/datalog-dafsa-timestamps.md`); recency ordering leans on the existing **permutation-index + order-statistics** machinery (`dl_select_perm` / `dl_rank_perm` / `dl_range_count_perm`) with a time column.

## Motivation

- `memory/server.py recent()` (~line 952): entities updated in the last `hours` window, `ORDER BY updated_at DESC LIMIT limit`; observations fetched window-scoped (only obs created within the window), relations by `created_at`.
- datalog-dafsa needs a way to ask "what changed recently" — which requires a time field (timestamps plan) and an ordering mechanism (this plan).

## Design: perm-indexed time ordering + window filter

### Contract it consumes (from the timestamps plan)

- `at(thing_sym_id, epoch_u32)` relation: `created_at` and `updated_at` rows per thing.
- A **permutation index** on `at` ordered by `(epoch, thing)` — this makes "most recent first" a descending `dl_select_perm`.

### Query API

```
/* N things updated within the last `hours`, newest first. cb receives (thing_sym, epoch). */
long dl_recent(dl_db *db, int hours, int limit, dl_recent_cb cb, void *user);
```

- Compute `cutoff = now - hours` (u32 epoch).
- Window filter: things with `updated_at >= cutoff` — via `dl_range_count_perm`/`dl_rank_perm` on the `(epoch, thing)` perm, or a bounded scan of the descending perm.
- Order: `dl_select_perm(at, perm, k, ...)` walking `k = 0, 1, ...` gives the most-recent-first ordering directly.

### Window-scoped observations (matching the MCP)

- For each recently-updated entity, fetch **only** observations whose `created_at` falls within the window — a `dl_range` over `at(obs, created)` bounded by cutoff — not the entity's full history.
- This preserves the MCP's "proportional to recent activity, not entity size" behavior.

### Relations

- Relations by `created_at` within the window, newest first (`at(rel, created)` perm), capped.

### Both paths

- Live relations + interner; snapshot path reads the published `at` relation + published vocabulary.

## Files that change

| File | Change |
|---|---|
| `src/dl.h` / `src/dl.c` | `dl_recent`, `dl_recent_since(ts, ...)`; window-scoped obs fetch via `at` range |
| `src/permindex` | `at(thing, epoch)` perm on `(epoch, thing)` (shared with timestamps plan) |
| `src/dl_cli.c` | `dl recent [--hours N] [--limit N]` |
| `docs/` | this doc |
| `tests/` | `test_recent.c`: window filter, newest-first order, limit, window-scoped obs (excludes old obs), relations-by-created, snapshot parity |

## Honest ceiling

- **Fully dependent on the timestamps plan** — nothing here works until `at(thing, epoch)` exists.
- Ordering is by u32 epoch seconds (lossy vs microsecond ISO, per timestamps plan).
- No pagination beyond `limit`; the MCP bounds results (20 default) to fit context.
- `dl_recent` returns thing sym-ids + epochs; the MCP resolves names/obs via `intern_str_of` + `observation` relation.

## Concrete next slice (if pursued)

1. (Prereq) Timestamps plan → `at(thing, epoch)` + `(epoch, thing)` perm.
2. `dl_recent` newest-first via `dl_select_perm`.
3. Window filter via `dl_range_count_perm`/`dl_rank_perm`.
4. Window-scoped observation fetch.
5. Wire memory `recent` onto `dl_recent`.

## References

- `memory/server.py` `recent()` (~line 952).
- `docs/datalog-dafsa-timestamps.md` (the `at(thing, epoch)` dependency).
- `src/dl.h` (`dl_select_perm`, `dl_rank_perm`, `dl_range_count_perm`).
- `design/datalog-dafsa-range-index.md` (rank/select/range + perm mechanics).
