# dlb — Measured Envelope

All numbers as measured on this host (openSUSE MicroOS, Python 3.13.14,
libdatalog.so = Zig release build `zig-out/lib/libdatalog.so`, ABI 1), by
`tests/bench_envelope.py` unless noted. **Numbers that miss the ≲10 µs
per-step budget are reported, not tuned away.**

## Budget verdict up front

| operation (per-step path) | median | ≲10 µs budget |
|---|---|---|
| lookup (hit or miss) | 3.4–3.6 µs | PASS |
| intern_find | 1.5 µs | PASS |
| count | 1.6 µs | PASS |
| rank | 3.7 µs | PASS |
| select | 8.0 µs | PASS (barely) |
| prefix, 1 row + Python cb | 6.6 µs | PASS |
| iter open+scan+close, 1 row | 8.5 µs | PASS |
| **iter full scan** | 1.97 µs/tuple | per-tuple PASS; a 1 000-row scan = ~2 ms — **NOT a per-step op** |
| query_magic (chain 40, bound) | 0.74 ms | **MISS by 74×** — off per-step path |
| query_rules_ro (chain 40) | 1.8 ms | **MISS by 180×** |
| query_rules_ro (chain 1000) | 1.23 s | **MISS by 123 000×** |
| vector_search (production obs index, r=8) | 34 ms | **MISS** — semantic recall, not per-step |

Point reads on an open handle are comfortably inside the budget (the brief's
1.6 µs raw-ctypes figure becomes ~3.5 µs through the checked binding — the
price of argtypes enforcement and the API wrapper). **Anything recursive or
vector-shaped is milliseconds-to-seconds and must be amortized (compile once
+ bound queries) or moved off the control-loop step.**

## 1. Open cost vs store size (open_ro; handle held afterwards)

| store | size | open_ro |
|---|---|---|
| empty | 0 MB | 0.12–0.14 ms |
| 1 000 edges | 0.02 MB | 0.37–0.40 ms |
| 10 000 edges | 0.21 MB | 3.3 ms |
| 100 000 edges | 2.0 MB | 35–37 ms |
| **~/.config/hax/memory.dl (production)** | **678 MB** | **645–662 ms** |

Roughly linear in store size, ~0.35 ms per MB dominated by DAFSA +
interner load; ~650 ms for the production store confirms the brief's 642 ms.
**Open once per session.** Re-opening per step is impossible at this cost.

RW open of an empty dir: 0.2–0.3 ms (mkdir + LOCK create).

## 2. Per-call latency (handle held open; 10 000-edge chain store)

| op | median | p10 | p90 |
|---|---|---|---|
| lookup hit | 3.45 µs | 3.43 | 3.47 |
| lookup miss | 3.40 µs | 3.38 | 3.42 |
| prefix (1 row, cb) | 6.55 µs | 6.51 | 6.59 |
| intern_find (miss) | 1.47 µs | 1.46 | 1.49 |
| count | 1.58 µs | 1.56 | 1.59 |
| rank | 3.66 µs | 3.63 | 3.69 |
| select (k=500) | 7.99 µs | 7.96 | 8.05 |
| iter open+scan+close (1 row) | 8.51 µs | 8.47 | 8.56 |
| iter full scan | 19.7 ms total | — | 1.97 µs/tuple |

## 3. Small recursive query — the expensive case

Program: `tc(X,Y) :- edge(X,Y).  tc(X,Y) :- edge(X,Z), tc(Z,Y).` over a
chain of n edges (worst case: n·(n+1)/2 derived tuples).

| n | derived tuples | query_rules_ro (parse+compile+eval, throwaway) | compiled dl_query (re-eval) |
|---|---|---|---|
| 40 | 820 | 1.82 ms | 1.81 ms |
| 100 | 5 050 | 10.8 ms | — |
| 1 000 | 500 500 | 1.23 s | 1.27 s |
| 5 000 | 12 502 500 | 35.9 s | — |

One-time `load_rules`+`compile_rules`: 0.74 ms (n=40), 444 ms (n=1000).

