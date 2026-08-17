# Eliminating the `.npy` vector sidecar (int8 re-rank vectors in-store)

**Status:** Design — recommended by the advisor (2026-08-17). A follow-on to `docs/datalog-dafsa-vector-search.md` (the MIH design of record). After the MIH engine is built, this design **kills the `.npy` float sidecar** that the MIH design retains for the exact-cosine re-rank step, by storing **int8-quantized re-rank vectors in datalog-dafsa**. Consolidates to one store/WAL/snapshot/crash-recovery story.

> ⚠ This is a *refinement of* the MIH design, not a replacement. Read `docs/datalog-dafsa-vector-search.md` first. The re-rank still happens; only its *data source* moves from a `.npy` file to in-store relations.

## TL;DR

Store the re-rank vectors in datalog-dafsa as an **int8-quantized, packed-u32 relation** `vec_q(entity_sym_id, chunk_idx, packed_4x_int8_u32)`, plus the **ITQ basis** in `itq_basis(dim_i, dim_j, float32_bits_u32)`. The C engine stores opaque u32 bit-patterns exactly as it already does for `sig_j` postings — **zero new C code**. int8 unpack + cosine re-rank stays in **Python**, preserving the "no float/vector code in C" boundary.

## Why this and not the alternatives

| Approach | Verdict |
|---|---|
| **int8-quantized re-rank vectors in-store** | **CHOSEN** — preserves re-rank precision (~3 decimals, ~99% top-10 agreement vs float), consolidates to one store, ~1.3 GB at N=1e6 (same order as the `.npy` it replaces), does NOT change the MIH ceiling. |
| float32-in-store | **REJECTED** — float32 *does* fit one u32 (encoding-feasible), but it's **4× the WAL/snapshot volume for zero re-rank precision benefit** over int8. Pure cost. |
| float-free Hamming re-rank | **REJECTED** — architecturally dishonest. Re-rank fixes *precision*, not recall; Hamming over the bit-codes is *exactly* the lossy MIH step, so it restores nothing. A longer second signature is just a bigger lossy bit-code. |

## Encoding + storage cost (honest numbers, bge-small d=384, m=16)

Per entity:
- MIH postings = **16 facts** (`sig_j`).
- int8 re-rank packed 4/u32 = `ceil(384/4)` = **96 facts** (`vec_q`).

So re-rank storage is ~6× the MIH postings per entity. Totals:

| N | `vec_q` facts | semantic tier on-disk |
|---|---|---|
| 1e5 | 9.6e6 | ~0.13 GB — tractable |
| 1e6 | 9.6e7 | **~1.35 GB** (vec_q ~1.15 GB + sig_j ~0.19 GB + basis ~1.8 MB) — same order as the 1.5 GB `.npy` it replaces, but durable/versioned/atomic |
| 1e7 | 9.6e8 | already past the MIH ceiling (see Honest ceiling); HNSW-lite governs |

Compare: float32-in-store at N=1e6 = 3.84e8 facts ≈ **4.6 GB** (4× the int8 cost) for no precision gain — rejected. The `.npy` sidecar = 1.5 GB single file, one fsync on rebuild, but a separate crash-recovery/durability story (the consolidation pain).

**The honest trade:** ~1.35 GB WAL/snapshot volume at N=1e6 in exchange for single-store consolidation. Worth it.

## ITQ basis handling

- The basis is d×d (384×384 = 147,456 float32 = 590 KB) — trivially small. Store in-store as `itq_basis(dim_i, dim_j, float32_bits_u32)` (arity-3, 147K u32 facts).
- **Correctness improvement over the sidecar:** the basis is pinned to the same snapshot version as the embeddings (`dl_publish_snapshot` is atomic over all relations), so **basis↔embedding drift cannot happen** — the sidecar's separate basis file could drift from the `.npy` rows.
- Basis stays **fit in Python** (PCA+rotation, the embed step already does it); only its *storage* moves in-store. Re-fit remains part of the re-embed trigger (wholesale re-embed → re-fit basis → bulk-publish `vec_q` + `itq_basis` + `sig_j` together).
- No float code in C: the stored column is a u32 bit-pattern; C never decodes it.

## Re-rank correctness story (precision preserved)

- MIH yields candidate set `C` (recall superset, |C| ~2K, Hamming-bounded).
- Re-rank reads int8 vectors: for each candidate `sym_id`, `dl_prefix(db, vec_q, leading=[sym_id])` → iterate chunks → unpack each u32 into 4 int8 → reassemble d-length int8 vector → compute int8 cosine in **Python** (`dot(a,b)/(|a|·|b|)`).
- **Precision:** int8 quantization of unit-normalized embeddings preserves cosine to ~3 decimal places; for ordering top-K from |C| ~2K this almost never reorders the true top-10.
- **Honest downgrade to document:** re-rank becomes **"int8-precision re-rank," not "exact-cosine."** The precision *restoration* vs raw MIH Hamming is still large (int8 ~3 decimals vs bit-code ~1 bit/dim), so re-rank still does its job — it just no longer reaches exact float cosine.
- **The `.npy` sidecar remains as an OPT-IN exact tier** for use cases that demand exact-cosine.

## The verification oracle (REQUIRED before dropping the sidecar)

The implementer MUST run this (or equivalent) on real bge-small embeddings before dropping `.npy` as the default. **Gate: int8-rerank top-10 agreement with float-exact ≥ 99% over 10K sampled queries.**

