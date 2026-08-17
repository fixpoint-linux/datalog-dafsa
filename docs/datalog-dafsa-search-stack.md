# The search stack: one sym-id space, co-versioned, Datalog-composable

**Status:** Synthesis / design thesis (2026-08-17). The capstone of the datalog-dafsa design docs — the framing that ties the graph, full-text, semantic, and fuzzy plans into one system, plus four ranked follow-on ideas. This doc is a thesis + an idea list, not a single implementable plan (each idea points to its concrete mechanism).

## The thesis

Everything we designed lives on one foundation: **a symbol-id-addressed store where the primary relations and every auxiliary search index share one append-only `sym_id` space, publish atomically, and are Datalog-joinable.**

The interner (`intern.h`) is append-only and idempotent — `sym_id`s never drift and are never reused. Every tier is keyed on it:

| Tier | Relation / index | Keyed on |
|---|---|---|
| Graph | `entity`, `edge`, `observation` | `sym_id` (names) |
| Full-text | `term → obs_id` postings DAFSA | `term_sym`, `sym_id` |
| Fuzzy | `trigram → name_id` | `trigram_sym`, `sym_id` |
| Semantic (MIH) | `sig_j band → sym_id` postings | `substr_u32`, `sym_id` |
| Re-rank | `vec_q(sym_id, chunk, packed_int8)` | `sym_id` |
| Basis | `itq_basis(dim_i, dim_j, bits)` | — |

Because they all `dl_publish_snapshot` **atomically**, at any snapshot version the graph + every index are **mutually consistent and time-travel-able together**. Because they all share the `sym_id` key, they are **Datalog-joinable** — the VM's joins compose across tiers.

**That** is what makes graph + full-text + semantic + fuzzy *one system* instead of four bolted together. The DAFSA is the substrate; the sym-id space + atomic publish is the integration. This is the real consolidation win, distinct from any single structure.

## The four follow-on ideas (ranked)

### Idea 1 — Unified "auxiliary postings index" primitive ★ cheap, high leverage

Full-text, trigram, MIH postings, and regex-on-symbols are all **the same shape**: a DAFSA keyed `token → sym_id`, joined on the interner. They're currently four separate designs. **One `aux_index(token, doc_sym_id)` primitive** — one build path, one walk, one WAL/publish story — lets all four (and any future index type) build on a single codepath.

- **Mechanism:** an abstraction over `dl_declare_relation + dl_add_fact + dl_prefix`, which already exist.
- **Cost:** cheap. This is the consolidation story applied to the *index layer itself*.
- **Docs:** `fulltext.md` (`term→obs_id`), `trigram.md` (`trigram→name_id`), `vector-search.md` (`sig_j` postings), `regex-on-symbols.md` (the automaton walk over a symbol DAFSA).

### Idea 2 — Unified time-travel across every tier + kill the archiver's JSONL ★ differentiator

Because all tiers publish atomically via `dl_publish_snapshot`, the **entire** system — graph + full-text + vectors + fuzzy — is jointly queryable **as-of any snapshot version**. `dl_vector_search_version` is just one instance; the same mechanism time-travels the whole search stack.

**Related win — kill the archiver's JSONL:** the memory archiver (`jing-meta/memory/archiver.py`) deletes old observations to a JSONL archive. With snapshot retention, the "archive" is just **older snapshots**; archived-observation search = `dl_query_version` over an old snapshot. No separate archive format at all.

- **Mechanism:** `dl_publish_snapshot` + `dl_query_version` + snapshot retention (all exist).
- **Cost:** snapshot retention storage; cheap and already designed.
- **Docs:** `vector-search.md` (atomic publish), `timestamps.md`/`recent.md` (temporal layer), `memory/archiver.py` (the JSONL to replace).

### Idea 3 — One embed pass emits all auxiliaries atomically ★ cheap wiring

`embed.py` already produces ITQ + `.npy`. Extend it once to emit, in a single wholesale pass: MIH `sig_j`, `vec_q`, `itq_basis`, full-text postings, trigram — then **one bulk `dl_publish_snapshot`** publishes them all consistently. One re-embed trigger maintains the *entire* semantic+lexical layer.

- **Mechanism:** the bulk-publish pattern from `vector-search-no-sidecar.md` (step 6), generalized to all auxiliaries.
- **Cost:** cheap — it's wiring, all the pieces are designed.

### Idea 4 — Search-as-Datalog: the whole search stack composes as rules ★ the biggest leap, partly aspirational

Because everything is a sym-id-joined relation, the search stack is expressible as **Datalog rules**, not bespoke glue:

```prolog
near(X,Y) :- entity(X), entity(Y),      % semantic neighborhood (derived)
             vec_q(X,_,_) ~ int8_cosine_ge(0.9, Y).
top(X)     :- entity(X), name(X) ~ '^tech',   % hybrid: symbolic gate ∩ vector
             near(X, "target").
```

The Datalog VM already does the joins; **IVM keeps derived relations (like `near`) incrementally updated as facts change.** So a semantic-neighborhood relation is just another derived view, maintained for free. The line between "graph query" and "search query" disappears.

- **Honest caveat:** the float/int8 cosine re-rank is a Python step, so rule-encoded *vector* math is approximate-or-hybrid, not in-VM. The symbolic + postings + intersection parts compose natively; full in-VM vector arithmetic does not (and the critique established that's fine — the DAFSA is not the ANN structure).
- **Cost:** partially cheap (the joins are native), partially aspirational (vector predicates in rules need the re-rank bridge).

## The framing, in one line

> **datalog-dafsa = a store where the primary relations AND every search index share one append-only `sym_id` space, publish atomically, and are Datalog-joinable** — which is what makes graph + full-text + semantic + fuzzy *one system* instead of four bolted together.

## Recommended order of work

1. **Idea 1 (unified aux-index)** + **Idea 3 (one embed pass)** — cheap, high-leverage; unify the index layer and its maintenance. Do these while building the individual tiers.
2. **Idea 2 (unified time-travel + kill-archiver-JSONL)** — a real differentiator; build once the tiers exist and snapshots are exercised.
3. **Idea 4 (search-as-Datalog)** — the leap; pursue incrementally as the tiers come online, starting with symbolic-gate rules that already compose natively.

## References

- The doc set it synthesizes: `datalog-dafsa-vector-search.md` + `-no-sidecar.md`, `datalog-dafsa-hybrid-search.md`, `datalog-dafsa-fulltext.md`, `datalog-dafsa-trigram.md`, `datalog-dafsa-regex-on-symbols.md`, `datalog-dafsa-traversal.md`, `datalog-dafsa-cas.md`, `datalog-dafsa-timestamps.md`, `datalog-dafsa-recent.md`.
- `src/intern.h` (append-only sym-id space — the load-bearing property), `src/dl.h` (`dl_publish_snapshot`, `dl_query_version`).
- Memory graph context: `jing-meta/memory/archiver.py` (the JSONL this proposes replacing with snapshot retention).
