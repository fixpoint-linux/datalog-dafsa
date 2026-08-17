# Semantic embeddings sidecar

**Status:** Design/plan — not yet implemented. Tier-2 gap (#6) for the jing-memory use case. `search_semantic` / `rebuild_semantic_index` use bge-small embeddings + cosine; datalog-dafsa is a C fact store with no vector capability. This plan defines the **boundary** — embeddings stay a sidecar, and defines how vectors rejoin the graph. **This is the *semantic half* of hybrid retrieval — see `docs/datalog-dafsa-hybrid-search.md`. For the scale path (1e5–1e6+), see the design of record `docs/datalog-dafsa-vector-search.md` (MIH over ITQ bit-codes as per-band DAFSA postings). This doc's `.npy` sidecar is the build-path step 1 of that design.**

## TL;DR

Semantic search is **not a datalog-dafsa concern** and should not be built into the C engine. The honest design is: **keep embeddings as an out-of-band sidecar** (an `.npy`/flat vector file keyed by entity name/sym-id), computed by an external embed step, and rejoin the graph through the shared entity identity. datalog-dafsa contributes only the *lookup identity* (the entity sym-id); the vector computation + cosine live outside it.

## Motivation

- `memory/server.py search_semantic` / `rebuild_semantic_index` embed entities with **bge-small** (fastembed/onnxruntime) and cosine-compare against cached vectors (`memory/semantic_index.py`: `entity_vectors.npy` + `entity_names.json`).
- datalog-dafsa facts are fixed-width u32 tuples. Floating-point embeddings **do not fit** the u32 encoding (no float column), and building an ANN index inside the C engine would be scope creep — the 2026-08-08 and 2026-08-11 analyses both concluded the real gap is *semantic retrieval*, which is not a DAFSA strength.

## Design: define the boundary, don't grow the engine

### 1. Where vectors live

Keep a **flat vector sidecar** — do NOT store vectors in a datalog-dafsa relation:

- `entity_vectors.npy` (float32, N×D) + `entity_names.json` (N names) — the current format.
- Keyed by position `i`; the name at `entity_names[i]` is interned to `sym_id` via datalog-dafsa's interner. This position↔sym-id bridge is the only touchpoint.

### 2. Who computes embeddings

**External Python embed step** (`rebuild_semantic_index` equivalent), entirely outside the C engine:
- Walks the graph relations → collects entity names (via `intern_str_of`).
- Embeds them (bge-small), writes the sidecar.
- Never touches the DAFSA engine internals.

### 3. How vectors rejoin the graph

Query flow:
1. Embed the query (Python).
2. Cosine vs the sidecar → ranked `entity_names[i]`.
3. Map each name → `sym_id` (interner lookup), then look up the entity in the graph relations (`entity(name_sym, type_sym)`), fetch observations.

The C engine's only role: **resolve `sym_id ↔ name`** so semantic hits land on graph nodes. That's it.

### 4. Rebuild / maintenance lifecycle

- Rebuild after publish or after gardening (matching the current `rebuild_semantic_index` trigger).
- The sidecar is rebuilt wholesale; no incremental vector maintenance needed at this scale (hundreds–thousands of entities).

### 5. Does the C engine expose anything new?

Minimal: a thin `dl_sym_of_name` / `dl_name_of_sym` (already exists as `intern_str_of`) plus a way to enumerate entity names for the embed step (`dl_entities` iterate over the `entity` relation). **No vector code in C.**

## Files that change

| File | Change |
|---|---|
| `src/dl.h` / `src/dl.c` | (tiny) `dl_entity_names` iterator so the embed step can collect names to embed — nothing vector-related |
| `scripts/embed.py` (new) | standalone embed → sidecar (port `memory/semantic_index.py` build half) |
| `docs/` | this doc |
| `tests/` | `test_semantic_bridge.c`: name→sym-id→entity round-trip; sidecar format |

## Honest ceiling

- **No semantic search in datalog-dafsa.** The value is a *clean boundary*, not a new capability.
- A single sidecar (no vector DB); fine for the memory graph's size. If semantic retrieval needs scale, a real vector index is a separate project — not here.
- The C engine's contribution is identity resolution only; the embedding model + cosine are external.

## Concrete next slice (if pursued)

1. `dl_entity_names` iterator (enumerate entity names to embed).
2. `scripts/embed.py` — walk names → embed → write sidecar.
3. Query-side cosine → name → sym-id → graph-node join.
4. Wire memory `search_semantic` to this flow.

## References

- `memory/semantic_index.py` (bge-small, `.npy` sidecar).
- `src/intern.h` (`intern_str_of`).
- Analysis: 2026-08-08 "don't replace SQLite; real gap is semantic retrieval"; 2026-08-11 "DAFSA-as-database".
