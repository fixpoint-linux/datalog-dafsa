# datalog-dafsa

A DAFSA-backed Datalog engine in Zig: load facts into an on-disk minimal-acyclic-DAFSA
fact store, compile Datalog rules to a small VM, materialize derived relations, and
serve reads from an mmap'd snapshot. M0–M9 + v2 IVM complete (306 test cases green).

## Quickstart

```sh
zig build -Drelease -p zig-out --build-file zig/build.zig   # build libdatalog.so + dl CLI
bash zig/run_zig_suites.sh                                   # run the 37 C test suites + smoke (oracle)
```

Use the `dl` CLI to load facts and query them:

```sh
# Load a headerless CSV (arity 1-8) into a relation, then query it.
./dl -d /tmp/db load edges.csv --rel edge
./dl -d /tmp/db lookup edge 1 2        # exact lookup
./dl -d /tmp/db prefix edge 1          # prefix enumeration (bind leading cols)
./dl -d /tmp/db bound edge 1 2         # bound query (snapshot path)
./dl -d /tmp/db pattern edge '(a|b).*' # regex pattern query

# Run a Datalog rule (compile + publish + query in one step).
./dl -d /tmp/db query 'tc(X,Y) :- edge(X,Y). tc(X,Y) :- edge(X,Z), tc(Z,Y).' tc

# Publish a snapshot for read-only serving.
./dl -d /tmp/db publish
```

CSV values that parse as integers are stored raw as u32; anything else is interned
to a symbol id. The database directory defaults to `dl-test-db` and can be overridden
with `-d <dir>`.

### Semantic search — REMOVED (2026-08-11)

> **The vector/embedding tier is gone.** `vsearch`, `vhybrid`, `dl_vector_*`, the
> `VEC_*` layout constants, the `dl-embed` encoder lookup and the whole
> `zig/src/vector.zig` module were deleted. **Reason:** agents author the handoff
> data and query it with the SAME words, so retrieval is lexical (the
> `docs/datalog-dafsa-search-stack.md` design had a semantic half for consumers
> that never needed one). Embeddings did not earn their machinery — an external
> encoder, a sidecar contract, 96 packed chunks per entity and a second query path
> — when the words being searched for are the words that were written.

Retrieval is the full-text index only — AND-intersect plus rank, version-aware:

```sh
./dl -d /tmp/db search 'gpu rental' --top 10
```

**What stays on disk:** the store data. `__vec_q__`, `__vec_obs__`, `__obssig*__`,
`__itq_basis__` and the `.npy` sidecars are the user's data and were NOT deleted —
the engine simply no longer reads them. An existing database opens and queries
exactly as before; the vector relations are just inert rows.

The design record is kept (not rewritten) in
[`docs/datalog-dafsa-vector-search.md`](docs/datalog-dafsa-vector-search.md) and its
companions, each carrying the REMOVED banner and the reason above.

## How It Works

**The linchpin: fixed-width big-endian encoding.** Every fact of arity `a` is encoded
as one fixed-width key of `4*a` bytes — each column a u32 in big-endian byte order, laid
out in column order with no inter-column separator (plus a trailing `\0` guard). Because
the columns sit at known byte offsets, *"bind the first `k` columns to constants and
enumerate the rest"* becomes exactly a byte-prefix lookup on the relation's DAFSA. The
DAFSA's two strongest primitives — exact-key lookup and prefix enumeration — are precisely
the two most common join access patterns. That is the whole storage thesis.

**One DAFSA per relation.** Each relation `R` of arity `a_R` has its own DAFSA (plus its
own WAL). Per-relation DAFSAs preserve prefix-enum selectivity, isolate compaction, and
keep schema/arity/permutation-index metadata clean. A separate symbol DAFSA maps interned
strings to u32 symbol ids (forward `str→sym` DAFSA + reverse `sym→str` array).

**Prefix enumeration = index-nested-loop join.** The VM implements joins as index-nested-loop:
for each tuple, bind the shared leading columns to constants and prefix-enumerate the next
relation (via `relation.c`'s prefix walker, which DFS-traverses the DAFSA from the prefix
state). Non-leading-column joins are handled by per-relation **permutation indices** — a
permuted DAFSA for every column-prefix the compiler sees used as a join key — plus a
hash-join fallback for the rest. `min`/`max` aggregates fall out of the big-endian encoding
(extreme prefix = extreme key).

**Lifecycle: load → compile → publish → serve-reads.** You load facts into a database
directory (`dl_load_facts` or incremental `dl_add_fact`), declare relations, compile rules
(`dl_load_rules` + `dl_compile`, running the semi-naive fixpoint VM), then atomically
publish a versioned snapshot (`dl_publish_snapshot`: save interner + relations, flip the
`CURRENT` pointer). After publish, `dl_query`/`dl_query_bound`/`dl_pattern` read from mmap'd
read-only views instead of running the VM. Single writer, multiple readers (mmap RO).

**Durability.** `dl_add_fact`/`dl_delete_fact` append to a per-relation WAL; the WAL is fsync'd at
cycle boundaries (`dl_publish_snapshot` / `dl_close` / WAL compaction), not per fact — an unclean
stop loses the current cycle's appends, everything before the last boundary replays at open. A
`fcntl` single-writer lock guards the database (a second *writer* is refused with
`DL_E_LOCKED`); a read-only handle (`dl_open_ro`) coexists with a live writer and sees its
committed state as of the reader's own open — the lock is split into two byte-range scopes,
writer exclusivity and a short mutation window (see `src/dl.h`, CONCURRENCY MODEL). The interner
is saved durably (and ordered *before* WAL records so crash recovery can decode symbol ids).
WAL compaction triggers at 25% of the relation size.

