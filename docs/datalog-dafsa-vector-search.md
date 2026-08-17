# Semantic vector search via multi-index hashing (MIH) over ITQ bit-codes

**Status:** Design of record — **this is the chosen approach.** Recommended by the advisor (2026-08-17), consistent with the bit-signature index critique. Semantic vector search in datalog-dafsa, designed for **scale** (~1e5–1e6+ entities). Replaces the bit-signature doc's flawed best-first-walk/PCA-sign framing. This is the design we will implement. **The `.npy` sidecar this doc retains for re-rank can be eliminated afterward — see `docs/datalog-dafsa-vector-search-no-sidecar.md` (int8 re-rank vectors in-store).**

## TL;DR

**Multi-index hashing (MIH, Norouzi et al., TPAMI 2012) over ITQ-binarized embeddings, stored as per-band DAFSA postings relations inside the existing datalog-dafsa store, with exact-cosine re-rank over a retained float sidecar.**

Concretely:
- Each embedding is binarized to a `d`-bit **ITQ signature** (e.g. 256 bits) in a Python sidecar.
- The signature is split into `m` substring **bands**; for band `j` the store gains a fixed-arity relation `sig_j(substr_u32, entity_sym_id)` — a postings index from bit-substring to sym-id, **the same shape as the full-text postings** the engine already supports.
- **Query** = for each band, enumerate the query's substring variants within the pigeonhole budget `⌊r/m⌋`, look each up via `dl_prefix` over `sig_j`, union candidates across bands, filter to live entities, then **exact-cosine re-rank** against the `.npy` sidecar.

