# Dhall-typed schema & Datalog rule typechecking

_Design note. Status: **implemented** (2026-08-17). S0–S6 landed as a series of
commits (08dd2ef, f284602, 83b3e70, 0eefc34, 02821ed, ff31a1d, 78954b5, d4d8b3e);
this doc now describes the shipped `dlp` tool as built. The gcc core (306 tests)
is untouched — `dlp` is opt-in via `make dlp DHALLC=<path-to-dhall-c>`._

## What landed

S0–S6 are implemented and committed. The shipped `dlp` binary is a new cosmocc build
that links the engine + dhall-c in-process; the gcc `make` / `make test` (306 tests)
are untouched, and `dlp` is opt-in (`make dlp DHALLC=<path-to-dhall-c>`, verified with
`make dlp-golden`). The default gcc build has **not** absorbed `schema.h/.c` or
`typecheck.c` — those live only in the `dlp` target, so the core stays gcc-clean and
portable.

**One documented deviation from this design note (now RESOLVED):** the DSL shipped
with the **`Bool`-payload union value syntax** — `let ColumnType = < Natural : Bool | Text : Bool >`
with union values `< Text = True >` — rather than the `{=}` payload sketched below.
That was the S0 spike finding: dhall-c's typechecker rejected `{=}` as a union-alternative
type payload, so the mechanical `Bool`-payload fallback (identical DSL structure) was
adopted. dhall-c was since fixed (`bae0e08`: the type-position predicate now accepts the
empty record type `{}`), so the `dlp` schema DSL was switched back to the intended
**`{=}` payload**: `let ColumnType = < Natural : {=} | Text : {=} >` with union values
`< Text = {=} >`.

## TL;DR

Add a project workflow (`dlp`, "dl-project") on top of the **untouched embedded
core**: a `schema.dhall` file defines the typed relations, dhall-c typechecks and
normalizes it in-process into a C `dl_schema` contract, typed data loaders
(CSV/JSON) validate rows against it, and a purpose-built C rule typechecker
(`src/typecheck.c`) checks `.datalog` rules against the same schema before the
existing compiler runs. The whole feature is **additive**: the gcc `make test`
build (306 tests) is untouched, and `dlp` lives entirely outside the default build.

**Central insight that keeps this tractable:** IDB relations are *declared* in
`schema.dhall` (closed-world project), so no Soufflé-style cross-rule type
*inference* is needed — the typechecker does per-rule **occurrence-consistency**
checking: every variable in a rule must agree with the schema types of every
column it touches. That is what makes the feature coder-sized instead of
research-grade.

## Motivation / the bug this exists for

Today every value is a `u32` (raw int or interned symbol id); there is **no column
type**. A rule like

```datalog
tc(A, W) :- weight(A, W).   -- weight.w is Natural, tc.dst is Text
```

silently stores a raw u32 `3` next to interned symbol ids in one column and joins
misbehave/no-op. The goal is to catch this at authoring time:

```
$ dlp check
rules/reach.datalog:1:22: variable W is Natural here (weight.w)
                         but Text at rules/reach.datalog:1:10 (tc.dst)
```

## Architecture

```
schema.dhall ──(dhall-c parse + infer_type + normalize, LINKED IN-PROCESS)──▶ dl_schema (C struct)
                                                                            │  the single typed contract
                                        ┌────────────────────────────────────┼─────────────────────────────┐
                                        ▼                                    ▼                             ▼
                              typed data loaders                   rule typechecker                [embedded engine]
                              (CSV/JSON, validate rows)            (src/typecheck.c, per-rule      dl_declare_relation /
                              → dl_add_fact                         occurrence-consistency)        u32 encoding / dl_compile
                                                                   → dl_load_rules
```

Three layers over an **untouched embedded core**; the embedded C API
(`dl_declare_relation`, `dl_add_fact`, `dl_load_rules`, `dl_compile`, `dl_publish_snapshot`)
stays exactly as-is underneath.

### 1. Dhall schema → `dl_schema`

dhall-c is **linked in-process** into the new `dlp` binary; the schema flows
in-memory: `parse → infer_type → normalize → walk normalized term → dl_schema`.
**No on-disk artifact in v1.** This avoids dhall-c's unverified JSON-union
serializer (where prior work failed) and gives positioned errors for free.