Bound/scoped paths after a one-time compile (chain 40):

| path | median |
|---|---|
| query_bound("tc",(1,)) | 66 µs |
| query_magic("tc",(1,)) | 0.74 ms |

Chain 1 000: query_bound 1.6 ms; query_magic 306 ms.

**Reading:** unbounded transitive closure is quadratic-plus in derived tuples
— per-step budget only survives with (a) pre-compiled rules + bound queries
(66 µs at chain 40 — still 6.6× over budget), or (b) magic-sets scoped to a
seed. The agent's control loop must treat recursive queries as amortized
work, not per-step reads.

## 4. Concurrency envelope (MEASURED, contradicts the naive reading)

- **Cross-process:** `dl_open_ro` from ANOTHER process returns `DL_E_LOCKED`
  while a writer holds the handle, and a second writer is likewise locked
  out. **Readers and the writer never coexist across processes** — the LOCK
  fcntl excludes in both directions. "Reader sees a consistent snapshot
  across a concurrent writer commit" is therefore **structurally vacuous**:
  the consistency point is open time (WAL replayed at open). The dl.h
  sentence "use dl_open_ro from other processes alongside one writer"
  describes the RO-open failure mode, not coexistence.
- **Same process:** a second `dl_open` of the same directory SUCCEEDS at the
  C level (fcntl locks never conflict within one process — measured). The
  binding enforces the single-writer invariant itself: a per-process
  registry refuses a second handle for the same directory (raises
  `DlLockedError` in ~22 µs). This also implements the dl.h same-process
  caveat (never RO+RW together in one process).
- **After writer close / crash:** an RO open sees everything the WAL
  contains — a child that `add_fact`s and `_exit(0)`s without close is fully
  visible to the next reader (verified: fact present, count correct).
- **DL_E_LOCKED surfacing:** cross-process `dl_open_ro` under a live writer
  raises `DlLockedError`; `dl_open2` under another writer raises
  `DlLockedError`; CAS/txn conflicts raise `DlConflictError` with nothing
  applied (verified by test).

## 5. Thread safety (one handle, many Python threads)

- API layer (RLock-serialized): 8 threads × 20 000 lookups = 160 000 ops,
  0 errors, 94 677 ops/s aggregate (~10.6 µs/op under contention).
- RAW `dl_lookup` with no lock, **per-thread buffers**: 160 000 ops, 0
  errors observed, 250 808 ops/s (~4 µs/op).
  (An earlier run with a SHARED cols buffer showed 6 "misses" — that was a
  data race in the bench, not the library; corrected.)
- The engine makes no documented thread-safety promise; concurrent raw
  reads showing no errors is evidence, not a contract. **The API layer
  serializes handle use with an RLock; single-threaded use is the contract.**
  A lock-free reader would need an engine-side guarantee we do not have.

## 6. Vector tier (production memory store, OBSERVATION_CONTENT corpus)

The production store (~/.config/hax/memory.dl, 37 679 observations) indexes
observation CONTENT (`__obssig0..15__`, `__vec_obs__`, `__itq_basis__`),
not entity names — the entity-corpus `dl_vector_search` returns -1 there;
the binding exposes the corpus-parameterized forms.

| step | cost |
|---|---|
| `dl_embed_encode_query` (libembed.so, bge-small, warm) | 27–33 ms |
| vector_search r=8 | 34 ms |
| vector_search r=16 | 457 ms |
| vector_search r=24 | 379 ms |
| vector_search r=32 | 1.13 s |
| vector_search r=48 | 2.19 s |
| vector_search_version (snapshot 148) r=8 / 16 / 24 | 47 / 533–562 / 525 ms |
| vector_rerank (8 candidates) | 255 µs |

**Reading:** radius is THE cost knob (probe count grows as C(r,16)·16 per
band); r=8 is ~65× cheaper than r=48. Encode is a fixed ~30 ms. Semantic
search is a recall tool at tens-of-ms minimum — never a per-step op.

## 7. Canonical .so decision

