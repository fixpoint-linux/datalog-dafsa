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

SCHEMA='-- golden schema (empty-record-payload DSL)
let ColumnType = < Natural : {=} | Text : {=} >
in let Column = { name : Text, type : ColumnType }
in let Relation = { name : Text, columns : List Column }
in let Schema = { relations : List Relation }
in { relations =
     [ { name = "node",
         columns = [ { name = "id", type = < Text = {=} > } ] },
       { name = "edge",
         columns = [ { name = "src", type = < Text = {=} > },
                     { name = "dst", type = < Text = {=} > } ] },
       { name = "weight",
         columns = [ { name = "src", type = < Text = {=} > },
                     { name = "w", type = < Natural = {=} > } ] },
       { name = "light_edge",
         columns = [ { name = "src", type = < Text = {=} > },
                     { name = "dst", type = < Text = {=} > } ] },
       { name = "tc",
         columns = [ { name = "src", type = < Text = {=} > },
                     { name = "dst", type = < Text = {=} > } ] } ] } : Schema
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

# --- JSON good project (S6): identical worked example, data as data/*.json ---
# Same schema + rules; node/edge/weight come from JSON arrays-of-objects with
# strict typing (JSON number -> Natural, JSON string -> Text).  The query must
# be identical to the CSV variant.
NODE_J='[{"id":"alice"},{"id":"bob"},{"id":"carol"},{"id":"dave"}]'
EDGE_J='[{"src":"alice","dst":"bob"},{"src":"bob","dst":"carol"},{"src":"alice","dst":"dave"},{"src":"dave","dst":"carol"}]'
WEIGHT_J='[{"src":"alice","w":3},{"src":"bob","w":10},{"src":"dave","w":2}]'

J="$WORK/jgood"
write "$J/schema.dhall" "$SCHEMA"
write "$J/data/node.json"   "$NODE_J"
write "$J/data/edge.json"   "$EDGE_J"
write "$J/data/weight.json" "$WEIGHT_J"
write "$J/rules/reach.datalog" "$GOOD_RULES"

"$DLP" check "$J" ; ok "check (JSON good)"
"$DLP" build "$J" ; ok "build (JSON good)"

JOUT="$("$DLP" query "$J" 'tc(alice,X)')"
if [ "$(printf '%s' "$JOUT" | sort)" != "$(printf '%s' "$EXPECTED")" ]; then
    fail "JSON query tc(alice,X) = $(printf '%s' "$JOUT" | sort); expected: $EXPECTED"
fi
ok "JSON query 'tc(alice,X)' == bob/carol/dave (sorted)"

# --- JSON negative typing (S6) ---
# A JSON string in a Natural column must be rejected (check + build).
B1="$WORK/jbad-natural"
write "$B1/schema.dhall" "$SCHEMA"
write "$B1/data/node.json"   "$NODE_J"
write "$B1/data/edge.json"   "$EDGE_J"
write "$B1/data/weight.json" '[{"src":"alice","w":"3"}]'
write "$B1/rules/reach.datalog" "$GOOD_RULES"

if "$DLP" check "$B1" 2>"$WORK/bad-natural-check.err"; then
    fail "check (string-in-Natural) unexpectedly succeeded"
fi
if ! grep -q 'Natural' "$WORK/bad-natural-check.err"; then
    fail "check (string-in-Natural) stderr lacks 'Natural': $(cat "$WORK/bad-natural-check.err")"
fi
ok "check (string in Natural JSON column) rejected (stderr has 'Natural')"

if "$DLP" build "$B1" 2>"$WORK/bad-natural-build.err"; then
    fail "build (string-in-Natural) unexpectedly succeeded"
fi
if ! grep -q 'Natural' "$WORK/bad-natural-build.err"; then
    fail "build (string-in-Natural) stderr lacks 'Natural': $(cat "$WORK/bad-natural-build.err")"
fi
ok "build (string in Natural JSON column) rejected (stderr has 'Natural')"

# A JSON number in a Text column must be rejected.
B2="$WORK/jbad-text"
write "$B2/schema.dhall" "$SCHEMA"
write "$B2/data/node.json"   '[{"id":42}]'
write "$B2/data/edge.json"   "$EDGE_J"
write "$B2/data/weight.json" "$WEIGHT_J"
write "$B2/rules/reach.datalog" "$GOOD_RULES"

if "$DLP" check "$B2" 2>"$WORK/bad-text.err"; then
    fail "check (number-in-Text) unexpectedly succeeded"
fi
if ! grep -q 'Text' "$WORK/bad-text.err"; then
    fail "check (number-in-Text) stderr lacks 'Text': $(cat "$WORK/bad-text.err")"
fi
ok "check (number in Text JSON column) rejected (stderr has 'Text')"

echo "ALL GOLDEN TESTS PASSED"
