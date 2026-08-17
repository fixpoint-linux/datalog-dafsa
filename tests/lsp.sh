#!/bin/sh
# LSP end-to-end test for the datalog-dafsa language server.
# usage: tests/lsp.sh [dl-lsp-binary]   (default: ./dl-lsp)
#
# Frames a JSON-RPC conversation over Content-Length framing and greps exact
# response fragments: initialize capabilities, a parse-error diagnostic, an
# undefined-predicate compile-error diagnostic (with its rule-level range), an
# empty-diagnostics-after-fix case, hover (arity + IDB/EDB), and completion
# (builtin + relation name).
set -u
BIN="${1:-./dl-lsp}"
case "$BIN" in
    /*) : ;;
    *) BIN="$(pwd)/$BIN" ;;
esac
cd "$(dirname "$0")/.." || exit 1

OUT=/tmp/dl-lsp-out.txt
pass=0
fail=0

check() {
    name="$1"
    ok="$2"
    if [ "$ok" -eq 1 ]; then
        pass=$((pass + 1))
        echo "PASS $name"
    else
        fail=$((fail + 1))
        echo "FAIL $name"
    fi
}

# send <json>  — emit one Content-Length-framed message (ASCII JSON, no newline)
send() {
    msg="$1"
    n=${#msg}
    printf 'Content-Length: %d\r\n\r\n%s' "$n" "$msg"
}

{
    send '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}'
    send '{"jsonrpc":"2.0","method":"initialized","params":{}}'
    # a.dl: valid program — facts + a rule.  edge is EDB (fact-only), path is IDB.
    send '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/a.dl","languageId":"datalog","version":1,"text":"edge(a,b).\npath(X,Y) :- edge(X,Y).\n"}}}'
    # b.dl: parse error — 'edge(a,b' is missing the closing ')' and the dot.
    send '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/b.dl","languageId":"datalog","version":1,"text":"edge(a,b\n"}}}'
    # c.dl: compile error — 'edge' is never declared, so the rule body references it.
    send '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/c.dl","languageId":"datalog","version":1,"text":"path(X,Y) :- edge(X,Y).\n"}}}'
    # hover: the fact predicate edge (line 0) -> EDB; the rule head path (line 1) -> IDB.
    send '{"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/a.dl"},"position":{"line":0,"character":0}}}'
    send '{"jsonrpc":"2.0","id":3,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/a.dl"},"position":{"line":1,"character":0}}}'
    # completion on the valid doc.
    send '{"jsonrpc":"2.0","id":4,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///tmp/a.dl"},"position":{"line":0,"character":0}}}'
    # fix c.dl (declare edge) -> empty diagnostics.
    send '{"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"file:///tmp/c.dl","version":2},"contentChanges":[{"text":"edge(a,b).\npath(X,Y) :- edge(X,Y).\n"}]}}'
    send '{"jsonrpc":"2.0","id":5,"method":"shutdown"}'
    send '{"jsonrpc":"2.0","method":"exit"}'
} | "$BIN" > "$OUT"
rc=$?

ok=1
grep -q '"hoverProvider":true' "$OUT"          || { echo "  missing hoverProvider"; ok=0; }
grep -q '"completionProvider":true' "$OUT"     || { echo "  missing completionProvider"; ok=0; }
grep -q '"textDocumentSync":1' "$OUT"          || { echo "  missing textDocumentSync"; ok=0; }
check "initialize-capabilities" "$ok"

ok=1
grep -q '"message":"parser:' "$OUT" || { echo "  missing parse-error message"; ok=0; }
check "parse-error-diagnostic" "$ok"

ok=1
grep -q '"message":"compile error: unknown predicate' "$OUT" \
    || { echo "  missing compile-error message"; ok=0; }
grep -q '"range":{"start":{"line":0,"character":0}' "$OUT" \
    || { echo "  missing rule-level range"; ok=0; }
check "compile-error-diagnostic" "$ok"

ok=1
grep -q '"uri":"file:///tmp/a.dl","diagnostics":\[\]' "$OUT" || { echo "  missing empty diagnostics for valid doc"; ok=0; }
grep -q '"uri":"file:///tmp/c.dl","diagnostics":\[\]' "$OUT" || { echo "  missing empty diagnostics after fix"; ok=0; }
check "empty-diagnostics-after-fix" "$ok"

ok=1
grep -q 'edge/2' "$OUT" || { echo "  missing edge arity hover"; ok=0; }
grep -q 'EDB' "$OUT"   || { echo "  missing EDB hover"; ok=0; }
grep -q 'path/2' "$OUT" || { echo "  missing path arity hover"; ok=0; }
grep -q 'IDB' "$OUT"   || { echo "  missing IDB hover"; ok=0; }
check "hover-arity-idb-edb" "$ok"

ok=1
grep -q '"label":"count"' "$OUT" || { echo "  missing builtin completion"; ok=0; }
grep -q '"label":"edge"'  "$OUT" || { echo "  missing relation completion"; ok=0; }
check "completion-builtin-and-relation" "$ok"

if [ "$rc" -eq 0 ]; then
    check "shutdown-exit-code-0" 1
else
    echo "  exit code was $rc"
    check "shutdown-exit-code-0" 0
fi

echo
echo "=== $pass passed, $fail failed ==="
[ "$fail" -eq 0 ]
