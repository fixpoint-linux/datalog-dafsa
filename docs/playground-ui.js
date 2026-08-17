/* playground-ui.js — UI glue for the datalog-dafsa language playground.
 *
 * docs/playground.js  — the MODULARIZE'd emscripten factory for the engine
 *                        (global `createPlayground`), overwritten by `make wasm`.
 * docs/dl-lsp.js       — the MODULARIZE'd emscripten factory for the LSP server
 *                        (global `createDlLsp`), also built by `make wasm`.
 * docs/vendor/*.js     — CodeMirror 5 + the `datalog` simple mode.
 *
 * This file (loaded LAST) wires the DOM, replaces the #program textarea with a
 * CodeMirror editor (mode:'datalog'), keeps the Run button driving
 * playground_run(), and adds LIVE LSP wiring: on every edit it sends
 * textDocument/didChange (full sync) and renders the publishDiagnostics
 * response as a squiggly underline + a diagnostics list; on cursorActivity it
 * sends textDocument/hover and shows a tooltip.
 *
 *   #program  — textarea (converted to CodeMirror)
 *   #goal     — text input for the goal relation name (default: path)
 *   #runBtn   — the Run button
 *   #output   — <pre> pane for the result
 *   #status   — optional status line
 *   #diagnostics — LSP diagnostics list
 *   #tooltip  — hover tooltip
 */
