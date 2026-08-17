// lsp-wasm-smoke.js — headless node test for docs/dl-lsp.js (emscripten
// MODULARIZE build of the LSP server).  Drives initialize -> didOpen
// (diagnostics) -> didChange (full sync) -> hover -> completion against the
// real wasm LSP core.
// Usage: node tests/lsp-wasm-smoke.js
const createDlLsp = require('../docs/dl-lsp.js');

const URI = 'file:///demo.dl';

function handle(Module, json) {
  const bytes = Module.lengthBytesUTF8(json);
  const ptr = Module._malloc(bytes + 1);
  Module.stringToUTF8(json, ptr, bytes + 1);
  Module._lsp_handle(ptr, bytes);
  Module._free(ptr);
  const outLen = Module._lsp_out_len();
  return outLen ? Module.UTF8ToString(Module._lsp_out(), outLen) : '';
}

// Split the Content-Length-framed response stream into message bodies.
function parseFrames(buf) {
  const msgs = [];
  let i = 0;
  while (i < buf.length) {
    const end = buf.indexOf('\r\n\r\n', i);
    if (end < 0) break;
    const m = /Content-Length: (\d+)/i.exec(buf.slice(i, end));
    if (!m) break;
    const len = parseInt(m[1], 10);
    msgs.push(buf.slice(end + 4, end + 4 + len));
    i = end + 4 + len;
  }
  return msgs;
}

function msg(method, params, id) {
  const o = { jsonrpc: '2.0', method };
  if (params !== undefined) o.params = params;
  if (id !== undefined) o.id = id;
  return JSON.stringify(o);
}

createDlLsp().then((Module) => {
  let fail = 0;
  function check(name, cond, detail) {
    console.log(`${name} => ${cond ? 'PASS' : 'FAIL'}${detail ? '  ' + detail : ''}`);
    if (!cond) fail++;
  }

  // initialize
  let out = handle(Module, msg('initialize', {}, 1));
  let frames = parseFrames(out).map(JSON.parse);
  check('initialize-capabilities',
    frames.some((f) => f.result && f.result.capabilities &&
      f.result.capabilities.hoverProvider === true &&
      f.result.capabilities.completionProvider === true &&
      f.result.capabilities.textDocumentSync === 1),
    out);

  // didOpen: compile error — 'edge' is undeclared, so the rule body references it.
  out = handle(Module, msg('textDocument/didOpen', {
    textDocument: { uri: URI, languageId: 'datalog', version: 1, text: 'path(X,Y) :- edge(X,Y).\n' },
  }));
  frames = parseFrames(out).map(JSON.parse);
  const diag = frames.find((f) => f.method === 'textDocument/publishDiagnostics');
  const d0 = diag && diag.params.diagnostics && diag.params.diagnostics[0];
  check('diagnostic-emitted', !!diag && !!d0, out);
  check('diagnostic-range', !!d0 && d0.range.start.line === 0 && d0.range.start.character === 0,
    d0 && JSON.stringify(d0.range));
  check('diagnostic-message', !!d0 && /compile error: unknown predicate/.test(d0.message),
    d0 && d0.message);

  // didChange (full sync) to a valid program -> empty diagnostics
  out = handle(Module, msg('textDocument/didChange', {
    textDocument: { uri: URI, version: 2 },
    contentChanges: [{ text: 'edge(a,b).\npath(X,Y) :- edge(X,Y).\n' }],
  }));
  frames = parseFrames(out).map(JSON.parse);
  const diag2 = frames.find((f) => f.method === 'textDocument/publishDiagnostics');
  check('empty-diagnostics-after-fix',
    !!diag2 && Array.isArray(diag2.params.diagnostics) && diag2.params.diagnostics.length === 0, out);

  // hover on the rule head 'path' (line 1) -> `path/2` — IDB
  out = handle(Module, msg('textDocument/hover', {
    textDocument: { uri: URI }, position: { line: 1, character: 0 },
  }, 2));
  frames = parseFrames(out).map(JSON.parse);
  const h = frames.find((f) => f.id === 2);
  check('hover-predicate-idb',
    !!h && h.result && h.result.contents && h.result.contents.value &&
      /path\/2/.test(h.result.contents.value) && /IDB/.test(h.result.contents.value),
    h && JSON.stringify(h.result));

  // hover on the fact 'edge' (line 0) -> `edge/2` — EDB
  out = handle(Module, msg('textDocument/hover', {
    textDocument: { uri: URI }, position: { line: 0, character: 0 },
  }, 3));
  frames = parseFrames(out).map(JSON.parse);
  const h2 = frames.find((f) => f.id === 3);
  check('hover-predicate-edb',
    !!h2 && h2.result && h2.result.contents && h2.result.contents.value &&
      /edge\/2/.test(h2.result.contents.value) && /EDB/.test(h2.result.contents.value),
    h2 && JSON.stringify(h2.result));

  // completion -> contains a builtin ('count') and a relation name ('edge')
  out = handle(Module, msg('textDocument/completion', {
    textDocument: { uri: URI }, position: { line: 0, character: 0 },
  }, 4));
  frames = parseFrames(out).map(JSON.parse);
  const c = frames.find((f) => f.id === 4);
  const labels = (c && c.result && c.result.items || []).map((i) => i.label);
  check('completion-builtin', labels.indexOf('count') >= 0, labels.join(','));
  check('completion-relation', labels.indexOf('edge') >= 0, labels.join(','));

  process.exit(fail ? 1 : 0);
}).catch((e) => {
  console.error('failed to load wasm:', e);
  process.exit(1);
});
