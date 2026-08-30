#!/bin/bash
# cli_diff.sh — U12 CLI byte-diff harness: C `dl` (gcc oracle) vs Zig `dl`.
#
# Builds TWO executables from the SAME src/dl_cli.c-free split:
#   - the C-reference dl, compiled by gcc from src/dl_cli.c + the full C
#     engine (the Makefile `dl` link, i.e. the migration oracle), and
#   - the Zig dl from zig/src/dl_cli.zig linked against zig-out/lib/
#     libdatalog.so (the strangler hybrid).
#
# Both run an IDENTICAL command corpus (all 16+ commands; Datalog programs
# lifted from tests/*.c) against SEPARATE db dirs with IDENTICAL inputs.
# Gates:
#   1. stdout BYTES identical for every command (and stderr + exit status);
#   2. the resulting db-dir TREES byte-identical (rels.txt, symbols.dafsa,
#      symbols.txt, <rel>.dafsa/.wal/.base.dafsa, terms.bin, txn.wal,
#      snapshots/N/*, CURRENT, LOCK — everything);
#   3. INTEROP both directions: the C-written db is opened/read by the Zig dl
#      (and vice versa) over a read/query corpus, outputs + resulting trees
#      byte-identical.
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd) || exit 1
cd "$ROOT" || exit 1

WORK=build-tmp/cli_diff
Z_DL=zig-out/bin/dl
C_DL=$WORK/dl_c
LIB_SRC="src/intern.c src/termstore.c src/relation.c src/vrelation.c src/tupleset.c \
src/parser.c src/compiler.c src/vm.c src/snapshot.c src/regexwalk.c src/permindex.c \
src/util.c src/dl.c src/iter.c src/magic.c src/topdown.c src/analyze.c src/schema.c \
src/typecheck.c src/txnwal.c src/index.c src/vector.c \
vendor/dafsa/dafsa.c vendor/dafsa/dafsa_state.c vendor/dafsa/dafsa_core.c \
vendor/dafsa/dafsa_persist.c vendor/dafsa/dafsa_view.c vendor/dafsa/dafsa_crc32.c \
vendor/dafsa/dafsa_wal.c vendor/dafsa/dafsa_build.c vendor/dafsa/dafsa_rank.c \
vendor/dafsa/dafsa_view_rank.c"

FAIL=0

note() { echo "cli_diff: $*"; }
bad()  { echo "cli_diff: FAIL: $*"; FAIL=1; }

[ -x "$Z_DL" ] || { note "zig-out/bin/dl missing — run zig build first"; exit 1; }

mkdir -p "$WORK"

# ─── Build the C-reference dl (gcc, static, Makefile `dl` link) ────────────
if [ ! -x "$C_DL" ] || [ src/dl_cli.c -nt "$C_DL" ]; then
    note "building C-reference dl (gcc oracle) -> $C_DL"
    gcc -O2 -Wall -Wextra -Werror -std=c11 -D_POSIX_C_SOURCE=200809L \
        -Isrc -Ivendor/dafsa -o "$C_DL" src/dl_cli.c $LIB_SRC || {
        note "gcc build of the C-reference dl failed"; exit 1;
    }
fi

# ─── Deterministic corpus fixtures ──────────────────────────────────────────
DATA=$WORK/data
rm -rf "$DATA"
mkdir -p "$DATA"

# Edge graph (traverse/tc fixtures from tests/test_topdown.c / test_m8_magic.c)
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

# Vector fixtures: deterministic entity/__sig{j}__/__vec_q__ corpora +
# a query sig/ivec hex pair (mirrors tests/test_vector_cli.c storage layout).
# Python writes the CSVs and emits the hex strings on stdout.
VGEN=$WORK/vgen.sh
python3 - "$DATA" > "$WORK/vhex.env" <<'PYEOF'
import sys, os
data = sys.argv[1]
def pack4(b0,b1,b2,b3):
    return (b0 & 0xFF) | ((b1 & 0xFF) << 8) | ((b2 & 0xFF) << 16) | ((b3 & 0xFF) << 24)
