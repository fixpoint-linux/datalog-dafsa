# CAS / optimistic concurrency for the write path

**Status:** Design/plan — not yet implemented. Tier-1 blocker (#1) for using datalog-dafsa as the jing-memory graph store. Context: the memory MCP writes with `expectedRev`-based compare-and-swap; datalog-dafsa today has no transaction or CAS primitive.

## TL;DR

Add optimistic-concurrency (CAS) writes to datalog-dafsa so the graph write path — "check a revision, then bump it + add an observation + add a relation" — happens as one **atomic, conflict-checked unit**. Layered on the existing WAL + snapshot discipline, not a from-scratch transaction engine.

## Motivation (the contract datalog-dafsa must satisfy)

`jing-meta/memory/server.py` writes every entity mutation with an expected revision:

- `delete_entities(entityNames, expectedRevs=None)` — each entity carries the revision it was read at.
- `create_entities` / `add_observations` / `create_relations` — create-or-append, revision-bumping on change.
- On a stale write the server returns `CONFLICT` and the caller re-reads and retries.

datalog-dafsa today (`src/dl.h`) has **no** CAS primitive:
- `DL_E_LOCKED` (1) = database locked by another *writer* (single-writer file lock).
- `dl_add_fact` / `dl_delete_fact` are WAL-appended + fsync'd, durable single ops — but they are **unconditional** (no expected-revision check) and **per-fact** (not atomic across a multi-fact mutation).

So the write path that the graph needs — "bump revision + add observation + add relation, all-or-nothing, reject if stale" — is impossible today.

## Design decision: layer CAS on WAL+snapshot, do NOT build a general transaction engine

Recommendation: **a small `begin / commit / rollback`-style mutation API with per-entity revision counters**, not a full ACID transaction system.

- The DAFSA is read-optimized / batch-append; it is not an OLTP row store. A full general transaction layer (isolation levels, multi-statement rollback of arbitrary rule evaluation) is out of scope and wrong-headed for this store.
- The graph's write pattern is *narrow and well-defined*: read a small entity's state, mutate it, verify a revision. A CAS-on-entity primitive covers it.
- The existing single-writer `DL_E_LOCKED` model already serializes writers; CAS adds **logical** conflict detection on top of that physical serialization. Under single-writer, CAS is still meaningful: a *different client* (a separate process/MCP call) can hold a stale view and write stale — CAS rejects it.

## Concrete pieces

### 1. Per-entity revision counter

Store revisions in a dedicated relation `rev(entity_sym_id, revision_u32)`. A `revision` is a monotonically increasing u32.

- Interning an entity name → `sym_id`; `rev(sym_id, n)`.
- `n = 0` = absent/never-created (creation is CAS from 0).
- Reuse the existing interner (`src/intern.h`) so entity names and revisions share the symbol space.

### 2. CAS primitive

```
/* returns 0 ok, DL_E_CONFLICT (new) if current != expected, -1 error */
int dl_cas_revision(dl_db *db, const char *entity,
                    uint32_t expected, uint32_t new_value);
```

Guarded by the existing writer lock; `expected==new_value` is a no-op success (idempotent).

### 3. Atomic multi-op mutation (the key piece)

The graph needs "revision bump + add observation + add relation" as one unit. Provide:

```
int dl_txn_begin(dl_db *db);     /* acquires writer lock, opens WAL txn */
int dl_txn_cas(dl_db *db, const char *entity, uint32_t expected, uint32_t next);
int dl_txn_add_fact(...);         /* buffered within the txn */
int dl_txn_delete_fact(...);
int dl_txn_commit(dl_db *db);     /* writes all WAL entries + fsync, then applies in-memory */
int dl_txn_rollback(dl_db *db);   /* discards buffered ops */
```

Mechanics:
- `dl_txn_begin` acquires the single-writer lock and starts an in-memory op buffer.
- Writes are appended to the WAL **in order** and fsync'd once at commit (preserving the M7 durability-ordering invariant: interner-before-relation).
- `dl_txn_commit` applies all buffered ops to the in-memory DAFSAs; `dl_txn_rollback` discards the buffer (WAL is never written for a rolled-back txn).
- A crashed txn never leaves a partial WAL commit because the buffer is written+fsync'd atomically at commit.

### 4. Conflict semantics

- Any `dl_txn_cas` whose `expected != current` fails the whole txn → `dl_txn_commit` returns `DL_E_CONFLICT`, nothing is applied, caller retries with a fresh read.
- The graph's CONFLICT→re-read→retry loop maps 1:1 onto this.

## Files that change

| File | Change |
|---|---|
| `src/dl.h` / `src/dl.c` | `DL_E_CONFLICT`; `rev()` relation wiring; `dl_cas_revision`; `dl_txn_begin/cas/add/delete/commit/rollback`; revision bump on `dl_add_fact`/`dl_delete_fact` when a `rev` entry exists |
| `src/relation.h/.c` | none required (rev is a normal relation) — verify no per-op assumptions |
| `src/vm.c` | ensure txn buffered ops don't interleave with IVM rule application; document interaction |
| `src/dl_cli.c` | `dl cas`, `dl txn` test/demo subcommands |
| `docs/` | this doc; link from README |
| `tests/` | new `test_cas.c`: CAS success / conflict / stale-retry / multi-op atomic commit / rollback / crash-in-txn |

## Honest ceiling

- This does **not** make datalog-dafsa a general OLTP store — it solves the graph's narrow write pattern only.
- Revisions and txn atomicity are **logical**; physical isolation remains single-writer-locked.
- Multi-writer concurrency (beyond lock+retry) is explicitly out of scope; the graph is not multi-writer.

## Concrete next slice (if pursued)

1. `rev()` relation + `dl_cas_revision` single-op (no txn yet) — proves CAS works.
2. `dl_txn_begin/commit/rollback` with buffered WAL — proves multi-op atomicity + crash-safety.
3. `DL_E_CONFLICT` retry wiring + `test_cas.c`.
4. Wire the graph write path (jing-memory adapter) onto `dl_txn_*`.

## References

- `src/dl.h` (`DL_E_LOCKED`, `dl_add_fact`/`dl_delete_fact` durability, publish-once snapshots).
- `src/dl.c` M7 durability-ordering invariant (interner-before-relation, WAL fsync).
- `jing-meta/memory/server.py` (`expectedRevs` on `delete_entities`, CONFLICT→retry).
- Design: `design/datalog-dafsa-architecture.md` §durability / publish-once.
