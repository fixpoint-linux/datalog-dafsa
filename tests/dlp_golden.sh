#!/bin/sh
# tests/dlp_golden.sh — S5 golden-output test for dlp check/build/query.
# Usage: tests/dlp_golden.sh ./dlp/dlp
set -eu

DLP="${1:-./dlp/dlp}"
WORK="$(mktemp -d /tmp/dlp-golden.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }
ok()   { echo "  ok: $*"; }

write() { # write <path> <content>  (path is absolute; $WORK already applied by caller)
  mkdir -p "$(dirname "$1")"
  printf '%s' "$2" > "$1"
}

SCHEMA='-- golden schema (Bool-payload DSL)
let ColumnType = < Natural : Bool | Text : Bool >
in let Column = { name : Text, type : ColumnType }
in let Relation = { name : Text, columns : List Column }
in let Schema = { relations : List Relation }
in { relations =
     [ { name = "node",
         columns = [ { name = "id", type = < Text = True > } ] },
       { name = "edge",
         columns = [ { name = "src", type = < Text = True > },
                     { name = "dst", type = < Text = True > } ] },
       { name = "weight",
         columns = [ { name = "src", type = < Text = True > },
                     { name = "w", type = < Natural = True > } ] },
       { name = "light_edge",
         columns = [ { name = "src", type = < Text = True > },
                     { name = "dst", type = < Text = True > } ] },
       { name = "tc",
         columns = [ { name = "src", type = < Text = True > },
                     { name = "dst", type = < Text = True > } ] } ] } : Schema
'

NODE='id
alice
bob
carol
dave
'

EDGE='src,dst
alice,bob
bob,carol
alice,dave
dave,carol
'

WEIGHT='src,w
alice,3
bob,10
dave,2
'

GOOD_RULES='tc(A,B):-edge(A,B).
tc(A,C):-edge(A,B),tc(B,C).
light_edge(A,B):-edge(A,B),weight(A,W),W<10.
'

BUG_RULES='tc(A,W):-weight(A,W).
'

# --- good project ---
P="$WORK/good"
write "$P/schema.dhall" "$SCHEMA"
write "$P/data/node.csv"   "$NODE"
write "$P/data/edge.csv"   "$EDGE"
write "$P/data/weight.csv" "$WEIGHT"
write "$P/rules/reach.datalog" "$GOOD_RULES"

"$DLP" check "$P" ; ok "check (good)"
"$DLP" build "$P" ; ok "build (good)"

OUT="$("$DLP" query "$P" 'tc(alice,X)')"
EXPECTED='bob
carol
dave'
if [ "$(printf '%s' "$OUT" | sort)" != "$(printf '%s' "$EXPECTED")" ]; then
    fail "query tc(alice,X) = $(printf '%s' "$OUT" | sort); expected: $EXPECTED"
fi
ok "query 'tc(alice,X)' == bob/carol/dave (sorted)"

# --- bug project ---
Q="$WORK/bug"
write "$Q/schema.dhall" "$SCHEMA"
write "$Q/data/node.csv"   "$NODE"
write "$Q/data/edge.csv"   "$EDGE"
write "$Q/data/weight.csv" "$WEIGHT"
write "$Q/rules/reach.datalog" "$BUG_RULES"

if "$DLP" check "$Q" 2>"$WORK/bug-check.err"; then
    fail "check (bug) unexpectedly succeeded"
fi
if ! grep -q 'Natural' "$WORK/bug-check.err"; then
    fail "check (bug) stderr lacks 'Natural': $(cat "$WORK/bug-check.err")"
fi
ok "check (bug) rejected (stderr has 'Natural')"

if "$DLP" build "$Q" 2>"$WORK/bug-build.err"; then
    fail "build (bug) unexpectedly succeeded"
fi
if ! grep -q 'Natural' "$WORK/bug-build.err"; then
    fail "build (bug) stderr lacks 'Natural': $(cat "$WORK/bug-build.err")"
fi
ok "build (bug) rejected (stderr has 'Natural')"

echo "ALL GOLDEN TESTS PASSED"