The C contract lives in new `src/schema.h` (gcc-clean, no dhall dependency):

```c
typedef enum { DLT_NATURAL = 1, DLT_TEXT = 2 } dl_coltype;

typedef struct {
    char     name[64];
    uint8_t  arity;        /* 1..8 */
    uint8_t  is_idb;
    dl_coltype cols[8];
} dl_reldef;

typedef struct {
    int     n_rels;
    dl_reldef rels[MAX_RELS];
} dl_schema;

/* builder: dup-name / arity-1-8 validation + lookup */
int dl_schema_add(dl_schema *s, const char *name, uint8_t arity, const dl_coltype *cols);
const dl_reldef *dl_schema_lookup(const dl_schema *s, const char *name);
```

`dlp` additionally echoes `.build/schema.json` (a hand-rolled ~60-line writer from
the struct) for **inspection/diff only** — explicitly not load-bearing, so a future
gcc-only JSON loader can be added without redesign.

**Rejected alternatives:** generated C header (forces recompile per schema change —
wrong for a data-project workflow); dhall.com subprocess emitting JSON (two moving
parts, unverified union JSON shape, worse errors).

### 2. Typed data loaders

- **CSV**: header row, file stem = relation name, columns matched to schema **by
  name** in any order. Unknown/missing header columns are errors listing both sets.
  Text-only cells: `Text` accepts any cell verbatim (incl. digit strings); `Natural`
  accepts `^[0-9]+$` ≤ `4294967295`.
- **JSON** (v1): array-of-objects per file, file stem = relation, object keys =
  column names. **Strict typing:** JSON number → `Natural` only, JSON string →
  `Text` only (asymmetry vs untyped CSV is documented).
- Coercion: `Text → dl_intern_str` (already exists, `dl.h:409` — no new interning
  API); `Natural → u32` range-checked. Adds via `dl_add_fact`.

### 3. Rule typechecker (`src/typecheck.c`)

Purpose-built C pass — **not dhall-c** (Datalog needs inference over unannotated
vars; Dhall checks annotations — different jobs). Per-rule var map
`var-name → dl_coltype` + first-conflict site list. **No polymorphism in v1**, so
this is occurrence-consistency, not unification.

Atom dispatch must mirror `compiler.c`'s name-based builtin recognition
(`compiler.c:459-560`):

- **relational atom** — pred must exist in `dl_schema` (closed world; undeclared
  head/body relation is an error suggesting `schema.dhall`), arity match, per-arg:
  `TOK_VAR → column type`; `TOK_INT → require DLT_NATURAL`; `TOK_IDENT/quoted →
  DLT_TEXT`; `TOK_LIST → v1 reject "lists not yet in the typed universe"`.
- **comparisons** `{<,<=,>,>=,!=}` — both args `Natural`; v1 rejects `Text` operands
  (OP_CMP compares raw u32 slots = intern-table order, not lexicographic — a
  semantic trap this typechecker exists to catch).
- **`=` with arithmetic** — result var `Natural`, every var in expr `Natural`
  (OP_ARITH is u32 with wraparound — still `Natural`).
- **`=` plain** — both sides same type.
- **producing builtins**: `concat(Res,A,B)` all `Text`; `length(Res,S)` Res
  `Natural` + S `Text`; `lower/upper(Res,S)` `Text` (parser puts Res as `args[0]`).
- **filter builtins** `prefix/suffix/contains` — both `Text`.
- **aggregates**: `count → Natural`; `sum(V) → V Natural, result Natural`;
  `min/max(V) → v1 Natural-only` (same intern-id-order trap), result `Natural`.
