#!/bin/sh
# run_zig_suites.sh — U1 strangler-hybrid oracle.
#
# Re-links every C test suite against the Zig-built hybrid libdatalog.so and
# runs it.  The LINK ITSELF is the ABI-completeness check (a missing export is
# a link error); the suite asserts are the behavioral oracle.  Mirrors
# tests/run_all.sh conditions exactly:
#   - cwd = repo root (suites use relative "build-tmp/..." paths)
#   - build-tmp/ exists
#   - ./dl at the repo root is the CLI under test (test_m4_review popen()s it,
#     test_vector_cli execv()s it, smoke.sh runs it) — the repo-root ./dl is
#     shadowed by the Zig-linked one for the duration and restored afterwards
#   - test_embed_math: standalone C++ (no engine link) — built with g++ like
#     the Makefile rule
#   - dl-embed: C++/ggml, run only if the binary exists, else skip (same as
#     run_all.sh)
# Exit status is non-zero if any suite fails to LINK or run.
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd) || exit 1
cd "$ROOT" || exit 1

LIBDIR=zig-out/lib
ZIGDL=zig-out/bin/dl
TMP=build-tmp
SAVED_DL=$TMP/dl.c-saved

export ZIG_GLOBAL_CACHE_DIR="${ZIG_GLOBAL_CACHE_DIR:-/tmp/zigcache-host}"
export TMPDIR="$ROOT/$TMP"

# ─── Restore the repo-root ./dl no matter how we exit ──────────────────────
restore_dl() {
    if [ -f "$SAVED_DL" ]; then mv -f "$SAVED_DL" ./dl; else rm -f ./dl; fi
}
trap restore_dl EXIT HUP INT TERM

# ─── Build the hybrid .so + dl CLI ─────────────────────────────────────────
echo "=== zig build: libdatalog.so (100% C hybrid) + dl CLI ==="
# -Drelease => ReleaseFast (gcc -O2 semantics for the C sources).
zig build -Drelease -p zig-out --build-file zig/build.zig || {
    echo "FAIL: zig build"; exit 1;
}
[ -f "$LIBDIR/libdatalog.so" ] || { echo "FAIL: $LIBDIR/libdatalog.so missing"; exit 1; }
[ -x "$ZIGDL" ] || { echo "FAIL: $ZIGDL missing"; exit 1; }

mkdir -p "$TMP"

# ─── Shadow ./dl with the Zig-linked one for this run ──────────────────────
if [ -e ./dl ] && [ ! -L ./dl ]; then mv ./dl "$SAVED_DL"; else rm -f ./dl; fi
cp "$ZIGDL" ./dl

# ─── Suite lists (mirrors tests/run_all.sh) ────────────────────────────────
# C suites re-linked against the hybrid .so (the ABI link check).
C_SUITES="
test_m0
test_m1
test_m2
test_m3
test_m4
test_m4_review
test_bulk
test_m5
test_m5_review
test_m6
test_m6_review
test_m6_deep_review
test_m7
test_cas
test_concurrency
test_m8_magic
test_topdown
test_m9_arith
test_m9_str
test_ivm
test_bushy
test_vararity
test_lists
test_m10_rank
test_m11_range
test_m12_snap_rank
test_m13_iter
test_m14_permsel
test_m15_vmiter
test_m16_travel
test_positions
test_schema
test_typecheck
test_traverse
test_search
test_vector_storage
test_vector_cli
test_vector_search_content
test_embed
"
# Standalone C++ (no engine link) — Makefile g++ rule, kept out of the .so link.
CPP_SUITES="test_embed_math"

TOTAL=0
PASSED=0
FAILED=""

run_one() {  # run_one <name> <run-cmd...>
    name=$1; shift
    printf '=== %s ===\n' "$name"
    if "$@"; then
        PASSED=$((PASSED + 1))
    else
        rc=$?
        FAILED="$FAILED $name(rc=$rc)"
        echo ">>> $name FAILED (rc=$rc)"
    fi
    TOTAL=$((TOTAL + 1))
}

# ─── Re-link + run the C suites against the Zig hybrid .so ─────────────────
for t in $C_SUITES; do
    extra=""
    case "$t" in
        test_vector_search|test_vector_cli|test_embed) extra="-lm" ;;
    esac
    # The link is the ABI-completeness check: an unexported/missing symbol
    # fails HERE, before the suite ever runs.
    if ! gcc -O2 -Wall -Wextra -Werror -std=c11 -D_POSIX_C_SOURCE=200809L \
            -Isrc -Ivendor/dafsa "tests/$t.c" \
            -L "$LIBDIR" -ldatalog -Wl,-rpath,"$LIBDIR" \
            -o "$TMP/z$t" $extra 2> "$TMP/z$t.linklog"; then
        echo "=== LINK FAIL: $t ==="
        sed -n '1,15p' "$TMP/z$t.linklog"
        FAILED="$FAILED $t(LINK)"
        TOTAL=$((TOTAL + 1))
        continue
    fi
    LD_LIBRARY_PATH="$LIBDIR" run_one "$t" "$TMP/z$t"
done

# ─── Standalone C++ suite (no engine link, Makefile g++ rule) ──────────────
for t in $CPP_SUITES; do
    if ! g++ -O2 -Wall -Wextra -Werror -std=c++17 -Isrc \
            "tests/$t.cpp" src/embed/itq.cpp src/embed/tokenizer.cpp \
            -o "$TMP/z$t" -lm 2> "$TMP/z$t.linklog"; then
        echo "=== LINK FAIL: $t ==="
        sed -n '1,15p' "$TMP/z$t.linklog"
        FAILED="$FAILED $t(LINK)"
        TOTAL=$((TOTAL + 1))
        continue
    fi
    run_one "$t" "$TMP/z$t"
done

# ─── dl-embed: C++/ggml, conditional exactly like run_all.sh ───────────────
if [ -x ./dl-embed ]; then
    run_one "dl-embed" ./dl-embed self-test
else
    echo "=== dl-embed not built (needs vendor/ggml + model) — skipping ==="
    TOTAL=$((TOTAL + 1)); PASSED=$((PASSED + 1))
fi

# ─── CLI smoke against the Zig-linked dl ───────────────────────────────────
run_one "smoke" sh tests/smoke.sh

# ─── Summary ───────────────────────────────────────────────────────────────
echo ""
echo "ZIG HYBRID ORACLE: $PASSED/$TOTAL passed (C suites re-linked against Zig-built libdatalog.so)"
if [ -n "$FAILED" ]; then
    echo "FAILED:$FAILED"
    exit 1
fi
exit 0