(function () {
  'use strict';

  var Module = null;      /* playground wasm */
  var LspModule = null;   /* LSP wasm */
  var editor = null;      /* CodeMirror instance */
  var progEl = document.getElementById('program');
  var goalEl = document.getElementById('goal');
  var outEl  = document.getElementById('output');
  var runBtn = document.getElementById('runBtn');
  var statusEl = document.getElementById('status');
  var diagEl  = document.getElementById('diagnostics');
  var tooltipEl = document.getElementById('tooltip');

  var DEFAULT_PROG =
    '# ---- datalog-dafsa in action ----\n' +
    '# A directed graph, a tag per node, and a numeric signal.\n' +
    'edge(a, b). edge(b, c). edge(c, d). edge(d, a). edge(a, x). edge(c, z).\n' +
    'edge(s, a). edge(s, x).\n' +
    'color(a, red). color(b, blue). color(c, green). color(d, blue).\n' +
    'color(x, red). color(z, blue).\n' +
    'signal(5). signal(12). signal(15). signal(20). signal(27).\n' +
    '\n' +
    '# Recursion: path(X,Y) is the transitive closure of edge.\n' +
    'path(X, Y) :- edge(X, Y).\n' +
    'path(X, Y) :- path(X, Z), edge(Z, Y).\n' +
    '\n' +
    '# Join: exactly two hops.\n' +
    'hop2(X, Y) :- edge(X, Z), edge(Z, Y).\n' +
    '\n' +
    '# Negation: nodes with an out-edge but no in-edge.\n' +
    'reached(X) :- edge(_, X).\n' +
    'source(X) :- edge(X, _), !reached(X).\n' +
    '\n' +
    '# Lists: neighbors of each node from a list-valued fact.\n' +
    'nb(a, [b, x]). nb(c, [d, z]).\n' +
    'neighbor(X, Y) :- nb(X, L), member(Y, L).\n' +
    '\n' +
    '# Strings: every node whose name contains "b".\n' +
    'hasb(X) :- edge(X, _), contains(X, "b").\n' +
    '\n' +
    '# Range: signals in [10, 25).\n' +
    'inband(X) :- range(X, signal, 10, 25).\n' +
    '\n' +
    '# Pick any of these as the goal: path, hop2, source, neighbor, hasb, inband.';

  var LSP_URI = 'file:///playground.dl';
  var lspVersion = 1;
  var lspMsgId = 0;
  var squiggleMarks = [];

  /* ─── Run (unchanged semantics: playground_run over the current text) ─── */

  function currentText() {
    return editor ? editor.getValue() : progEl.value;
  }

  function run() {
    var prog = currentText();
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

  /* ─── LSP bridge (mirrors the playground's playground_run bridge) ─────── */

  function lspHandle(json) {
    var bytes = LspModule.lengthBytesUTF8(json);
    var ptr = LspModule._malloc(bytes + 1);
    LspModule.stringToUTF8(json, ptr, bytes + 1);
    LspModule._lsp_handle(ptr, bytes);
    LspModule._free(ptr);
    var n = LspModule._lsp_out_len();
    return n ? LspModule.UTF8ToString(LspModule._lsp_out(), n) : '';
  }

  function parseFrames(buf) {
    var msgs = [];
    var i = 0;
    while (i < buf.length) {
      var end = buf.indexOf('\r\n\r\n', i);
      if (end < 0) break;
      var m = /Content-Length: (\d+)/i.exec(buf.slice(i, end));
      if (!m) break;
      var len = parseInt(m[1], 10);
      msgs.push(buf.slice(end + 4, end + 4 + len));
      i = end + 4 + len;
    }
    return msgs;
  }

  function lspSend(method, params, id) {
    var o = { jsonrpc: '2.0', method: method };
    if (params !== undefined) o.params = params;
    if (id !== undefined) o.id = id;
    return parseFrames(lspHandle(JSON.stringify(o))).map(function (f) {
      try { return JSON.parse(f); } catch (e) { return null; }
    });
  }

  /* ─── Diagnostics rendering ───────────────────────────────────────────── */

  function clearSquiggles() {
    squiggleMarks.forEach(function (m) { m.clear(); });
    squiggleMarks = [];
  }

  function renderDiagnostics(diags) {
    clearSquiggles();
    if (!diagEl) return;

    if (!diags.length) {
      diagEl.innerHTML = '';
      var ok = document.createElement('div');
      ok.className = 'dl-ok';
      ok.textContent = '\u2713 no errors';
      diagEl.appendChild(ok);
      return;
    }

    diagEl.innerHTML = '';
    diags.forEach(function (d) {
      var row = document.createElement('div');
      row.className = 'dl-diag';
      var r = d.range;
      row.textContent = 'Ln ' + (r.start.line + 1) + ', Col ' + (r.start.character + 1) +
                        ' \u2014 ' + d.message.trim();
      diagEl.appendChild(row);

      /* squiggle the token (single-character range, as emitted by the server). */
      if (editor) {
        var from = { line: r.start.line, ch: r.start.character };
        var to = { line: r.end.line, ch: r.end.character };
        if (to.line === from.line && to.ch === from.ch) {
          var lineText = editor.getLine(from.line) || '';
          if (from.ch < lineText.length) to.ch = from.ch + 1;
        }
        squiggleMarks.push(editor.markText(from, to, { className: 'cm-dl-squiggle', title: d.message }));
      }
    });
  }

  function refreshDiagnostics() {
    if (!LspModule) return;
    lspVersion++;
    var frames = lspSend('textDocument/didChange', {
      textDocument: { uri: LSP_URI, version: lspVersion },
      contentChanges: [{ text: currentText() }]
    });
    var all = [];
    frames.forEach(function (f) {
      if (f && f.method === 'textDocument/publishDiagnostics' && f.params && f.params.diagnostics)
        all = all.concat(f.params.diagnostics);
    });
    renderDiagnostics(all);
  }

  /* ─── Hover ───────────────────────────────────────────────────────────── */

  function showTooltip(ev, text) {
    if (!tooltipEl) return;
    if (!text) { tooltipEl.hidden = true; return; }
    tooltipEl.textContent = text;
    tooltipEl.hidden = false;
    var wrap = document.getElementById('editor-wrap');
    var rect = wrap.getBoundingClientRect();
    tooltipEl.style.left = (ev.clientX - rect.left + 14) + 'px';
    tooltipEl.style.top = (ev.clientY - rect.top - 14) + 'px';
  }

  function doHover() {
    if (!LspModule || !editor) return;
    var cur = editor.getCursor();
    var frames = lspSend('textDocument/hover', {
      textDocument: { uri: LSP_URI },
      position: { line: cur.line, character: cur.ch }
    }, ++lspMsgId);
    var r = frames.find(function (f) { return f && f.id === lspMsgId; });
    if (r && r.result && r.result.contents) {
      return typeof r.result.contents === 'string'
        ? r.result.contents
        : r.result.contents.value;
    }
    return null;
  }

  /* ─── Boot ────────────────────────────────────────────────────────────── */

  function bootPlayground() {
    return window.createPlayground().then(function (M) {
      Module = M;
      runBtn.addEventListener('click', run);
    });
  }

  function bootLsp() {
    /* dl-lsp.js may be absent until `make wasm` runs on the host; degrade to a
     * syntax-highlighted editor without live diagnostics. */
    if (typeof window.createDlLsp !== 'function') {
      if (statusEl) statusEl.textContent =
        'ready (live diagnostics unavailable — run `make wasm` to build dl-lsp.js)';
      return Promise.resolve();
    }
    return window.createDlLsp().then(function (M) {
      LspModule = M;
      lspSend('initialize', {}, 1);
      lspSend('textDocument/didOpen', {
        textDocument: { uri: LSP_URI, languageId: 'datalog', version: lspVersion, text: currentText() }
      });
      refreshDiagnostics();

      /* live re-analysis on every edit (debounced). */
      var t = null;
      editor.on('change', function () {
        if (t) clearTimeout(t);
        t = setTimeout(function () { t = null; refreshDiagnostics(); }, 200);
      });

      /* hover on cursor movement (debounced). */
      var hoverTimer = null;
      editor.on('cursorActivity', function () {
        if (hoverTimer) clearTimeout(hoverTimer);
        hoverTimer = setTimeout(function () {
          hoverTimer = null;
          var pos = editor.cursorCoords(true, 'window');
          var txt = doHover();
          showTooltip({ clientX: pos.left, clientY: pos.top }, txt);
        }, 150);
      });
      editor.on('blur', function () { if (tooltipEl) tooltipEl.hidden = true; });
    });
  }

  /* Create the CodeMirror editor over the textarea, then boot both wasm
   * modules.  Run still evaluates immediately once the engine is ready. */
  if (typeof CodeMirror === 'undefined') {
    /* no CodeMirror: fall back to the plain textarea for Run only. */
    editor = null;
    progEl.value = DEFAULT_PROG;
    bootPlayground().then(function () {
      if (statusEl) statusEl.textContent = 'ready';
      run();
    }).catch(function (e) {
      if (statusEl) statusEl.textContent = 'failed to load wasm: ' + e;
    });
  } else {
    editor = CodeMirror.fromTextArea(progEl, {
      mode: 'datalog',
      lineNumbers: true,
      lineWrapping: true,
      indentUnit: 2
    });
    editor.setValue(DEFAULT_PROG);
    editor.refresh();

    Promise.all([bootPlayground(), bootLsp()]).then(function () {
      if (statusEl && Module) statusEl.textContent =
        'ready — the real C engine, compiled to WebAssembly, running in your browser';
      run();
    }).catch(function (e) {
      if (statusEl) statusEl.textContent = 'failed to load wasm: ' + e;
    });
  }
})();
