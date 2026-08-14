# Architectural Design: DAFSA-Backed Deductive Database (Datalog VM)

**Status:** Implemented M0-M7 (2026-08-12): fact store, interner, parser/compiler/VM, aggregates/equality/disjunction, snapshot publish + mmap query path, regex walker, permutation indices + hash-join, durability (lock + interner + WAL). 148 tests green. Remaining: M8+ v2 (deferred).
**Author:** advisor-offpeak (GLM-5.2 deep-think), routed tier=hard.
**Grounded in:** the proven jing-meta DAFSA C engine (Carrasco & Forcada 2002 incremental minimal acyclic DFA), measured ~5.8 B/key at 9.47M keys / 55 MB; PDWG v4 on-disk format; mmap zero-copy `dafsa_view`; WAL `DAWL` + compaction. Verified via `indexer/dafsa/dafsa.h` and `dafsa_view.c`.

---

## 0. The linchpin insight (read this first)

The 2026-08-11 prior assessment flagged a **critical mismatch**: a DAFSA is a minimized DAG, not a sorted range structure (B-tree), so merge-joins over it are awkward. That critique is correct **for naive encodings**. It is **dissolved by one encoding choice**:

> **Encode each fact as a fixed-width key, columns laid out as big-endian u32 symbols with no inter-column separators, in column order.**

With fixed-width columns at known byte offsets, "bind all columns left of position k to constants, enumerate the rest" becomes **exactly a byte-prefix lookup** on the relation's DAFSA -- `dafsa_prefix_enum(prefix=k*4 bytes)`. The DAFSA's two strongest primitives (exact-key lookup + prefix enumeration) **are** the two most common join access patterns. The join mismatch is not eliminated -- non-leading-column joins still need work -- but it is **moved out of the hot path** by (a) the fixed-width encoding and (b) per-relation **permutation indices** for every column-prefix the compiler sees used as a join key (the Souffle index-building pass, repaid in our terms). Everything in this design follows from that encoding decision.

> **M0 correction (verified against the vendored engine, 2026-08-11).** The literal claim "`rel_prefix(k bound cols)` == `dafsa_prefix_enum(prefix=k*4 bytes)`" is **inaccurate for the jing-meta engine**. That engine's `dafsa_prefix_enum` has `W\0` semantics: it walks the prefix, then **requires a `0x00` edge** before DFS'ing the payload (it enumerates keys of form `prefix\0payload`, for the `{word}\0{payload}` flat-key format). Our no-separator fixed-width encoding has a `\0` guard only at the very end (offset `4*a`), so partial binds (`k < arity`) have no `\0` at offset `k*4` and native `dafsa_prefix_enum` returns 0. **Resolution (implemented in M0):** `relation.c` provides its own prefix walker — walk the `k*4` prefix bytes via the DAFSA's `trans_find`, then DFS from the resulting state (no `\0` requirement), reconstructing the full tuple from the accumulated bytes. This preserves the design's *intent* (bind leading columns, enumerate the rest via DAFSA state traversal) even though it is not literally the vendored `dafsa_prefix_enum`. Note the walker currently reaches into `dafsa_internal.h` internals on the in-memory `dafsa*` handle; the M4 mmap `dafsa_view` read path will need an equivalent view-based walker (`view_enum_dfs` already exists).

---

## 1. Project shape & language -- **RECOMMENDATION: C core + Python ctypes shim**

Reuse `dafsa.c` / `dafsa_core.c` / `dafsa_persist.c` / `dafsa_view.c` / `dafsa_wal.c` **verbatim** as the storage layer. Write the new VM, compiler, and interner in **C** as `libdatalog.so`. Ship a thin Python ctypes wrapper for the parser, REPL, fact-load tooling, and tests.

**Why C core (not Rust, not pure Python):**
- The user's two stated instincts -- "build specialized fast machinery" (the DAFSA-replaced-FST precedent) and "own the engine" (Souffle is compile-to-native, not embeddable) -- both point to a C core. Reusing the proven dafsa.c directly eliminates the single biggest implementation risk: re-porting a correct minimal-DAFSA with incremental add/delete + WAL + mmap. You don't get a second chance to write that engine correctly; it's already written.
- The VM inner loop (semi-naive fixpoint, tuple-at-a-time binding, prefix-bound index joins) is exactly the kind of pointer-chasing, allocation-averse hot loop where C wins. Python would be ~50-100x slower on the fixpoint.
- mmap zero-copy reads are a first-class feature of the existing `dafsa_view_open` / `dafsa_view_open_layered`; an embedding library that opens a read-only mmap per query matches SQLite's contract and beats Souffle (which synthesizes a native binary per ruleset, not an embeddable lib).
- **Rust** would either (a) require an unsafe FFI binding to the existing C dafsa (no safety win, integration cost), or (b) re-port dafsa.c to Rust (months, high correctness risk, no functional win). Not worth it.
- **Pure Python** is too slow for the VM; the ctypes shim is the right amount of Python.

