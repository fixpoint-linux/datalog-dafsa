# `~ <regex>` on column string content (via `symbols.dafsa`)

_Design/plan — not yet implemented._ Status: **Design/plan — not yet implemented.**
Settled by the handoff plan (2026-08-17) and validated by a standalone C prototype
(`read_sym_id` payload reconstruction). This doc records the decisions and the
file-level work items; the engine change itself is **not** done.

## TL;DR

Re-point the `~ <regex>` operator (and its full stack — `dl_pattern` /
`rel_pattern` / `view_pattern` / `vrel_pattern` / `OP_WALK` / the CLI `dl pattern`)
from **whole-key byte matching** to **column string-content matching via
`symbols.dafsa`**.

Today the regex walks the relation DAFSA's keys, which encode whole tuples as
fixed-width `u32BE` column ids. The new primitive `symbols_dfa_walk` /
`symbols_dfa_walk_view` walks the **symbol DAFSA** (keys `utf8_str`, NUL,
`sym_id u32BE`) instead: the regex matches only the **string bytes before the NUL**
and the walker emits the **sym_id** (reconstructed from the 4-byte `u32BE`
payload). The relation filter becomes "keep tuple iff `cols[k] ∈ S`", where `S`
is the set of sym-ids whose string matched. The generic whole-key
`regex_dfa_walk` / `regex_dfa_walk_view` primitives **stay** (the raw-DAFSA tests
keep passing) but are no longer the `~` path.

## Motivation

Today `q(X,Y) :- edge(X,Y) ~ '(a|b).*'` matches the **whole `4*arity+1`-byte
key** — i.e. the raw byte encoding `X_id` and `Y_id` as big-endian u32s. The
regex therefore runs over **binary sym-id bytes**, not the underlying string text:

- `(a|b)` matches against the **high byte of `X`'s sym-id** — a near-useless
  byte/numeric filter, not a text filter.
- The anchor/alternation semantics a user expects (`"first char is a or b"`) do
  not hold for strings whose ids share a high byte for an unrelated reason.
- Two different strings that happen to alias at a byte position match; two that
  genuinely start with `a`/`b` may not.

Concretely, `p("alice"). q(X,Y) :- p(X,Y) ~ 'a.*'` is **wrong today**: `~ 'a.*'`
is evaluated on the byte encoding of `(X,Y)` ids, so it is not "does column 0
start with `a`". There is no way to express "filter the leading column's string
content" at all — the operator is effectively useless for text.

## Design decisions (resolved)

