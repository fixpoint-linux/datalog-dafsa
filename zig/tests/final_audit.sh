#!/bin/bash
# final_audit.sh — FINAL nm ABI audit (post-C-removal).
#
# Asserts that the Zig-built libdatalog.so exports EVERY symbol referenced as
# undefined by the retained C consumers — which, since the C oracle removal,
# are exactly the test-suite binaries re-linked against the .so
# (build-tmp/ztest_*, produced by zig/run_zig_suites.sh — the link itself is
# the check there; here `nm -u` on each is cross-checked symbol-by-symbol),
# DATA symbols included:
#
#   missing = (union of undefined) - (Zig .so exports)
#                         - (system DSOs: libc/libm/libgcc/ld per ldd)
#                         - (the 3 canonical weak undefined ELF stubs)
# must be EMPTY.
#
# The pre-removal consumer TUs this audit used to compile — src/dl_cli.c
# (the CLI oracle, now zig/src/dl_cli.zig), dl-lsp (src/lsp.c + src/json.c,
# retired with the C engine), the still-C .so TUs src/index.c + src/vector.c
# (ported to Zig in U14), and the dlp cosmocc tool (deferred; its engine +
# dhall-c C dependencies are both removed) — are gone.  The retained C
# consumers are the 42 test suites, so their re-link IS the ABI contract.
#
# Additionally asserts the engine DATA globals C consumers reference are
# exported as OBJECTS from the Zig .so (g_bushy/g_reorder are set by
# test_bushy; vm_nomaterialize/vm_export_relid/vm_export_ts by dl.c, now
# dl.zig — pinned here so the U9 copy-relocation contract stays explicit).
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd) || exit 1
cd "$ROOT" || exit 1

SO=zig-out/lib/libdatalog.so
WORK=build-tmp/final_audit

FAIL=0
note() { echo "final_audit: $*"; }
bad()  { echo "final_audit: FAIL: $*"; FAIL=1; }

[ -f "$SO" ] || { note "$SO missing — run zig build first"; exit 1; }
ls build-tmp/ztest_* >/dev/null 2>&1 || {
    note "build-tmp/ztest_* missing — run zig/run_zig_suites.sh first (it re-links the suites)"; exit 1;
}

mkdir -p "$WORK"

# ─── Symbol sets ────────────────────────────────────────────────────────────

# Re-linked suite binaries (executables only — the glob also matches .linklog).
SUITE_BINS=$(find build-tmp -maxdepth 1 -name 'ztest_*' -type f -perm -u+x)
# Zig .so exports (functions AND data objects).
nm -D --defined-only "$SO" | awk '{print $NF}' | sort -u > "$WORK/so_exports"

# System DSO providers (whatever the re-linked suites actually need: libc,
# libm, libgcc, libstdc++ for the standalone C++ suite, ld).  nm prints
# versioned names ("waitpid@GLIBC_2.2.5"); strip the @VERSION suffix so the
# names compare equal to the unversioned undefined references.
: > "$WORK/system"
for bin in $SUITE_BINS; do
    for dso in $(ldd "$bin" 2>/dev/null | awk '$3 ~ /^\// {print $3}'); do
        nm -D --defined-only "$dso" 2>/dev/null | awk '{print $NF}' | sed 's/@.*//'
    done
done | sort -u > "$WORK/system"

# Canonical weak undefined stubs present in every ELF (provided by ld.so).
cat > "$WORK/weak_stubs" <<'EOF'
_ITM_deregisterTMCloneTable
_ITM_registerTMCloneTable
__gmon_start__
EOF

# Union of undefined symbols: every re-linked suite binary.
{
    for bin in $SUITE_BINS; do nm -u "$bin" 2>/dev/null; done
} | awk '{print $2}' | sed 's/@.*//' | sort -u > "$WORK/undefined"

# missing = undefined - so_exports - system - weak_stubs
sort -u "$WORK/so_exports" "$WORK/system" "$WORK/weak_stubs" \
    -o "$WORK/covered"
missing=$(comm -23 "$WORK/undefined" "$WORK/covered")

NUND=$(wc -l < "$WORK/undefined")
NSO=$(wc -l < "$WORK/so_exports")
NSUITES=$(echo "$SUITE_BINS" | wc -l)
note "undefined union: $NUND symbols from $NSUITES suite binaries"
note "zig .so exports: $NSO symbols"

# ─── Verdict: zero missing ──────────────────────────────────────────────────
if [ -n "$missing" ]; then
    bad "symbols undefined by the suites but missing from the Zig .so:"
    echo "$missing" | sed 's/^/    MISSING /'
else
    note "union of undefined symbols ⊆ Zig .so exports (zero missing)"
fi

# ─── DATA globals (copy-relocation contract, U9/U12) ───────────────────────
for g in g_bushy g_reorder vm_nomaterialize vm_export_relid vm_export_ts; do
    if nm -D --defined-only "$SO" | awk '$3 == "'"$g"'" && $2 ~ /^[BDR]$/ {found=1} END {exit !found}'; then
        note "DATA global exported: $g ($(nm -D --defined-only "$SO" | awk '$3 == "'"$g"'" {print $2}')"
    else
        bad "DATA global $g not exported from the Zig .so"
    fi
done

if [ "$FAIL" -eq 0 ]; then
    echo "final_audit: CLEAN — every suite-referenced symbol (incl. DATA) is exported by the Zig .so"
    exit 0
fi
echo "final_audit: FAILED"
exit 1
