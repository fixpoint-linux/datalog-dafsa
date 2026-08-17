/* datalog-mode.js — CodeMirror 5 simple mode for datalog-dafsa.
 * Token table mirrors the real lexer in src/parser.c:127 (kinds in parser.h).
 * Load AFTER codemirror.min.js and codemirror-simple.js (defineSimpleMode). */
(function () {
  'use strict';
  if (typeof CodeMirror === 'undefined' || !CodeMirror.defineSimpleMode) return;

  CodeMirror.defineSimpleMode('datalog', {
    start: [
      { regex: /#.*/, token: 'comment' },                          /* line comment */
      { regex: /"(?:[^"\\]|\\.)*"/, token: 'string' },            /* double-quoted string */
      { regex: /'(?:[^'\\]|\\.)*'/, token: 'string' },            /* single-quoted (regex after ~) */
      { regex: /:-/, token: 'def' },                                /* rule head/body separator */
      { regex: /[0-9]+/, token: 'number' },                         /* integer literal */
      { regex: /\b(?:count|sum|min|max|concat|length|lower|upper|prefix|suffix|contains|cons|car|cdr|append|member|range)\b/,
        token: 'keyword' },                                          /* builtins */
      { regex: /[A-Z_][A-Za-z0-9_]*/, token: 'variable-2' },        /* variable (uppercase/_)*/
      { regex: /[a-z][A-Za-z0-9_]*/, token: 'atom' },               /* predicate / constant (lowercase) */
      { regex: /[!<>=+\-*\/%~|]/, token: 'operator' },              /* ! != < <= > >= = + - * / % ~ | */
      { regex: /[(),\[\].]/, token: 'bracket' }                      /* ( ) [ ] . , */
    ]
  });

  CodeMirror.defineMIME('text/x-datalog', 'datalog');
})();
