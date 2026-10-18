# vendor/ — DAFSA engine (git submodule)

The DAFSA C engine (Carrasco & Forcada 2002 incremental minimal acyclic DFA)
is consumed from the **`fixpoint-linux/dafsa`** repo as a git submodule at
`vendor/dafsa`, per the project working principle **"reuse, don't re-port"**
(see `design/datalog-dafsa-implementation-plan.md` §0).

- **Upstream:** https://github.com/fixpoint-linux/dafsa
- **Pin:** commit `deaacd2` (2026-08-21) — the split-engine revision, see
  `vendor/dafsa` for the exact submodule HEAD.
- **Measured:** ~5.8 B/key at 9.47M keys / 55 MB.

## Rule

The engine lives in `vendor/dafsa/` (the submodule) and is **frozen** there.
No edits inside the submodule unless a blocker is proven. All datalog-dafsa
code lives in `src/`, `dl/`, `pydl/`.

## Contents

The submodule provides the split engine: `dafsa.{c,h}`, `dafsa_internal.h`,
`dafsa_core.c`, `dafsa_state.c`, `dafsa_persist.c`, `dafsa_view.c`,
`dafsa_wal.c`, `dafsa_crc32.c`, `dafsa_build.c` (`dafsa_build_sorted`),
`dafsa_rank.c`, `dafsa_view_rank.c`.

## Removed: `vendor/ggml` (was a git submodule)

`vendor/ggml` is **no longer tracked**: the gitlink left the index and the
`[submodule "vendor/ggml"]` stanza left `.gitmodules` when the C++ embed
sidecar was deleted. Nothing in this repository builds, links or vendors ggml
any more — the engine (`libdatalog.so`, `dl`) never did.

An old clone may still have the ggml *working tree* on disk at `vendor/ggml`;
it is ignored (`.gitignore`) and safe to delete, but this repo neither
delivers nor requires it.
