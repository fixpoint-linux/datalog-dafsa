#!/bin/sh
# abi_audit.sh — U1 ABI-completeness audit.
#
# Checks the dynamic exports of the Zig-built hybrid libdatalog.so against
#   (a) the gcc-built reference ./libdatalog.so (ABI regression check), and
#   (b) the union of extern C functions declared in the engine headers
#       (src/*.h of every TU in the .so + vendor/dafsa/*.h).
# FAILS if a reference export is missing from the Zig .so, or if a
# header-declared function is missing — unless the C reference .so also lacks
# it (declared-but-static in the C engine; pre-existing, not a migration
# regression) or it is listed in ALLOWED_MISSING below.
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd) || exit 1
cd "$ROOT" || exit 1

SO=zig-out/lib/libdatalog.so
REF_SO=libdatalog.so   # gcc-built reference from `make`

# Headers of every TU linked into the .so.  (No iter.h exists; json.h is the
# LSP-only TU; embed headers are the dl-embed tier — none are in this .so.)
HEADERS="
src/dl.h
src/dl_internal.h
src/intern.h
src/termstore.h
src/relation.h
src/vrelation.h
src/tupleset.h
src/parser.h
src/compiler.h
src/vm.h
src/snapshot.h
src/regexwalk.h
src/permindex.h
src/util.h
src/magic.h
src/topdown.h
src/analyze.h
src/schema.h
src/typecheck.h
src/txnwal.h
src/index.h
src/vector.h
vendor/dafsa/dafsa.h
vendor/dafsa/dafsa_internal.h
"

# Declared in a header but intentionally not exported (must carry a reason).
# dafsa_check_invariants (dafsa_internal.h:171) and reg_lookup_no_count
# (dafsa_internal.h:196) are declared in the vendor header but defined
# static in the vendor C — absent from the gcc-built .so too (pre-existing).
ALLOWED_MISSING="dafsa_check_invariants reg_lookup_no_count"

[ -f "$SO" ] || { echo "abi_audit: $SO missing — run zig build first"; exit 1; }

mkdir -p build-tmp

# Comment-strip every header (no macro expansion, no #include follow), then
# extract candidate function names with real tokenization.
for h in $HEADERS; do
    [ -f "$h" ] || { echo "abi_audit: header $h missing"; exit 1; }
    gcc -fpreprocessed -dD -E -P -Isrc -Ivendor/dafsa "$h"
done | python3 -c '
import re, sys
text = sys.stdin.read()
# drop preprocessor lines (incl. continuations — gcc already merged them,
# but be safe)
text = re.sub(r"(?m)^[ \t]*#.*(?:\\\n.*)*", "", text)
# remove __attribute__((...)) with balanced parens
out, i = [], 0
while True:
    j = text.find("__attribute__", i)
    if j < 0:
        out.append(text[i:]); break
    out.append(text[i:j])
    k = text.find("(", j)
    depth, k2 = 1, k + 1
    while k2 < len(text) and depth:
        if text[k2] == "(": depth += 1
        elif text[k2] == ")": depth -= 1
        k2 += 1
    i = k2
text = "".join(out)
# remove static function definitions/declarations (header-only helpers with
# bodies would otherwise leak calls/casts like memcmp( / (void)( into the
# candidate set)
while True:
    m = re.search(r"\bstatic\b", text)
    if not m:
        break
    semi, brace = text.find(";", m.start()), text.find("{", m.start())
    if 0 <= semi and (brace < 0 or semi < brace):
        text = text[:m.start()] + text[semi + 1:]
    elif 0 <= brace:
        depth, k = 1, brace + 1
        while k < len(text) and depth:
            if text[k] == "{": depth += 1
            elif text[k] == "}": depth -= 1
            k += 1
        text = text[:m.start()] + text[k:]
    else:
        break
KEYWORDS = {"if", "while", "for", "switch", "return", "sizeof", "defined",
            "_Static_assert", "offsetof"}
names = set()
for m in re.finditer(r"\b([A-Za-z_]\w*)[ \t]*\(", text):
    name = m.group(1)
    if name in KEYWORDS or name == "__attribute__":
        continue
    # function-pointer declarator:  ( * name )  (
    before = text[:m.start(1)].rstrip()
    if before.endswith("*"):
        stripped = before[:-1].rstrip()
        if stripped.endswith("("):
            continue
    # return type of a function-pointer typedef:  int  ( * name )  (
    after = text[m.end(0):].lstrip()
    if after.startswith("*"):
        continue
    names.add(name)
print("\n".join(sorted(names)))
' > build-tmp/.abi_expected

nm -D --defined-only "$SO" | awk '{print $NF}' | sort -u > build-tmp/.abi_exported
if [ -f "$REF_SO" ]; then
    nm -D --defined-only "$REF_SO" | awk '{print $NF}' | sort -u > build-tmp/.abi_ref
    REFCOUNT=$(wc -l < build-tmp/.abi_ref)
else
    : > build-tmp/.abi_ref
    REFCOUNT="absent"
fi

TOTAL=$(wc -l < build-tmp/.abi_expected)
NEXP=$(wc -l < build-tmp/.abi_exported)
echo "abi_audit: zig .so exports $NEXP symbols; headers declare $TOTAL functions; gcc reference .so: $REFCOUNT exports"

fail=0

# (a) ABI regression: every reference export must exist in the Zig .so.
REGRESSED=$(comm -13 build-tmp/.abi_exported build-tmp/.abi_ref)
for s in $REGRESSED; do
    echo "REGRESSION vs C .so: missing export $s"
    fail=1
done

# (b) header completeness.
for fn in $(cat build-tmp/.abi_expected); do
    grep -q -x "$fn" build-tmp/.abi_exported && continue
    if [ -s build-tmp/.abi_ref ] && grep -q -x "$fn" build-tmp/.abi_ref; then
        echo "MISSING EXPORT: $fn"
        fail=1
        continue
    fi
    # C reference lacks it too: pre-existing declared-but-static — only OK
    # if acknowledged.
    ok=0
    for allowed in $ALLOWED_MISSING; do
        [ "$fn" = "$allowed" ] && ok=1
    done
    if [ "$ok" -eq 0 ]; then
        echo "MISSING EXPORT: $fn (also absent from C reference .so; add to ALLOWED_MISSING with a reason or investigate)"
        fail=1
    fi
done

if [ "$fail" -ne 0 ]; then
    echo "abi_audit: FAILED"
    exit 1
fi
echo "abi_audit: CLEAN — Zig .so exports cover the C reference and every header-declared function"
exit 0
