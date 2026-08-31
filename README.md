# datalog-dafsa

A DAFSA-backed Datalog engine in C: load facts into an on-disk minimal-acyclic-DAFSA
fact store, compile Datalog rules to a small VM, materialize derived relations, and
serve reads from an mmap'd snapshot. M0–M9 + v2 IVM complete (306 test cases green).

## Quickstart

```sh
zig build -Drelease -p zig-out --build-file zig/build.zig   # build libdatalog.so + dl CLI
bash zig/run_zig_suites.sh                                   # run the 42 C test suites (oracle)
```

(The deferred C++ embed/ggml opt-in targets are `make dl-embed` / `make libembed.so` /
`make fetch-model` — see `make help`.)

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

### Semantic search

The vector tier adds meaning-aware retrieval on top of the full-text index, all stored
in-relation and snapshot-versioned:

```sh
# Full-text (lexical) — AND-intersect + rank, version-aware.
./dl -d /tmp/db search 'gpu rental' --top 10

# Semantic vector search — bge-small embedding + MIH retrieval + int8 re-rank.
./dl -d /tmp/db vsearch 'affordable GPU rental' --k 10

# Hybrid — lexical ∩ semantic, then re-rank.
./dl -d /tmp/db vhybrid 'gpu rental' 'affordable GPU rental' --k 10
```

Semantic embedding is provided by the `dl-embed` companion tool (C++, ggml-based):

```sh
make dl-embed          # build ./dl-embed (needs cmake + the vendored ggml submodule)
make fetch-model       # ensure the bge-small GGUF is present under models/
./dl-embed self-test   # golden-embedding gate (cosine >= 0.9999 vs the reference model)
./dl-embed pipeline --db /tmp/db   # embed the entity corpus into the vector index
```

The model (`models/bge-small-en-v1.5-f16.gguf`, ~67 MB) is tracked with **git-lfs**;
a fresh clone materializes it with `git lfs pull` (or `make fetch-model`). `dl-embed`
resolves the model from the repo-local `models/` first, then the per-user cache. See
[`docs/datalog-dafsa-vector-search.md`](docs/datalog-dafsa-vector-search.md) for the
full design (MIH over ITQ, in-store int8 re-rank, snapshot consistency).

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

**Schema DSL (Optional-payload union).** The schema is Dhall-as-data — self-contained
`let`s with a `: Schema` annotation. The column-type universe spans **flat scalars**
(raw u32: `Natural`, `Text`/interned symbol, `Bool`, `Char`, `Date`=yyyymmdd,
`Timestamp`=epoch seconds, `Signed`=i32 zigzag) and **parameterized** types
(`List`/`Optional` of a flat element type, and `Enum` with a fixed value set).
Flat scalars carry an **Optional constraint payload** — `min`/`max` for
Natural/Char/Date/Timestamp/Signed and a `regex` for Text — enforced at data
load (`dlp check`/`build`); `None` (or `{=}` for Bool and List/Optional
elements) means unconstrained:

```dhall
let Elem = < Natural : {=} | Text : {=} | Bool : {=} | Char : {=} |
             Date : {=} | Timestamp : {=} | Signed : {=} >
let NC = { min = None Natural, max = None Natural }   -- unconstrained Natural
let TC = { regex = None Text }                         -- unconstrained Text
let SC = { min = None Integer, max = None Integer }    -- unconstrained Signed
let ColumnType = < Natural : { min : Optional Natural, max : Optional Natural } |
                   Text : { regex : Optional Text } | Bool : {=} |
                   Char : { min : Optional Natural, max : Optional Natural } |
                   Date : { min : Optional Natural, max : Optional Natural } |
                   Timestamp : { min : Optional Natural, max : Optional Natural } |
                   Signed : { min : Optional Integer, max : Optional Integer } |
                   List : { elem : Elem } | Optional : { elem : Elem } |
                   Enum : { values : List Text } >
let Column = { name : Text, type : ColumnType }
let Relation = { name : Text, columns : List Column }
let Schema = { relations : List Relation }
in { relations =
     [ { name = "node", columns = [ { name = "id", type = < Text = TC > },
                                    { name = "score", type = < Natural = { min = Some 0, max = Some 150 } > },
                                    { name = "tags", type = < List = { elem = < Text = {=} > } > },
                                    { name = "nick", type = < Optional = { elem = < Text = {=} > } > },
                                    { name = "color", type = < Enum = { values = [ "red", "green" ] } > } ] } ]
   } : Schema
```

