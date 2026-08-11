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
