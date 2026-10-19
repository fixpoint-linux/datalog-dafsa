# Trigram fuzzy name matching (`search_similar`)

> **SUPERSEDED (2026-08-11) — do not build. This page is kept as the design
> record only.** The capability it exists for (typo-tolerant entity-name lookup)
> is delivered with **no relation, no build step and no incremental
> maintenance** by the Levenshtein automaton over the live interner's symbol
> table (it scanned `symbols.dafsa` while that file was persisted; since
> `3a588a0` it scans the names directly) — see
> [datalog-dafsa-fuzzy-lookup.md](datalog-dafsa-fuzzy-lookup.md) and the
> `~~` atom operator / `dl similar`. This doc's own TL;DR already named
> "Levenshtein over a candidate set" as the refinement, and the refinement is
> strictly cheaper than the trigram index it refines: the trigram table would
> have to be built, persisted, kept consistent with `entity(name, type)` and
> re-published, whereas the automaton is compiled per query (measured 10–28 µs at
> k=1) and walks the interner DAFSA that is already there.
>
> What is NOT delivered here: **similarity ranking** (trigram overlap scores, a
> `threshold` parameter). The edit-distance automaton answers "which names are
> within k edits", ranked by edit distance — it does not rank arbitrary
> similarity. If weighted similarity ever becomes the need, that is a separate
> design.

**Status (original, kept for the record):** Design/plan — not yet implemented. Tier-3 gap (#7) — a *nice-to-have*, only worth building if fuzzy entity-name lookup matters for the memory use case. The memory MCP `search_similar` does trigram-based fuzzy name matching; datalog-dafsa has no trigram index.

## TL;DR

Add a trigram index to datalog-dafsa so fuzzy entity-name matching (`search_similar` equivalent) works in-engine. A `trigram(trigram_str, name_sym_id)` relation over the interner, built from the graph's `entity(name, type)` relation, queried by Jaccard/Dice trigram overlap. Reuses the DAFSA's prefix-enum and the existing symbol space. **Honest ceiling: this is the lowest-value item — build only if fuzzy name lookup is actually a need.**

## Motivation

- `memory/server.py search_similar(name, threshold)` (~line 1034) fuzzy-matches entity names via trigram similarity.
- datalog-dafsa has exact lookup (`dl_lookup`) and (planned) regex-on-symbols (`docs/datalog-dafsa-regex-on-symbols.md`), but no fuzzy/substring-similarity matching.
- Trigrams are a standard cheap technique: split a name into 3-char sliding windows, compare two names by the overlap of their trigram sets.

## Design: a trigram relation

### Schema

```
trigram:  trigram_str \0 name_sym_id      /* key = trigram, payload = name sym-id */
```

- `trigram_str` interned via the existing interner.
- Padding: prepend/append `^^`/`$` or use ` name ` padding so short names produce trigrams (standard: `^ab`, `abc`, `bc$`).

### Build

- On load/publish, for each `entity(name, type)`, tokenize `name` → trigram set, add `trigram: tri \0 name_id`.
- Incremental: add/delete rows when names change (WAL).

### Query

```
/* Names whose trigram Jaccard/Dice similarity to `name` >= threshold, ranked. */
long dl_similar(dl_db *db, const char *name, float threshold,
                dl_score_cb cb, void *user);
```

- Generate the query name's trigram set.
- For each trigram, prefix-enum `trigram` → candidate name_ids; accumulate into a candidate→overlap-count map.
- Score = Dice coefficient `2*|A∩B| / (|A|+|B|)`; keep those above threshold, ranked desc.

### CLI

```
dl similar "memory-graph" --threshold 0.3
```

## Files that change

| File | Change |
|---|---|
| `src/trigram.c` (new) | trigram generation (padding, sliding window); `dl_similar` (candidate accumulation + Dice scoring) |
| `src/dl.h` / `src/dl.c` | `dl_similar`, trigram relation wiring, incremental build |
| `src/dl_cli.c` | `dl similar <name> [--threshold]` |
| `docs/` | this doc |
| `tests/` | `test_trigram.c`: padding edges (short names), overlap scoring, threshold filtering, ranking, incremental add/delete |

## Honest ceiling

- **Lowest-value item in the set.** Exact-name lookup + regex-on-symbols already cover most name-finding. Fuzzy matching is a UX nicety.
- Trigram quality is crude (no edit-distance); if the use case needs true typo-tolerance, Levenshtein over a candidate set is a later refinement.
- Padding scheme affects short-name matching — pick one, document the edge cases.

## Concrete next slice (if pursued)

1. Trigram generation (padding, sliding window) + unit tests.
2. `trigram` relation build at load.
3. `dl_similar` (candidate accumulation, Dice scoring, threshold).
4. Incremental maintenance on name change.
5. Wire memory `search_similar` onto `dl_similar`.

## References

- `memory/server.py` `search_similar()` (~line 1034).
- `src/intern.h` (shared symbol space).
- `docs/datalog-dafsa-regex-on-symbols.md` (complementary single-term regex matching).
