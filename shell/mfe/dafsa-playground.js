// shell/mfe/dafsa-playground.js — the interactive WASM + CodeMirror playground
// as a non-Elm @mfe MFE module.
//
// The playground cannot be Elm-ified: docs/playground-ui.js is an id-driven IIFE
// that wires #program/#goal/#runBtn/#output/#status/#diagnostics/#tooltip/#editor-wrap
// into two MODULARIZE'd emscripten factories (window.createPlayground from
// playground.js, window.createDlLsp from dl-lsp.js) plus CodeMirror.
//
// Two hard constraints (validated against docs/playground.js line 1):
//   1. The emscripten factories capture `document.currentScript.src` AT LOAD to
//      derive scriptDirectory for locateFile("*.wasm"). They MUST be loaded as
//      REAL <script src> tags (never fetch+eval, which leaves currentScript
//      null and breaks the wasm path).
//   2. The .wasm files MUST be colocated with their .js (dist/playground.wasm
//      next to dist/playground.js) because scriptDirectory resolves relative to
//      the script's own URL.
//
// Load order (must match docs/playground.html): playground.js -> dl-lsp.js ->
// codemirror.min.js -> codemirror-simple.js -> datalog-mode.js -> playground-ui.js.
// The factory + CodeMirror scripts are loaded ONCE (module-level cache); the
// playground-ui.js IIFE is re-run on every mount because it captures its DOM
// element references at load time.

const BASE = '/datalog-dafsa';

// Once-per-page-session load of the reusable classic scripts (factories +
// CodeMirror). These define globals (window.createPlayground, window.createDlLsp,
// window.CodeMirror); loaded exactly once so re-mounts don't double-define.
let factoriesPromise = null;

function loadScript(src) {
  return new Promise((resolve, reject) => {
    const s = document.createElement('script');
    s.src = src;
    s.async = false; // preserve ordering
    s.onload = () => { s.remove(); resolve(); };
    s.onerror = () => { s.remove(); reject(new Error(`dafsa-playground: failed to load ${src}`)); };
    (document.head || document.documentElement).appendChild(s);
  });
}

function loadFactories() {
  if (!factoriesPromise) {
    factoriesPromise = (async () => {
      await loadScript(`${BASE}/playground.js`);
      await loadScript(`${BASE}/dl-lsp.js`);
      await loadScript(`${BASE}/vendor/codemirror.min.js`);
      await loadScript(`${BASE}/vendor/codemirror-simple.js`);
      await loadScript(`${BASE}/vendor/datalog-mode.js`);
    })().catch((err) => {
      factoriesPromise = null;
      throw err;
    });
  }
  return factoriesPromise;
}

// Minimal DOM builder for the static chrome.
function el(tag, attrs, children) {
  const node = document.createElement(tag);
  if (attrs) {
    for (const [k, v] of Object.entries(attrs)) {
      if (k === 'class') node.className = v;
      else if (k === 'html') node.innerHTML = v;
      else node.setAttribute(k, v);
    }
  }
  for (const c of children || []) {
    node.appendChild(typeof c === 'string' ? document.createTextNode(c) : c);
  }
  return node;
}

// Nav items mirroring docs/playground.html header (raw <a> + data-mfe-route so
// the shell router intercepts in-shell cross-nav; absolute href for fresh loads).
const NAV_ITEMS = [
  ['/datalog-dafsa', 'Overview'],
  ['/datalog-dafsa/language', 'Language'],
  ['/datalog-dafsa/cli', 'CLI'],
  ['/datalog-dafsa/api', 'C API'],
  ['/datalog-dafsa/architecture', 'Architecture'],
  ['/datalog-dafsa/order-statistics', 'Order Stats'],
  ['/datalog-dafsa/time-travel', 'Time Travel'],
  ['/datalog-dafsa/vector-search', 'Vector Search'],
  ['/datalog-dafsa/typed-projects', 'Typed Projects'],
];

function buildHeader() {
  const brand = el('a', { class: 'brand', href: `${BASE}/` }, [
    el('span', { class: 'fx' }, ['fx']),
    '://datalog-dafsa',
  ]);
  const nav = el('nav', null, []);
  for (const [route, label] of NAV_ITEMS) {
    nav.appendChild(el('a', {
      href: `https://fixpointlinux.org${route}/`,
      'data-mfe-route': route,
    }, [label]));
  }
  nav.appendChild(el('a', { class: 'nav-cta', href: `${BASE}/playground/`, 'data-mfe-route': '/datalog-dafsa/playground' }, ['Playground']));
  nav.appendChild(el('a', { class: 'ext', href: 'https://fixpointlinux.org/', 'data-mfe-route': '/' }, ['fixpoint-linux \u2192']));
  const wrap = el('div', { class: 'wrap' }, [brand, nav]);
  return el('header', { class: 'site' }, [wrap]);
}

