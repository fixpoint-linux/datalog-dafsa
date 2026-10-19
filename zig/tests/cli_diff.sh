#!/bin/bash
# cli_diff.sh — U12 CLI golden harness: the Zig `dl` is the oracle.
#
# The C-reference `dl` (gcc oracle compiled from src/dl_cli.c + src/*.c +
# vendor/dafsa/*.c) is REMOVED.  The Zig dl (zig-out/bin/dl, built by
# zig/build.zig against the 100%-Zig libdatalog.so) is now authoritative:
# this harness runs the FULL command corpus (all 16+ commands, lifted from
# tests/*.c) against a fresh db dir and byte-compares stdout/stderr/exit code
# — plus the resulting db-dir tree manifest — against pinned goldens under
# zig/tests/golden/.  The goldens were captured from the differentially-
# verified Zig engine (the last pre-removal C-vs-Zig run was byte-green in
# both directions), so each baseline is byte-identical to what the trusted
# engine emits.
#
# The read corpus (queries re-run against a COPY of the built db) exercises
# on-disk persistence: the Zig dl re-opens a Zig-written db and must produce
# identical answers.  The C<->Zig interop phases are gone — with no second
# engine there is nothing to interoperate with; golden-vs-Zig is the check.
#
# Gate: `bash zig/tests/cli_diff.sh` from the repo root must exit 0.
# Regenerate baselines with: RECORD_GOLDEN=1 bash zig/tests/cli_diff.sh
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd) || exit 1
cd "$ROOT" || exit 1

WORK=build-tmp/cli_diff
Z_DL=zig-out/bin/dl
GOLDEN=zig/tests/golden
RECORD=0
[ "${RECORD_GOLDEN:-0}" = "1" ] && RECORD=1

FAIL=0

note() { echo "cli_diff: $*"; }
bad()  { echo "cli_diff: FAIL: $*"; FAIL=1; }

[ -x "$Z_DL" ] || { note "zig-out/bin/dl missing — run zig build first"; exit 1; }

mkdir -p "$WORK" "$GOLDEN"

# ─── Deterministic corpus fixtures ──────────────────────────────────────────
DATA=$WORK/data
rm -rf "$DATA"
mkdir -p "$DATA"

# Edge graph (traverse/tc fixtures)
cat > "$DATA/edge.csv" <<'EOF'
1,2
2,3
3,4
1,3
4,1
EOF
# 9-column row: arity cap error
cat > "$DATA/arity9.csv" <<'EOF'
1,2,3,4,5,6,7,8,9
EOF
# quoted-string observation corpus (index/search/obs fixtures)
cat > "$DATA/obs.csv" <<'EOF'
"doc1","red fox jumps over the lazy dog"
"doc2","quick brown dog walks"
"doc3","red dog barks"
"doc4","the fox is lazy"
EOF
# Datalog rule programs lifted from the test suites
cat > "$DATA/tc.dl" <<'EOF'
tc(X,Y):-edge(X,Y).
tc(X,Y):-edge(X,Z),tc(Z,Y).
EOF
cat > "$DATA/tc3.dl" <<'EOF'
tc(X,Y):-edge(X,Y).
tc(X,Y):-edge(X,Z),edge(Z,Y).
tc(X,Y):-edge(X,Z),tc(Z,Y).
EOF
cat > "$DATA/ntc.dl" <<'EOF'
ntc(X,Y):-edge(X,Y),!blocked(X,Y).
EOF
cat > "$DATA/cnt.dl" <<'EOF'
cnt(X,N):-edge(X,Y),N=count().
EOF
# list program from tests/test_lists.c
cat > "$DATA/lists.dl" <<'EOF'
r(X, H, T, A) :- p(X), L = cons(X, [7,8]), H = car(L), T = cdr(L), A = append(L, [9]).
EOF
cat > "$DATA/p.csv" <<'EOF'
1
2
EOF
# empty file: exercises the "could not determine arity" (empty-CSV) load path
: > "$DATA/empty.csv"

