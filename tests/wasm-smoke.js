// wasm-smoke.js — headless node smoke test for the WebAssembly playground.
//
// Run from the repo root after `make wasm`:
//   node tests/wasm-smoke.js
//
// Loads docs/playground.js (the MODULARIZE'd createPlayground factory),
// drives playground_run(program, goal) through the allocUTF8 /
// lengthBytesUTF8 / _malloc / stringToUTF8 / UTF8ToString / _free bridge,
// and asserts the supported in-memory subset gives EXACTLY the byte-identical
// output the native engine produces.
//
// INTEGER data is used for the arithmetic/aggregate/range cases so the
// results cannot collide with symbol ids (the documented B6 int-vs-symbol
// rendering heuristic in the CLI's print_value).

'use strict';
const path = require('path');
const assert = require('assert');

const createPlayground = require(path.join(__dirname, '..', 'docs', 'playground.js'));

createPlayground().then((M) => {
  function run(program, goal) {
    const pb = M.lengthBytesUTF8(program);
    const pp = M._malloc(pb + 1);
    M.stringToUTF8(program, pp, pb + 1);
    const gb = M.lengthBytesUTF8(goal);
    const gp = M._malloc(gb + 1);
    M.stringToUTF8(goal, gp, gb + 1);
    const out = M.UTF8ToString(M._playground_run(pp, gp));
    M._free(pp);
    M._free(gp);
    return out;
  }

  const cases = [
    // (1) transitive closure (recursion)
    ['tc',
      'edge(a,b). edge(b,c). edge(c,d).\n' +
      'path(X,Y) :- edge(X,Y).\n' +
      'path(X,Y) :- path(X,Z), edge(Z,Y).\n',
      'path', 'a b\na c\na d\nb c\nb d\nc d\n'],

    // (2) stratified negation
    ['negation',
      'p(a). p(b). q(b).\n' +
      'r(X) :- p(X), !q(X).\n',
      'r', 'a\n'],

    // (3) aggregate count
    ['aggregate count',
      'edge(1,2). edge(1,3). edge(2,4).\n' +
      'cnt(X,N) :- edge(X,Y), N=count().\n',
      'cnt', '1 2\n2 1\n'],

    // (4) aggregate sum
    ['aggregate sum',
      'val(10,2). val(10,3). val(20,10).\n' +
      'total(X,S) :- val(X,V), S=sum(V).\n',
      'total', '10 5\n20 10\n'],

    // (5) arithmetic
    ['arithmetic',
      'pair(10,2). pair(20,3).\n' +
      'add(X,S) :- pair(X,N), S=N+100.\n',
      'add', '10 102\n20 103\n'],

    // (6) string concat
    ['string concat',
      'w("foo"). tail("bar").\n' +
      'cat(X) :- w(A), tail(B), X=concat(A,B).\n',
      'cat', 'foobar\n'],

    // (7) range predicate
    ['range',
      'r(10). r(15). r(20). r(25).\n' +
      'q(X) :- range(X, r, 12, 22).\n',
      'q', '15\n20\n'],

    // (8) list cons
    ['list cons',
      'num(7). num(8).\n' +
      'r(X,L) :- num(X), L=cons(X,[7,8]).\n',
      'r', '7 [7, 7, 8]\n8 [8, 7, 8]\n'],

    // (9) error: unknown goal relation
    ['err unknown goal', 'edge(a,b).\n', 'nope',
      'error: query failed (unknown goal relation?)'],

    // (10) error: parse failure (missing dot)
    ['err parse', 'edge(a,b)\n', 'edge', 'error: parse failed'],
  ];

  let failed = 0;
  for (const [name, prog, goal, expected] of cases) {
    const got = run(prog, goal);
    try {
      assert.strictEqual(got, expected);
      console.log('[ ok ] ' + name);
    } catch (e) {
      failed++;
      console.log('[FAIL] ' + name);
      console.log('  expected: ' + JSON.stringify(expected));
      console.log('  got     : ' + JSON.stringify(got));
    }
  }
  console.log(failed === 0
    ? 'ALL WASM SMOKE OK'
    : (failed + ' WASM SMOKE FAILURES'));
  process.exit(failed === 0 ? 0 : 1);
}).catch((e) => {
  console.error('failed to load wasm:', e);
  process.exit(1);
});
