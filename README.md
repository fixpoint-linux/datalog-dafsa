# datalog-dafsa

A DAFSA-backed Datalog engine in C: load facts into an on-disk minimal-acyclic-DAFSA
fact store, compile Datalog rules to a small VM, materialize derived relations, and
serve reads from an mmap'd snapshot. M0–M7 complete (224 test cases green).

## Quickstart

```sh
make                 # build libdatalog.so, dl CLI, test binaries
make test            # run the full test suite
make bench           # run the demonstration benchmark
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

**Durability.** `dl_add_fact`/`dl_delete_fact` append to a per-relation WAL and fsync before
committing in memory; a `fcntl` single-writer lock guards the database; the interner is
saved durably (and ordered *before* WAL records so crash recovery can decode symbol ids).
WAL compaction triggers at 25% of the relation size.

## API Summary

Public C API in [`src/dl.h`](src/dl.h). All value arrays are u32 (raw ints or symbol ids).

- **Lifecycle:** `dl_open`, `dl_open2`, `dl_close`
- **Schema:** `dl_declare_relation` (arity 1–8)
- **Facts:** `dl_load_facts` (CSV bulk load), `dl_add_fact`, `dl_delete_fact`
- **Queries:** `dl_lookup`, `dl_prefix`, `dl_query`, `dl_query_bound`, `dl_pattern`
  (callbacks of type `dl_tuple_cb`)
- **Rules:** `dl_load_rules`, `dl_compile`
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

Total: 224 test cases across 13 test binaries + a CLI smoke test.

## Design Docs

- [`design/datalog-dafsa-architecture.md`](design/datalog-dafsa-architecture.md) — the
  storage thesis, encoding linchpin (§0–§1), and system design.
- [`design/datalog-dafsa-implementation-plan.md`](design/datalog-dafsa-implementation-plan.md)
  — milestone-by-milestone plan with implementation status.

## Building from Source

Requires `gcc`, `make`, and POSIX headers (`-D_POSIX_C_SOURCE=200809L`), targeting
`c11`. No third-party dependencies — the DAFSA engine is vendored under `vendor/`.

- `make` builds `libdatalog.so`, the `dl` CLI, and all test binaries.
- Test and CLI binaries are **statically linked** for portability.
- The build sets `TMPDIR` to `./build-tmp`; the CLI's default database directory is
  `dl-test-db` unless `-d <dir>` is given.

## License

MIT. See [LICENSE](LICENSE).