function buildFooter() {
  return el('footer', { class: 'site-foot' }, [
    el('div', { class: 'wrap' }, ['The playground runs ', el('code', null, ['src/playground-wasm.c']), ' \u2014 a thin entry point over the same engine core as the CLI \u2014 compiled to WebAssembly.']),
  ]);
}

const SUBSET_ITEMS =
  '<li><strong>Facts</strong> \u2014 bare lowercase symbols, double-quoted strings, integers, and <em>flat</em> list literals (elements are single values; nested lists aren\u2019t yet accepted in a fact).</li>' +
  '<li><strong>Rules</strong> \u2014 including recursion (semi-naive fixpoint), stratified negation (<code>!q(X)</code>), and conjunctions.</li>' +
  '<li><strong>Aggregates</strong> \u2014 <code>count()</code>, <code>sum()</code>, <code>min()</code>, <code>max()</code>.</li>' +
  '<li><strong>Equality &amp; comparisons</strong> \u2014 <code>=</code>, <code>&lt;</code> <code>&lt;=</code> <code>&gt;</code> <code>&gt;=</code> <code>!=</code>.</li>' +
  '<li><strong>Arithmetic</strong> \u2014 <code>+</code> <code>-</code> <code>*</code> <code>/</code> <code>%</code>.</li>' +
  '<li><strong>Strings</strong> \u2014 <code>concat</code>, <code>length</code>, <code>lower</code>, <code>upper</code>, <code>prefix</code>, <code>suffix</code>, <code>contains</code>.</li>' +
  '<li><strong>Lists</strong> \u2014 <code>cons</code>, <code>car</code>, <code>cdr</code>, <code>append</code>, <code>member</code>, <code>[X|Xs]</code> patterns.</li>' +
  '<li><strong>Range predicate</strong> \u2014 <code>range(X, Rel, Lo, Hi)</code>.</li>' +
  '<li><strong>Regex pattern walks</strong> \u2014 the <code>~ \'...\'</code> form.</li>';

function buildBody() {
  const h1 = el('h1', null, ['Playground']);
  const intro = el('p', { html: 'Type Datalog facts and rules below and hit <strong>Run</strong>. The program is evaluated <em>entirely in your browser</em> by the real C engine, compiled to WebAssembly \u2014 no server, no filesystem. The goal relation&rsquo;s tuples are streamed back as plain text.' });
  const status = el('p', { id: 'status', class: 'note' });
  const editorWrap = el('div', { id: 'editor-wrap' }, [
    el('textarea', { id: 'program', rows: '16', spellcheck: 'false', 'aria-label': 'Datalog program' }),
    el('div', { id: 'tooltip', hidden: '' }),
  ]);
  const diagnostics = el('div', { id: 'diagnostics', class: 'note' });
  const controls = el('div', { class: 'playground-controls' }, [
    el('label', { for: 'goal' }, ['Goal relation']),
    el('input', { id: 'goal', type: 'text', value: 'path', 'aria-label': 'Goal relation name' }),
    el('button', { id: 'runBtn', type: 'button' }, ['Run']),
  ]);
  const outH2 = el('h2', null, ['Output']);
  const output = el('pre', { id: 'output', class: 'code' }, ['(no results)']);
  const subsetH2 = el('h2', null, ['Supported subset']);
  const subsetIntro = el('p', { html: 'The in-browser engine supports the <em>in-memory</em> feature set of the language:' });
  const subsetList = el('ul', { html: SUBSET_ITEMS });
  const warn = el('p', { class: 'warn', html: 'Not supported in the browser: <code>publish</code>/snapshot, time-travel (as-of) queries, variadic relations, and the WAL / incremental-maintenance API \u2014 all of which require the disk-backed engine.' });
  const note = el('p', { class: 'note', html: 'Rendering caveat: the engine uses the CLI&rsquo;s value heuristic, so a raw integer result (including one inside a list literal) that happens to equal a small symbol id can display as that symbol instead of the number. This is the documented B6 int-vs-symbol collision \u2014 a display heuristic, not a bug. Examples above use integer data to stay unambiguous.' });
  const tail = el('p', { html: 'The WebAssembly bundle is rebuilt from the C source with <code>make wasm</code>; see <a href="https://github.com/fixpoint-linux/datalog-dafsa">the repo</a> for details.' });
  return el('main', { class: 'wrap' }, [h1, intro, status, editorWrap, diagnostics, controls, outH2, output, subsetH2, subsetIntro, subsetList, warn, note, tail]);
}