- **negation** — type as the positive atom (stratification stays the compiler's job).
- **regex `~ 'pat'`** — constrains the atom's column to `Text`.
- **list builtins** `cons/car/cdr/append/member` + `range` — v1 reject
  "not yet typed" (typed `List` comes later).

### Error surfacing

The AST currently has **no positions** (token/atom structs, `parser.h:50-99`).
Additive change: `int line, col;` fields on `token` and `atom`, set by the lexer
(newline counting, ~25 lines in `parser.c`); existing consumers ignore them.

- Rule errors: `rules/reach.datalog:7:12: variable W is Natural here (weight.w)
  but Text at rules/reach.datalog:5:18 (tc.dst)`.
- Dhall-side errors carry positions from dhall-c in-process.
- Data errors: `data/weight.csv:3: column 'w' expects Natural, got "heavy"`.
- All go to stderr with project-relative path + 1-based line/col; `dlp check`
  aggregates a count and exits nonzero.

## The Dhall schema DSL

schema-as-data, self-contained `let`s, `: Schema` body annotation — the exact
dnsd-proven dhall-c pattern (partial union literals + annotated heterogeneous
lists). Every union alternative carries a payload; flat scalars carry an
**Optional constraint payload** (`min`/`max`, or a `regex` for Text) that the
loaders enforce, and `None` means unconstrained:

```dhall
let Elem = < Natural : {=} | Text : {=} | Bool : {=} | Char : {=} | Date : {=} | Timestamp : {=} | Signed : {=} >
let NC = { min = None Natural, max = None Natural }   -- unconstrained Natural
let TC = { regex = None Text }                        -- unconstrained Text
let ColumnType = < Natural : { min : Optional Natural, max : Optional Natural }
                | Text : { regex : Optional Text } | Bool : {=}
                | Char : { min : Optional Natural, max : Optional Natural }
                | Date : { min : Optional Natural, max : Optional Natural }
                | Timestamp : { min : Optional Natural, max : Optional Natural }
                | Signed : { min : Optional Integer, max : Optional Integer }
                | List : { elem : Elem } | Optional : { elem : Elem }
                | Enum : { values : List Text } >
let Column = { name : Text, type : ColumnType }
let Relation = { name : Text, columns : List Column }
let Schema = { relations : List Relation }
in { relations =
     [ { name = "node",
         columns = [ { name = "id", type = < Text = TC > } ] }
     , { name = "score",
         columns = [ { name = "val", type = < Natural = { min = Some 0, max = Some 150 } > } ] }
     , { name = "edge",
         columns = [ { name = "src", type = < Text = TC > }
                   , { name = "dst", type = < Text = TC > } ] }
     , { name = "weight",
         columns = [ { name = "src", type = < Text = TC > }
                   , { name = "w",   type = < Natural = NC > } ] }
     , { name = "light_edge",
         columns = [ { name = "src", type = < Text = TC > }
                   , { name = "dst", type = < Text = TC > } ] }
     , { name = "tc",
         columns = [ { name = "src", type = < Text = TC > }
                   , { name = "dst", type = < Text = TC > } ] }
     ] } : Schema
```

- **Arity** = list length (1–8 enforced by the tool with a named error — Dhall
  cannot express list-length types).
- **Encoding contract** (implicit per tag, declared once): `Natural → raw u32`
  (must fit `0..4294967295`), `Text → dl_intern_str`. Users do not restate encoding
  per column.
- **Per-column constraints** (implemented, finish-dlp Item 2): Natural/Char/
  Date/Timestamp and Signed carry `min`/`max` bounds; Text carries a `regex`.
  They are DATA-LOAD metadata only — the typechecker's occurrence-consistency
  (`dl_colspec_eq`) deliberately ignores them (a `Natural[1..10]` and a plain
  `Natural` are the SAME type). The loaders (`dlp check`/`build`) reject rows
  outside `[min,max]` and Text cells that fail the regex (full-key match,
  implicit `^...$` — do not write anchors). `Signed` bounds are `Integer`
  literals and require an explicit sign (`Some -10` / `Some +10`); a bare `10`
  is `Natural`.
- **Extensibility hook**: the union payload is reserved for future type
  parameters, e.g. `< Enum : { values : List Text } >`.

## Workflow surface (project folder)

```
mydb/
  schema.dhall      # the typed contract (: Schema-annotated)
  data/*.csv|json   # validated on load (file stem = relation)
  rules/*.datalog   # concatenated in sorted glob order; typechecked
  .build/           # dlp-owned: dl_open dir, schema.json echo, snapshots
```

Commands (new `dlp` binary — *not* an extension of the existing `dl` CLI, which
stays unchanged as the low-level single-db tool):

- `dlp init [dir]` — scaffold template incl. example.
- `dlp check [dir]` — dhall-typecheck schema + dry-run data validation +
  parse+typecheck rules. The CI command; **no writes**.
- `dlp build [dir]` — check + declare relations from schema + load+validate data +
  `dl_load_rules` (auto-typechecked via attached schema) + `dl_compile` + publish
  snapshot. v1 always rebuilds (staleness via mtime later).
- `dlp query [dir] 'tc(alice, X)'` — build then evaluate goal, print rows via
  `dl_intern_str_of`. Goal literals follow `dl_cli` convention: bare int = raw u32,
  else interned.

**EDB/IDB** are inferred, not declared: relations appearing as rule heads are IDB;
loading data into an IDB relation is an error (`tc is rule-defined; put facts in an
EDB relation`).

### Worked example

`rules/reach.datalog` against the schema above:

```datalog
tc(A, B) :- edge(A, B).
tc(A, C) :- edge(A, B), tc(B, C).
light_edge(A, B) :- edge(A, B), weight(A, W), W < 10.
```

- `W` is `Natural` from `weight.w`, `10` is a `Natural` int literal, comparison OK;
  `light_edge` args are `Text` from `edge` — checks clean.
- **The bug this feature exists for**: `tc(A, W) :- weight(A, W).` → `dlp check`
  prints the occurrence conflict (`W` Natural at `weight.w` vs Text at `tc.dst`).

**Data validation scenario** — `data/weight.csv` header `src,w`: row `alice,3` OK;
row `bob,heavy` → `data/weight.csv:3: column 'w' expects Natural, got "heavy"`;
row `carol,4294967296` → out of u32 range. `data/node.csv` `123` in a Text column
is OK (interned as text "123" — CSV is untyped).

## Extensibility (minimal universe, no redesign)

**Status: implemented (2026-08-17).** Both growth axes landed. The type universe is
now: flat scalars `Natural`, `Text`, `Bool`, `Char`, `Date` (yyyymmdd), `Timestamp`
(epoch), `Signed` (i32 zigzag) — all raw u32 — plus the parameterized `List` /
`Optional` (of a flat element type) and `Enum` (fixed value set). `dl_reldef.cols[]`
upgraded from `dl_coltype` to a `{tag, param}` `dl_colspec` struct; the schema union
was widened (backward-compatible — schema files only construct values, never
case-analyze). The encoding contract stays: one switch in the coercion path.

Design notes that drove it:

1. **Flat scalars fitting u32** (`Timestamp`=epoch, `Date`=yyyymmdd, `Char`, `Bool`,
   `Signed` via zigzag): each is a `DLT_*` enum value + a schema union alternative
   `< Timestamp : {=} >` + one coercion case. Signed is deliberately **not** in the
   ordering-comparison/min/max rules: zigzag's u32 order is not numeric order (the
   VM would silently mis-order); it is an equality/print type.
