#!/bin/sh
# build-wasm.sh — build the language playground's WebAssembly bundle.
#
# Compiles src/playground-wasm.c with the FULL engine core (all LIB_OBJS +
# VENDOR_OBJS, minus dl_cli.c) via emscripten to docs/playground.js +
# docs/playground.wasm.  The emitted artifacts are COMMITTED to docs/ (there is
# no CI build step — the Pages workflow just deploys the static files).
#
# Requires emscripten + node.  On Arch:
#   pacman -S emscripten clang lld llvm nodejs
# (emscripten bundles binaryen; do NOT also install binaryen — conflict.)
#
# -D_POSIX_C_SOURCE=200809L matches the native Makefile CFLAGS so
# getline/strdup/fsync/mmap have the correct prototypes.  NO FORCE_FILESYSTEM:
# the entry never fopen()s (it builds a dir==NULL in-memory db), so no MEMFS.
# The snapshot/WAL/dafsa_view POSIX surface (mmap MAP_PRIVATE read-only,
# munmap, fcntl F_SETLK, opendir/readdir, fsync, rename, unlink, mkdir) is all
# MEMFS-backed emscripten libc — it links fine and is simply never reached
# (dir==NULL, snap_version==0, never publish).  NO stubbing needed.
set -euo pipefail
cd "$(dirname "$0")/.."

EMCONF=$(mktemp)
trap 'rm -f "$EMCONF"' EXIT
cat > "$EMCONF" <<'EOF'
import os
NODE_JS = '/usr/bin/node'
LLVM_ROOT = '/usr/bin'
BINARYEN_ROOT = '/usr'
EMSCRIPTEN_ROOT = '/usr/lib/emscripten'
CACHE = os.path.expanduser('~/.cache/emscripten')
EOF

EMCC=/usr/lib/emscripten/emcc
OUT=$(mktemp -d)
trap 'rm -f "$EMCONF"; rm -rf "$OUT"' EXIT

COMMON='-O2 -D_POSIX_C_SOURCE=200809L -I src -I vendor -s MODULARIZE=1 -s ALLOW_MEMORY_GROWTH=1 -s TOTAL_STACK=5242880'
RUNTIME='-s EXPORTED_RUNTIME_METHODS=ccall,cwrap,stringToUTF8,UTF8ToString,lengthBytesUTF8,HEAPU8'

# The FULL engine core: every LIB_OBJS + VENDOR_OBJS source, minus dl_cli.c
# (which defines main).  dl.c is monolithic and references snapshot.c /
# vendor/dafsa_view.c / WAL unconditionally — those symbols must resolve at
# link, but their POSIX/disk paths are dead code in the playground.
CORE='src/intern.c src/termstore.c src/relation.c src/vrelation.c src/tupleset.c src/parser.c src/compiler.c src/vm.c src/snapshot.c src/regexwalk.c src/permindex.c src/util.c src/dl.c src/iter.c src/magic.c src/topdown.c src/analyze.c src/schema.c vendor/dafsa.c vendor/dafsa_state.c vendor/dafsa_core.c vendor/dafsa_persist.c vendor/dafsa_view.c vendor/dafsa_crc32.c vendor/dafsa_wal.c vendor/dafsa_build.c vendor/dafsa_rank.c vendor/dafsa_view_rank.c'

EM_CONFIG="$EMCONF" "$EMCC" $COMMON \
    -s EXPORT_NAME=createPlayground \
    -s EXPORTED_FUNCTIONS=_playground_run,_malloc,_free \
    $RUNTIME \
    -o "$OUT/playground.js" \
    src/playground-wasm.c $CORE

# (2) LSP server: src/lsp.c + src/json.c.  -DLSP_NO_MAIN drops the stdio main();
# lsp_handle / lsp_out / lsp_out_len are the wasm entry surface.  src/analyze.c
# is already part of $CORE (shared with the playground build), so it must not
# be listed again here (a duplicate source would double-link its symbols).
EM_CONFIG="$EMCONF" "$EMCC" $COMMON -DLSP_NO_MAIN \
    -s EXPORT_NAME=createDlLsp \
    -s EXPORTED_FUNCTIONS=_lsp_handle,_lsp_out,_lsp_out_len,_malloc,_free \
    $RUNTIME \
    -o "$OUT/dl-lsp.js" \
    src/lsp.c src/json.c $CORE

mkdir -p docs
cp "$OUT/playground.js" "$OUT/playground.wasm" "$OUT/dl-lsp.js" "$OUT/dl-lsp.wasm" docs/
echo "built docs/playground.js + docs/playground.wasm + docs/dl-lsp.js + docs/dl-lsp.wasm"