**Canonical: `~/fixpoint-linux/datalog-dafsa/zig-out/lib/libdatalog.so`**
(Zig release build; the repo README names `zig build -Drelease` as the
canonical build; 6.7 MB, exports all 86 `dl_*` symbols used).

- The 23 MB `libdatalog.so` at the repo root is the gcc-built ABI reference
  (with debug info); loads fine, same ABI — kept as the abi_audit reference,
  not shipped.
- `~/.config/hax/bin/libdatalog.so` (412 KB) is a STALE pre-migration C
  build (Aug 29): it lacks `dl_snapshot_relations`, `dl_colspec_eq` and
  other newer exports. **Do not bind against it**; the binding's resolution
  order puts zig-out first and `$DLBLIB` can override.
- **ABI check:** `dafsa_abi_version() == 1` is the only versioning hook; the
  binding verifies it at load and refuses mismatched libraries. Beyond that,
  the repo's `zig/abi_audit.sh` compares dynamic exports against the gcc
  reference and the headers — run it when the engine is rebuilt.

## 8. Answers to the brief's five questions

1. **Canonical .so / ABI versioning:** zig-out release build (above);
   `dafsa_abi_version()` is the only ABI gate (returns 1 everywhere) —
   no symbol-versioning beyond it; abi_audit.sh for export completeness.
2. **Does `dl_open_ro` see committed writes without reopening?** NO. A held
   RO handle reads its open-time state; and it cannot even be held while
   the writer is open (mutual exclusion both directions). Visibility of a
   commit = close the writer, then (re)open the reader.
3. **Cost of a small recursive query:** see §3 — 1.8 ms at chain 40
   (query_rules_ro), 66 µs bound-query after one-time compile, growing
   quadratically with derived tuples; far above 10 µs except nothing here
   is a per-step op.
4. **Is dl-embed needed at runtime?** For QUERY encoding: yes, one
   `libembed.so` call (`dl_embed_encode_query`, ~30 ms warm) produces
   (q_sig, q_int8) — then `dl_vector_search`+`dl_vector_rerank` run against
   the pre-built in-store index. The agent does NOT need the `dl-embed`
   CLI; the in-process encoder + the corpus-parameterized query path
   suffice. For INDEXING new content (offline, ITQ fit + bulk emit), the
   dl-embed/embed.py pipeline remains the tool.
5. **Own directory vs shared memory store:** they can share the same store
   FORMAT, but not the same directory concurrently: the memory store's
   writer (fx-agent-memory) excludes all other handles while open. If the
   agent needs always-available reads, it should keep its OWN store
   directory (its interned vocabulary + checked definitions), opening the
   memory store RO only when the memory writer is closed (or via published
   snapshot versions + `dl_query_bound_version`, which still requires the
   RO open). Recommended: agent's own directory, per the brief.

## Negatives (explicit)

- Recursive queries (unscoped) blow the per-step budget by 10²–10⁵×.
- vector_search r≥16 costs 0.4–2.2 s on the production index; only r=8
  (~34 ms) is even in the tens-of-ms class.
- `dl_open_ro` on the production store costs ~650 ms — every handle
  acquisition, not just the first, after a writer cycle.
- No coexistence of reader and writer, cross-process (single big lock) —
  the "many readers alongside one writer" pattern is NOT available with
  this engine; readers must wait for writer close.
- Same-process double-open is NOT prevented by the C library (binding
  registry does it).
- Thread safety of raw concurrent reads is undocumented in the engine;
  observed fine, but the API serializes anyway.
- `select` (8.0 µs) sits near the 10 µs line; `query_bound` on a 40-node
  closure is 6.6× over.
- The stale `~/.config/hax/bin/libdatalog.so` will silently work for the
  common symbols but is missing newer ones — a provenance trap the canonical
  decision closes.

## How to reproduce

```sh
cd ~/thing/dlb
python3 tests/test_binding.py          # 42 correctness tests
python3 tests/bench_envelope.py        # this envelope (~6 min: the 5000-chain
                                       # recursive query dominates the wall time)
```