def band(sig, j):  # vector.c band layout: 16-bit slices, MSB half first
    return (sig[j // 2] >> ((1 - (j % 2)) * 16)) & 0xFFFF
# query signature: fixed 256 bits, entity sigs differ in <=1 bit per band so
# radius 16 (pigeonhole budget 1) retrieves every entity
q_sig = [0x01234567, 0x89ABCDEF, 0xFEDCBA98, 0x76543210,
         0x0F1E2D3C, 0x4B5A6978, 0x8899AABB, 0xCCDDEEFF]
ent_sig = []
for k in range(3):
    s = list(q_sig)
    s[k] ^= (1 << ((k * 5) % 32))     # flip one bit in one band-half word
    ent_sig.append(s)
def ivec_close(seed):
    v = []
    x = seed
    for i in range(96):
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        base = (x % 60) - 30
        v.append([base, min(127, base + 3), max(-128, base - 2), base] if False else
                 [max(-128, min(127, base)), max(-128, min(127, base + 1)),
                  max(-128, min(127, base - 1)), max(-128, min(127, base + 2))])
    return v
q_ivec = ivec_close(7)
ent_ivec = [ivec_close(101), ivec_close(202), ivec_close(303)]

with open(os.path.join(data, "vec_entity.csv"), "w") as f:
    for k in range(3):
        f.write("%d,%d\n" % (k + 1, 4))          # (name_sym, type_sym) raw ints
for j in range(16):
    with open(os.path.join(data, "vec_sig%d.csv" % j), "w") as f:
        for k in range(3):
            f.write("%d,%d\n" % (band(ent_sig[k], j), k + 1))
with open(os.path.join(data, "vec_q.csv"), "w") as f:
    for k in range(3):
        for c in range(96):
            f.write("%d,%d,%d\n" % (k + 1, c, pack4(*ent_ivec[k][c])))

sig_hex = "".join("%08x" % (w & 0xFFFFFFFF) for w in q_sig)
q_ivec_words = [pack4(*q_ivec[c]) for c in range(96)]
ivec_hex = "".join("%08x" % (w & 0xFFFFFFFF) for w in q_ivec_words)
print("QSIG=%s" % sig_hex)
print("QIVEC=%s" % ivec_hex)
PYEOF
QSIG=$(sed -n 's/^QSIG=//p' "$WORK/vhex.env")
QIVEC=$(sed -n 's/^QIVEC=//p' "$WORK/vhex.env")
[ -n "$QSIG" ] && [ -n "$QIVEC" ] || { note "vector fixture generation failed"; exit 1; }

# ─── Corpus drivers ─────────────────────────────────────────────────────────
# run_cmd <outdir> <tag> <dl> <dbdir> <args...>: one command, capture
# stdout/stderr/rc under <outdir>/<tag>.
n_cmd=0
run_cmd() {
    local outdir=$1 tag=$2 dl=$3 dbdir=$4
    shift 4
    "$dl" -d "$dbdir" "$@" > "$outdir/$tag.out" 2> "$outdir/$tag.err"
    echo $? > "$outdir/$tag.rc"
    n_cmd=$((n_cmd + 1))
}

# build_corpus <dl> <dbdir> <outdir> — the FULL mutating corpus (all 16+
# commands).  Order is significant (interning order -> sym ids) and identical
# for both binaries.
build_corpus() {
    local dl=$1 db=$2 out=$3
    mkdir -p "$out"

    # versions/publish/query/qmagic/bound/pattern/rev/cas/txn/traverse/obs/
    # index/search/vsearch/vhybrid + load/lookup/prefix
    run_cmd "$out" c01 "$dl" "$db" versions
    run_cmd "$out" c02 "$dl" "$db" load "$DATA/edge.csv" --rel edge
    run_cmd "$out" c03 "$dl" "$db" lookup edge 1 2
    run_cmd "$out" c04 "$dl" "$db" lookup edge 9 9
    run_cmd "$out" c05 "$dl" "$db" prefix edge
    run_cmd "$out" c06 "$dl" "$db" prefix edge 2
    run_cmd "$out" c07 "$dl" "$db" prefix edge --raw
    run_cmd "$out" c08 "$dl" "$db" prefix edge 2 --raw
    run_cmd "$out" c09 "$dl" "$db" prefix nosuchrel
    run_cmd "$out" c10 "$dl" "$db" load "$DATA/obs.csv" --rel observation
    run_cmd "$out" c11 "$dl" "$db" index
    run_cmd "$out" c12 "$dl" "$db" index extra_arg
    run_cmd "$out" c13 "$dl" "$db" search "red"
    run_cmd "$out" c14 "$dl" "$db" search "red fox"
    run_cmd "$out" c15 "$dl" "$db" search "zzz"
    run_cmd "$out" c16 "$dl" "$db" search ""
    run_cmd "$out" c17 "$dl" "$db" search "!!!"
    run_cmd "$out" c18 "$dl" "$db" search "red" --top 1
    run_cmd "$out" c19 "$dl" "$db" search "red" --top 0
    run_cmd "$out" c20 "$dl" "$db" search "red" --bogus
    run_cmd "$out" c21 "$dl" "$db" search "red" --version 1
    run_cmd "$out" c22 "$dl" "$db" publish
    run_cmd "$out" c23 "$dl" "$db" publish --keep 3
    run_cmd "$out" c24 "$dl" "$db" publish --keep
    run_cmd "$out" c25 "$dl" "$db" publish bogus
    run_cmd "$out" c26 "$dl" "$db" search "red" --version 1
    run_cmd "$out" c27 "$dl" "$db" search "red" --version 99
    run_cmd "$out" c28 "$dl" "$db" versions
    run_cmd "$out" c29 "$dl" "$db" traverse 1
    run_cmd "$out" c30 "$dl" "$db" traverse 1 2
    run_cmd "$out" c31 "$dl" "$db" traverse 1 5
    run_cmd "$out" c32 "$dl" "$db" traverse 1 --max-nodes 2
    run_cmd "$out" c33 "$dl" "$db" traverse 1 2 --max-nodes 3
    run_cmd "$out" c34 "$dl" "$db" traverse 1 --max-nodes 0
    run_cmd "$out" c35 "$dl" "$db" traverse 1 --max-nodes
    run_cmd "$out" c36 "$dl" "$db" traverse 1 -x
    run_cmd "$out" c37 "$dl" "$db" obs doc1
    run_cmd "$out" c38 "$dl" "$db" obs doc1 --max-obs 2
    run_cmd "$out" c39 "$dl" "$db" obs nosuch
    run_cmd "$out" c40 "$dl" "$db" obs doc1 --max-obs 0
    run_cmd "$out" c41 "$dl" "$db" query "$DATA/tc.dl" tc
    run_cmd "$out" c42 "$dl" "$db" query "tc3(X,Y):-edge(X,Y)." tc3
    run_cmd "$out" c43 "$dl" "$db" query "tc(X,Y):-edge(" tc
    run_cmd "$out" c44 "$dl" "$db" query "$DATA/ntc.dl" ntc
    run_cmd "$out" c45 "$dl" "$db" query "$DATA/cnt.dl" cnt
    run_cmd "$out" c46 "$dl" "$db" load "$DATA/p.csv" --rel p
    run_cmd "$out" c47 "$dl" "$db" query "$DATA/lists.dl" r
    run_cmd "$out" c48 "$dl" "$db" bound tc 1
    run_cmd "$out" c49 "$dl" "$db" bound edge 1
    run_cmd "$out" c50 "$dl" "$db" bound nosuchrel
    run_cmd "$out" c51 "$dl" "$db" qmagic "$DATA/tc.dl" tc 1
    run_cmd "$out" c52 "$dl" "$db" qmagic "$DATA/tc3.dl" tc 1
    run_cmd "$out" c53 "$dl" "$db" qmagic "$DATA/tc.dl" tc -a bf 1
    run_cmd "$out" c54 "$dl" "$db" qmagic "$DATA/tc.dl" tc -a xx 1
    run_cmd "$out" c55 "$dl" "$db" qmagic "$DATA/ntc.dl" ntc 2
    run_cmd "$out" c56 "$dl" "$db" pattern edge "^[12]$"
    run_cmd "$out" c57 "$dl" "$db" pattern edge 1 "^3$"
    run_cmd "$out" c58 "$dl" "$db" pattern edge "^zzz$"
    run_cmd "$out" c59 "$dl" "$db" pattern edge "["
    run_cmd "$out" c60 "$dl" "$db" pattern edge
    run_cmd "$out" c61 "$dl" "$db" rev e1
    run_cmd "$out" c62 "$dl" "$db" cas e1 0 1
    run_cmd "$out" c63 "$dl" "$db" rev e1
    run_cmd "$out" c64 "$dl" "$db" cas e1 0 9
    run_cmd "$out" c65 "$dl" "$db" cas e1 x y
    run_cmd "$out" c66 "$dl" "$db" cas e1 1
    run_cmd "$out" c67 "$dl" "$db" txn
    run_cmd "$out" c68 "$dl" "$db" txn
    run_cmd "$out" c69 "$dl" "$db" load "$DATA/edge.csv" --rel edge2
    run_cmd "$out" c70 "$dl" "$db" load "$DATA/edge2.csv" --rel edge
    run_cmd "$out" c71 "$dl" "$db" load "$DATA/missing.csv" --rel r9
    run_cmd "$out" c72 "$dl" "$db" load "$DATA/arity9.csv" --rel r9
    run_cmd "$out" c73 "$dl" "$db" load "$DATA/empty.csv" --rel r9
    # vector tier: entity names interned in a fixed order (sym ids 5..7),
    # then raw-int CSV loads for entity/__sig{j}__/__vec_q__
    run_cmd "$out" c74 "$dl" "$db" lookup entity ent0 doc
    run_cmd "$out" c75 "$dl" "$db" lookup entity ent1 doc
    run_cmd "$out" c76 "$dl" "$db" lookup entity ent2 doc
    run_cmd "$out" c77 "$dl" "$db" load "$DATA/vec_entity.csv" --rel entity
    for j in $(seq 0 15); do
        run_cmd "$out" "c78s$j" "$dl" "$db" load "$DATA/vec_sig$j.csv" --rel "__sig${j}__"
    done
    run_cmd "$out" c79 "$dl" "$db" load "$DATA/vec_q.csv" --rel __vec_q__
    run_cmd "$out" c80 "$dl" "$db" vsearch "q" --sig "$QSIG" --ivec "$QIVEC" --k 10 --radius 16
    run_cmd "$out" c81 "$dl" "$db" vsearch "q" --sig "$QSIG" --ivec "$QIVEC" --k 1
    run_cmd "$out" c82 "$dl" "$db" vsearch "q" --sig "$QSIG" --ivec "$QIVEC" --cand-only
    run_cmd "$out" c83 "$dl" "$db" vsearch "q" --sig "$QSIG" --ivec "$QIVEC" --radius 0
    run_cmd "$out" c84 "$dl" "$db" vsearch "q" --sig "$QSIG"
    run_cmd "$out" c85 "$dl" "$db" vsearch "q" --sig "zz" --ivec "$QIVEC"
    run_cmd "$out" c86 "$dl" "$db" vsearch "q" --ivec "$QIVEC"
    run_cmd "$out" c87 "$dl" "$db" vsearch "q" --k 0
    run_cmd "$out" c88 "$dl" "$db" vsearch "q" --sig "$QSIG" --ivec "$QIVEC" --version 1
    run_cmd "$out" c89 "$dl" "$db" vhybrid "red" "q" --sig "$QSIG" --ivec "$QIVEC"
    run_cmd "$out" c90 "$dl" "$db" vhybrid "zzz" "q" --sig "$QSIG" --ivec "$QIVEC"
    run_cmd "$out" c91 "$dl" "$db" vhybrid "red" "q" --sig "$QSIG" --ivec "$QIVEC" --version 1
    run_cmd "$out" c92 "$dl" "$db" vhybrid "red"
    # error paths
    run_cmd "$out" c93 "$dl" "$db" vsearch "q" --sig "$QSIG" --ivec "$QIVEC" --radius -1
    run_cmd "$out" c94 "$dl" "$db" search
    run_cmd "$out" c95 "$dl" "$db" versions extra
    run_cmd "$out" c96 "$dl" "$db" frobnicate
    # encode-helper path (fork+execv of ./dl-embed): no ITQ basis in this db,
    # so the helper fails fast and identically for both binaries
    run_cmd "$out" c97 "$dl" "$db" vsearch "test query"
    run_cmd "$out" c98 "$dl" "$db" vhybrid "red" "test query"
}

# read_corpus <dl> <dbdir> <outdir> — the read-only INTEROP corpus (queries
# only: lookup/prefix/query/qmagic/bound/pattern/traverse/obs/search/vsearch/
# vhybrid/versions/rev).  Run against COPIES of a fully-built db.
read_corpus() {
    local dl=$1 db=$2 out=$3
    mkdir -p "$out"
    run_cmd "$out" r01 "$dl" "$db" lookup edge 1 2
    run_cmd "$out" r02 "$dl" "$db" lookup edge 9 9
    run_cmd "$out" r03 "$dl" "$db" prefix edge
    run_cmd "$out" r04 "$dl" "$db" prefix edge --raw
    run_cmd "$out" r05 "$dl" "$db" prefix tc 1
    run_cmd "$out" r06 "$dl" "$db" versions
    run_cmd "$out" r07 "$dl" "$db" rev e1
    run_cmd "$out" r08 "$dl" "$db" traverse 1 2
    run_cmd "$out" r09 "$dl" "$db" obs doc3
    run_cmd "$out" r10 "$dl" "$db" search "red fox"
    run_cmd "$out" r11 "$dl" "$db" search "dog" --top 2
    run_cmd "$out" r12 "$dl" "$db" search "red" --version 1
    run_cmd "$out" r13 "$dl" "$db" query "tc(X,Y):-edge(X,Y)." tc
    run_cmd "$out" r14 "$dl" "$db" bound tc 1
    run_cmd "$out" r15 "$dl" "$db" qmagic "$DATA/tc.dl" tc 1
    run_cmd "$out" r16 "$dl" "$db" qmagic "$DATA/tc.dl" tc -a bf 1
    run_cmd "$out" r17 "$dl" "$db" pattern edge "^[12]$"
    run_cmd "$out" r18 "$dl" "$db" vsearch "q" --sig "$QSIG" --ivec "$QIVEC" --k 10 --radius 16
    run_cmd "$out" r19 "$dl" "$db" vhybrid "red" "q" --sig "$QSIG" --ivec "$QIVEC"
    run_cmd "$out" r20 "$dl" "$db" txn
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

# diff_runs <outA> <nameA> <outB> <nameB> — compare every common tag's
# stdout/stderr/rc bytes.  The two binaries live at different paths, so the
# one place argv[0] legitimately differs is usage() (it echoes the invoked
# program path — the C oracle prints the same bytes under the same argv[0]);
# that path is normalized to @PROG@ on both sides before the byte compare.
diff_runs() {
    local A=$1 na=$2 B=$3 nb=$4
    local t tag
    for f in "$A"/*.out; do
        t=$(basename "$f")
        tag=${t%.out}
        # (the nested ./dl spawned by dl-embed echoes the db dir too, so the
        # db path is normalized as well — only the names differ by design)
        sed -e "s|$na|@PROG@|g" -e "s|$WORK/dbC|@DB@|g" -e "s|$WORK/dbZ|@DB@|g" \
            "$A/$tag.out" > "$A/$tag.out.n"
        sed -e "s|$nb|@PROG@|g" -e "s|$WORK/dbC|@DB@|g" -e "s|$WORK/dbZ|@DB@|g" \
            "$B/$tag.out" > "$B/$tag.out.n"
        sed -e "s|$na|@PROG@|g" -e "s|$WORK/dbC|@DB@|g" -e "s|$WORK/dbZ|@DB@|g" \
            "$A/$tag.err" > "$A/$tag.err.n"
        sed -e "s|$nb|@PROG@|g" -e "s|$WORK/dbC|@DB@|g" -e "s|$WORK/dbZ|@DB@|g" \
            "$B/$tag.err" > "$B/$tag.err.n"
        if ! cmp -s "$A/$tag.out.n" "$B/$tag.out.n"; then
            bad "$tag STDOUT differs ($na vs $nb)"
            diff "$A/$tag.out.n" "$B/$tag.out.n" | head -5
        fi
        if ! cmp -s "$A/$tag.err.n" "$B/$tag.err.n"; then
            bad "$tag STDERR differs ($na vs $nb)"
            diff "$A/$tag.err.n" "$B/$tag.err.n" | head -5
        fi
        if ! cmp -s "$A/$tag.rc" "$B/$tag.rc"; then
            bad "$tag EXIT STATUS differs ($na vs $nb): $(cat "$A/$tag.rc") vs $(cat "$B/$tag.rc")"
        fi
    done
}

# ─── Phase 1+2: full corpus, C vs Zig, separate db dirs ────────────────────
note "phase 1/2: full corpus — C oracle vs Zig dl (separate db dirs)"
rm -rf "$WORK/dbC" "$WORK/dbZ" "$WORK/outC" "$WORK/outZ"
build_corpus "$C_DL" "$WORK/dbC" "$WORK/outC"
build_corpus "$Z_DL" "$WORK/dbZ" "$WORK/outZ"
note "  $n_cmd corpus commands x2"
n_cmd=0
diff_runs "$WORK/outC" "$C_DL" "$WORK/outZ" "$Z_DL"
mC=$(tree_manifest "$WORK/dbC")
mZ=$(tree_manifest "$WORK/dbZ")
if [ "$mC" = "$mZ" ]; then
    note "  db trees byte-identical ($(find "$WORK/dbC" -type f | wc -l) files)"
else
    bad "db-dir trees differ (C vs Zig)"
    diff <(echo "$mC") <(echo "$mZ") | head -20
fi

# ─── Phase 3: C-WRITTEN db read by BOTH binaries (interop) ─────────────────
note "phase 3: interop — Zig dl opens/reads the C-written db"
rm -rf "$WORK/dbC_a" "$WORK/dbC_b" "$WORK/outC_a" "$WORK/outZ_b"
cp -r "$WORK/dbC" "$WORK/dbC_a"
cp -r "$WORK/dbC" "$WORK/dbC_b"
read_corpus "$C_DL" "$WORK/dbC_a" "$WORK/outC_a"
read_corpus "$Z_DL" "$WORK/dbC_b" "$WORK/outZ_b"
n_cmd=0
diff_runs "$WORK/outC_a" "$C_DL" "$WORK/outZ_b" "$Z_DL"
mCa=$(tree_manifest "$WORK/dbC_a")
mCb=$(tree_manifest "$WORK/dbC_b")
if [ "$mCa" = "$mCb" ]; then
    note "  interop db trees byte-identical (C-dl vs Zig-dl, both on the C-written db)"
else
    bad "interop db trees differ (C-written db)"
    diff <(echo "$mCa") <(echo "$mCb") | head -20
fi

# ─── Phase 4: ZIG-WRITTEN db read by BOTH binaries (interop) ───────────────
note "phase 4: interop — C dl opens/reads the Zig-written db"
rm -rf "$WORK/dbZ_a" "$WORK/dbZ_b" "$WORK/outZ_a" "$WORK/outC_b"
cp -r "$WORK/dbZ" "$WORK/dbZ_a"
cp -r "$WORK/dbZ" "$WORK/dbZ_b"
read_corpus "$Z_DL" "$WORK/dbZ_a" "$WORK/outZ_a"
read_corpus "$C_DL" "$WORK/dbZ_b" "$WORK/outC_b"
diff_runs "$WORK/outZ_a" "$Z_DL" "$WORK/outC_b" "$C_DL"
mZa=$(tree_manifest "$WORK/dbZ_a")
mZb=$(tree_manifest "$WORK/dbZ_b")
if [ "$mZa" = "$mZb" ]; then
    note "  interop db trees byte-identical (Zig-dl vs C-dl, both on the Zig-written db)"
else
    bad "interop db trees differ (Zig-written db)"
    diff <(echo "$mZa") <(echo "$mZb") | head -20
fi

# ─── Verdict ────────────────────────────────────────────────────────────────
NC=$(ls "$WORK/outC"/*.out | wc -l)
NR=$(ls "$WORK/outC_a"/*.out | wc -l)
if [ "$FAIL" -eq 0 ]; then
    echo "cli_diff: BYTE-GREEN both directions ($NC full-corpus + $NR interop commands per binary x2 binaries; stdout/stderr/rc + db trees identical)"
    exit 0
fi
echo "cli_diff: FAILED"
exit 1
