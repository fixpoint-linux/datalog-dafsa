# datalog-dafsa

A DAFSA-backed Datalog engine in C: load facts into an on-disk minimal-acyclic-DAFSA
fact store, compile Datalog rules to a small VM, materialize derived relations, and
serve reads from an mmap'd snapshot. M0–M9 + v2 IVM complete (306 test cases green).

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

## dlp — typed database projects

[`dlp`](design/datalog-dafsa-dhall-schema.md) is a typed database-project workflow
layered on the engine. A `schema.dhall` file defines the typed relations
(typechecked + normalized in-process by the dhall-c interpreter); data files are
validated against it on load; and `.datalog` rules are typechecked against the
schema *before* compilation. It catches int/text mixing at authoring time — e.g.
`tc(A,W):-weight(A,W).` is rejected because `weight.w` is Natural while `tc.dst` is
Text.

```text
mydb/
  schema.dhall      # the typed contract (: Schema-annotated)
  data/*.csv|json   # validated on load (file stem = relation)
  rules/*.datalog   # concatenated; typechecked against the schema
  .build/           # dlp-owned: build snapshot, schema.json echo
```

**Schema DSL (Bool-payload union).** The schema is Dhall-as-data — self-contained
`let`s with a `: Schema` annotation. `Natural` = raw u32, `Text` = interned symbol:

```dhall
let ColumnType = < Natural : Bool | Text : Bool >
let Column = { name : Text, type : ColumnType }
let Relation = { name : Text, columns : List Column }
let Schema = { relations : List Relation }
in { relations =
     [ { name = "node", columns = [ { name = "id", type = < Text = True > } ] } ]
   } : Schema
```

**Commands** (the new `dlp` binary; the low-level `dl` CLI is unchanged):

- `dlp init [dir]` — scaffold a project template (incl. an example).
- `dlp schema [dir]` — dhall-typecheck + normalize the schema.
- `dlp check [dir]` — schema + dry-run data validation + parse/typecheck rules.
  The CI command; **no writes**.
- `dlp build [dir]` — check + declare relations + load+validate data + compile +
  publish a snapshot into `.build/`.
- `dlp query [dir] 'goal'` — build then evaluate a goal and print the rows.

**Data validation.** CSV headers are matched to columns **by name** in any order:
`Text` accepts any cell verbatim, `Natural` must be `^[0-9]+$` ≤ `4294967295`. JSON
is strict: array of objects, keys = column names, JSON number → `Natural`, JSON
string → `Text`; any other value type is an error.

**Closed world.** Rules are closed-world: relations appearing as rule heads are
IDB, so loading data into a rule-defined relation is an error, and undeclared
relations are rejected.

**Build.** `dlp` is a cosmocc binary linking the engine + dhall-c (a sibling repo);
build it with `make dlp DHALLC=../dhall-c`. The default gcc `make` / `make test`
(306 tests) are unaffected.

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
| M8 | Magic-sets (`dl_query_magic` / `dl_query_magic_adorn`) | 44 |
| M9 | Arithmetic + comparison builtins (`< <= > >= !=`, `X = E`) | 11 |
| M9-s | String builtins (concat, length, lower/upper, prefix/suffix/contains) | 9 |
| IVM | Incremental view maintenance (v2, Slices 0-5) | 25 |

Total: 306 test cases across 17 test binaries + a CLI smoke test.

## Documentation site

A static, dependency-free documentation site lives in [`docs/`](docs/): an
overview with quickstart, the full Language Reference, the CLI and C API
references, architecture notes, and feature pages for order-statistics and
time-travel. It is served by GitHub Pages directly from `docs/` on `main` —
see [`docs/README.md`](docs/README.md) for how to enable Pages.

## Design Docs

- [`design/datalog-dafsa-architecture.md`](design/datalog-dafsa-architecture.md) — the
  storage thesis, encoding linchpin (§0–§1), and system design.
- [`design/datalog-dafsa-implementation-plan.md`](design/datalog-dafsa-implementation-plan.md)
  — milestone-by-milestone plan with implementation status.
- [`design/datalog-dafsa-dhall-schema.md`](design/datalog-dafsa-dhall-schema.md) — the
  `dlp` typed database-project workflow (Dhall schema, typed CSV/JSON loaders, rule
  typechecking).

## Building from Source

Requires `gcc`, `make`, and POSIX headers (`-D_POSIX_C_SOURCE=200809L`), targeting
`c11`. No third-party dependencies — the DAFSA engine is vendored under `vendor/`.

- `make` builds `libdatalog.so`, the `dl` CLI, and all test binaries.
- Test and CLI binaries are **statically linked** for portability.
- The build sets `TMPDIR` to `./build-tmp`; the CLI's default database directory is
  `dl-test-db` unless `-d <dir>` is given.

## License

MIT. See [LICENSE](LICENSE).
