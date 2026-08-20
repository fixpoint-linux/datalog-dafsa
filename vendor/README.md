# vendor/ — frozen DAFSA engine

Copy of the jing-meta DAFSA C engine (Carrasco & Forcada 2002 incremental
minimal acyclic DFA), per the project working principle **"reuse, don't
re-port"** (see `design/datalog-dafsa-implementation-plan.md` §0).

- **Source:** `jing-meta/indexer/dafsa/`
- **Revision:** `07074cf` (2026-08-10 22:57:28 +0000)
- **Measured:** ~5.8 B/key at 9.47M keys / 55 MB.

## Rule

These files are **frozen**. No edits here unless a blocker is proven. All
datalog-dafsa code lives in `src/`, `dl/`, `pydl/`.

## Contents

| file | role |
|---|---|
| `dafsa.c` | public API + length-delimited add/lookup/delete |
| `dafsa_core.c` | minimality maintenance (register, replace, clone) |
| `dafsa_persist.c` | PDWG v4 save/load (fsync + atomic rename) |
| `dafsa_view.c` | zero-copy mmap read-only view + prefix enum |
| `dafsa_wal.c` | DAWL write-ahead log + overlay |
| `dafsa_crc32.c` | CRC32 checksums |
| `dafsa.h` | public API (opaque handle) |
| `dafsa_internal.h` | shared internals (not public) |

Note: the `/* M0 stub */` comments in `dafsa.h` are stale from the jing-meta
roadmap — `dafsa_save`, `dafsa_load`, `dafsa_prefix_enum`, WAL, and the mmap
view are all fully implemented here.

## Exception: `vendor/ggml` (git submodule, NOT a frozen copy)

`vendor/ggml` is an **upstream library dependency**, pinned via a git
submodule — an explicit exception to the "frozen copied sources" rule above:

- **Upstream:** https://github.com/ggml-org/ggml
- **Pin:** tag `v0.20.2`, commit `8c63e70982c95ceb862e3a1073a2c1beef75d60a`
  (verified GPG-signed tag, 2026-08-18)
- **Why a submodule, not a copy:** ggml is a large multi-backend tree
  (CUDA/Metal/Vulkan/OpenCL + per-arch SIMD); a copy would bloat the repo
  and bake in unused backends.
- **Materialize once on a networked host:** `git submodule update --init vendor/ggml`
- **Build:** the top-level Makefile runs cmake on it (CPU-only:
  `-DGGML_CUDA=OFF -DGGML_METAL=OFF -DGGML_VULKAN=OFF -DGGML_OPENCL=OFF`,
  examples/tests OFF) into `build-tmp/ggml` static libs. ggml compiles
  under its OWN cmake flags — the project `-Werror` never applies to it.
- **Consumers:** `src/embed/*` (the `dl-embed` vector-tier tool) only. The
  engine (`libdatalog.so`, `dl`) never links ggml.

The default `make` / `make test` targets do NOT require the submodule; only
the opt-in `make dl-embed` / `make embed-test` targets do.
