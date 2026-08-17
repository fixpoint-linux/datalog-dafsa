# Bit-signature vector index via random-hyperplane LSH (the "DAFSA for vectors")

**Status:** Design note — **SCALE-ONLY, SUPERSEDED for the chosen path.** Independently reviewed (2026-08-17, see **Critique & corrections** below); the review found it is a legitimate architecture but **not worth building at ~2K entities** — a plain exact-cosine scan dominates it on every axis that matters at that scale. **The chosen design of record is `docs/datalog-dafsa-vector-search.md` (multi-index hashing + ITQ as per-band DAFSA postings). This doc is kept as the record of the exploratory bit-signature framing and its critique.**

> ⚠ **Read the Critique & corrections section first.** Two of the four "improvements" below (#1 best-first walk, #2 whitened PCA) were found **substantially wrong** on review — #1 is correct but *slower than a scan*; #2 breaks the very LSH math it relies on. The corrected recommendation is **multi-index hashing + ITQ**, not the walk/PCA-sign framing below. The body of this doc records the original (flawed) design so the critique has something to correct; don't implement from it directly.

## TL;DR

Cosine similarity can be preserved as **Hamming distance on fixed-length bit-strings** (random-projection LSH / simhash family). Those bit-strings are just strings — so index them in a DAFSA/trie, where near-identical vectors share long signature prefixes (the DAFSA's core strength). Search = a **bounded-mismatch automaton walk** over the DAFSA collecting all signatures within Hamming radius `r` of the query, unioned across an **LSH forest** (K independent projections) for recall, then **re-ranked by exact cosine** over the small candidate set. This is "approximate candidate generation + exact re-ranking," and it reuses the automaton-walk machinery the engine already has.

> **See Critique & corrections below** — several claims in this TL;DR (prefix-sharing "core strength", the automaton-walk reuse, and both headline improvements) were found overstated or wrong on independent review.

## Critique & corrections (2026-08-17, independent review — READ FIRST)

The design below was reviewed and **empirically verified** against the host engine (regexwalk.c, intern.h). Verdict: legitimate architecture, but **not worth building at jing-memory scale**, and the two headline "improvements" (#1, #2) are substantially wrong as advertised. Summary:

### What's wrong

1. **#2 "whitened PCA → shared prefix = semantic hierarchy" is doubly flawed.**
   - *"Whitened" is a no-op for sign binarization:* `sign(λx)=sign(x)` for λ>0, so scaling PCs to unit variance changes nothing in the output bit. Only axis *choice* (rotation) and *ordering* matter.
   - *"Shared prefix = semantic hierarchy" is wishful.* PCA axes are global-**variance** directions, not semantic axes; ordering bits by variance gives a *geometric* coarse-to-fine partition (a PCA-tree), which tracks semantics only insofar as embedding geometry does (weak for bge-small).
   - **#2 breaks the recall math.** The clean `Pr[bit agrees]=1−θ/π` law holds **only for isotropic random hyperplanes**. Data-aligned PCA axes make per-bit agreement data-dependent. **Verified:** with a dominant data axis, two vectors at fixed angle agree on the dominant-axis bit ~always (measured 1.0 vs theory 0.857). You cannot have both clean simhash math *and* importance-ordered PCA prefixes. **Fix = ITQ** (learned rotation after whitening), not plain PCA-sign.

2. **#1 "best-first exact-Hamming walk" is correct but NOT efficient — "the big one" is overstated.**
   - Correctness is sound (Hamming is monotone along a trie path → subtree pruning is exact).
   - **Efficiency is wrong.** **Verified:** at d=256, a true 0.9-cosine neighbor (distance ~37) makes the walk visit ~210K trie nodes (N=1000) vs the exact scan's 256K bit-compares — *slower than a scan*, plus heap overhead. It is ~O(N·d), not sublinear. The tension is fundamental: at d=256 the encoding is so lossy that true neighbors sit at Hamming ~37, so *any* exact Hamming search must explore most of the space.

3. **"Exact-in-Hamming, only encoding lossy" is technically true but misleading.** Exact-in-Hamming does **not** recover cosine top-k; the encoding loss (cosine→Hamming) is the *entire* recall bottleneck. #1 buys determinism, not recall, at a cost (critique #2). The candidate-gen + exact-cosine re-rank pipeline is sound — as a *recall-guaranteed* pipeline, not an exact one. (Also unstated: re-rank needs the original float vectors retained; the signature DAFSA is an *additional* index over the `.npy` sidecar, not a replacement.)

4. **DAFSA minimization is inert here — the compression false-friend is reintroduced.** The parent doc (hybrid-search.md) explicitly rejects "a DAFSA compresses the vector index." This doc's line ~20 re-asserts it. For fixed-length random/whitened bit-strings with a **unique per-vector sym_id payload, there is NO suffix sharing** — **verified:** ~0 minimization at N=2000 (44K trie nodes, keys suffix-distinct). "Index in a DAFSA" = a plain binary trie, not compression. What the DAFSA *actually* contributes: the shared sym-id join key, a traversal substrate, and hybrid symbolic+vector composition (#4) — **integration, not compression**.

5. **"Reuse the automaton-walk" is overstated and partly infeasible.** `regex_dfa_walk` is a DFS + visited-set; a best-first Hamming walk is a priority-queue Dijkstra — a **new** primitive sharing only the DAFSA traversal substrate. And "Hamming search IS a bounded-mismatch regex" is infeasible: you can't express "≤ r mismatches" in the supported regex subset, and a radius-37 automaton exceeds the 8192-state DFA cap.

### Corrected recommendations (ranked)

1. **Use MULTI-INDEX HASHING (Norouzi et al., "Fast exact search in Hamming space", TPAMI 2012)** instead of the exact-Hamming trie walk: split the d-bit signature into m substrings → m inverted lists; by pigeonhole any point within Hamming r has a substring with ≤⌊r/m⌋ mismatches, so enumerate only those variants. **Verified design estimate:** d=256, r=37, m=16 → ~2.2K substring probes vs ~210K trie nodes (~100× fewer), exact, sublinear. *The single biggest fix. Keep the DAFSA as identity/postings + hybrid gate; let multi-index hashing be the vector-NN engine.*
2. **Use ITQ (Iterative Quantization, Gong & Lazebnik 2011)** instead of PCA-sign for #2 — the correct "whitened PCA" that keeps importance-ordering *and* restores near-isotropic geometry (so the angle↔Hamming math holds). Or, if you want clean math with zero training, keep isotropic SRP and **drop the "importance-ordered" claim entirely**.
3. **Reframe the DAFSA premise** (fix line 20 and the "good home" argument): the DAFSA contributes **integration** (shared sym-id + hybrid composition + one store), *not* compression or spatial structure.
4. **Reframe #1 as a bounded candidate-budget (beam) walk** (expand best-first until B candidates or a distance ceiling, then re-rank) rather than "exact top-k" — restores sublinear-ish cost, drops the misleading "exact" label.
5. **Drop the LSH forest (Flavor B) as the default** — multi-index hashing gives exact-Hamming with one table; a beam walk's B is cheaper to raise than K× tables. Mark the forest "optional."
6. SIMD/BLAS the re-rank and make float retention explicit (≤~100K entities, a SIMD exact scan beats the whole candidate-gen pipeline).
7. **Center the data before PCA** (hyperplanes through the mean, not the origin).

### Build-or-not

- **At ~2K entities: do NOT build.** Exact cosine scan = N·D = 768K multiply-adds ≈ **<1ms scalar C**, ~10–50μs SIMD/BLAS. Exact (zero recall loss), zero tuning (no d/r/K), zero training, zero stale-basis risk. The bit-index is strictly worse on every axis that matters here.
- **Threshold:** ~**10⁵–10⁶ entities** (or high sustained QPS, or memory-bounded quantized storage). Even then multi-index hashing / HNSW / FAISS dominate. Reserve the DAFSA bit-signature for the specific case where you need hybrid symbolic+vector composed in one automaton **at scale**.
- **For vector capability now:** ship the semantic sidecar (`.npy` + exact cosine). At 2K it already covers `search_semantic` with zero new engine code. The hybrid symbolic+vector use case at 2K is already served by the hybrid-search doc's lexical-gate pattern (DAFSA regex filter → exact cosine over the small gated subset), which needs **no vector index**.
- **Conflict to resolve:** semantic-sidecar.md's "no vector code in C" contradicts this doc's `src/lsh.c`. Reconcile by keeping the bit-index as scale-only/off-path (as this doc now states).

## The construction

### 1. Encoding: vector → bit-string

- Choose `d` random hyperplanes through the origin (fixed, persisted).
- For each embedding `v`, bit `i` = sign of `v·h_i`. Result: a `d`-bit string.
- **Key fact:** for two vectors at angle `θ`, the probability any single bit **agrees** is `1 − θ/π`. So cosine similarity → Hamming distance on the signatures (angle ≈ Hamming).

### 2. Indexing: bit-strings in a DAFSA/trie

- Each bit-string is a fixed-length string. Insert into a DAFSA/trie (over the interner, `src/intern.h`), or a dedicated signature DAFSA.
- Near-identical vectors share long signature prefixes → the DAFSA's prefix-sharing and prefix-enumeration directly mirror "similar vectors cluster in the tree." *(⚠ Corrected in the Critique: the DAFSA contributes **integration** — shared sym-id, hybrid composition, one store — **not** compression; minimization is inert on fixed-length suffix-distinct bit-strings. See Critique #4.)*

### 3. Search: bounded-Hamming automaton walk (multi-probe, Flavor A)

- Descend the query's signature **and** explore the DAFSA neighborhood — signatures within **Hamming radius `r`** of the query.
- This is a **mismatch-tolerant automaton walk over the bit-strings** — the exact primitive the engine already has (the regex/automaton walk, re-pointed per `docs/datalog-dafsa-regex-on-symbols.md`). Hamming-neighborhood search *is* a wildcard/bounded-mismatch regex over bit-strings. *(⚠ Corrected in the Critique: a best-first Hamming walk is a **new** priority-queue Dijkstra primitive, NOT `regex_dfa_walk` (DFS+visited-set), and "≤ r mismatches" is **not** expressible in the supported regex subset — see Critique #5.)*
- Collect all buckets within `r` bits → the candidate set.

**Recommended refinement: best-first Hamming walk instead of a fixed-radius ball** (see Improvements, #1).

## Why it is "approximate" — the two layers

1. **Lossy encoding:** a `d`-bit signature is a coarse, noisy estimate of direction (magnitude discarded — fine for cosine). Genuinely-different vectors can collide; similar ones can diverge.
2. **Bucketed retrieval:** LSH returns a *candidate set* that *usually* contains the true neighbors — with probabilistic false negatives and false positives. Not exhaustive, not exact.

## Why single-tree lookup fails (the concrete numbers)

Take two 0.9-cosine neighbors (angle `θ ≈ 0.45` rad). Per-bit agreement `≈ 1 − 0.45/π ≈ 0.857`. At `d = 256` bits the expected Hamming distance between them is:

```
256 × (1 − 0.857) ≈ 36 bits
```

- **r = 0** (exact match): almost no genuine neighbors share an identical 256-bit signature → terrible recall.
- **r = 36:** you'd catch the neighbor, but the number of signatures within Hamming 36 of any point in 256-dim bit space is astronomically large → no speedup.

Single-table lookup is caught between no-recall and no-speedup. Multi-probe fixes it.

## Multi-probe: the two flavors

**Flavor A — bounded-Hamming walk over the DAFSA (probe the neighborhood, not just the exact hash).**
Walk the DAFSA collecting all signatures within `r` bits of the query via a mismatch-tolerant automaton. This is exactly what the DAFSA's automaton-walk does — and why a DAFSA is a *good* home (not just any hash table). This is the "multi-probe LSH" idea, naturally expressed as a DAFSA walk.

**Flavor B — LSH forest (K independent tables).**
Use K independent random-hyperplane sets → K signatures per vector → K separate DAFSAs/tries. Query all K (exact or small radius), **union** the candidates. Recall:

```
recall ≈ 1 − (1 − p1)^K
```

With K = 5–10, recall → high while each lookup stays fast. "Repetition over noise": K independent noisy votes, true neighbors win the majority.

**Note on flavors:** the improvements below are **upgrades to Flavor A** (the DAFSA walk). Flavor B (the forest) stays **orthogonal** — a recall booster you stack on top of the best-first walk, not a replacement for it. The forest multiplies independent noisy votes; the best-first walk makes each vote exact-in-Hamming.

## Improvements

> ⚠ **Corrected by the Critique & corrections section above.** Read it first. #1 (best-first exact-Hamming walk) is correct but *slower than a scan*; #2 (whitened PCA) *breaks the LSH math*. The corrected approach is **multi-index hashing** (fixes #1) and **ITQ** (fixes #2). The subsections below record the original reasoning for context.

Four upgrades, all specific to having a DAFSA as the home (not generic ANN tweaks). Ranked by value.

### #1 — Best-first Hamming walk: exact top-k, no radius knob ★ the big one

Replace "enumerate everything within radius `r`" (combinatorial, needs tuning) with a **best-first / priority-queue walk** over the DAFSA returning the **top-k signatures by Hamming distance directly**:

- Priority = the query's **current Hamming distance** at each node.
- Prune any node whose current distance already exceeds the running `k`-th best.

**Safe/exact** because Hamming distance is **monotone non-decreasing along a path** — a subtree's signatures only get farther from the query, never closer. So a node past the top-k boundary kills its whole subtree.

Buys:
- **Exact top-k in Hamming space** — the search is no longer approximate; *only the encoding* (cosine→Hamming) is lossy. Cleaner "approximate" story.
- **Density-aware, no radius parameter** — the walk goes deeper in dense regions, stops early in sparse ones. Subsumes the `r` knob.
- Faster than radius enumeration — never materializes a combinatorial Hamming ball.

### #2 — Whitened (PCA) projections instead of random hyperplanes ★ most elegant

Random projections are isotropic noise, and — the catch for a DAFSA — **random projections don't cluster, so they don't share prefixes.** Whitening changes that:

- PCA the embeddings, project onto the top `d` principal axes (whitened), *then* binarize signs.
- Signature becomes **ordered by importance**: bit 0 = highest-variance direction, bit 1 = next, etc.
- Two similar vectors agree on the *early* (dominant) bits first → **shared DAFSA prefix = semantic closeness on the principal axes.** The DAFSA's prefix-depth becomes meaningful, not coincidental.

This strengthens the whole "indexed in a DAFSA" premise: the prefix structure is the *semantic hierarchy*, not a happy accident.
**Cost:** a training pass over the embeddings to learn the PCA basis (fine for a static-ish graph); **caveat:** if embeddings drift, the basis goes stale — re-fit on rebuild.

### #3 — Coarse-to-fine via the DAFSA's own prefix structure

Instead of one fixed `d`, exploit that a DAFSA natively does prefix queries:
- Use the **short prefix** (first ~32 bits = whitened dominant axes) for fast, coarse bucketing.
- Refine with the full signature only for surviving candidates.

"Short signature for cheap rejection, long for exactness," and the DAFSA's prefix-enum *is* the coarse-to-fine mechanism — no separate multi-resolution index. Compounds #2 (prefix is only meaningful if bits are importance-ordered).

### #4 — Unify the symbolic and vector walks in ONE DAFSA (the unique one)

No off-the-shelf vector index can do this, and it's why "in a DAFSA" is more than a gimmick:

- The signature DAFSA and the symbol DAFSA are **the same store, same sym-ids.**
- Run a walk that intersects **both** a symbolic constraint and a Hamming neighborhood — e.g. *"entities whose name matches `^tech` AND whose vector is near X"* — in a single DAFSA traversal.

The symbolic gate (`dl_pattern_symbols`, hybrid-search doc) and the Hamming walk compose *inside* the same automaton — **hybrid filtering at the index level, not post-hoc.**

### Honest ranking / caveats

- **Most valuable: #1** — de-approximates the search, kills the tuning knob. Build first.
- **Most elegant: #2** — makes "indexed in a DAFSA" actually mean something. Needs a training pass (not free).
- **#3** is free leverage on #2; **#4** is the differentiator but only matters for hybrid (symbolic+vector) queries.
- **Unchanged:** the encoding is still approximate (cosine→Hamming is lossy regardless); you still re-rank survivors by exact cosine. #1 only makes the *candidate* search exact-in-Hamming, not the final cosine ranking.

**One-line upgrade:** *replace the radius-ball with a best-first monotone Hamming walk, and feed it whitened, importance-ordered signatures — then the DAFSA prefix is the semantic hierarchy, and the search is exact-in-Hamming.*

## The end-to-end shape (honest)

1. **DAFSA stage (fast, approximate):** candidates = union of bounded-Hamming walks across K tables. High recall, meh precision, small set.
2. **Re-rank stage (precise):** exact cosine over the candidate set only (a few hundred) → exact ranking over an approximate-candidate set.

This mirrors every practical ANN system (HNSW returns candidates that get re-scored). The three knobs — bit length `d`, radius `r`, forest count `K` — trade recall vs speed; the DAFSA walk keeps multi-probe cheap instead of exponential.

## Files that change (when implemented)

| File | Change |
|---|---|
| `docs/datalog-dafsa-bit-signature-index.md` | this doc; link from hybrid-search doc |
| `src/lsh.c` (new) | random-hyperplane generation (fixed seed); vector→bit-string encode; `dl_vector_index` build; `dl_vector_search` (bounded-Hamming automaton walk across forest + union) |
| `src/dl.h` / `src/dl.c` | `dl_vector_index`, `dl_vector_search`, signature relation wiring |
| `src/dl_cli.c` | `dl vindex <csv-of-vectors>`, `dl vsearch <vector> [--radius] [--tables]` |
| `tests/` | `test_lsh.c`: encoding determinism, cosine-vs-Hamming correlation, recall at fixed `(d,r,K)`, re-rank correctness, false-negative/positive behavior |

## Honest ceiling

- **Approximate by design.** Recall is probabilistic (governed by `d`, `r`, `K`); exact ranking only restored in the re-rank step. Not an exact kNN index.
- The DAFSA contributes **symbolic-prefix structure + a cheap automaton multi-probe** — not approximate-search math. The LSH math (hyperplanes, Hamming, forest) is the added machinery.
- If the memory graph is only ~2K entities, exact cosine scan (O(N×D) ≈ 800K ops) is already trivial — the bit-signature index is only worth building for **scale or to unify the index machinery**, not to beat an exact scan at this size.

## Concrete next slice (if pursued — SCALE-ONLY, see Critique)

**Do not build at ~2K entities** (exact cosine scan dominates). If this is pursued at scale (~10⁵–10⁶ entities), the corrected path is:

1. Isotropic random hyperplanes (or ITQ if a training pass is acceptable) → vector→bit-string encode.
2. **Multi-index hashing** (split signature into m substrings, m inverted lists) as the vector-NN engine — NOT the exact-Hamming trie walk.
3. DAFSA retained as identity/postings layer + hybrid symbolic gate (not as the compression/NN structure).
4. Exact-cosine re-rank over candidates (SIMD/BLAS); retain float vectors in the `.npy` sidecar.
5. Hybrid symbolic+vector composition only if the single-store-automaton composition is a hard requirement at scale.

## References

- `docs/datalog-dafsa-hybrid-search.md` (this is the vector-tier half).
- `docs/datalog-dafsa-regex-on-symbols.md` (the automaton-walk primitive to reuse for Hamming-neighborhood search).
- `src/intern.h` (shared symbol space for signatures).
- Established ideas: random-projection LSH / simhash (Charikar), LSH forest, multi-probe LSH, HNSW (as the re-rank comparison).