# ─── Corpus drivers ─────────────────────────────────────────────────────────
# run_cmd <outdir> <tag> <dbdir> <args...>: one command, capture
# stdout/stderr/rc under <outdir>/<tag>.
run_cmd() {
    local outdir=$1 tag=$2 dbdir=$3
    shift 3
    "$Z_DL" -d "$dbdir" "$@" > "$outdir/$tag.out" 2> "$outdir/$tag.err"
    echo $? > "$outdir/$tag.rc"
}

# build_corpus <dbdir> <outdir> — the FULL mutating corpus (all 16+
# commands).  Order is significant (interning order -> sym ids).
build_corpus() {
    local db=$1 out=$2
    mkdir -p "$out"

    # versions/publish/query/bound/pattern/rev/cas/txn/traverse/obs/
    # index/search + load/lookup/prefix
    run_cmd "$out" c01 "$db" versions
    run_cmd "$out" c02 "$db" load "$DATA/edge.csv" --rel edge
    run_cmd "$out" c03 "$db" lookup edge 1 2
    run_cmd "$out" c04 "$db" lookup edge 9 9
    run_cmd "$out" c05 "$db" prefix edge
    run_cmd "$out" c06 "$db" prefix edge 2
    run_cmd "$out" c07 "$db" prefix edge --raw
    run_cmd "$out" c08 "$db" prefix edge 2 --raw
    run_cmd "$out" c09 "$db" prefix nosuchrel
    run_cmd "$out" c10 "$db" load "$DATA/obs.csv" --rel observation
    run_cmd "$out" c11 "$db" index
    run_cmd "$out" c12 "$db" index extra_arg
    run_cmd "$out" c13 "$db" search "red"
    run_cmd "$out" c14 "$db" search "red fox"
    run_cmd "$out" c15 "$db" search "zzz"
    run_cmd "$out" c16 "$db" search ""
    run_cmd "$out" c17 "$db" search "!!!"
    run_cmd "$out" c18 "$db" search "red" --top 1
    run_cmd "$out" c19 "$db" search "red" --top 0
    run_cmd "$out" c20 "$db" search "red" --bogus
    run_cmd "$out" c21 "$db" search "red" --version 1
    run_cmd "$out" c22 "$db" publish
    run_cmd "$out" c23 "$db" publish --keep 3
    run_cmd "$out" c24 "$db" publish --keep
    run_cmd "$out" c25 "$db" publish bogus
    run_cmd "$out" c26 "$db" search "red" --version 1
    run_cmd "$out" c27 "$db" search "red" --version 99
    run_cmd "$out" c28 "$db" versions
    run_cmd "$out" c29 "$db" traverse 1
    run_cmd "$out" c30 "$db" traverse 1 2
    run_cmd "$out" c31 "$db" traverse 1 5
    run_cmd "$out" c32 "$db" traverse 1 --max-nodes 2
    run_cmd "$out" c33 "$db" traverse 1 2 --max-nodes 3
    run_cmd "$out" c34 "$db" traverse 1 --max-nodes 0
    run_cmd "$out" c35 "$db" traverse 1 --max-nodes
    run_cmd "$out" c36 "$db" traverse 1 -x
    run_cmd "$out" c37 "$db" obs doc1
    run_cmd "$out" c38 "$db" obs doc1 --max-obs 2
    run_cmd "$out" c39 "$db" obs nosuch
    run_cmd "$out" c40 "$db" obs doc1 --max-obs 0
    run_cmd "$out" c41 "$db" query "$DATA/tc.dl" tc
    run_cmd "$out" c42 "$db" query "tc3(X,Y):-edge(X,Y)." tc3
    run_cmd "$out" c43 "$db" query "tc(X,Y):-edge(" tc
    run_cmd "$out" c44 "$db" query "$DATA/ntc.dl" ntc
    run_cmd "$out" c45 "$db" query "$DATA/cnt.dl" cnt
    run_cmd "$out" c46 "$db" load "$DATA/p.csv" --rel p
    run_cmd "$out" c47 "$db" query "$DATA/lists.dl" r
    run_cmd "$out" c48 "$db" bound tc 1
    run_cmd "$out" c49 "$db" bound edge 1
    run_cmd "$out" c50 "$db" bound nosuchrel
    # Anchors ('^' … '$').  NOTE the edge relation below holds RAW u32 ints,
    # not interned strings, so `pattern edge '<anything>'` is "(no results)" by
    # construction (the symbol DAFSA walk can only see interned names) — c56,
    # c57 and r17 are NOT evidence about anchor handling.  c51-c55 exercise
    # anchors where they are observable: observation's interned string columns.
    run_cmd "$out" c51 "$db" pattern observation "^doc[12]$"
    run_cmd "$out" c52 "$db" pattern observation "doc[12]"
    run_cmd "$out" c53 "$db" pattern observation 1 "^.*barks$"
    run_cmd "$out" c54 "$db" pattern observation "^doc1$"
    run_cmd "$out" c55 "$db" pattern observation "doc^1"
    run_cmd "$out" c56 "$db" pattern edge "^[12]$"
    run_cmd "$out" c57 "$db" pattern edge 1 "^3$"
    run_cmd "$out" c58 "$db" pattern edge "^zzz$"
    run_cmd "$out" c59 "$db" pattern edge "["
    run_cmd "$out" c60 "$db" pattern edge
    run_cmd "$out" c61 "$db" rev e1
    run_cmd "$out" c62 "$db" cas e1 0 1
    run_cmd "$out" c63 "$db" rev e1
    run_cmd "$out" c64 "$db" cas e1 0 9
    run_cmd "$out" c65 "$db" cas e1 x y
    run_cmd "$out" c66 "$db" cas e1 1
    run_cmd "$out" c67 "$db" txn
    run_cmd "$out" c68 "$db" txn
    run_cmd "$out" c69 "$db" load "$DATA/edge.csv" --rel edge2
    run_cmd "$out" c70 "$db" load "$DATA/edge2.csv" --rel edge
    run_cmd "$out" c71 "$db" load "$DATA/missing.csv" --rel r9
    run_cmd "$out" c72 "$db" load "$DATA/arity9.csv" --rel r9
    run_cmd "$out" c73 "$db" load "$DATA/empty.csv" --rel r9
    run_cmd "$out" c94 "$db" search
    run_cmd "$out" c95 "$db" versions extra
    run_cmd "$out" c96 "$db" frobnicate
    # --with-derived is the CLI binding of DL_PUB_DERIVED=1: the snapshot gets
    # its own symbols.dafsa, so the tree manifest below proves it was written.
    run_cmd "$out" c97 "$db" publish --with-derived
}