// Consolidated playground CSS: the fixpoint palette + the note/warn/code/
// playground-controls rules from docs/style.css + the CodeMirror Tokyo-Night /
// squiggle / tooltip / editor-wrap / output rules from docs/playground.html.
const PLAYGROUND_CSS = `
:root{--bg:#0b0e11;--bg2:#10141a;--fg:#d8dee6;--dim:#7d8794;--accent:#6ad6a1;--accent2:#8ab4f8;--line:#1e2730;--mono:"SFMono-Regular","Cascadia Code","JetBrains Mono","Fira Code",Menlo,Consolas,monospace;--sans:-apple-system,"Segoe UI",Roboto,Helvetica,Arial,sans-serif}
*{box-sizing:border-box;margin:0;padding:0}
body{background:var(--bg);color:var(--fg);font-family:var(--sans);line-height:1.6;-webkit-font-smoothing:antialiased}
.wrap{max-width:780px;margin:0 auto;padding:0 24px}
.site{position:sticky;top:0;z-index:50;background:rgba(11,14,17,0.88);backdrop-filter:blur(8px);border-bottom:1px solid var(--line)}
.site .wrap{display:flex;align-items:center;justify-content:space-between;flex-wrap:wrap;column-gap:16px;row-gap:6px;min-height:52px;padding:10px 24px}
.site .brand{font-family:var(--mono);font-weight:700;color:var(--fg);text-decoration:none;font-size:15px;margin-right:8px}
.site .brand .fx{color:var(--accent)}
.site nav{display:flex;flex-wrap:wrap;gap:4px 14px}
.site nav a{color:var(--dim);text-decoration:none;font-family:var(--mono);font-size:13px}
.site nav a:hover{color:var(--fg)}
.site nav a.ext{color:var(--accent);font-weight:600;margin-left:6px}
.site nav a.nav-cta{color:var(--bg);background:var(--accent);border-radius:14px;padding:2px 12px;font-weight:600}
.site nav a.nav-cta:hover{background:var(--accent2)}
h1{font-size:clamp(26px,4.5vw,38px);font-weight:750;letter-spacing:-0.02em;margin:0.6em 0 0.3em}
h2{font-size:1.35em;font-weight:650;margin:1.8em 0 0.4em;letter-spacing:-0.01em;border-bottom:1px solid var(--line);padding-bottom:0.25em}
p{margin:0.6em 0;color:#c3ccd6}
a{color:var(--accent2);text-decoration:none}
a:hover{text-decoration:underline}
code,.code{font-family:var(--mono);font-size:0.9em}
p code,li code,td code,h2 code,h3 code{background:var(--bg2);border:1px solid var(--line);border-radius:5px;padding:1px 6px;color:var(--accent)}
pre.code{background:var(--bg2);border:1px solid var(--line);border-radius:10px;padding:16px 18px;overflow-x:auto;margin:0.8em 0;line-height:1.5;white-space:pre;color:#c3ccd6}
pre.code code{background:none;padding:0;font-size:0.9em}
.note{background:#1a2e24;border-left:4px solid var(--accent);padding:10px 14px;border-radius:6px;margin:1em 0;color:#d8dee6}
.warn{background:#2e1a1e;border-left:4px solid #ff7b72;padding:10px 14px;border-radius:6px;margin:1em 0;color:#ffd7d5}
ul{list-style:none;padding-left:0}
ul li{color:#c3ccd6;margin-bottom:6px}
#program{width:100%;font-family:var(--mono);font-size:0.9em;line-height:1.5;background:var(--bg2);border:1px solid var(--line);border-radius:8px;padding:14px;color:var(--fg);margin:0.5em 0;resize:vertical;tab-size:4}
.playground-controls{display:flex;align-items:center;gap:10px;flex-wrap:wrap;margin:0.6em 0 1.2em}
.playground-controls label{font-weight:600;font-size:0.95em}
.playground-controls input[type="text"]{font-family:var(--mono);font-size:0.95em;padding:6px 10px;border:1px solid var(--line);border-radius:6px;background:var(--bg2);color:var(--fg)}
.playground-controls button{font-size:0.95em;font-weight:600;padding:7px 18px;border:none;border-radius:6px;background:var(--accent);color:var(--bg);cursor:pointer}
.playground-controls button:hover{background:var(--accent2)}
#output{min-height:4em;white-space:pre-wrap;word-break:break-word;background:#16161e;color:#c0caf5;border:1px solid #414868}
footer.site-foot{margin-top:3em;padding:1.4em 0 2em;border-top:1px solid var(--line);color:var(--dim);font-family:var(--mono);font-size:13px;text-align:center}
.CodeMirror{border:1px solid #414868;height:440px;font-size:14px}
.CodeMirror,.CodeMirror-scroll{background:#1a1b26;color:#c0caf5}
.CodeMirror-gutters{background:#16161e;border-right:1px solid #292e42}
.CodeMirror-linenumber{color:#3b4261}
.CodeMirror-cursor{border-left:2px solid #c0caf5}
.CodeMirror-selected{background:#33467c}
.cm-s-tokyonight .cm-comment{color:#565f89;font-style:italic}
.cm-s-tokyonight .cm-string{color:#9ece6a}
.cm-s-tokyonight .cm-number{color:#ff9e64}
.cm-s-tokyonight .cm-keyword{color:#bb9af7;font-weight:600}
.cm-s-tokyonight .cm-variable-2{color:#7dcfff}
.cm-s-tokyonight .cm-pred{color:#89ddff;font-weight:600}
.cm-s-tokyonight .cm-atom{color:#e0af68}
.cm-s-tokyonight .cm-operator{color:#89ddff}
.cm-s-tokyonight .cm-bracket{color:#c0caf5}
.cm-s-tokyonight .cm-def{color:#73daca;font-weight:600}
.cm-dl-squiggle{text-decoration:underline wavy #f7768e;text-decoration-skip-ink:none}
#diagnostics{margin:12px 0;font-size:14px}
#diagnostics .dl-diag{color:#f7768e;padding:2px 0;font-family:monospace}
#diagnostics .dl-ok{color:#9ece6a}
#tooltip{position:absolute;z-index:10;background:#1f2335;border:1px solid #7aa2f7;color:#c0caf5;padding:4px 8px;font-size:13px;font-family:monospace;max-width:420px;white-space:pre-wrap;pointer-events:none;border-radius:3px;box-shadow:0 2px 8px rgba(0,0,0,0.5)}
#editor-wrap{position:relative}
`;

