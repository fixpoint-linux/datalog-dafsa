#!/bin/bash
# final_audit.sh — U12 FINAL nm ABI audit.
#
# Asserts that the Zig-built hybrid libdatalog.so exports EVERY symbol
# referenced as undefined by the retained C consumers, DATA symbols included:
#
#   - the test-suite binaries re-linked against the .so (build-tmp/ztest_*,
#     produced by zig/run_zig_suites.sh — the link itself is the check there;
#     here `nm -u` on each is cross-checked symbol-by-symbol),
#   - src/dl_cli.c (the CLI oracle; in the migration end-state it links the
#     .so — its Zig replacement already does),
#   - dl-lsp (src/lsp.c + src/json.c + vendor/yyjson),
#   - dlp (dlp/*.c, the cosmocc tool, with the dhall-c core it links),
#   - the still-C .so TUs src/index.c and src/vector.c (deferred to U14),
#
# missing = (union of undefined) - (Zig .so exports)
#                         - (definitions that legitimately live OUTSIDE the
#                            .so: the consumers' own TUs, yyjson, dhall-c core)
#                         - (system DSOs: libc/libm/libgcc/ld per ldd)
#                         - (the 3 canonical weak undefined ELF stubs)
# must be EMPTY.
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
DHALLC=${DHALLC:-../dhall-c}

FAIL=0
note() { echo "final_audit: $*"; }
bad()  { echo "final_audit: FAIL: $*"; FAIL=1; }

[ -f "$SO" ] || { note "$SO missing — run zig build first"; exit 1; }
ls build-tmp/ztest_* >/dev/null 2>&1 || {
    note "build-tmp/ztest_* missing — run zig/run_zig_suites.sh first (it re-links the suites)"; exit 1;
}

mkdir -p "$WORK"

CFLAGS="-O2 -Wall -Wextra -Werror -std=c11 -D_POSIX_C_SOURCE=200809L -Isrc -Ivendor/dafsa"

# ─── Compile the consumer TUs to .o ─────────────────────────────────────────
# gcc side: engine consumers + LSP + the still-C .so TUs.
cc_err=$WORK/cc.err
for src in src/dl_cli.c src/lsp.c src/json.c src/index.c src/vector.c \
           vendor/yyjson/yyjson.c; do
    obj=$WORK/$(basename "${src%.c}").o
    if ! gcc $CFLAGS -c "$src" -o "$obj" 2> "$cc_err"; then
        note "gcc -c failed: $src"; sed -n '1,5p' "$cc_err"; exit 1
    fi
done

# cosmocc side: dlp (links the engine + dhall-c core; NOT part of any .so).
COSMOCC=${COSMOCC:-cosmocc}
DLP_OBJS=""
if command -v "$COSMOCC" >/dev/null 2>&1 && [ -d "$DHALLC/src" ]; then
    DLP_CFLAGS="-std=c11 -O2 -Wall -Wextra -I$DHALLC/src -Ivendor/dafsa -Isrc"
    for src in dlp/main.c dlp/schema_load.c dlp/init.c dlp/csv_load.c \
               dlp/json_load.c dlp/workflow.c dlp/schema_check.c \
               "$DHALLC"/src/arena.c "$DHALLC"/src/lexer.c "$DHALLC"/src/parser.c \
               "$DHALLC"/src/ast.c "$DHALLC"/src/normalize.c "$DHALLC"/src/typecheck.c \
               "$DHALLC"/src/builtins.c "$DHALLC"/src/serialize.c "$DHALLC"/src/import.c \
               "$DHALLC"/src/bignum.c "$DHALLC"/src/sha256.c "$DHALLC"/src/ssrf.c \
               "$DHALLC"/src/http.c; do
        obj=$WORK/dlp_$(basename "${src%.c}").o
        if ! "$COSMOCC" $DLP_CFLAGS -c "$src" -o "$obj" 2> "$cc_err"; then
            note "cosmocc -c failed: $src"; sed -n '1,5p' "$cc_err"; exit 1
        fi
        DLP_OBJS="$DLP_OBJS $obj"
    done
else
    bad "dlp not audited: cosmocc or $DHALLC unavailable"
fi

# ─── Symbol sets ────────────────────────────────────────────────────────────

# Re-linked suite binaries (executables only — the glob also matches .linklog).
SUITE_BINS=$(find build-tmp -maxdepth 1 -name 'ztest_*' -type f -perm -u+x)
# Zig .so exports (functions AND data objects).
nm -D --defined-only "$SO" | awk '{print $NF}' | sort -u > "$WORK/so_exports"