# read_corpus <dbdir> <outdir> — the read-only persistence corpus (queries
# only: lookup/prefix/query/bound/pattern/traverse/obs/search/
# versions/rev/txn).  Run against a COPY of the fully-built db so the
# on-disk format is re-opened and re-read from scratch.
read_corpus() {
    local db=$1 out=$2
    mkdir -p "$out"
    run_cmd "$out" r01 "$db" lookup edge 1 2
    run_cmd "$out" r02 "$db" lookup edge 9 9
    run_cmd "$out" r03 "$db" prefix edge
    run_cmd "$out" r04 "$db" prefix edge --raw
    run_cmd "$out" r05 "$db" prefix tc 1
    run_cmd "$out" r06 "$db" versions
    run_cmd "$out" r07 "$db" rev e1
    run_cmd "$out" r08 "$db" traverse 1 2
    run_cmd "$out" r09 "$db" obs doc3
    run_cmd "$out" r10 "$db" search "red fox"
    run_cmd "$out" r11 "$db" search "dog" --top 2
    run_cmd "$out" r12 "$db" search "red" --version 1
    run_cmd "$out" r13 "$db" query "tc(X,Y):-edge(X,Y)." tc
    run_cmd "$out" r14 "$db" bound tc 1
    run_cmd "$out" r17 "$db" pattern edge "^[12]$"
    run_cmd "$out" r18 "$db" pattern observation "^doc[12]$"
    run_cmd "$out" r19 "$db" pattern observation "doc^1"
    run_cmd "$out" r20 "$db" txn
}