const live = new WeakMap();
const headTags = new WeakMap();

function clearChildren(node) {
  while (node.firstChild) node.removeChild(node.firstChild);
}

/**
 * Wait until `element` is attached to the live document. The framework
 * reconciles a DETACHED template clone and only appends it to the DOM after
 * every MFE `mount()` resolves, so a mount that blocks here would deadlock.
 * Call this only in an async post-mount continuation (never awaited inline in
 * a way that blocks mount's own promise from resolving).
 */
function waitConnected(element) {
  if (element.isConnected) return Promise.resolve();
  return new Promise((resolve) => {
    const check = () => {
      if (element.isConnected) resolve();
      else requestAnimationFrame(check);
    };
    requestAnimationFrame(check);
  });
}

/**
 * Load the emscripten factories (cached) and then re-run playground-ui.js
 * against the fresh DOM, once the slot is attached to the live document.
 * playground-ui.js is an IIFE that captures its DOM element references
 * (document.getElementById('program') etc.) at load time, so it MUST run only
 * once #program is actually reachable via the document.
 */
async function bootPlayground(element) {
  await waitConnected(element);
  await loadFactories();
  await loadScript(`${BASE}/playground-ui.js`);
}

/** The MFE lifecycle, per @mfe/core types.ts. */
export default {
  async mount(element, ctx) {
    if (live.has(element)) return;
    // 1. Build the full page DOM (header + body + footer) into the slot.
    clearChildren(element);
    element.appendChild(buildHeader());
    element.appendChild(buildBody());
    element.appendChild(buildFooter());
    // 2. Inject CSS via the host (tracking disposers for unmount).
    const disposers = [];
    const link = document.createElement('link');
    link.rel = 'stylesheet';
    link.href = `${BASE}/vendor/codemirror.css`;
    disposers.push(ctx.host.addHeadTag(link));
    const style = document.createElement('style');
    style.textContent = PLAYGROUND_CSS;
    disposers.push(ctx.host.addHeadTag(style));
    headTags.set(element, disposers);
    // 3. Kick off script boot in a non-blocking continuation. mount() must
    //    resolve NOW so the framework appends the (already content-filled)
    //    slot to the document; only once connected do we load the factories +
    //    playground-ui.js. A mount that awaited connection here would never
    //    resolve (the append happens only after mount resolves).
    void bootPlayground(element);
    live.set(element, true);
  },

  async unmount(element, ctx) {
    if (!live.has(element)) return;
    clearChildren(element);
    const disposers = headTags.get(element);
    if (disposers) for (const d of disposers) d();
    headTags.delete(element);
    live.delete(element);
  },

  async update(prev, next, ctx) {
    if (prev === next) return;
    clearChildren(next);
    for (const child of Array.from(prev.childNodes)) next.appendChild(child);
    if (live.has(prev)) {
      live.delete(prev);
      live.set(next, true);
    }
    const disposers = headTags.get(prev);
    if (disposers) {
      headTags.delete(prev);
      headTags.set(next, disposers);
    }
  },
};