## Python bindings — `python/`

[`python/dlb`](python/README.md) is an in-process Python binding over
`libdatalog.so` (stdlib `ctypes` only, no third-party dependency). It lives here
because it is **1:1 with the engine's C ABI** (`dafsa_abi_version()`, ABI 1,
checked at every library load), so an ABI change and its Python mirror land in one
commit.

```sh
python3 python/tests/test_binding.py     # 37 correctness tests (needs libdatalog.so)
python3 python/tests/bench_envelope.py   # the measured latency/concurrency envelope
```

`python/ENVELOPE.md` is the recorded envelope — the load-bearing deliverable, with
numbers that miss the ≲10 µs per-step budget reported rather than tuned away.
Library resolution: `$DLBLIB` → `zig-out/lib/libdatalog.so` in this repo → the
developer's checkout → the loader's default path. Put `<repo>/python` on
`sys.path` to `import dlb`.

## API Summary

Public C API in [`src/dl.h`](src/dl.h). All value arrays are u32 (raw ints or symbol ids).

- **Lifecycle:** `dl_open`, `dl_open2`, `dl_close`
- **Schema:** `dl_declare_relation` (arity 1–8)
- **Facts:** `dl_load_facts` (CSV bulk load), `dl_add_fact`, `dl_delete_fact`
- **Queries:** `dl_lookup`, `dl_prefix`, `dl_query`, `dl_query_bound`, `dl_pattern`
  (callbacks of type `dl_tuple_cb`)
- **Rules:** `dl_load_rules`, `dl_compile`
- **CAS / transactions:** `dl_cas_revision`, `dl_rev_get`, `dl_txn_begin`/`dl_txn_cas`/
  `dl_txn_add_fact`/`dl_txn_delete_fact`/`dl_txn_commit`/`dl_txn_rollback`
- **Snapshot:** `dl_publish_snapshot`
- **Fault hooks (test-only):** `dl_set_fault_hook`
- **Interner:** `dl_intern_str`, `dl_intern_str_of`

## Milestones

| Milestone | Scope | Tests |
|-----------|-------|-------|
| M0 | Fact store + interner (fixed-width u32-BE encoding, exact + prefix) | 9 |
| M1 | Rule parser, compiler, non-recursive VM | 18 |
| M2 | Semi-naive fixpoint + stratified negation | 18 |
| M3 | Aggregates (count/sum/min/max) + equality + disjunction | 17 |
| M4 | Snapshot publish + mmap query path | 8 (+16 review) |
| M5 | Regex/pattern walker (`dl_pattern`, `OP_WALK`) | 25 (+60 review) |
| M6 | Permutation indices + hash-join | 8 (+5 review +10 deep) |
| M7 | Durability (lock, WAL, incremental add/delete) | 14 |
| bulk | Bulk DAFSA construction | 16 |
| M9 | Arithmetic + comparison builtins (`< <= > >= !=`, `X = E`) | 11 |
| M9-s | String builtins (concat, length, lower/upper, prefix/suffix/contains) | 9 |
| IVM | Incremental view maintenance (v2, Slices 0-5) | 25 |
| CAS | Optimistic concurrency (`rev()`, `dl_cas_revision`, `dl_txn_*` API) | 7 |

Total: 313 test cases across 18 test binaries + a CLI smoke test.

## Design Docs

- [`design/datalog-dafsa-architecture.md`](design/datalog-dafsa-architecture.md) — the
  storage thesis, encoding linchpin (§0–§1), and system design.
- [`design/datalog-dafsa-implementation-plan.md`](design/datalog-dafsa-implementation-plan.md)
  — milestone-by-milestone plan with implementation status.
- [`design/datalog-dafsa-dhall-schema.md`](design/datalog-dafsa-dhall-schema.md) — design
  of record for the `dlp` typed database-project tool, **NOT SHIPPED** (the tool was
  removed; kept for the record).
- [`docs/datalog-dafsa-cas.md`](docs/datalog-dafsa-cas.md) — CAS / optimistic-concurrency
  transactions for the write path (`rev()`, `dl_cas_revision`, the `dl_txn_*` API, txn-WAL
  crash recovery).
- [`docs/language-reference.md`](docs/language-reference.md) — the complete rule-language
  reference: facts, rules, recursion, negation, aggregates, string builtins, lists, range,
  regex patterns, variadic relations and the compile-error semantics.

## Building from Source

The engine is built with Zig (canonical; see [zig/build.zig](zig/build.zig)):

```sh
zig build -Drelease -p zig-out --build-file zig/build.zig   # libdatalog.so + dl CLI
```

Zig is the only build system: there is no Makefile and no C/C++ build of the
engine (the C++ embed/ggml sidecar was removed). The C test suites (the
behavioral oracle) re-link against the Zig-built `libdatalog.so` via
`bash zig/run_zig_suites.sh` (needs `gcc` and POSIX headers,
`-D_POSIX_C_SOURCE=200809L`, targeting `c11`). No third-party dependencies for
the engine itself — the DAFSA engine is vendored under `vendor/`.

## License

MIT. See [LICENSE](LICENSE).
