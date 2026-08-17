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

SCHEMA='-- golden schema (Optional-payload DSL, finish-dlp)
let Elem = < Natural : {=} | Text : {=} | Bool : {=} | Char : {=} | Date : {=} | Timestamp : {=} | Signed : {=} >
in let NC = { min = None Natural, max = None Natural }
in let TC = { regex = None Text }
in let SC = { min = None Integer, max = None Integer }
in let ColumnType = < Natural : { min : Optional Natural, max : Optional Natural } | Text : { regex : Optional Text } | Bool : {=} | Char : { min : Optional Natural, max : Optional Natural } | Date : { min : Optional Natural, max : Optional Natural } | Timestamp : { min : Optional Natural, max : Optional Natural } | Signed : { min : Optional Integer, max : Optional Integer } | List : { elem : Elem } | Optional : { elem : Elem } | Enum : { values : List Text } >
in let Column = { name : Text, type : ColumnType }
in let Relation = { name : Text, columns : List Column }
in let Schema = { relations : List Relation }
in { relations =
     [ { name = "node",
         columns = [ { name = "id", type = < Text = TC > } ] },
       { name = "edge",
         columns = [ { name = "src", type = < Text = TC > },
                     { name = "dst", type = < Text = TC > } ] },
       { name = "weight",
         columns = [ { name = "src", type = < Text = TC > },
                     { name = "w", type = < Natural = NC > } ] },
       { name = "light_edge",
         columns = [ { name = "src", type = < Text = TC > },
                     { name = "dst", type = < Text = TC > } ] },
       { name = "tc",
         columns = [ { name = "src", type = < Text = TC > },
                     { name = "dst", type = < Text = TC > } ] },
       { name = "catalog",
         columns = [ { name = "tags", type = < List = { elem = < Text = {=} > } > },
                     { name = "nick", type = < Optional = { elem = < Text = {=} > } > },
                     { name = "color", type = < Enum = { values = [ "red", "green", "blue" ] } > } ] },
       { name = "membertag", columns = [ { name = "x", type = < Text = TC > } ] },
       { name = "cartag",    columns = [ { name = "x", type = < Text = TC > } ] },
       { name = "constag",   columns = [ { name = "r", type = < List = { elem = < Text = {=} > } > } ] } ] } : Schema
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

# List<Text> cells are bracketed + QUOTED; Optional empty cell -> None; the
# third row tests a quoted-field-with-embedded-comma kept as ONE field AND a
# single-quoted Text element protecting its comma.
CATALOG='tags,nick,color
"[alice,bob,carol]",alice,red
"[x]",,green
"['"'"'a,b'"'"',c]",bob,blue
'

GOOD_RULES='tc(A,B):-edge(A,B).
tc(A,C):-edge(A,B),tc(B,C).
light_edge(A,B):-edge(A,B),weight(A,W),W<10.
'
# List builtin rules (Stage B typecheck): member/car/cons over catalog.tags.
LIST_RULES='membertag(X):-catalog(T,N,C),member(X,T).
cartag(X):-catalog(T,N,C),car(X,T).
constag(R):-catalog(T,N,C),cons(R,newtag,T).
'

BUG_RULES='tc(A,W):-weight(A,W).
'

# --- good project ---
P="$WORK/good"
write "$P/schema.dhall" "$SCHEMA"
write "$P/data/node.csv"   "$NODE"
write "$P/data/edge.csv"   "$EDGE"
write "$P/data/weight.csv" "$WEIGHT"
write "$P/data/catalog.csv" "$CATALOG"
write "$P/rules/reach.datalog" "$GOOD_RULES"
write "$P/rules/lists.datalog" "$LIST_RULES"

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

# List<Text> column from a CSV bracketed+quoted cell, Optional Some(nick=alice),
# and a Text list element containing a comma round-trip through the printer.
OUT="$(printf '%s' "$("$DLP" query "$P" 'catalog(X,Y,red)')" | sort)"
EXPECTED='[alice, bob, carol] alice'
if [ "$OUT" != "$EXPECTED" ]; then
    fail "query catalog(X,Y,red) = [$OUT]; expected: $EXPECTED"
fi
ok "List/Optional round-trip from CSV (catalog(X,Y,red))"

# member/car/cons list-builtin rules typechecked and evaluate.  catalog's 3
# List cells contribute {alice,bob,carol} U {x} U {"a,b",c}.
OUT="$(printf '%s' "$("$DLP" query "$P" 'membertag(X)')" | sort)"
EXPECTED='a,b
alice
bob
c
carol
x'
if [ "$OUT" != "$EXPECTED" ]; then
    fail "query membertag(X) = [$OUT]; expected: $EXPECTED"
fi
ok "member over List<Text> typechecks + evaluates"

# --- Enum out-of-set rejected by check + build (CSV) ---
E1="$WORK/badenum"
write "$E1/schema.dhall" "$SCHEMA"
write "$E1/data/catalog.csv" 'tags,nick,color
"[a]",alice,purple
'
write "$E1/rules/reach.datalog" "$GOOD_RULES"
if "$DLP" check "$E1" 2>"$WORK/badenum-check.err"; then
    fail "check (Enum out-of-set) unexpectedly succeeded"
fi
if ! grep -q 'Enum' "$WORK/badenum-check.err"; then
    fail "check (Enum out-of-set) stderr lacks 'Enum': $(cat "$WORK/badenum-check.err")"
fi
ok "check (Enum out-of-set) rejected (stderr has 'Enum')"

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
# List<Text> = JSON array; Optional null -> None; Enum valid.
CATALOG_J='[{"tags":["a","b"],"nick":null,"color":"red"},{"tags":[],"nick":"zoe","color":"green"}]'