This is simultaneously **scaled** (MIH is ~100× fewer probes than a trie walk — verified ~2.2K probes vs ~210K nodes at d=256/m=16/r=37) and **elegant** (maximal reuse of the DAFSA's existing machinery — no new traversal primitive, no foreign library, no new on-disk format).

## Why this design (vs the alternatives)

The advisor weighed four options against the goals (scale + "consolidate onto my own stack" + elegance):

| Option | Verdict |
|---|---|
| **MIH over ITQ codes, per-band DAFSA postings** | **CHOSEN** — reuses the engine's native idiom (postings + `dl_prefix`), ~100× fewer probes than a trie walk, wins the consolidation story, no foreign dependency. |
| HNSW-lite (separate graph) | Best raw latency/recall at 1e7+, most-studied tuning — but a **foreign structure**: new persistence, new WAL/crash story, new mmap discipline, **zero reuse**, breaks "own stack." Keep as the graceful fallback beyond the ceiling. |
| FAISS | Dominates on performance — but a **full foreign dependency**, rejected by the consolidation goal outright. |
| DAFSA-walk hybrid (best-first Hamming trie walk) | **Empirically killed** by the critique: at d=256 a 0.9-cosine neighbor sits at Hamming ~37, so the walk visits ~210K nodes vs the scan's 256K compares — *slower than a scan*; and "≤ r mismatches" is not expressible in the supported regex subset (blows the 8192-state DFA cap). |
| LSH forest (Flavor B) | Orthogonal recall booster — **not the default**; MIH gives exact-in-Hamming with one table per band, so the forest's K-table multiplication is optional. |

**Why MIH wins the elegance contest:** the "hash table" MIH needs *is* a postings index — the engine's native idiom. Every other candidate either duplicates existing machinery (HNSW-lite reimplements persistence) or fights the engine's grain (DAFSA-walk needs a new priority-queue Dijkstra primitive the engine does not have).

## The integration seam (sym-id bridge + snapshot + WAL)

### 1. Sym-id stability is the load-bearing property

`intern.h:7-9,27-28`: the interner is **append-only and idempotent** — "the same string always returns the same id"; the reverse map is an "append-only flat array, indexed by sym_id." **Sym-ids never drift and are never reused.** So the `sig_j` postings reference the same entity across every snapshot forever. The vector index inherits this for free — no separate id-translation layer, no tombstone churn.

### 2. Snapshot / publish consistency

`sig_j` are ordinary relations, so `dl_publish_snapshot`'s atomic publish (`dl.h:400-405`, interner + ALL relations together) publishes them atomically with `entity(name,type)`. At any snapshot version, the vector index and the entity set are **mutually consistent** — no drift window. Time-travel (`dl_query_version`, `dl.h:421`) works for vector search for free: `dl_vector_search_version` opens the version's `sig_j` views.

### 3. WAL / crash

Incremental entity add → embed → `m` new facts (one per band) flow through the **same WAL+fsync path** as any `dl_add_fact` (`dl.h:88-92`). Crash recovery is inherited, not re-built. Cost note: 1e6 entities × 16 bands = 1.6e7 extra facts — non-trivial WAL volume but the engine is sized for it; tune `m` down (8) if WAL pressure matters.

### 4. EDGE CASE — entity deletion / gardening (do not miss this)

The interner is append-only, so a deleted entity's sym-id **persists** in the interner but the `entity` relation no longer contains it. Two valid policies:
- **(a) query-time live-filter** (recommended default): every MIH candidate sym-id must pass `dl_lookup(entity, [sym_id, *])` before re-rank — cheap, always correct.
- **(b) rebuild `sig_j` on gardening** (the sidecar doc's existing rebuild trigger) — periodic compaction.

Choose (a) at query time and (b) as periodic compaction. **This is the one consistency subtlety a naive implementation would miss.**

## Hybrid symbolic + vector composition

Under MIH, the bit-signature doc's #4 (single-DAFSA intersection of a symbolic regex AND a Hamming neighborhood) is **not available** — MIH stores per-band postings, not a single signature trie, so there is no one automaton to intersect. The composition reverts to the honest **lexical-gate pattern** (`hybrid-search.md`):

1. **Symbolic gate** via `dl_pattern_symbols` / `dl_prefix` over `entity(name,...)` or the full-text postings → candidate sym-id set `C_sym`.
2. **Vector MIH** → candidate sym-id set `C_vec`.
3. **Intersect** `C_sym ∩ C_vec` on the shared sym-id.
4. **Exact-cosine re-rank** the intersection.

This is a **post-hoc JOIN on sym-ids**, not index-level intersection. The single-store value is **not** "one automaton for both axes" — it is *"one identity space + one snapshot + one WAL + one crash-recovery story,"* which is the real consolidation win and is fully preserved.

## The reference sketch (test oracle)

The advisor's artifact (`handoff-semantic-vector-scale-artifact-1`) holds a Python oracle that specifies the implementation and doubles as a test oracle. Key points:

```python
def band_width(d, m): return (d + m - 1) // m          # bits/band, last absorbs remainder
def band_slice(sig, j, d, m):                          # MSB-first banding
    w = band_width(d, m); mask = (1 << w) - 1
    return (sig >> (d - (j+1)*w)) & mask
def variants_within(sub, w, budget):                   # all w-bit values within Hamming budget
    for t in range(budget+1):
        for flips in combinations(range(w), t): ...    # C(w,t); w=16,budget=2 → 137
def vector_search(db, q, k, d, m, r, snap_version):
    q_sig = itq_encode(q)                              # Python sidecar
    budget = r // m                                    # pigeonhole
    for j in range(m):
        for var in variants_within(band_slice(q_sig,j), w, budget):
            # dl_prefix over sig_j leading=[var]  — READ FROM SNAPSHOT VIEW when snap_version>0
            candidates |= probe(...)
    candidates = live_filter(candidates, snap_version) # dl_lookup(entity,...) — THE invariant
    return cosine_rerank(candidates)                   # sym_id → name → .npy row
```

**Variant-count sanity check** (validates MIH over the trie walk): d=256, m=16 → w=16. A 0.9-cosine neighbor at Hamming ~37 → pigeonhole budget `⌊37/16⌋=2`. Variants/band within Hamming 2 of a 16-bit substring = 1+16+C(16,2) = **137**. Total = 16×137 = **2192 probes** vs ~210K trie nodes → **~100× fewer**. The budget `⌊r/m⌋` is the tuning knob (raise `r` or lower `m` to widen the net, at the cost of more variants).

## Edge cases the implementer MUST NOT miss

1. **Live-entity filter (step 4)** is the most important correctness invariant after gardening — a naive impl returning raw MIH candidates surfaces deleted entities because the interner is append-only.
2. **Snapshot-vs-live view selection must be CONSISTENT** for both `sig_j` and `entity` reads — take a single `snap_version` arg and route BOTH through `dl_query_version`-style views (or both live). A publish between the two reads yields an inconsistent set.
3. **`.npy` sidecar lifecycle**: the sidecar has its own rebuild trigger; pin it to a snapshot version (name the sidecar dir after `snap_version`, or store a `sidecar_generation` in a metadata relation) so the name→row bridge never points at a stale vector.
4. **ITQ basis drift**: the basis is fit on the embed pass and persisted; if embeddings drift (model upgrade, re-embed), re-fit or recall collapses silently. Treat basis re-fit as part of the sidecar rebuild trigger.

## Files that change (when implemented)

| File | Change |
|---|---|
| `docs/datalog-dafsa-vector-search.md` | this doc (design of record) |
| `scripts/embed.py` | ALSO emit ITQ bit-codes (`d` bits) alongside `.npy`; persist ITQ basis; retain floats for re-rank |
| `src/dl.h` / `src/dl.c` | `dl_vector_search(db, q, k, [r], [m])` — variant enumeration → `dl_prefix` per `sig_j` → union → live-filter → cosine re-rank; `dl_vector_search_version` (time-travel); `dl_search_hybrid` (compose with `dl_pattern_symbols`) |
| `src/dl_cli.c` | `dl vsearch <vector> [--k] [--radius] [--bands]`, `dl vhybrid <prefix> <vector>` |
| storage (no new code) | `sig_j` via existing `dl_declare_relation` + `dl_add_fact` |
| `tests/` | `test_vector_search.c` (probabilistic recall, edge cases above, snapshot parity) |

## Build path (ranked)

1. **Ship the sidecar now** (`semantic-sidecar.md`) — `.npy` + exact cosine covers ~2K–1e4 with **zero** new engine code; don't block the scale path on it.
2. **Add `dl_entity_names` iterator** (semantic-sidecar slice 1) — needed by the embed step regardless.
3. **Extend `scripts/embed.py`** to emit ITQ bit-codes alongside `.npy`; keep float retention for re-rank; persist + re-fit the ITQ basis.
4. **Add the MIH postings relations** `sig_j(substr_u32, entity_sym_id)` for `j in 0..m-1`, populated from the embed step. No new storage code.
5. **Implement `dl_vector_search`** (the correctness-sensitive slice — see Implementer tier).
6. **Hybrid symbolic-gate API** `dl_search_hybrid`.
7. **Snapshot/publish**: nothing to do — `sig_j` publish atomically for free; add `dl_vector_search_version` as a thin shim.
8. **Benchmark `d/m/r` on real data at the target N**; if MIH loses decisively to HNSW-lite at the actual workload, fall back to HNSW-lite as a foreign sym-id-composed tier (graceful degradation, not an architecture change).

## Honest ceiling

- **Recall is bounded by the bit encoding** (cosine→Hamming is lossy regardless of search method); re-rank fixes PRECISION, not RECALL. True of every LSH scheme, not a DAFSA weakness.
- MIH is competitive with HNSW on recall/latency at **1e5–1e6** entities for moderate bit-lengths (d=256, m=8–16), and wins the consolidation story outright.
- Beyond **~1e7** entities OR adversarial embeddings, HNSW/FAISS dominate on raw latency; `sig_j` also grows linearly (one fact per band per entity) so at 1e7 × 16 bands = 1.6e8 facts the per-relation footprint + WAL volume start to bite.
- **Honest ceiling for the in-store MIH approach: ~1e6–1e7 entities, moderate QPS.** Above that, escalate to HNSW-lite as a foreign sym-id-composed tier (build path step 8).
- The DAFSA contributes **integration + postings-over-discrete-keys**, NEVER compression and NEVER spatial NN structure. This design does not claim otherwise.

## References

- Advisor plan: `handoff-semantic-vector-scale-plan` + artifact `handoff-semantic-vector-scale-artifact-1`.
- Critique: `docs/datalog-dafsa-bit-signature-index.md` (the corrected path this design builds on).
- `docs/datalog-dafsa-hybrid-search.md` (lexical-gate composition).
- `docs/datalog-dafsa-semantic-sidecar.md` (the .npy boundary, preserved).
- `docs/datalog-dafsa-fulltext.md` (postings relation shape `term→doc`, same as `sig_j`).
- `src/intern.h` (append-only sym-id stability), `src/dl.h` (snapshots, WAL, `dl_prefix`).
- Norouzi et al., "Fast exact search in Hamming space," TPAMI 2012 (MIH); Gong & Lazebnik, ITQ 2011.
