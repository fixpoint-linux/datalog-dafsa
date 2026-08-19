# Graph traversal / neighbor API

**Status:** Implemented (2026-08-19). Tier-2 gap (#4) for the jing-memory use case. Snapshot parity and MCP wiring are documented follow-ups, not yet implemented. The memory MCP `traverse` does N-hop BFS returning nodes + observations; datalog-dafsa needs a first-class traversal/neighbor operation.

## TL;DR

Add a graph traversal primitive to datalog-dafsa that returns, from a start node, the entities within N hops *and their observations* — matching the memory MCP `traverse(start_node, depth, max_obs_per_entity)` tool. The graph is modelled as relations (`edge(from,to,type)`, `observation(entity, content)`); traversal is a **wrapper over prefix-enum + a neighbor fetch**, not a new engine feature. Datalog reachability rules already give in-engine transitive closure; this plan adds the *node+observation-returning* API the MCP needs.

## Motivation

- `memory/server.py traverse()` (~line 868): BFS from `start_node` up to `depth` (max 3), returning entities, their observations (capped), and relations.
- In-engine reachability (`reach(X,Y) :- edge(X,Y), ...`) gives you *which* nodes are connected, but the MCP needs **neighbors + their observations**, not just derived tuples.
- datalog-dafsa already has the primitives: prefix-enum (`dl_prefix`), iterators (`dl_iter_open`/`next`), merge-join, and permutation indices for reverse-edge traversal.

## Design: graph-as-relations + a traversal wrapper

### Graph model

Represent the graph as relations over the interner:

```
entity(name_sym, type_sym)                 /* arity 2 */
edge(from_sym, to_sym, type_sym)           /* arity 3 */
observation(entity_sym, content_sym)       /* arity 2 */
```

- Entity/edge/observation **names** are interned sym-ids (matching the string-content regex plan).
- `observation(entity, content)` — content is a sym-id of the interned text; prefix-enum on `observation(entity, ...)` returns that entity's observations.

> **Note (2026-08-19, implementation):** `dl_traverse` only requires the `edge(from,to,type)` relation; the `entity(name,type)` relation is **not** consulted by the current implementation (traversal is self-contained on `edge`). `dl_node_observations` requires `observation(entity,content)`. Entity *type* resolution and snapshot (mmap-view) traversal are documented follow-ups, not yet wired.

### Forward + reverse edges

- Forward neighbors of X = prefix-enum `edge(X, *, *)`.
- Reverse neighbors (X reached from others) = prefix-enum on a **permutation index** of `edge` ordered by `(to, from, type)`.
- This is the "bidirectional adjacency from prefix-enum" synergy from the analysis: both directions are native DAFSA operations.

### Traversal API

```
/* Visit nodes within `depth` hops of start. cb receives (node_sym, type_sym,
 * depth). Returns node count. */
long dl_traverse(dl_db *db, const char *start,
                 int depth, int max_nodes,
                 dl_traverse_cb cb, void *user);

/* Fetch a node's observations (capped). */
long dl_node_observations(dl_db *db, const char *node,
                          int max_obs, dl_str_cb cb, void *user);
```

- BFS with a **visited set** (reuse the visited-set pattern from `regexwalk.c`).
- `depth` clamped (max 3, mirroring the MCP), `max_nodes` bounds the frontier.
- `dl_node_observations` prefix-enums `observation(node, ...)`, resolving each `content_sym` via `intern_str_of`.

### Both paths

- In-memory: operate on `db->ir` interner + live relations.
- Snapshot: use the mmap view + the published vocabulary (`<sdir>/symbols.dafsa`) — the same live-vs-snapshot split as the regex plan (`docs/datalog-dafsa-regex-on-symbols.md`).

## Files that change

| File | Change |
|---|---|
| `src/dl.h` / `src/dl.c` | `dl_traverse`, `dl_node_observations` (BFS over `edge` prefix-enum + reverse perm; obs via `observation` prefix-enum + `intern_str_of`); snapshot paths |
| `src/permindex` | ensure `edge` reverse `(to,from,type)` perm exists on demand (auto-perm selection may already cover it — verify) |
| `src/dl_cli.c` | `dl traverse <start> [depth]`, `dl obs <node>` |
| `docs/` | this doc |
| `tests/` | `test_traverse.c`: forward+reverse BFS, depth cap, visited-set (no cycles), obs fetch, snapshot parity |

## Honest ceiling

- Traversal is a **wrapper**, not a rewrite — it reuses existing primitives. The "reachability as Datalog rule" and "traverse as API" coexist: rules for *derived* connectivity, the API for *node+obs* retrieval.
- `depth <= 3` and a visited set keep it bounded; no unbounded path enumeration (that's what the Datalog reachability rule is for, with IVM).
- No ranking of traversal results — the MCP returns them in BFS order.

## Concrete next slice (if pursued)

1. `edge(from,to,type)` + `observation(entity,content)` relations as the graph schema.
2. `dl_traverse` forward-only BFS (visited set, depth cap).
3. Reverse-edge perm for bidirectional traversal.
4. `dl_node_observations` (prefix-enum + `intern_str_of`).
5. Wire the memory MCP `traverse` tool onto `dl_traverse`.

## References

- `memory/server.py` `traverse()` (~line 868).
- `jing_meta/schema.py` (relations/observations).
- `src/dl.h` (`dl_prefix`, `dl_iter_*`, `dl_db_declare_perm`).
- `src/regexwalk.c` (visited-set pattern to reuse).
- `docs/datalog-dafsa-regex-on-symbols.md` (live vs snapshot symbol handling).