# tree_manifest <dir> — file list + sha256 of every regular file AND symlink
# (deterministic order), so db-dir trees compare byte-for-byte.  `find -L` and
# `-type f -o -type l` ensure a symlinked db file is hashed, not silently
# ignored; broken symlinks hash the link target path via sha256sum's own read.
tree_manifest() {
    local dir=$1
    ( cd "$dir" && find . \( -type f -o -type l \) -print0 | sort -z | xargs -0 sha256sum ) 2>/dev/null
    ( cd "$dir" && find . -type d | sort )
}

# normalize — the only machine/path-dependent byte in the whole corpus is
# usage(), which echoes argv[0] (`zig-out/bin/dl`); map it to @PROG@ so the
# goldens are independent of the binary's relative path.  Everything else in
# the output (db dir, data paths) is already repo-relative and stable.
normalize() {
    sed -e "s|$Z_DL|@PROG@|g"
}

# ─── Run the Zig dl over the full corpus + read corpus ─────────────────────
note "running the Zig dl (oracle) over the full corpus"
rm -rf "$WORK/db" "$WORK/out" "$WORK/db_read" "$WORK/outr"
build_corpus "$WORK/db" "$WORK/out"
cp -r "$WORK/db" "$WORK/db_read"
read_corpus "$WORK/db_read" "$WORK/outr"
tree_manifest "$WORK/db" > "$WORK/tree.manifest"

NC=$(ls "$WORK/out"/*.out | wc -l)
NR=$(ls "$WORK/outr"/*.out | wc -l)
note "  $NC full-corpus + $NR persistence commands"

# ─── Record or verify goldens ──────────────────────────────────────────────
if [ "$RECORD" = 1 ]; then
    note "RECORD_GOLDEN=1: writing goldens to $GOLDEN"
    rm -rf "$GOLDEN"
    mkdir -p "$GOLDEN"
    for d in "$WORK/out" "$WORK/outr"; do
        for f in "$d"/*; do
            b=$(basename "$f")
            case "$b" in
                *.out|*.err) normalize < "$f" > "$GOLDEN/$b" ;;
                *.rc)        cp "$f" "$GOLDEN/$b" ;;
            esac
        done
    done
    cp "$WORK/tree.manifest" "$GOLDEN/tree.manifest"
    note "recorded $(find "$GOLDEN" -type f | wc -l) golden files"
    exit 0
fi

# verify: for every golden artifact, run the same tag and byte-compare.
for g in "$GOLDEN"/*.out "$GOLDEN"/*.err "$GOLDEN"/*.rc; do
    [ -e "$g" ] || continue
    b=$(basename "$g")
    tag=${b%.*}
    case "$b" in
        *.out|*.err)
            rf="$WORK/out/$b"
            [ -f "$rf" ] || rf="$WORK/outr/$b"
            if [ ! -f "$rf" ]; then bad "$tag missing run output"; continue; fi
            normalize < "$rf" > "$WORK/$b.n"
            if ! cmp -s "$g" "$WORK/$b.n"; then
                bad "$tag ${b##*.} differs from golden"
                diff "$g" "$WORK/$b.n" | head -5
            fi
            ;;
        *.rc)
            rf="$WORK/out/$b"
            [ -f "$rf" ] || rf="$WORK/outr/$b"
            if [ ! -f "$rf" ]; then bad "$tag missing run rc"; continue; fi
            if ! cmp -s "$g" "$rf"; then
                bad "$tag exit status differs from golden: $(cat "$rf") vs $(cat "$g")"
            fi
            ;;
    esac
done

if ! cmp -s "$GOLDEN/tree.manifest" "$WORK/tree.manifest"; then
    bad "db-dir tree differs from golden"
    diff "$GOLDEN/tree.manifest" "$WORK/tree.manifest" | head -20
fi

# ─── Verdict ────────────────────────────────────────────────────────────────
if [ "$FAIL" -eq 0 ]; then
    echo "cli_diff: BYTE-GREEN golden mode ($NC full-corpus + $NR persistence commands; stdout/stderr/rc + db tree identical to goldens)"
    exit 0
fi
echo "cli_diff: FAILED"
exit 1