J="$WORK/jgood"
write "$J/schema.dhall" "$SCHEMA"
write "$J/data/node.json"   "$NODE_J"
write "$J/data/edge.json"   "$EDGE_J"
write "$J/data/weight.json" "$WEIGHT_J"
write "$J/data/catalog.json" "$CATALOG_J"
write "$J/rules/reach.datalog" "$GOOD_RULES"
write "$J/rules/lists.datalog" "$LIST_RULES"

"$DLP" check "$J" ; ok "check (JSON good)"
"$DLP" build "$J" ; ok "build (JSON good)"

JOUT="$(printf '%s' "$("$DLP" query "$J" 'tc(alice,X)')" | sort)"
if [ "$JOUT" != 'bob
carol
dave' ]; then
    fail "JSON query tc(alice,X) = $JOUT; expected bob/carol/dave"
fi
ok "JSON query 'tc(alice,X)' == bob/carol/dave (sorted)"

# List from a JSON array + Optional null (None) round-trip: bound color=green
# gives the empty list [] and Some(nick=zoe).
JOUT="$(printf '%s' "$("$DLP" query "$J" 'catalog(X,Y,green)')" | sort)"
if [ "$JOUT" != '[] zoe' ]; then
    fail "JSON query catalog(X,Y,green) = [$JOUT]; expected '[] zoe'"
fi
ok "List from JSON array + Optional null (catalog(X,Y,green))"

# Optional empty CSV cell -> None: catalog CSV row 2 has empty nick.  Query
# bound color=green over the CSV project (P) must show None (null) for nick.
P_OUT="$(printf '%s' "$("$DLP" query "$P" 'catalog(X,Y,green)')" | sort)"
if [ "$P_OUT" != '[x] null' ]; then
    fail "CSV query catalog(X,Y,green) = [$P_OUT]; expected '[x] null'"
fi
ok "Optional empty CSV cell -> None (catalog(X,Y,green) over CSV)"

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

# --- per-column constraints (finish-dlp Item 2) ---
# A Natural[0..150] column + a regex Text column: out-of-range / non-matching
# rows rejected by check+build; in-range / matching rows pass.
CSCHEMA='-- constrained schema (finish-dlp Item 2)
let Elem = < Natural : {=} | Text : {=} | Bool : {=} | Char : {=} | Date : {=} | Timestamp : {=} | Signed : {=} >
in let NC = { min = None Natural, max = None Natural }
in let TC = { regex = None Text }
in let ColumnType = < Natural : { min : Optional Natural, max : Optional Natural } | Text : { regex : Optional Text } | Bool : {=} | Char : { min : Optional Natural, max : Optional Natural } | Date : { min : Optional Natural, max : Optional Natural } | Timestamp : { min : Optional Natural, max : Optional Natural } | Signed : { min : Optional Integer, max : Optional Integer } | List : { elem : Elem } | Optional : { elem : Elem } | Enum : { values : List Text } >
in let Column = { name : Text, type : ColumnType }
in let Relation = { name : Text, columns : List Column }
in let Schema = { relations : List Relation }
in { relations =
     [ { name = "score",
         columns = [ { name = "val", type = < Natural = { min = Some 0, max = Some 150 } > } ] },
       { name = "code",
         columns = [ { name = "id", type = < Text = { regex = Some "[A-Z]+[0-9]+" } > } ] },
       { name = "score_out", columns = [ { name = "val", type = < Natural = NC > } ] },
       { name = "code_out", columns = [ { name = "id", type = < Text = TC > } ] } ] } : Schema
'
SCORE_GOOD='val
0
75
150
'
SCORE_BAD='val
151
'
CODE_GOOD='id
ABC12
XYZ99
'
CODE_BAD='id
abc
'
CONST_RULES='score_out(X):-score(X).
code_out(C):-code(C).
'
CGOOD="$WORK/constgood"
write "$CGOOD/schema.dhall" "$CSCHEMA"
write "$CGOOD/data/score.csv" "$SCORE_GOOD"
write "$CGOOD/data/code.csv"  "$CODE_GOOD"
write "$CGOOD/rules/r.datalog" "$CONST_RULES"
"$DLP" check "$CGOOD" ; ok "check (constrained good)"
"$DLP" build "$CGOOD" ; ok "build (constrained good)"

CBAD1="$WORK/constbad-range"
write "$CBAD1/schema.dhall" "$CSCHEMA"
write "$CBAD1/data/score.csv" "$SCORE_BAD"
write "$CBAD1/data/code.csv"  "$CODE_GOOD"
write "$CBAD1/rules/r.datalog" "$CONST_RULES"
if "$DLP" check "$CBAD1" 2>"$WORK/constbad-range.err"; then
    fail "check (out-of-range Natural) unexpectedly succeeded"
fi
if ! grep -q 'out of range' "$WORK/constbad-range.err"; then
    fail "check (out-of-range Natural) stderr lacks 'out of range': $(cat "$WORK/constbad-range.err")"
fi
ok "check (out-of-range Natural[0..150] row) rejected"

CBAD2="$WORK/constbad-regex"
write "$CBAD2/schema.dhall" "$CSCHEMA"
write "$CBAD2/data/score.csv" "$SCORE_GOOD"
write "$CBAD2/data/code.csv"  "$CODE_BAD"
write "$CBAD2/rules/r.datalog" "$CONST_RULES"
if "$DLP" check "$CBAD2" 2>"$WORK/constbad-regex.err"; then
    fail "check (regex non-matching row) unexpectedly succeeded"
fi
if ! grep -q 'does not match regex' "$WORK/constbad-regex.err"; then
    fail "check (regex non-match) stderr lacks 'does not match regex': $(cat "$WORK/constbad-regex.err")"
fi
ok "check (regex Text non-matching row) rejected"

echo "ALL GOLDEN TESTS PASSED"