# Definitions that legitimately live OUTSIDE the .so (consumer-side TUs).
# NOTE: index.o/vector.o defines are deliberately NOT here — they are .so
# members, and their symbols MUST be covered by the .so exports.
: > "$WORK/consumer_defs"
for obj in "$WORK/dl_cli.o" "$WORK/lsp.o" "$WORK/json.o" "$WORK/yyjson.o" $DLP_OBJS; do
    nm --defined-only "$obj" 2>/dev/null | awk '$2 ~ /^[A-Z]$/ {print $3}' >> "$WORK/consumer_defs"
done
sort -u "$WORK/consumer_defs" -o "$WORK/consumer_defs"

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

# Cosmopolitan-libc providers for the cosmocc-compiled dlp objects: cosmo
# compiles errno/flag macros into real data-symbol references (EAGAIN,
# CLOCK_MONOTONIC, POLLIN, ...) resolved from libcosmo.a at cosmo link time
# (note: libc.a is an 8-byte stub; libcosmo.a is the actual libc archive).
COSMO_LIB=${COSMO_LIB:-$(dirname "$(command -v "$COSMOCC" 2>/dev/null)")/../x86_64-linux-cosmo/lib/libcosmo.a}
if [ -n "${COSMOCC##*/*}" ] && command -v "$COSMOCC" >/dev/null 2>&1 && [ -f "$COSMO_LIB" ]; then
    nm --defined-only "$COSMO_LIB" 2>/dev/null | awk '$2 ~ /^[TDBR]$/ {print $3}' | sed 's/@.*//' >> "$WORK/system"
    sort -u "$WORK/system" -o "$WORK/system"
fi

# Canonical weak undefined stubs present in every ELF (provided by ld.so).
cat > "$WORK/weak_stubs" <<'EOF'
_ITM_deregisterTMCloneTable
_ITM_registerTMCloneTable
__gmon_start__
EOF

# Re-linked suite binaries (executables only — the glob also matches .linklog).

# Union of undefined symbols: every re-linked suite binary + every consumer .o.
{
    for bin in $SUITE_BINS; do nm -u "$bin" 2>/dev/null; done
    for obj in "$WORK"/*.o; do nm -u "$obj" 2>/dev/null; done
} | awk '{print $2}' | sed 's/@.*//' | sort -u > "$WORK/undefined"

# missing = undefined - so_exports - consumer_defs - system - weak_stubs
sort -u "$WORK/so_exports" "$WORK/consumer_defs" "$WORK/system" "$WORK/weak_stubs" \
    -o "$WORK/covered"
missing=$(comm -23 "$WORK/undefined" "$WORK/covered")

NUND=$(wc -l < "$WORK/undefined")
NSO=$(wc -l < "$WORK/so_exports")
NSUITES=$(echo "$SUITE_BINS" | wc -l)
note "undefined union: $NUND symbols from $NSUITES suite binaries + $(ls "$WORK"/*.o | wc -l) consumer .o files"
note "zig .so exports: $NSO symbols"

# ─── Per-consumer coverage report ───────────────────────────────────────────
covered_count() {  # covered_count <undefined-set file>
    comm -23 "$1" "$WORK/covered" | wc -l
}
u_bin=$WORK/.ubin; u_cli=$WORK/.ucli; u_lsp=$WORK/.ulsp; u_dlp=$WORK/.udlp; u_idx=$WORK/.uidx
{
    for bin in $SUITE_BINS; do nm -u "$bin" 2>/dev/null; done
} | awk '{print $2}' | sed 's/@.*//' | sort -u > "$u_bin"
nm -u "$WORK/dl_cli.o" | awk '{print $2}' | sed 's/@.*//' | sort -u > "$u_cli"
# (nm each object — a cat'ed blob would expose only its first member)
{
    nm -u "$WORK/lsp.o"; nm -u "$WORK/json.o"; nm -u "$WORK/yyjson.o"
} | awk '{print $2}' | sed 's/@.*//' | sort -u > "$u_lsp"
if [ -n "$DLP_OBJS" ]; then
    for obj in $DLP_OBJS; do nm -u "$obj" 2>/dev/null; done \
        | awk '{print $2}' | sed 's/@.*//' | sort -u > "$u_dlp"
fi
nm -u "$WORK/index.o" "$WORK/vector.o" | awk '{print $2}' | sed 's/@.*//' | sort -u > "$u_idx"
note "covered: suites(undef $(wc -l < "$u_bin")) dl_cli(undef $(wc -l < "$u_cli")) dl-lsp(undef $(wc -l < "$u_lsp")) dlp(undef $(wc -l < "$u_dlp" 2>/dev/null || echo 0)) index+vector(undef $(wc -l < "$u_idx"))"

# ─── Verdict: zero missing ──────────────────────────────────────────────────
if [ -n "$missing" ]; then
    bad "symbols undefined by consumers but missing from the Zig .so:"
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
    echo "final_audit: CLEAN — every consumer-referenced symbol (incl. DATA) is exported by the Zig .so"
    exit 0
fi
echo "final_audit: FAILED"
exit 1