**(Q1) Which column — `pred(...) ~ [k] 'pattern'`, optional column literal.**
The language expr is `pred(...) ~ [k] 'pattern'` with an optional **0-based
integer column literal `k`**, default `0` = the leading column (mirrors the range
builtin's "leading column only" precedent). It filters that column's sym-id
content. Multi-column relations use an explicit `k`; a variadic atom targets its
syntactic-arity variant.

**(Q2) int vs sym-id — no per-column type metadata.** The filter is **pure
sym-id set membership** (`cols[k] ∈ S`). There is **no** per-column type
metadata. An int column therefore matches nothing (a B6 collision is the
documented caveat — the same backstop the `intern_str_of → NULL` path uses for
string ops on non-string columns, so this is consistent with the existing
engine's behavior).

**(Q3) Escape hatch — replace whole-key matching on the `~` surface entirely.**
Whole-key byte matching is **removed** from the `~` surface. The generic
`regex_dfa_walk` / `regex_dfa_walk_view` remain only as **tested raw-DAFSA
primitives** (used by the raw-DAFSA engine tests), not as the `~` path.

**(Q4) Both in-memory and snapshot mmap paths.** `symbols_dfa_walk` runs over
`db->ir->fwd` (live); `symbols_dfa_walk_view` runs over `<sdir>/symbols.dafsa`
(snapshot mmap), mirroring how `dl_pattern` already routes live-vs-snapshot.

## The new primitive

**`symbols_dfa_walk` / `symbols_dfa_walk_view`** — automaton intersection over
`symbols.dafsa`, whose keys are `utf8_str`, NUL, `sym_id u32BE`:

- The regex matches only the **string bytes before the NUL** separator.
- The walker **emits the sym_id**, reconstructed from the **4-byte `u32BE`
  payload** that follows the NUL.
- Reuses the existing `visited_set` + product-DFS machinery already in
  `regexwalk.c`; the new branch is at `edge->sym == 0x00` (string terminator):
  check `dfa->accept[rstate]`; if accepting, follow **exactly 4 edges** to read
  the payload and emit the id.

Validated `read_sym_id` helper (prototype, verified to reconstruct all ids):

```c
static uint32_t read_sym_id(const dafsa *d, unsigned int s, int *bad) {
    uint32_t id = 0; unsigned int cur = s;
    for (int i = 0; i < 4; i++) {
        const State *st = &d->states[cur];
        if (st->ntrans != 1) { *bad = 1; return 0xFFFFFFFFu; }
        const Edge *e = &trans_arr_c(st)[0];
        id = (id << 8) | e->sym; cur = e->target;
    }
    return id;
}
```

The payload is a **linear 4-edge chain** — verified to hold even under id suffix
sharing by the prototype (ids `{1,2,3,4,256,257,65536,16777217,257,65538}` all
reconstruct; no payload node has more than one outgoing edge).

The set `S` is an open-addressing u32 hash set (`symset_init` / `symset_add` /
`symset_contains` / `symset_free`).

## File-level steps

| # | File | Change |
|---|------|--------|
| 1 | `src/regexwalk.h` + `src/regexwalk.c` | Add `sym_walk_cb`, `sym_set` (u32 hash set: `symset_init`/`add`/`contains`/`free`), `symbols_dfa_walk`, `symbols_dfa_walk_view`. Reuse `visited_set` + product-DFS; new `edge->sym==0x00` branch reads the 4-byte payload via `read_sym_id`. |
| 2 | `src/parser.h` + `src/parser.c` | Add `int pattern_col` to `struct atom`; in `parse_body_atom` (~1167) parse the optional `TOK_INT` after `~` (default 0) then require `TOK_STRING`. |
| 3 | `src/relation.h` + `src/relation.c` | Replace `rel_pattern` with `rel_filter_col(rel, col, const sym_set*, rel_enum_cb, user)` — enumerate all tuples (`rel_prefix(rel, NULL, 0)`) and call cb only when `cols[col] ∈ set`. |
| 4 | `src/vrelation.h` + `src/vrelation.c` | `vrel_pattern` → `vrel_filter_col` fan-out. |
| 5 | `src/snapshot.h` + `src/snapshot.c` | `view_pattern` → `view_filter_col(view, arity, col, const sym_set*, dl_tuple_cb, user)`. |
| 6 | `src/compiler.c` + `src/compiler.h` | Carry the column — add `uint8_t pat_col[]` parallel to `pat_idx` (body-atom indexed); set `ip->c = pat_col` (`vm_instr.c` unused for `OP_WALK`) at **all 3** `OP_WALK` emission sites (~1704, ~2654, ~2748); compile-time reject `col >= nargs`. |
| 7 | `src/vm.c` `OP_WALK` (~619) | Build the sym set via `symbols_dfa_walk(db->ir->fwd, dfa)` then `rel_filter_col(r, in->c, &set, tbuf_cb, &f->tuples)`. |
| 8 | `src/dl.h` + `src/dl.c` `dl_pattern` (~3507) | Add `uint8_t col` param. Snapshot path opens `<sdir>/symbols.dafsa` view + `symbols_dfa_walk_view` → set → `view_filter_col` per (variadic) variant; in-memory path `symbols_dfa_walk(db->ir->fwd)` → set → `rel_filter_col`/`vrel_filter_col`. |
| 9 | `src/dl_cli.c` `dl pattern` (~439) + usage (~123) | Optional `col` arg — `dl pattern <rel> [<col>] '<regex>'`. |
| 10 | `src/magic.c` `atom_copy` (~294) | Also copy `src->pattern_col`. |
| 11 | `docs/language.html` regex section (~293-304) + `CHANGELOG.md` (~440-444) | Rewrite the regex operator description for column string-content semantics. |

## Test / verification plan

**Keep (unchanged):**
- `tests/test_m5.c` raw `regex_dfa_walk` engine tests (`test_regex_*` + property).
- `tests/test_m5_review.c` sections A/C/D/G/H/I (generic whole-key walker on raw DAFSAs).

**Break + update:**
- `tests/test_m5.c` `test_walk_instruction` (~637) and `test_walk_all_match` (~678)
  use `x00x00x00x01.*` / `.*` over **int** facts — rewrite to **string** facts
  (CSV `alice`,`bob`; rule `q(X,Y) :- p(X,Y) ~ 'a.*'` matches `col0=alice`;
  `~ 1 'b.*'` matches `col1=bob`).
- `tests/test_m5.c` `test_snapshot_pattern` (~594) stub → real snapshot
  string-facts test.
- `tests/test_m5_review.c` E (`t_e03`) + F (`t_f01`, `t_f03`) → rewrite to string-content.

**New:**
- (a) `symbols_dfa_walk` unit tests on `str`/NUL/id DAFSA — anchor, `.*`,
  alternation, no-match.
- (b) int-column `~` → empty.
- (c) column-index `col0` vs `col1`.
- (d) out-of-range `col` → compile error.
- (e) snapshot `dl_pattern` string filter.
- (f) `symbols_dfa_walk_view` parity vs in-memory.

Cross-check every emitted sym_id against `intern_str_of(db->ir, sym_id)` in a test.

**Run:** `make tests/test_m5 tests/test_m5_review`, then the full `make test`.

## Risks (flagged)

1. **Exactly-4-edges payload** — reconstruction must follow exactly 4 edges
   (verified linear by the prototype, even under id suffix sharing); guard with
   the `ntrans != 1` check and `*bad` out-param.
2. **Snapshot must walk `<sdir>/symbols.dafsa`**, not the live `db->ir` — the
   live interner may be a superset after post-publish inserts.
3. **Don't forget `magic.c` `atom_copy` `pattern_col`** — a missed copy silently
   drops the column selection in magic-set re-written rules.
4. **NUL separator is safe** — interned strings never contain NUL (`intern_str`
   uses `strlen`), so `0x00` unambiguously terminates the string portion.
5. **Update ALL `dl_pattern` callers** (CLI + `test_m5` + `test_m5_review`) for
   the new `col` param.

## References

- `src/regexwalk.c` / `src/regexwalk.h` — `regex_dfa_walk` / `regex_dfa_walk_view`
  (the generic whole-key primitives, unchanged; the new `symbols_dfa_walk*` live here).
- `src/intern.c` / `src/intern.h` — `interner` + `intern_str_of`; `db->ir->fwd`
  is the live symbol DAFSA.
- `src/dl.c` `dl_pattern` (~3507), `src/relation.c` `rel_pattern` (~940),
  `src/snapshot.c` `view_pattern` (~426), `src/vrelation.c` `vrel_pattern` (~147).
- `src/vm.c` `OP_WALK` (~619), `src/compiler.c` OP_WALK emission (~1704/2654/2748)
  + `pat_idx` plumbing (~1435/1951).
- `src/dl_cli.c` `dl pattern` (~439) + usage (~123).
- `docs/language.html` §Regex (~293-304).
