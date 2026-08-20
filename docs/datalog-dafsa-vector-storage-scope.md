# Vector tier storage — implementation scope

**Status:** Scoping (2026-08-20), **refined 2026-08-20 by consultant + advisor** (both planner passes; corrections F1–F7 / C1–C3 folded in below). Operationalizes the design of record — `docs/datalog-dafsa-vector-search.md` (MIH over ITQ bit-codes as per-band DAFSA postings) + `docs/datalog-dafsa-vector-search-no-sidecar.md` (int8 re-rank vectors in-store, killing the `.npy` sidecar) — down to concrete relations, encodings, lifecycle, and slices. **This doc is about STORAGE** (the durable relations); the query path (`dl_vector_search`), embed pipeline, and hybrid composition are scoped here too but as downstream slices.

**Relationship to the roadmap:** this is the common prerequisite behind stack Ideas 3 (one embed pass → all auxiliaries), 4 (search-as-Datalog), and the hybrid-search lexical-gate pattern. The full-text tier (`__postings__` + `dl_search`, Ideas 1 + this session's version-aware search) is the symbolic half and is done; this is the semantic half.

## Grounding facts (verified against the engine)

- Relation columns are **fixed-width u32BE** (`relation.c`). Any 32-bit value is a legal column. **No float *type*, but float32/int8 bit-patterns fit one u32.**
- Arity cap: **1–8** fixed, 1–8 variadic (`rel_arity`, `dl_declare_relation`).
- **`MAX_RELS = 64`** (`dl_internal.h:39`) — this is the load-bearing constraint for the `sig_j` naming decision.
- Baseline relations in a memory-graph DB: `entity`, `edge`, `observation`, `__postings__`, plus any user relations. `m=16` `sig_j` + `vec_q` + `itq_basis` ≈ 19 more slots — comfortably inside 64 for a search DB, but worth documenting.
- `dl_publish_snapshot` saves **all** relations + interner + permutation indices atomically → time-travel/version search is free.
- `dl_iter` exists (iterate a relation); `dl_entity_names` does **not** exist yet — the embed step needs a way to walk `entity(name, type)`; `dl_iter` over `entity` suffices (no new iterator strictly required).
- `dl_prefix`, `dl_declare_relation`, `dl_add_fact` all exist — the storage layer needs **zero new C engine code**.

## The three storage relation families

| Relation | Arity | Key | Facts/entity | Purpose |
|---|---|---|---|---|
| **`sig_j`** (j = 0..m-1) | 2 | `substr_u32 → entity_sym_id` | m (default 16) | MIH candidate postings — same shape as `__postings__` |
| **`__vec_q__`** | 3 | `entity_sym_id, chunk_idx, packed_4x_int8_u32` | ceil(D/4) = 96 (D=384) | int8 re-rank vectors |
| **`__itq_basis__`** | 3 | `dim_i, dim_j, float32_bits_u32` | **D×c = 384×256 = 98,304** | ITQ encode matrix, pinned to snapshot |

> **F4** — `itq_basis` is the combined ITQ **encode matrix D×c = 384×256 = 98,304** float32 (~393 KB), **NOT** 384×384. D (embedding dim) = 384 (bge-small) ≠ c (bit-code length) = 256. `vec_q` chunks = D/4 = 96 (the full re-rank vector, independent of c). Naming normalized to `__vec_q__` / `__itq_basis__` (**F7**).

### sig_j (MIH candidate postings)
- ITQ signature is `c` bits (e.g. 256), split into `m` bands of width `w = ceil(c/m)` (MSB-first, last absorbs remainder). With c=256/m=16 → w=16.
- Each band `j` is a `w`-bit substring, stored as a **u32**, keyed as the leading column of `sig_j`.
- **Band layout (C1 — MUST match embed.py exactly):** MSB-first, band 0 = bits (c-1)..(c-w) = HIGH 16 of `sig[0]`. Even j → high 16 of word j/2 (shift 16); odd j → low 16 (shift 0). **The write (embed.py) and read (C query) MUST use the IDENTICAL formula, or the bands scramble and recall collapses SILENTLY.** Pinned formula:
  ```c
  static uint32_t band_slice(const uint32_t *sig, int j) {
      return (sig[j / 2] >> ((1u - (j % 2u)) * 16u)) & 0xFFFFu;
  }
  static void band_set(uint32_t *sig, int j, uint32_t val16) { /* inverse, for round-trip test */
      int shift = (int)((1u - (j % 2u)) * 16u);
      sig[j/2] = (sig[j/2] & ~(0xFFFFu << shift)) | ((val16 & 0xFFFFu) << shift);
  }
  ```
  (The earlier `handoff-vector-tier-artifact-1` had a comment/code mismatch here — the fix is folded in.)
- Query-side: enumerate the query band's variants within pigeonhole budget `⌊r/m⌋`, `dl_prefix(sig_j, leading=[variant])` per band, union candidates, live-filter, re-rank.
- **Same shape as `__postings__(term_sym, obs_id)`** — a `token → id` postings DAFSA. This is the reuse that makes the storage slice cheap.

### vec_q (int8 re-rank vectors)
- Unit-normalized embedding → int8 (`v / (max|v|+1e-12) * 127`), packed 4 int8 per u32 (little-endian: `b0|b1<<8|b2<<16|b3<<24`).
- Read back: unpack each u32 to 4 int8, re-normalize, int8-cosine **re-rank in C as pure integer math** (F5) — int32 dot (max |dot| = 384·127² = 6.2e6 < 2³¹, exact), **int64 cross-multiply rank** (`dot_a²·|b|²` vs `dot_b²·|a|²`, since dot² up to 3.8e13 > 2³¹), **int8 sign-extend via `(int8_t)(w & 0xFF)`** (bytes are two's-complement int8). No float, no sqrt → respects the "no float/embedding code in C" boundary. No Python FFI needed (`pydl/` is empty).
- Precision: preserves cosine ~3 decimals; int8 re-rank top-10 agreement with float-exact must be **≥99%** (oracle gate).
- `dl_vector_search` takes a **pre-encoded c-bit signature** (`q_sig` as 8 u32 words) + pre-quantized int8 query vector — **C never embeds**. Embedding + ITQ encode stay in Python.

### itq_basis (ITQ encode matrix)
- Combined encode matrix D×c = 384×256 = 98,304 float32, stored as u32 bit-patterns (`float32_bits_u32`). ~393 KB.
- Pinned to the same snapshot as vectors (`dl_publish_snapshot` atomic) → basis↔embedding drift impossible. **Never read by the C query path** — stored for snapshot-pinning only.
- Basis is **fit in Python** (PCA 384→256 + orthogonal rotation during embed); only storage moves in-store.

## DECISION — `sig_j` naming: `m` fixed arity-2 relations `__sig0__`..`__sig<M-1>__`

**Locked.** Rationale:
- Each band needs its **own** `dl_prefix` keyed on a different band's substring. A single variadic relation would store `(band, substr, entity)` — still workable via leading `[band, substr]`, but adds an extra leading column to every probe and complicates the "same shape as `__postings__`" reuse argument.
- `m` fixed arity-2 relations keeps each probe a plain 1-column prefix — byte-identical to the existing full-text postings walk. Maximal reuse of `__postings__` machinery.
- **Relation-slot cost:** `m=16` fixed `sig_j` → 16 slots + `vec_q` (1) + `itq_basis` (1) = 18 additional slots out of `MAX_RELS=64`. A memory-graph DB with entity/edge/observation/__postings__ (~4) + 18 = ~22 — fine. **If `MAX_RELS` pressure ever matters, the knob is `m` (drop to 8 → 8 sig_j + 2 = 10 slots), which the WAL-tuning path already contemplates.** Documented, not a blocker.
- **Interned band substrings?** The band value is a raw u32 bit-pattern, **not** an interned string. It's a plain integer leading column. No interner involvement for `sig_j` keys. (`vec_q` chunk_idx and `itq_basis` dim indices are likewise raw u32.)

## Data flow / lifecycle

```
embed.py (new):
  fit:  ITQ basis B (D×c = 384×256) via PCA(384→256) + orthogonal rotation
  for each entity name → content:
    v = embed(name/content)              # bge-small, D=384
    v_norm = normalize(v)
    sig = itq_encode(v_norm, B)          # c=256 bits, as 8 u32 words
    for j in 0..m-1:                      # emit MIH postings → __sig{j}__ (arity-2)
        emit_csv("__sig{j}__", band_slice(sig, j), entity_id)
    vi8 = quantize_int8(v_norm)           # int8 re-rank vector, D=384
    for chunk in 0..95:                   # emit __vec_q__ (arity-3)
        emit_csv("__vec_q__", entity_id, chunk, pack4(vi8[chunk*4:chunk*4+4]))
  # emit __itq_basis__ (arity-3) once per re-embed: dim_i, dim_j, float32_bits_u32
  for i in 0..D-1, j in 0..c-1:
      emit_csv("__itq_basis__", i, j, float32_bits(B[i][j]))
  dl_load_facts per relation (bulk CSV → single rel_save_base each)
  dl_publish_snapshot()                   # ONE atomic publish → all auxiliaries consistent
```

- **Bulk path (F6):** `dl_add_fact` WAL-appends + fsyncs **per fact** (`dl.c:1588`) — the "one fsync on rebuild" property requires **`dl_load_facts` (CSV → single rebuild+save per relation, `dl.c:1231`)**, then one publish. Never per-fact `dl_add_fact` for the bulk load. **CSV parser accepts full u32 `[0, 2³²−1]`** (`dl.c:1135-1136`) — packed int8 u32 values exceed INT32_MAX, verified OK.
- **Re-embed is wholesale** (ITQ basis re-fit), so all aux facts are written then **one `dl_publish_snapshot`** — crash before publish discards the partial write, previous snapshot stays consistent.
- `__itq_basis__` + `__vec_q__` + `sig_j` publish atomically with `entity` → time-travel (`dl_vector_search_version`) gets a consistent vector index + basis for free.

## The two correctness invariants (from the design of record — do not miss)

1. **Live-entity filter (F2).** The interner is append-only; a deleted entity's sym-id persists in `sig_j`. Every MIH candidate must pass a membership check before re-rank, or dead entities surface. **The primitive is a PREFIX query `dl_prefix(db,'entity',[sym_id],1,cb)` returning >0 rows — NOT `dl_lookup(entity,[sym_id,*])`** (entity is arity-2; `dl_lookup` requires exact arity, no wildcard). Version mode: `dl_query_bound_version(...,'entity',[sym_id],1,cb)`. Recommended: query-time filter (always correct) + periodic `sig_j` rebuild on gardening (compaction).
2. **Snapshot-vs-live view consistency (F1).** `dl_prefix` and `dl_lookup` are **LIVE-ONLY** — they never route to the snapshot view regardless of `db->snap_version`. Version reads MUST use `dl_query_bound_version`. So: **LIVE search reads BOTH `sig_j` and `entity` via `dl_prefix`; VERSION search reads BOTH via `dl_query_bound_version(db, version, ...)`** — the two modes differ ONLY in the read primitive (mirror `dl_search` vs `dl_search_version` in `index.c`). A publish between a `sig_j` read and an `entity` read yields an inconsistent candidate set. This single-structure, two-mode shape is what makes the invariant auditable.

## Downstream slices (scope boundary)

**Storage slice (this scope — cheap, no new C code):**
- S1 (`coder`): declare `__sig0__`..`__sig<M-1>__` (arity-2), `__vec_q__` (arity-3), `__itq_basis__` (arity-3) via `dl_declare_relation`. Load a handful of synthetic vectors via `dl_load_facts` CSV (this is where F6's full-u32 CSV parsing is caught cheaply). Round-trip test: `dl_prefix` over a `sig_j` returns the right `entity_sym_id`; `dl_prefix` over `__vec_q__` (k=1) returns 96 chunks in order. **Must include the band-layout round-trip gate (C1):** pack a known 256-bit code with `band_set`, slice each band with `band_slice`, reconstruct, assert equality — pins the write/read formula agreement that prevents silent recall collapse. Deterministic (not probabilistic).

**Query path (the correctness-sensitive slice — separate, larger, `pro-coder`):**
- S2 (`pro-coder` + mandatory reviewer): `dl_vector_search(db, q_sig[8], q_int8[96], k, r, cb)` — variant enumeration → per-band `dl_prefix` → union → **live-filter** → int8 re-rank. Signature **removes the `[m]` free param (F3):** `m`, `c`, `D` are compile-time `#define`s (VEC_M=16, VEC_C=256, VEC_D=384) shared with embed.py; only `r` (pigeonhole budget) stays runtime. Port `band_slice`/`variants_within`/`emit_live` from `handoff-vector-tier-artifact-1` with the C1 one-line fix; **`emit_live` + `dl_vector_rerank` must be written fresh from the downstream spec (C2), not ported-truncated.** `dl_vector_search_version` — thin shim, same structure, differs only in the read primitive (dl_query_bound_version). int8 re-rank integer-in-C per F5 (int64 cross-multiply, int8 sign-extend).
- `dl_search_hybrid` — lexical-gate composition (S4).

**Embed pipeline (the bulk of the work — new Python):**
- S3 (`coder` + cheap reviewer on packing formula): `scripts/embed.py` — embed → ITQ fit+encode → emit sig_j/__vec_q__/__itq_basis__ CSV → `dl_load_facts` per relation → one publish. Uses `dl_iter` over `entity` (no new iterator required). No standalone `dl declare` — declaration folds into `dl load` (`dl_cli.c:240-281` auto-infers arity from CSV header).

**CLI:** `dl vsearch`, `dl vhybrid`.

**Tests:** `test_vector_search.c` — pack/unpack round-trip, MIH recall (probabilistic, seed-fixed), edge cases (live-filter, view consistency), snapshot parity. **Oracle (C3):** the ≥99% float-exact agreement gate needs a **C-vs-Python int8 100% sub-gate** first — assert C re-rank and Python re-rank agree exactly on the same input (isolates quantization loss from C-implementation bugs like a missed int64 promotion). Then int8-vs-float ≥99% over 10K queries.

## Honest ceiling

- Recall bounded by the bit encoding (cosine→Hamming lossy regardless); re-rank fixes precision, not recall.
- In-store MIH is competitive at **1e5–1e6** entities (d=256, m=8–16); beyond ~1e7 or adversarial embeddings, HNSW-lite as a foreign sym-id-composed tier is the graceful fallback (not an architecture change).
- The DAFSA contributes **integration + postings-over-discrete-keys**, never compression and never spatial NN structure.

## Files that change (when implemented)

| File | Change |
|---|---|
| `docs/datalog-dafsa-vector-storage-scope.md` | this doc |
| `scripts/embed.py` (new) | embed → ITQ → emit sig_j/__vec_q__/__itq_basis__ CSV → `dl_load_facts` → one publish |
| `src/vector.h` / `src/vector.c` (new) | `dl_vector_search` / `_version` / `dl_vector_rerank`; `dl_search_hybrid`; VEC_M/C/D constants |
| `src/dl.h` / `src/dl.c` | expose `dl_vector_search` / `_version` |
| `src/dl_cli.c` | `dl vsearch`, `dl vhybrid` |
| storage (no new code) | sig_j/__vec_q__/__itq_basis__ via existing `dl_declare_relation` + `dl_load_facts` |
| `tests/` | `test_vector_storage.c` (S1), `test_vector_search.c` (S2) + oracle |
| `Makefile` | add `src/vector.o`, `test_vector_storage`, `test_vector_search` targets |

## References

- `docs/datalog-dafsa-vector-search.md` (design of record, MIH).
- `docs/datalog-dafsa-vector-search-no-sidecar.md` (int8-in-store, basis handling, oracle gate).
- `docs/datalog-dafsa-hybrid-search.md` (lexical-gate composition).
- `docs/datalog-dafsa-fulltext.md` + `src/index.c` (postings shape reused for `sig_j`; version search pattern reused for `dl_vector_search_version`).
- `src/intern.h` (append-only sym-id stability), `src/dl.h` (arity cap, `MAX_RELS` in `dl_internal.h`, `dl_publish_snapshot`, `dl_prefix`, `dl_iter`).