2. **Parameterized types** (`List`/`Optional`/`Enum`): the union payload earned its
   keep — `< List : { elem : Elem } >`, `< Optional : { elem : Elem } >`,
   `< Enum : { values : List Text } >`. The typechecker's list builtins
   (member/car/cdr/cons/append) now type with the element type; **TOK_LIST
   literals** (finish-dlp Item 1) type in relational args (against the column's
   `List<elem>`) and infer their element type as list-builtin operands
   (`member(X,[a,b])`, `car([a,b])`, `cons(R,x,[a,b])`). v1 keeps a left-to-right
   element-type inference boundary (an empty `[]` literal is 'cannot infer').
3. **Per-column constraints** (min/max, regex) ride the same payload — **implemented**
   (finish-dlp Item 2): Natural/Char/Date/Timestamp/Signed carry `min`/`max`,
   Text carries a `regex`. Enforced by the loaders (`dlp check`/`build`); the
   typechecker stays pure (`dl_colspec_eq` ignores constraints).

## Build / toolchain

- `dlp` = new top-level dir, built **only** by an opt-in Makefile target
  `make dlp DHALLC=<path-to-dhall-c>` using **cosmocc**, compiling
  datalog-dafsa `src/*.c` + dhall-c `src/*.c` together (dhall-c referenced by path
  or vendored — decided at impl time). dhall-c is a project-layer dependency only.
