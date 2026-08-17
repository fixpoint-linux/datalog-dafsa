# Hybrid retrieval: DAFSA-as-sparse-half + vector tier

**Status:** Design/plan — not yet implemented. Architectural note that reframes the full-text doc (`docs/datalog-dafsa-fulltext.md`) and the semantic doc (`docs/datalog-dafsa-semantic-sidecar.md`) as **two halves of one story**: a DAFSA and a vector index are orthogonal query axes that compose on shared sym-ids. **The chosen vector-tier design of record is `docs/datalog-dafsa-vector-search.md`** (MIH over ITQ bit-codes as per-band DAFSA postings, exact-cosine re-rank); this doc's **lexical-gate pattern** (symbolic filter → exact cosine over the gated subset, post-hoc join on sym-ids) is the composition path that design uses. See also `docs/datalog-dafsa-bit-signature-index.md` (superseded exploratory framing + critique).

## TL;DR

A DAFSA and a vector index are **not** competing ways to do the same thing — they are **complements**. The DAFSA handles *symbolic/lexical/structural* queries (exact, prefix, regex/pattern, order-stats); a vector index handles *semantic/continuous* similarity (nearest-neighbor). Composed over shared sym-ids, one store gives you both query axes. The DAFSA is the **symbolic half** (a compact, prefix/regex-capable postings index); the vector tier is the **semantic half**. This is the classic sparse+dense hybrid-search architecture.

## Motivation

- The consolidation goal is "everything on my own stack." The tension was: full-text (symbolic) and semantic (vector) seemed like two unrelated features.
- Insight: they are **two halves of one hybrid-retrieval capability**. The DAFSA's powers are exactly what a vector index is bad at (exact/prefix/regex over discrete keys), and vice versa (similarity over dense vectors). **That complementarity is the synergy.**
- The shared interner (`src/intern.h`) already gives every entity/term a `sym_id` — the join key that lets the DAFSA and the vector tier coexist in one identity space.

## The two query axes

| | DAFSA | Vector index |
|---|---|---|
| Query type | **symbolic / lexical / structural** | **semantic / continuous** |
| Primitives | exact, prefix, regex/pattern, order-stats | nearest-neighbor / similarity |
| Data | discrete symbol sequences | dense high-dim vectors |
| Strong when | you know *what* (a token, prefix, pattern) | you know *what it's like* (not the token) |

## The three real synergies

### 1. Hybrid retrieval (the big one)

- **DAFSA** holds `term_sym → [doc_id]` — a compact postings index (minimized, prefix/regex-capable via automaton intersection). Better than a plain inverted list for this use.
- **Vector tier** holds dense embeddings of the same doc-ids.
- Query = **fuse both**: symbolic hits (DAFSA) + semantic hits (vector), ranked/merged on shared doc-ids. Answers both "contains this exact token / matches this pattern" AND "semantically like this."

### 2. DAFSA as a lexical *gate* that narrows vector search

- Use the DAFSA's regex/prefix-over-symbols (`dl_pattern_symbols`, see `docs/datalog-dafsa-regex-on-symbols.md`) to filter candidates first ("entities whose name matches `^tech`"), then run vector cosine **only over those candidates**.
- Two-tier query: **symbolic filter → semantic rank**. Uses each structure's strongest primitive in the right place, and avoids a full-vector scan.

### 3. Dual modality over shared identity

- Everything indexed by the same `sym_id`. datalog-dafsa owns identity + all symbolic queries; the vector tier is a layer over the same keys.
- One store, two index types, joined on identity — the consolidation goal, made coherent.

## The false friends (explicitly rejected)

- **"A DAFSA compresses the vector index."** No. DAFSA compression comes from shared *prefixes/suffixes* of discrete keys; dense vectors (even int8-quantized) don't share suffixes. The minimization does almost nothing for dense data.
- **"A DAFSA is the ANN structure."** No. A DAFSA is a lexical/ordered DAG — no spatial/similarity structure. It cannot do nearest-neighbor ranking.
- **"Store PQ codes in a DAFSA, search by Hamming."** Weak. Product-quantization codes are discrete but searched by *similarity*, not lexicographic prefix — prefix-enum doesn't help.

## The concrete shape for datalog-dafsa

> **datalog-dafsa = the central identity + symbolic store. A vector index is a *tier* over the same sym-ids, composed for hybrid / gated retrieval. The DAFSA never becomes a vector index — it becomes the symbolic complement that makes a vector tier useful in a single-store system.**

Concretely:
- DAFSA relations: `term_sym → [doc_id]` (postings, from full-text doc) and `entity(name, type)` (identity).
- Vector tier: `.npy` sidecar OR a `vec(entity_sym, dim, float32)` relation (see semantic doc, Option C), keyed by the same sym-ids.
- Query pipeline: symbolic gate (DAFSA) → vector similarity (tier) → fused ranking on shared doc-ids.

## Files that change

| File | Change |
|---|---|
| `docs/datalog-dafsa-hybrid-search.md` | this doc |
| `docs/datalog-dafsa-fulltext.md` | add "this is the symbolic half of hybrid retrieval" cross-link |
| `docs/datalog-dafsa-semantic-sidecar.md` | add "this is the semantic half; compose with the DAFSA for hybrid" cross-link |
| `src/index.c` (future) | postings build + `dl_search` (from full-text doc) |
| `src/*` (future) | vector tier storage + cosine layer (from semantic doc, Option C) |

## Honest ceiling

- **No ANN in the DAFSA.** The vector tier is exact-cosine or a separate ANN library; the DAFSA contributes symbolic filtering + postings, not approximate search.
- The hybrid value is real but depends on the *composition* — neither half is novel alone; the synergy is in fusing them on shared sym-ids.
- Ranking fusion (how to merge symbolic + semantic scores) is a design decision to make when both halves exist.

## Concrete next slice (if pursued)

1. Ship the full-text postings DAFSA (`dl_search`, from full-text doc) — the symbolic half.
2. Choose the vector tier storage (sidecar vs in-engine `vec` relation, from semantic doc).
3. Add the lexical-gate pattern (DAFSA regex filter → vector cosine over candidates).
4. Implement score fusion (symbolic + semantic) on shared doc-ids.
5. Wire a combined query API.

## References

- `docs/datalog-dafsa-fulltext.md` (symbolic postings half).
- `docs/datalog-dafsa-semantic-sidecar.md` (semantic tier half).
- `docs/datalog-dafsa-regex-on-symbols.md` (`dl_pattern_symbols` lexical gate).
- `src/intern.h` (shared `sym_id` join key).
- Analysis memory: "replacement assessment: datalog-dafsa vs jing-memory/indexer/unified-search 2026-08-17".