- **Quantize:** `v / (max(|v|)+1e-12) * 127.0`, round to int8.
- **Pack (embed step writes via `dl_add_fact`):** d=384 → 96 u32 (4 int8 each, little-endian within u32): `out[i] = b[0] | (b[1]<<8) | (b[2]<<16) | (b[3]<<24)` where `b = int8_vec[4i:4i+4].astype(uint32) & 0xFF`. Lossless.
- **Unpack (re-rank reads u32 facts back from `vec_q` via `dl_prefix(sym_id leading)`):** `out[4i]=w&0xFF; out[4i+1]=(w>>8)&0xFF; out[4i+2]=(w>>16)&0xFF; out[4i+3]=(w>>24)&0xFF`, then view as signed int8 (two's complement). Re-normalize after dequant for cosine.
- **Gate:** for each of 10K sampled queries, take a candidate set (in production: the actual MIH candidates from `dl_vector_search`, not a proxy), compute top-k by float-exact and by int8 cosine, accumulate agreement. Assert ≥ 99%, else keep the float sidecar.

This packing/unpacking is the bit-pattern contract with the C store (`dl_add_fact` writes the u32; re-rank reads it back via `dl_prefix`); the C engine never inspects the bits.

## Migration path (sidecar-first, then eliminate)

1. **Ship MIH + `.npy` float sidecar first** (design-of-record build path steps 1–7 of `vector-search.md`). Don't block consolidation on this; the sidecar is correct and shippable.
2. **Add int8 quantization to `scripts/embed.py`** alongside ITQ: emit BOTH the `.npy` float sidecar AND int8 packed vectors; persist the ITQ basis. No store change yet. **Run the precision oracle gate (≥99% top-10 agreement) before proceeding.**
3. **Add the in-store relations** `vec_q(sym_id, chunk_idx, packed_4x_int8_u32)` and `itq_basis(dim_i, dim_j, float32_bits_u32)`, populated from `embed.py` via `dl_add_fact`. **Zero new C engine code** — uses existing `dl_declare_relation` + `dl_add_fact` + `dl_prefix` + `dl_iter`.
4. **Switch the re-rank path** to read `vec_q` from the store (Python: `dl_prefix(sym_id leading)` → iter chunks → unpack int8 → cosine). Keep `.npy` as a flagged fallback exact tier.
5. **Drop `.npy` as the DEFAULT** once int8 re-rank is validated in production; keep it opt-in for adversarial/exact-cosine cases.
6. **WAL mitigation (the key lever):** treat `vec_q` as a **bulk-publish relation** — re-embed is wholesale anyway (ITQ basis re-fit), so write all `vec_q` facts into the live store then a **single `dl_publish_snapshot`** atomically publishes them. This recovers the sidecar's "one fsync on rebuild" property in-store: a partial `vec_q` write before publish is discarded on crash (the previous snapshot's `vec_q` stays consistent). Never incrementally WAL `vec_q` per entity.
7. **Snapshot / time-travel:** `vec_q` + `itq_basis` publish atomically with `sig_j` + `entity`, so `dl_vector_search_version` (time-travel) gets consistent re-rank vectors for free — a capability the `.npy` sidecar never had (it needed a `sidecar_generation` pin).

## Files that change (when implemented)

| File | Change |
|---|---|
| `docs/datalog-dafsa-vector-search-no-sidecar.md` | this doc |
| `scripts/embed.py` | add int8 quantization + 4-per-u32 packing; emit `vec_q` + `itq_basis` via `dl_add_fact`; persist basis |
| re-rank path (Python) | read `vec_q` via `dl_prefix`, unpack int8, cosine re-rank |
| storage (no new C code) | `vec_q`, `itq_basis` via existing `dl_declare_relation` + `dl_add_fact` |
| `tests/` | precision-gate oracle; pack/unpack round-trip; bulk-publish crash consistency |

## Honest ceiling

- Killing the sidecar via **int8-in-store does NOT change the MIH feasibility ceiling** (~1e6–1e7 entities). `vec_q` is the same order as the MIH postings at 1e6 and both dominate at 1e7; HNSW-lite remains the graceful fallback beyond that. **The consolidation is "free" w.r.t. the ceiling.**
- **The one ceiling shift to flag:** float32-in-store WOULD push WAL pressure 4× and could lower the practical ceiling ~4× — precisely why int8 is chosen over float32.
- If int8 re-rank precision is ever insufficient for a use case, the float sidecar remains an opt-in exact tier — the architecture does not foreclose it.

## Boundary integrity (respects `semantic-sidecar.md`)

The semantic-sidecar boundary was "no float code in C" / "no vector code in C." This design **respects that boundary**: the C engine stores opaque u32 bit-patterns (exactly what it already does for `sig_j` postings); it never decodes them. int8 unpacking + cosine re-rank stays in Python. So we kill the `.npy` *storage* sidecar (the consolidation goal) WITHOUT moving vector arithmetic into C.

The earlier "floats don't fit the u32 encoding" framing was about float *types* — bit-patterns fit (float32 IS 32 bits = one u32 column). But we don't even need float bit-patterns, because int8 packed 4/u32 is strictly better for re-rank and 4× smaller. The boundary that actually moves: the embed step now writes `vec_q` facts via `dl_add_fact` (it already writes `sig_j` facts) — no new C API, just more facts.

## References

- Advisor plan: `handoff-no-sidecar-plan` + artifact `handoff-no-sidecar-artifact-1` (the verification oracle).
- `docs/datalog-dafsa-vector-search.md` (the MIH design of record this refines).
- `docs/datalog-dafsa-semantic-sidecar.md` (the `.npy` boundary, preserved).
- `src/intern.h` (sym-id bridge), `src/dl.h` (arity-8 cap, `dl_add_fact`, `dl_publish_snapshot`).