**Distribution artifacts (single embedded library + CLI):**
- `libdatalog.so` -- embeddable C library (SQLite model): `dl_open`, `dl_load_facts`, `dl_load_rules`, `dl_compile`, `dl_publish_snapshot`, `dl_query`, `dl_close`.
- `dl` -- single CLI binary, statically linked where possible (matches jing-meta's `dafsa-cli` APE/`.com.dbg` pattern).
- `datalog/` -- Python package: parser, REPL (`dl repl`), fact loader (`dl load`), test harness.

**Tier separation (matches the 2026-08-08 gardener two-tier instinct):** the deductive DB is the **deterministic/rules tier**; an LLM/semantic tier is explicitly out of scope and out-of-process. This is a feature, not a limitation -- it's the same architecture the user already chose for the gardener.

---

## 2. Storage model

### 2.1 One DAFSA per relation (NOT one shared DAFSA)

Each relation `R` of declared arity `a_R` has its own DAFSA file `R.dafsa` and its own WAL `R.wal`. Reasoning:
- **Prefix-enum selectivity.** A shared DAFSA keyed by `{rel}{args}` destroys selectivity: a prefix lookup now has to traverse the relation-name trie first. Per-relation DAFSAs keep prefix-enum O(matching tuples).
- **Compaction isolation.** WAL > 25% triggers compaction per relation (the proven jing-meta trigger). A churny relation doesn't force a global compact.
- **Schema boundary.** Arity, column types, and indices are per-relation metadata; one DAFSA per relation makes this clean.
- **mmap cost.** Each open relation is one mmap; for typical queries touching 1-5 relations this is fine. A "hot relation" cache keeps recently-used views mmap'd across queries.

The only thing shared DAFSAs would buy is a single mmap and shared suffix-minimization across relations -- not worth losing per-relation selectivity and compaction isolation.

### 2.2 Term interning (symbol table)

Every string (and any non-int literal) is interned to a **u32 symbol id**. The symbol dictionary is itself a DAFSA:

- **Forward:** `symbols.dafsa` keyed `utf8_str \0 sym_id_u32BE`. Lookup `str -> sym_id` is exact-key membership on the DAFSA (with the sym_id recovered from the payload bytes). Insert new symbol = `dafsa_add_n`.
- **Reverse:** `sym_id -> str` is a flat array file `symbols.array` indexed by sym_id (append-only, grows with vocabulary). Loaded as a read-only mmap.
- **Why a DAFSA for the forward map:** string keys share prefix/suffix aggressively (entity names, type labels, relation names cluster), so the density wins carry over. Intern is rare at query time (most queries reference already-interned constants); it's hot at **load** time.

Integers up to u32 are stored **directly in the fact key** (not interned) -- they ARE the 4-byte column value. This halves storage for the common integer-keyed case (node ids, indexes).

### 2.3 Fact encoding (the linchpin)

For relation `R` of arity `a`, each fact `(v_0, v_1, ..., v_{a-1})` is encoded as the **fixed-width** key:

~~~
| v_0 u32 BE | v_1 u32 BE | ... | v_{a-1} u32 BE |     (4*a bytes, no separators)
~~~

Properties:
- **No inter-column separator.** Fixed width means column `i` is always at byte offset `4*i`.
- **No payload.** The key IS the fact. (v1: facts are atoms, no per-fact value; aggregates derive their values.) This is denser than jing-meta's `{word}\0{file}{entry}` because we drop both the word-length variance and the payload.
- **Big-endian** so bytewise prefix ordering is consistent with numeric ordering within a column (relevant for `min`/`max` aggregates via prefix-enum of the minimal/maximal key -- see section 5).
- **Trailing guard optional.** A trailing `\0` (so total key length is `4*a + 1`) makes per-fact delimitation in logs/debugging unambiguous. v1: include the trailing `\0`; it costs ~1 B / fact and the DAFSA shares it across all final states so the real cost is much less than 1 B / fact.
- **Arity limit v1: <= 8** -> max key length 33 B. Keeps composite keys inside one DAFSA cache-friendly traversal (the perf analysis flagged Phase-1 traversal as the hot path; long keys hurt). u32 limits symbol space to 4 G -- far above any realistic fact set.

**Variable arity per relation: forbidden in v1.** Each relation declares fixed arity at creation. Variable-arity (e.g., Datalog lists) is v2+ and would need a length-tagged encoding that breaks fixed-width -- explicitly out of scope.

### 2.4 Permutation indices (the join index)

For every column-prefix that the compiler sees used as a join key in some rule, build a **permutation-index DAFSA** for that relation: the same facts re-encoded with the join columns rotated to the front. E.g., relation `edge/2` accessed both as `edge(A,?)` and `edge(?,B)` gets:
- Primary DAFSA keyed `(A, B)`.
- Permutation index `edge__1_0.dafsa` keyed `(B, A)`.

This is **exactly Souffle's automatic index construction**, applied to a DAFSA store instead of a B-tree. It converts every compiler-observed join access pattern into a leading-column-prefix lookup, which is what the DAFSA does fastest. Cost: extra disk (one DAFSA per index, each ~5-6 B/fact), built at snapshot time. v1.0 ships with primary-only + automatic permutation indices added in v1.1; v1.0 falls back to hash-join for non-leading-column access.

### 2.5 Read mode vs write mode

- **Read mode (default at query time):** open each relation via `dafsa_view_open` (zero-copy mmap, no per-state allocation -- already optimal per the perf analysis). Optional WAL overlay via `dafsa_view_open_layered` for reading un-compacted updates. Multi-reader safe (mmap is shared, read-only).
- **Write mode (single writer, batch load):** `dafsa_load_rw` opens a mutable handle; bulk `dafsa_add_n` in **sorted key order** (the perf note: sorted-order add converts the hot path from random to sequential access with no algorithmic change to dafsa.c). WAL provides crash durability; compaction triggers at WAL > 25% of base. This matches jing-meta's lifecycle exactly.

### 2.6 Regex/pattern walker (third fact-store primitive)

Because the DAFSA is a DFA, it supports **automaton intersection** with any other DFA -- the technique `rust-fst` exposes via the `Automata` trait and `regex-automata` provides a builder for. Add:

~~~
/* New in dafsa_view.c (or a sibling dafsa_walk.c) */
long dafsa_view_automaton_walk(const dafsa_view *v,
                               const dafsa_automaton *a, /* start state, step(byte)->state, is_accept */
                               dafsa_enum_cb cb, void *user);
~~~

Product construction: walk `(dafsa_state, regex_state)` pairs in lockstep, emitting matching keys. Compile regex/glob -> DFA at query parse time using a regex-automata-style builder (cap DFA states at ~50k, reject pathological patterns). This is the **third fact-store query primitive** alongside exact-lookup and prefix-enum, and it's net-new code (none exists in jing-meta today -- verified). Ship in **v1.1**, not v1.0; v1.0 is exact + prefix only.

---

## 3. Datalog subset (v1 scope)

### 3.1 IN v1
- **Facts** (EDB): ground tuples inserted via load.
- **Rules** (IDB): `head :- body_1, body_2, ..., body_k.`
- **Variables** (uppercase) and **constants** (lowercase / quoted / int).
- **Conjunction** (`,` = AND = join).
- **Disjunction** (multiple rules with same head = OR).
- **Stratified negation** (`!atom`): allowed, must be stratifiable (compile-time check rejects un-stratifiable rulesets).
- **Recursion** via semi-naive fixpoint (delta relations).
- **Equality** (`X = Y`): unify two variables; `X = c` filter by constant.
- **Comparison builtins** (M9): `<`, `<=`, `>`, `>=`, `!=` as infix body atoms.
- **Arithmetic builtins** (M9): `X = E` over `+ - * / %` (precedence-climbing
  expression parser); division/modulo by zero backtracks; u32 wrap-around.
- **String builtins** (M9): `concat`, `length`, `lower`, `upper`, `prefix`,
  `suffix`, `contains`; `concat`/`lower`/`upper` produce a new interned string
  at runtime.
- **Aggregates** (v1.0): `count`, `sum`, `min`, `max` -- grouped by zero or more group-by columns. One aggregate per rule head.
- **Arity limit <= 8** per relation.

### 3.2 DEFERRED to v2
- Richer string ops (regex-based functions, locale-aware case mapping, ...).
- Magic-sets / QSQ top-down rewrites (constant propagation only in v1).
- Negation inside aggregates; aggregates inside negation.
- Nested subqueries / rule composition beyond standard Datalog.
- Time-travel (CozoDB-style valid-time) -- would need a time-axis in the key encoding.
- Variable-arity relations.
- Bushy join plans (v1 = left-deep only).
- Incremental view maintenance across snapshot changes (v1 = full re-eval at snapshot time; see section 7).
- Trace/JIT compilation of hot rules (v1 = pure bytecode interpreter).

---

## 4. The bytecode VM

### 4.1 VM state
- **Binding table:** `var_id (u8) -> value (u32 symbol or UNBOUND)`. One binding table per active rule evaluation; size = max vars in any rule (<= 16 for v1).
- **Tuple cursors:** for materialized intermediate relations (delta sets, worklists), a `tuple_cursor` streams u32 tuples from an in-memory array or a DAFSA view.
- **Program counter**, **stratum register**, **fixpoint-delta-nonempty flag**.

### 4.2 Instruction set (opinionated, minimal)

~~~
/* Relation / cursor */
OPEN_REL    rel_id, mode            ; open DAFSA view (read) or handle (write)
CLOSE_REL   rel_id
SCAN        rel_id, var_list        ; bind var_list to each tuple (full scan)
LOOKUP      rel_id, key_cols, var_list
                                    ; byte-prefix lookup binding remaining cols
WALK        rel_id, automaton_ptr, var_list   ; v1.1: pattern/regex walk
GETCOL      src_var, col_idx, dst_var

/* Binding / filter */
BIND        slot, var_id            ; assign from current tuple
EQ          var_a, var_b            ; unify (filter or propagate)
EQ_CONST    var, sym_id             ; filter by constant
NEQ_CONST   var, sym_id             ; v1.1: negation-as-membership filter

/* Joins (compiler-emitted; see section 5) */
NESTED_LOOP outer_cursor, inner_rel, key_cols, out_cursor
HASH_JOIN   outer_cursor, inner_rel, join_cols, out_cursor

/* Rule body / projection */
PROJECT     rel_id, var_list        ; insert tuple into rel_id (the rule head)
APPEND      worklist_id, var_list   ; append to a delta worklist

/* Aggregates */
AGG_BEGIN   group_cols
AGG_ACC     op, src_var             ; op in {COUNT, SUM, MIN, MAX}
AGG_EMIT    rel_id, group_cols, dst_var

/* Semi-naive / fixpoint */
DELTA_NEW   rel_id                  ; declare rel_id is the new delta
DIFF        dst, new                ; dst = new - existing (set difference)
FIXPOINT    loop_pc, exit_pc        ; jump to loop_pc if any delta non-empty, else exit_pc
STRATUM     id                      ; stratum boundary; reset all deltas

/* Control */
JMPZ        pc, cond_var
HALT
~~~

### 4.3 How the VM exploits DAFSA ops

| Datalog concept | DAFSA primitive | VM instruction |
|---|---|---|
| Fact membership | `dafsa_view_lookup_n(key, 4*a)` | `LOOKUP` with all cols bound |
| Bind trailing variables | `dafsa_view_prefix_enum(prefix)` | `LOOKUP` with leading cols bound |
| Pattern/LIKE predicate | `dafsa_view_automaton_walk` (new) | `WALK` |
| `min`/`max` aggregate | minimal/maximal key via prefix-enum of a single column (BE -> byte-min = num-min) | `AGG_ACC(MIN or MAX)` |
| Negation `!atom` | membership check on the stratified-lower relation | `NEQ_CONST` / membership test |
| Insert derived fact | `dafsa_add_n` (write mode) | `PROJECT` |
| Delta relation | in-memory tuple array + set difference | `DIFF`, `DELTA_NEW` |
| Fixpoint loop | iterate until all deltas empty | `FIXPOINT` |

The big-endian fixed-width encoding makes `min`/`max` free: the lexicographically smallest key with a given prefix IS the numerically smallest tuple, so prefix-enum delivers aggregates by enumeration order if needed. (For count/sum you accumulate while scanning; for min/max you take the first/last.)

---

## 5. Join strategy -- **the hard part, concrete choice**

**Three algorithms, two primary, one explicitly rejected.**

### 5.1 PRIMARY: index-nested-loop via DAFSA prefix-binding
For the common case where the join column on the inner relation is a **leading column** (after permutation-index rotation if needed): for each tuple of the outer relation, do a `dafsa_view_prefix_enum` on the inner relation with the bound join columns as the byte prefix. This is O(outer * matching-inner) -- **index-nested-loop join where the index is the DAFSA itself**. This is the killer use of the DAFSA: it's already the perfect non-clustered index for leading-column access.

Example: rule `tc(X,Y) :- edge(X,Z), tc(Z,Y).` After the recursive `tc` relation has a permutation index keyed with `Z` leading, the inner lookup `tc(Z, ?)` becomes a prefix lookup. Each outer tuple `(X,Z)` triggers one prefix-enum on `tc.dafsa` with prefix = `Z` (4 bytes) -- O(fanout from Z).

### 5.2 FALLBACK: hash-join
For non-leading-column joins where no permutation index exists (or the compiler decided not to build one): materialize the smaller side into an in-memory hash table keyed by the join column(s), then probe with each tuple of the larger side. Plain, well-understood, O(smaller + larger). Workload for hash-join should be rare in v1.1+ (permutation indices cover most cases); v1.0 uses it whenever index-nested-loop isn't applicable.

### 5.3 REJECTED: sort-merge join over the DAFSA
**Do not build this.** The 2026-08-11 assessment established the DAFSA is not a sorted-range structure (minimized DAG, not B-tree); merge-join over DAFSA traversal order is meaningless. Materialize-and-sort-merge would mean building sorted arrays just to merge them -- strictly dominated by hash-join (cheaper to build the hash than to sort). Skip it; do not be tempted by the symmetry with B-tree query planners.

### 5.4 Join order
**v1: left-deep, greedy cardinality estimation.** Estimate each relation's cardinality from `dafsa_stats` (gives `n_final` = number of facts). Order joins so that:
1. Constants/known-selective predicates come first (apply `EQ_CONST` filters before any join).
2. Among remaining relations, pick the one whose estimated post-filter cardinality is smallest, place it outermost.
3. For each subsequent join, prefer the side where the join column is a leading column (index-nested-loop); fall back to hash-join otherwise.

**v2: magic-sets transformation** to push goal constants back into the rule body (turns many bottom-up evaluations into something approaching top-down selectivity without abandoning the bottom-up model). v2 also considers **bushy plans** (binary tree of joins) for high-arity rules.

### 5.5 N-ary joins
Compiler decomposes every n-ary rule body into pairwise binary joins; the VM only ever executes binary `NESTED_LOOP` / `HASH_JOIN`. The intermediate materialized relations live in memory (or spill to a temp DAFSA in v1.1 if they exceed a threshold).

### 5.6 Arity interplay
Arity <= 8 means at most 8 possible single-column access patterns per relation, plus their multi-column prefixes -- a tractable number for the index builder. Higher arity would explode the number of useful permutation indices; the limit caps this combinatorially.

---

## 6. Query API & embeddability

### 6.1 Query = goal predicate (Souffle `.output` style)
A query asks for the contents of a distinguished derived relation. The Souffle idiom is the right one:

~~~
.decl tc(symbol, symbol)           // derived
.decl edge(symbol, symbol)         // EDB (loaded facts)
tc(X,Y) :- edge(X,Y).
tc(X,Y) :- edge(X,Z), tc(Z,Y).
.output tc                         // <-- query: "what's in tc?"
~~~

A query is `dl_query(goal_pred)` against the **materialized** goal relation (see section 7 -- the fixpoint is run once at snapshot publish, not per query). The query returns tuples either by full scan of the goal's DAFSA or by a `LOOKUP` if the caller supplies bound leading columns (point query).

### 6.2 Embedding (SQLite model)

~~~
typedef struct dl_db dl_db;
dl_db   *dl_open(const char *dir);              // open or create DB dir
int      dl_declare_relation(dl_db*, const char *name, uint8_t arity, int is_edb);
int      dl_load_facts_csv(dl_db*, const char *rel, FILE *csv);
int      dl_load_rules(dl_db*, const char *dl_source);   // parses + compiles
int      dl_compile(dl_db*);                    // stratify, plan, emit bytecode
int      dl_publish_snapshot(dl_db*);           // run fixpoint, materialize IDB, switch readers to new mmap
int      dl_query(dl_db*, const char *goal_pred, dl_result_cb cb, void *user);
int      dl_query_bound(dl_db*, const char *goal_pred, const uint32_t *bound_cols, uint8_t n_bound, dl_result_cb cb, void *user);
void     dl_close(dl_db*);
~~~

The lifecycle is **load-declare-compile-publish-query**. After `publish_snapshot`, all readers see a consistent fixpoint; reads are pure mmap and multi-reader safe. This is the SQLite/Souffle hybrid: embedded like SQLite, evaluated like Souffle.

### 6.3 CLI
- `dl load facts.csv --rel edge` -- bulk load facts.
- `dl rules program.dl` -- declare + compile rules.
- `dl publish` -- run fixpoint, publish snapshot.
- `dl query tc` -- print goal relation (or `dl query 'tc(A,?)'` for point query via bound first column).
- `dl repl` -- interactive (Python ctypes wrapper).

---

## 7. Evaluation strategy -- **RECOMMENDATION: hybrid bottom-up + point-lookup**

This is the second big design call (after the encoding). Two pure strategies and why neither is right alone:

- **Pure top-down (Prolog/SLD + tabling, XSB-style):** good for highly-selective point queries against non-materialized views; terrible for read-heavy workloads where the same view is queried many times (recomputation/memo overhead per miss), and table-correctness (mode declarations) is a famous footgun.
- **Pure bottom-up semi-naive, recomputed per query:** Souffle's engine, but Souffle is invoked as a batch compiler; recomputing the fixpoint per query in an embedded lib would be absurdly slow for read-heavy.

### 7.1 The hybrid (recommended)
- **Bottom-up semi-naive to fixpoint, ONCE per DB snapshot**, at `publish_snapshot` time. All EDB + IDB relations are evaluated to fixpoint and materialized to their DAFSAs. This is Souffle's evaluation model and it's the right one.
- **Top-down point lookup for queries** is just `LOOKUP goal_pred, key_prefix` against the materialized goal DAFSA -- O(matching tuples), no recomputation. The "top-down" part degenerates to a point lookup because the goal is already fully materialized.
- **Invalidation: none in v1.** Because the write model is batch-load + compact + publish-snapshot (NOT OLTP), there is no incremental view maintenance. The lifecycle `load -> compile -> publish -> serve-reads` makes this clean: each snapshot is immutable; new facts trigger a new publish which re-runs the fixpoint. This **avoids the entire incremental-IVM rabbit hole** (counting, delta propagation through multi-rule views) at the cost of one full fixpoint per snapshot.

### 7.2 Justification
The user's stated workload is **read-heavy + batch-write**. That makes the right cost split "spend at publish time, serve for free at query time" -- exactly the SQLite (materialized) and Souffle (fixpoint) contract, not the XSB (table-on-demand) contract. Push the fixpoint cost to the rare publish event; make every query a mmap'd lookup. This matches the "specialized fast machinery" instinct: the read path is the specialized fast machinery, and it's about as fast as a fact store can be (mmap + prefix-enum).

### 7.3 What we explicitly DON'T do in v1
- Incremental view maintenance across snapshot changes (full re-eval instead).
- Top-down tabling / SLG resolution.
- Magic-sets transformation (deferred to v2; would narrow the gap between bottom-up materialization and top-down selectivity for queries with constants).

---

## 8. References to borrow (and what to avoid)

| Reference | What it is | What to steal | What to avoid |
|---|---|---|---|
| **Souffle** (souffle-lang.github.io) | Datalog -> synthesized native C++; relations-as-tries; semi-naive; stratified negation; .decl/.output syntax; compiler (rules -> relational algebra -> code) | Semi-naive evaluation; stratification algorithm; automatic index construction (permutation indices); `.output`-as-query; the rule-algebra decomposition | Their synthesized-native-binary deployment model (we embed; we don't emit a fresh binary per ruleset). v1 = bytecode interpreter, not source-to-source compiler. |
| **CozoDB** | Embedded Datalog in Rust; ACID; time-travel; native vector search | Proof that Datalog-as-query-language for a SQLite-like embedded library works; embedding ergonomics; their store model is not what we want -- only the API shape | Their store (RocksDB-style LSM); our DAFSA-based store is the differentiator |
| **rust-fst** + **regex-automata** | FST (sorted map, DFA-based); `Automata` trait; regex-automata builds DFA from regex | The `Automata` trait shape (stateful byte-stepper matcher); regex-to-DFA builder with state cap; the automaton-intersection product construction for the WALK primitive | Nothing specific -- this is purely a borrowing target for the regex walker |
| **DatalogD / mu-DB / Abiteboul-Hull-Vianu** | Reference Datalog evaluation; tuple-at-a-time semi-naive | The semi-naive algebra formalization; delta-relation correctness argument | Their focus on top-down / SLD; we are bottom-up |
| **XSB Prolog (SLG tabling)** | Top-down Datalog with memoization | Acknowledge as the **alternative evaluation strategy** we explicitly defer | Don't borrow: top-down tabling, mode declarations, the SLG correctness model. v1 = bottom-up only. |
| **TACET / neuro-symbolic cascade** (from 2026-08-08 memory) | Pattern: deterministic rules tier + LLM tier | Architectural precedent for keeping the deductive DB as the **deterministic tier** in a larger system with a separate LLM tier | Out of scope here; justifies the tier separation |

---

## 9. Milestones / build order (vertical slice first)

Each milestone is independently testable. The first vertical slice (M0+M1) is one week to a working non-recursive Datalog evaluator.

- **M0 (~1 wk) -- Fact store + interner.** Reuse dafsa.c. Implement term interner (symbols DAFSA + reverse array). Implement per-relation DAFSA with fixed-width encoding. `dl load facts.csv` works. `dl lookup edge A B` (exact) and `dl prefix edge A` work. No rules yet. **Deliverable:** a fact store you can query with the two DAFSA primitives.

- **M1 (~1-2 wk) -- Rule parser + compiler + non-recursive VM.** Parser for the v1 subset (no negation, no recursion, no aggregates). Compiler: rules -> bytecode. VM implements `OPEN_REL`, `SCAN`, `LOOKUP`, `EQ`, `EQ_CONST`, `NESTED_LOOP`, `PROJECT`, `HALT`. Hard-coded index-nested-loop join (prefix-bound). The transitive-closure example runs **non-recursively** (single application). **Deliverable:** `tc(X,Y) :- edge(X,Y).` works; `tc(X,Y) :- edge(X,Z), tc(Z,Y).` evaluated as a single step.

- **M2 (~1 wk) -- Semi-naive fixpoint + stratified negation.** `DELTA_NEW`, `DIFF`, `FIXPOINT`, `STRATUM`. Compiler pass: stratification (reject un-stratifiable rulesets with clear error). Recursive `tc/2` reaches fixpoint. `!atom` works. **Deliverable:** full Datalog recursion + negation.

- **M3 (~1 wk) -- Aggregates + equality + disjunction.** `AGG_*` instructions; count/sum/min/max grouped. Disjunction via multiple rules for same head (already works structurally in M1; this milestone tests it). **Deliverable:** grouped aggregates work.

- **M4 (~1 wk) -- Snapshot publish + query path.** `dl_publish_snapshot`, `dl_query`, mmap read-only views, hot-relation view cache. **Deliverable:** full load-compile-publish-query lifecycle; the system is now usable end-to-end.

- **M5 (~1 wk) -- Regex/pattern walker.** `dafsa_view_automaton_walk`, regex->DFA compiler, `WALK` VM instruction. **Deliverable:** third fact-store primitive live.

- **M6 (~1 wk) -- Permutation indices + hash-join fallback.** Compiler pass that observes access patterns and declares permutation indices; index builder runs at publish time. `HASH_JOIN` for residual non-leading-column joins. **Deliverable:** join strategy fully covers arbitrary access patterns.

- **M7 (~1 wk) -- Durability wiring.** WAL + compaction already exist in dafsa.c; wire them into the publish lifecycle. Snapshot atomicity (tmp + fsync + rename + dir fsync, same as dafsa_save). **Deliverable:** crash-safe publish.

- **M8+ (v2 work):** magic-sets, bushy plans, IVM, time-travel, variable arity, trace compilation.

---

## 10. Risks (ranked)

1. **DAFSA join mismatch on non-leading-column joins** -- *Central risk, partially mitigated.* The fixed-width encoding dissolves it for leading-column access; permutation indices + hash-join fallback cover the rest. **Mitigation:** automatic index construction (M6); aggressive cardinality estimation to pick the side that binds leading columns; arity cap <= 8 to limit join-order combinatorics. **Watch:** if real-world rulesets are dominated by non-leading joins without useful permutation indices, this falls back to hash-join everywhere and the DAFSA advantage evaporates. Test against realistic rulesets early (M1).

2. **Bottom-up materialization cost on snapshot churn** -- Every snapshot publish re-runs the full fixpoint. Fine for read-heavy + batch-write (the stated workload); fatal for OLTP. **Mitigation:** document the lifecycle explicitly; defer IVM to v2; if a workload turns out to need it, the publish cost becomes the metric to optimize. **Watch:** a ruleset with a huge derived relation (transitive closure on a dense graph) makes publish slow. Cap derived-relation size; spill to temp DAFSA.

3. **Stratification correctness** -- Classic Datalog footgun: rules with negation through a recursion cycle are un-stratifiable. **Mitigation:** compile-time stratification checker that rejects with a clear error; test corpus of known-bad programs (negation cycle, clique, negation-in-recursion variants). **Watch:** a buggy stratifier that silently accepts un-stratifiable rules produces wrong answers, not errors -- property-based testing essential.

4. **Term-interner memory growth** -- Symbols accumulate; intern table grows monotonically. **Mitigation:** symbol DAFSA + reverse array; periodic rebuild at publish (re-emit only referenced symbols); document. **Watch:** if symbol set >> fact set (many unique constants), interner dominates. Unlikely for typical KB workloads.

5. **Materialized join/delta arrays** -- Semi-naive delta relations can be large for highly-fan-out rules. **Mitigation:** bounded workset; spill to temp DAFSA in v1.1 when delta exceeds N tuples. **Watch:** pathological rules (e.g., cartesian product in a body) blow up memory; cap body size, reject at compile time.

6. **Regex-DFA compilation cost** -- Pathological regex -> exponential DFA blowup. **Mitigation:** cap DFA states (~50k, rust-fst's cap); reject unbounded patterns with a clear error; rely on prefix-enum for the common case. **Watch:** a malicious or naive regex at query time could OOM the walker; the cap is a hard ceiling.

7. **Concurrency (no file locking anywhere in jing-meta -- verified 2026-08-10)** -- Single-writer/multi-reader in v1. **Mitigation:** document; v1 = single writer process; multi-writer requires a server process (v2). The mmap read path is multi-reader safe by construction (read-only mmap). **Watch:** two processes calling `publish_snapshot` concurrently will corrupt each other's WAL -- enforce single-writer at the API or filesystem level (flock on a lockfile in the DB dir).

8. **Perf vs Souffle on raw rule eval** -- Souffle synthesizes native code per rule; we interpret bytecode. We'll be slower on the fixpoint itself. **Mitigation:** lean on the DAFSA density advantage for storage and the mmap zero-copy read path; if VM is the bottleneck, add hot-rule trace compilation in v2. **Watch:** if a workload is dominated by complex rules over small data, Souffle wins outright; the user's value prop is embedding + storage density, not raw rule throughput.

9. **WAL/compaction races inherited from dafsa.c** -- The known jing-meta "no file locking" caveat carries over. **Mitigation:** v1 single-writer model; explicit lifecycle (one process publishes). **Watch:** an embedded use case where the host app and a maintenance process both publish will race -- refuse at the API level (flock on a lockfile in the DB dir).

10. **Composite-key width (u32)** -- Limits arity <= 8 (32 B key) and symbol space to 4 G. **Mitigation:** documented limit; u64 args would double key width and roughly halve DAFSA density (the perf measurement assumes ~5.8 B/key at the current width) -- u32 is the right default. **Watch:** a workload needing u64 ids (e.g., 128-bit hashes) needs a different encoding; out of v1 scope.

---

## 11. Summary of opinionated choices (one-liner each)

1. **Language:** C core (reuse dafsa.c) + Python ctypes for tooling.
2. **Storage:** one DAFSA per relation; fixed-width u32-BE columns, no separators; trailing `\0` guard; arity <= 8.
3. **Interning:** symbol DAFSA + reverse array; ints stored raw, strings interned.
4. **Indices:** automatic permutation indices for compiler-observed join access patterns (Souffle-style).
5. **Query primitives:** exact lookup + prefix-enum in v1.0; **+ regex/pattern walk (automaton intersection) in v1.1**.
6. **Datalog v1:** facts, rules, conj/disj, stratified negation, semi-naive recursion, equality, count/sum/min/max aggregates.
7. **VM:** flat binding-table interpreter; instructions listed in section 4.2; exploits DAFSA ops per the table in section 4.3.
8. **Joins:** index-nested-loop via prefix-binding (primary), hash-join (fallback), **sort-merge rejected**.
9. **Evaluation:** bottom-up semi-naive to fixpoint once per snapshot; point-lookup queries against materialized goal relations.
10. **Lifecycle:** load -> declare -> compile -> publish-snapshot -> serve-reads; single writer, multi reader.
11. **Deferred (v2):** magic-sets, bushy plans, IVM, time-travel, variable arity, trace compilation, negation/aggregates interaction.
