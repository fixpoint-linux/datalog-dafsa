/* playground-ui.js — UI glue for the datalog-dafsa language playground.
 *
 * docs/playground.js is the MODULARIZE'd emscripten factory emitted by
 * scripts/build-wasm.sh (global `createPlayground`), overwritten by
 * `make wasm`.  This file is loaded AFTER it, wires the DOM elements of
 * docs/playground.html, and drives the engine through the allocUTF8 /
 * lengthBytesUTF8 / _malloc / stringToUTF8 / UTF8ToString / _free bridge:
 *   #program  — textarea holding facts + rules (and '#' comments)
 *   #goal     — text input for the goal relation name (default: path)
 *   #runBtn   — the Run button
 *   #output   — <pre> pane for the result
 *   #status   — optional status line
 *
 * The engine runs 100% in the browser: no server, no filesystem.
 */
(function () {
  'use strict';

  var Module = null;
  var progEl = document.getElementById('program');
  var goalEl = document.getElementById('goal');
  var outEl  = document.getElementById('output');
  var runBtn = document.getElementById('runBtn');
  var statusEl = document.getElementById('status');

  var DEFAULT_PROG =
    '# A transitive-closure example.\n' +
    '# Facts: the base edges of a small graph.\n' +
    'edge(a, b).\n' +
    'edge(b, c).\n' +
    'edge(c, d).\n' +
    '\n' +
    '# Rules: path(X,Y) is the transitive closure of edge.\n' +
    'path(X, Y) :- edge(X, Y).\n' +
    'path(X, Y) :- path(X, Z), edge(Z, Y).\n' +
    '\n' +
    '# Type a goal relation below (default "path") and press Run.';

  function run() {
    var prog = progEl.value;
    var goal = (goalEl.value || '').trim() || 'path';
    var pb = Module.lengthBytesUTF8(prog);
    var pp = Module._malloc(pb + 1);
    Module.stringToUTF8(prog, pp, pb + 1);
    var gb = Module.lengthBytesUTF8(goal);
    var gp = Module._malloc(gb + 1);
    Module.stringToUTF8(goal, gp, gb + 1);
    var out = Module.UTF8ToString(Module._playground_run(pp, gp));
    Module._free(pp);
    Module._free(gp);
    outEl.textContent = out || '(no results)';
  }

  window.createPlayground().then(function (M) {
    Module = M;
    progEl.value = DEFAULT_PROG;
    runBtn.addEventListener('click', run);
    if (statusEl) statusEl.textContent =
      'ready — the real C engine, compiled to WebAssembly, running in your browser';
    run();
  }).catch(function (e) {
    if (statusEl) statusEl.textContent = 'failed to load wasm: ' + e;
  });
})();