Notes: `Signed` bounds are `Integer` literals and REQUIRE an explicit sign —
`Some -10` / `Some +10` (bare `10` is `Natural`). Text `regex` uses full-key
matching (implicit `^...$` — do not write `^`/`$` anchors).

**Commands** (the new `dlp` binary; the low-level `dl` CLI is unchanged):

- `dlp init [dir]` — scaffold a project template (incl. an example).
- `dlp schema [dir]` — dhall-typecheck + normalize the schema.
- `dlp check [dir]` — schema + dry-run data validation + parse/typecheck rules.
  The CI command; **no writes**.
- `dlp build [dir]` — check + declare relations + load+validate data + compile +
  publish a snapshot into `.build/`.
- `dlp query [dir] 'goal'` — build then evaluate a goal and print the rows.

**Data validation.** CSV headers are matched to columns **by name** in any order;
JSON is strict (array of objects, keys = column names). Coercion is per-type:
`Text`/`Enum` verbatim or interned, `Natural` = `^[0-9]+$` ≤ `4294967295`,
`Bool` = `true/false/0/1` (CSV) or JSON boolean, `Char` = one Unicode codepoint,
`Date` = `yyyy-mm-dd`, `Timestamp` = unix-seconds integer, `Signed` = signed i32,
`List` = JSON array or a bracketed quoted CSV cell `[a,b,c]`, `Optional` = JSON
`null` or empty CSV cell. Mismatched data is rejected with `path:line:col`.

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
| M8 | Magic-sets (`dl_query_magic` / `dl_query_magic_adorn`) | 44 |
| M9 | Arithmetic + comparison builtins (`< <= > >= !=`, `X = E`) | 11 |
| M9-s | String builtins (concat, length, lower/upper, prefix/suffix/contains) | 9 |
| IVM | Incremental view maintenance (v2, Slices 0-5) | 25 |
| CAS | Optimistic concurrency (`rev()`, `dl_cas_revision`, `dl_txn_*` API) | 7 |

Total: 313 test cases across 18 test binaries + a CLI smoke test.

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
- [`docs/datalog-dafsa-cas.md`](docs/datalog-dafsa-cas.md) — CAS / optimistic-concurrency
  transactions for the write path (`rev()`, `dl_cas_revision`, the `dl_txn_*` API, txn-WAL
  crash recovery).

## Building from Source

Requires `gcc`, `make`, and POSIX headers (`-D_POSIX_C_SOURCE=200809L`), targeting
`c11`. No third-party dependencies for the engine itself — the DAFSA engine is
vendored under `vendor/`.

- `make` builds `libdatalog.so`, the `dl` CLI, and all test binaries.
- Test and CLI binaries are **statically linked** for portability.
- The build sets `TMPDIR` to `./build-tmp`; the CLI's default database directory is
  `dl-test-db` unless `-d <dir>` is given.
- The **semantic tier** (`dl-embed`, `dl vsearch`/`vhybrid`) is opt-in and adds a
  vendored [ggml](https://github.com/ggml-org/ggml) submodule (v0.20.2) + `cmake`:
  `git submodule update --init vendor/ggml && make dl-embed`. It is not built by
  the default `make`/`make test`. The model is git-lfs-tracked under `models/`.

## License

MIT. See [LICENSE](LICENSE).
