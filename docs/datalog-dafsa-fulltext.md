# Full-text search over observation content

**Status:** Implemented. Tier-2 feature. Multi-token / ranked search over free-text observation content, composed **beside** the graph as a word→doc-id DAFSA. The planned regex-on-symbols gives single-term pattern matching; full-text uses a postings index. **This is the *symbolic half* of hybrid retrieval — see `docs/datalog-dafsa-hybrid-search.md`.**

**Design correction (implemented):** The relation store is SET-semantic (DAFSA collapses duplicate keys), so tf-from-duplicate-keys is IMPOSSIBLE. Ranking is based on DISTINCT matched terms per obs_id (more matched → higher). A separate count relation would be needed for true tf ranking (future refinement).

## TL;DR

Add ranked, multi-token full-text search over observation content by building a **postings DAFSA** (`term_sym → obs_id`) as a separate store joined on the shared interner/sym-id space. This reuses the exact pattern `jing-meta/indexer` already uses (composite key `word \0 doc_id`) and composes with the graph's `observation(entity, content)` relation rather than living inside the fixed-width relation encoding.

## Motivation

- Users search memory-graph *content* ("find the observation containing both 'heap' and 'nursery'").
- The planned `symbols_dfa_walk` (regex over interned strings, `docs/datalog-dafsa-regex-on-symbols.md`) is **single-term pattern matching** (O(len) `contains`/`prefix` over resolved text). It does **not** do multi-token AND or ranking.
- datalog-dafsa facts are fixed-width u32 tuples — free-text does not belong in a relation column (established in the analysis). Full-text belongs in a **word→doc-id DAFSA** beside the graph.

## Design: a postings DAFSA joined on the interner

### Schema

```
postings:  term_sym \0 obs_id_u32BE        /* one DAFSA; key = term, payload = obs_id */
```

- Terms and observation text are interned via the existing interner (`src/intern.h`) — **one shared symbol space**.
- `obs_id` is a synthetic u32 id; the graph's `observation(entity, content)` relation maps `obs_id ↔ content_sym` (add an `obs(id, entity, content)` arity-3 relation, or reuse the content sym-id as the doc id).
- This is exactly `jing-meta/indexer`'s composite-key pattern (`word \0 file_idx \0 entry_idx`), lifted into datalog-dafsa's own store.

### Build (at load / publish)

- On each observation, tokenize content → terms; for each term, add `postings: term \0 obs_id`.
- Tokenization: split on non-alphanumeric, lowercase (mirror `jing_meta/text.py` if it exists; else a simple C tokenizer). Stopwords optional.
- Incremental: `dl_add_fact(postings, term, obs_id)` / `dl_delete_fact` via WAL — postings stay consistent with the graph on add/delete.

### Query

```
/* All obs_ids whose content matches ALL terms (AND), ranked. */
long dl_search(dl_db *db, const char **terms, int n_terms,
               dl_score_cb cb, void *user);
```

- For each term, prefix-enum/lookup `postings` → a per-term obs_id set.
- **Multi-token AND** = intersect the per-term sets (smallest-first).
- **Ranking**: term frequency (how many times term appears, from duplicate postings keys) and/or doc-frequency inverse weighting. Start with a simple score (sum of tf) and note IDF as a later refinement.

### CLI

```
dl search "heap nursery"        # AND across terms
dl search "heap nursery" --top 10
```

## Files that change

| File | Change |
|---|---|
| `src/index.c` (new) | tokenizer; postings build; `dl_search` (per-term enum → intersect → rank); incremental add/delete |
| `src/index.h` | `dl_search`, `dl_search_top`, `dl_search_version`, `dl_search_top_version` |
| `src/dl.h` / `src/dl.c` | `dl_search`, `dl_index_observation`, postings relation wiring |
| `src/dl_cli.c` | `dl search <terms>` [--version N], `dl versions` |
| `docs/` | this doc |
| `tests/` | `test_search.c`: single/multi-term AND, no-match, ranking order, incremental add/delete, snapshot parity |

## Time-travel / Version-aware search

Full-text search supports **time-travel** via snapshot versions.  The same AND-intersect + rank logic runs against a published snapshot's `__postings__` relation instead of the live index.

```c
/* Search as-of a specific snapshot version */
long dl_search_version(dl_db *db, uint32_t version, const uint32_t *terms, int n_terms,
                       dl_search_cb cb, void *user);

/* Top-N version-aware search */
int dl_search_top_version(dl_db *db, uint32_t version, const uint32_t *terms, int n_terms,
                         uint32_t *obs_ids_out, int *scores_out, int limit);
```

Since the sym_id space is append-only and never reused, term sym_ids interned in the live database are valid across all snapshot versions — no re-interning is needed.

**Error contract:** Returns -1 if `version == 0`, the version doesn't exist, or the `__postings__` relation is absent from that snapshot version.  An absent term (empty postings set) returns 0 results, not an error.

CLI:
```bash
dl versions                    # List all published snapshot versions
dl search 'hello world' --version 1  # Search as-of snapshot version 1
dl search 'hello' --version 2 --top 5  # Top-5 results as-of version 2
```

## Honest ceiling

- This is a **second store** (postings DAFSA) beside the graph — not "full-text in the relation encoding." That's the correct boundary, not a compromise.
- Ranking is simple (tf) to start; IDF/positional ranking is a later refinement. "Good enough" for finding content beats the O(len) `contains` scan.
- Tokenization quality bounds result quality — reuse/port `jing_meta/text.py` behavior to keep parity with the existing indexer.
- No relevance snippet generation in-engine (the MCP can render snippets from the matched observations).

## Concrete next slice (if pursued)

1. `obs(id, entity, content)` relation (doc-id ↔ content) if not already present.
2. Tokenizer + postings build at load.
3. `dl_search` single-term (prefix-enum + set), then multi-term AND.
4. Ranking (tf), then IDF refinement.
5. Wire memory search onto `dl_search`.

## References

- `jing-meta/indexer` (word→doc composite-key DAFSA pattern).
- `docs/datalog-dafsa-regex-on-symbols.md` (single-term regex over interned strings — the complement to this).
- `src/intern.h` (shared symbol space).
- `docs/datalog-dafsa-traversal.md` (graph `observation(entity, content)` relation).