- Default `make` / `make test` (gcc, 306 tests) **never** touch dhall-c or
  cosmocc — the core stays gcc-clean/portable.
- `dlp` compiles engine sources directly, so internals (`parse_rules`,
  `compiler.c` builtin dispatch) are reachable without a new parse-exposure API.
- Existing `dl` CLI unchanged and orthogonal: `dl` = low-level single-db tool,
  `dlp` = typed project workflow embedding the same engine.

## Blast radius / staging

Purely additive to the 306-test gcc build:
new `src/schema.h/.c` + `src/typecheck.h/.c` join `ALL_OBJS`;
new test binaries `test_schema` + `test_typecheck` appended to the test target;
`dlp/` + its Makefile target entirely outside the default build.

Touched existing files (each minimal):

| File | Change |
|------|--------|
| `parser.h` / `parser.c` | `line`/`col` fields on token/atom (behavior-neutral) |
| `dl.h` | `dl_schema` fwd decl + `dl_attach_schema` declaration |
| `dl.c` | `dl_db` gains `const dl_schema *schema` member (default NULL) + ~8-line guarded hook in `dl_load_rules` |
| `Makefile` | new objs/tests/target |
| `README.md` + `design/dhall-schema.md` | docs |

Stage order (each independently landable + green; status as of 2026-08-17):

- **[done] S0** — spikes: dhall-c `{=}` **rejected** by typechecker → adopted the
  `Bool`-payload fallback (`< Text = True >`) at the time; dhall-c was later fixed
  (`bae0e08`) and the DSL switched back to the `{=}` payload; dnsd patches
  (partial-union + type-alias resolution) landed; datalog-dafsa builds under cosmocc.
- **[done] S1** — parser positions (`line`/`col`) + tests.
- **[done] S2** — `schema.h/.c` + `dl_attach_schema` no-op hook + `test_schema`.
- **[done] S3** — `typecheck.c` + fixtures (positive: worked example; negative: int/text
  mixing, undeclared relation, Text comparison, aggregate on Text, int-const in
  Text col, untyped var) + `test_typecheck`. *(the correctness-dense stage)*
- **[done] S4** — `dlp` skeleton (cosmocc Makefile target + schema.dhall walker +
  declare/attach + `init` template).
- **[done] S5** — typed CSV loader + `check`/`build`/`query` over the worked example
  (golden-output test).
- **[done] S6** — JSON loader.
- **[done] S7** — docs + design note (this document; the README `dlp` section).

## Risks

1. **dhall-c subset gaps beyond the spike** (`{=}`, `/\` merge, file imports,
   let-bound helpers needing inner `: Column` annotations): the example schema
   uses only dnsd-proven constructs; helpers/imports are template niceties, not
   load-bearing.
2. **Typechecker/compiler drift**: the builtin table must be derived by *copying*
   the pred-name dispatch lists from `compiler.c:459-560` at impl time (that file
   is the ground truth for accepted syntax); a fixture per builtin locks it.
3. **cosmo+gcc double build rot**: `dlp` target is opt-in and CI-light; keep
   dhall-c linkage in **one** `.c` (the walker).
4. **sum/min-max wraparound semantics** (OP_ARITH u32 wrap, documented) —
   typechecker accepts, docs disclose.
5. **magic-sets/QSQ paths re-compile rules internally** (`dl.c:2564`, `:2823`) —
   the typecheck hook lives in `dl_load_rules` only, so adorned re-compiles are
   unaffected; verify no double-report on double `load_rules` (idempotent parse →
   re-typecheck is fine, same errors).

## Open decisions (deferred to implementation)

- dhall-c referenced by **path** vs **vendored** (built by path via `DHALLC=..` today).
- Whether `{=}` parses in dhall-c — **resolved at S0**: it did not typecheck, so the
  `Bool`-payload fallback (`< Text = True >`) shipped. dhall-c was later fixed
  (`bae0e08`) and the DSL was switched back to the `{=}` payload. See "What landed".
- `toml` loading is a natural S7+ add once JSON lands (not in v1).
