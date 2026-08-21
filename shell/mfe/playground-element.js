// shell/mfe/playground-element.js — the <datalog-playground> custom element.
//
// Defines a custom element that encapsulates the WASM + CodeMirror playground editor.
// The element builds its editor DOM on connectedCallback and loads the required
// scripts (playground.js, dl-lsp.js, CodeMirror, datalog-mode.js, playground-ui.js).
//
// DOM contract (must be satisfied for playground-ui.js):
//   #program   — textarea (converted to CodeMirror)
//   #goal      — text input for the goal relation name
//   #runBtn    — the Run button
//   #output    — <pre> pane for the result
//   #status    — optional status line
//   #diagnostics — LSP diagnostics list
//   #tooltip   — hover tooltip
//   #editor-wrap — position:relative wrapper for tooltip
//
// The element uses light DOM (NOT shadow DOM) because playground-ui.js uses
// document.getElementById to find these elements.

const BASE = '/datalog-dafsa';

// Once-per-element boot guard.
const BOOTED = Symbol('booted');

// Reusable script loading (cached per URL).
const scriptCache = new Map();

function loadScript(src) {
  if (scriptCache.has(src)) {
    return scriptCache.get(src);
  }
  const promise = new Promise((resolve, reject) => {
    const s = document.createElement('script');
    s.src = src;
    s.async = false; // preserve ordering
    s.onload = () => { s.remove(); resolve(); };
    s.onerror = () => { s.remove(); reject(new Error(`playground-element: failed to load ${src}`)); };
    (document.head || document.documentElement).appendChild(s);
  });
  scriptCache.set(src, promise);
  return promise;
}

function loadFactories() {
  return Promise.all([
    loadScript(`${BASE}/playground.js`),
    loadScript(`${BASE}/dl-lsp.js`),
    loadScript(`${BASE}/vendor/codemirror.min.js`),
    loadScript(`${BASE}/vendor/codemirror-simple.js`),
    loadScript(`${BASE}/vendor/datalog-mode.js`),
  ]);
}

// Minimal DOM builder helper.
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

// Consolidated playground CSS: the fixpoint palette + the note/warn/code/
// playground-controls rules from docs/style.css + the CodeMirror Tokyo-Night /
// squiggle / tooltip / editor-wrap / output rules.
const PLAYGROUND_CSS = `
:root{--bg:#0b0e11;--bg2:#10141a;--fg:#d8dee6;--dim:#7d8794;--accent:#6ad6a1;--accent2:#8ab4f8;--line:#1e2730;--mono:"SFMono-Regular","Cascadia Code","JetBrains Mono","Fira Code",Menlo,Consolas,monospace;--sans:-apple-system,"Segoe UI",Roboto,Helvetica,Arial,sans-serif}
* {box-sizing:border-box;margin:0;padding:0}
body {background:var(--bg);color:var(--fg);font-family:var(--sans);line-height:1.6;-webkit-font-smoothing:antialiased}
.wrap {max-width:780px;margin:0 auto;padding:0 24px}
.site {position:sticky;top:0;z-index:50;background:rgba(11,14,17,0.88);backdrop-filter:blur(8px);border-bottom:1px solid var(--line)}
.site .wrap {display:flex;align-items:center;justify-content:space-between;flex-wrap:wrap;column-gap:16px;row-gap:6px;min-height:52px;padding:10px 24px}
.site .brand {font-family:var(--mono);font-weight:700;color:var(--fg);text-decoration:none;font-size:15px;margin-right:8px}
.site .brand .fx {color:var(--accent)}
.site nav {display:flex;flex-wrap:wrap;gap:4px 14px}
.site nav a {color:var(--dim);text-decoration:none;font-family:var(--mono);font-size:13px}
.site nav a:hover {color:var(--fg)}
.site nav a.ext {color:var(--accent);font-weight:600;margin-left:6px}
.site nav a.nav-cta {color:var(--bg);background:var(--accent);border-radius:14px;padding:2px 12px;font-weight:600}
.site nav a.nav-cta:hover {background:var(--accent2)}
h1 {font-size:clamp(26px,4.5vw,38px);font-weight:750;letter-spacing:-0.02em;margin:0.6em 0 0.3em}
h2 {font-size:1.35em;font-weight:650;margin:1.8em 0 0.4em;letter-spacing:-0.01em;border-bottom:1px solid var(--line);padding-bottom:0.25em}
p {margin:0.6em 0;color:#c3ccd6}
a {color:var(--accent2);text-decoration:none}
a:hover {text-decoration:underline}
code,.code {font-family:var(--mono);font-size:0.9em}
p code,li code,td code,h2 code,h3 code {background:var(--bg2);border:1px solid var(--line);border-radius:5px;padding:1px 6px;color:var(--accent)}
pre.code {background:var(--bg2);border:1px solid var(--line);border-radius:10px;padding:16px 18px;overflow-x:auto;margin:0.8em 0;line-height:1.5;white-space:pre;color:#c3ccd6}
pre.code code {background:none;padding:0;font-size:0.9em}
.note {background:#1a2e24;border-left:4px solid var(--accent);padding:10px 14px;border-radius:6px;margin:1em 0;color:#d8dee6}
.warn {background:#2e1a1e;border-left:4px solid #ff7b72;padding:10px 14px;border-radius:6px;margin:1em 0;color:#ffd7d5}
ul {list-style:none;padding-left:0}
ul li {color:#c3ccd6;margin-bottom:6px}
#program {width:100%;font-family:var(--mono);font-size:0.9em;line-height:1.5;background:var(--bg2);border:1px solid var(--line);border-radius:8px;padding:14px;color:var(--fg);margin:0.5em 0;resize:vertical;tab-size:4}
.playground-controls {display:flex;align-items:center;gap:10px;flex-wrap:wrap;margin:0.6em 0 1.2em}
.playground-controls label {font-weight:600;font-size:0.95em}
.playground-controls input[type="text"] {font-family:var(--mono);font-size:0.95em;padding:6px 10px;border:1px solid var(--line);border-radius:6px;background:var(--bg2);color:var(--fg)}
.playground-controls button {font-size:0.95em;font-weight:600;padding:7px 18px;border:none;border-radius:6px;background:var(--accent);color:var(--bg);cursor:pointer}
.playground-controls button:hover {background:var(--accent2)}
#output {min-height:4em;white-space:pre-wrap;word-break:break-word;background:#16161e;color:#c0caf5;border:1px solid #414868}
footer.site-foot {margin-top:3em;padding:1.4em 0 2em;border-top:1px solid var(--line);color:var(--dim);font-family:var(--mono);font-size:13px;text-align:center}
.CodeMirror {border:1px solid #414868;height:440px;font-size:14px}
.CodeMirror,.CodeMirror-scroll {background:#1a1b26;color:#c0caf5}
.CodeMirror-gutters {background:#16161e;border-right:1px solid #292e42}
.CodeMirror-linenumber {color:#3b4261}
.CodeMirror-cursor {border-left:2px solid #c0caf5}
.CodeMirror-selected {background:#33467c}
.cm-s-tokyonight .cm-comment {color:#565f89;font-style:italic;}
.cm-s-tokyonight .cm-string {color:#9ece6a;}
.cm-s-tokyonight .cm-number {color:#ff9e64;}
.cm-s-tokyonight .cm-keyword {color:#bb9af7;font-weight:600;}
.cm-s-tokyonight .cm-variable-2 {color:#7dcfff;}
.cm-s-tokyonight .cm-pred {color:#89ddff;font-weight:600;}
.cm-s-tokyonight .cm-atom {color:#e0af68;}
.cm-s-tokyonight .cm-operator {color:#89ddff;}
.cm-s-tokyonight .cm-bracket {color:#c0caf5;}
.cm-s-tokyonight .cm-def {color:#73daca;font-weight:600;}
.cm-dl-squiggle {text-decoration:underline wavy #f7768e;text-decoration-skip-ink:none;}
#diagnostics {margin:12px 0;font-size:14px}
#diagnostics .dl-diag {color:#f7768e;padding:2px 0;font-family:monospace;}
#diagnostics .dl-ok {color:#9ece6a;}
#tooltip {
  position:absolute;z-index:10;background:#1f2335;border:1px solid #7aa2f7;
  color:#c0caf5;padding:4px 8px;font-size:13px;font-family:monospace;
  max-width:420px;white-space:pre-wrap;pointer-events:none;border-radius:3px;
  box-shadow:0 2px 8px rgba(0,0,0,0.5)}
#editor-wrap {position:relative}
`;

/**
 * Wait until `element` is attached to the live document.
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
 * Boot the playground: load factories then playground-ui.js against the element's DOM.
 */
async function bootPlayground(element) {
  await waitConnected(element);
  await loadFactories();
  await loadScript(`${BASE}/playground-ui.js`);
}

/**
 * The <datalog-playground> custom element.
 */
class DatalogPlayground extends HTMLElement {
  constructor() {
    super();
    this[BOOTED] = false;
    this._styleEl = null;
    this._linkEl = null;
  }

  connectedCallback() {
    if (this[BOOTED]) return;
    this[BOOTED] = true;

    // Build the editor DOM inside the element (light DOM).
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
    const output = el('pre', { id: 'output', class: 'code' }, ['(no results)']);

    // Clear any existing content and append the editor DOM.
    this.textContent = '';
    this.appendChild(status);
    this.appendChild(editorWrap);
    this.appendChild(diagnostics);
    this.appendChild(controls);
    this.appendChild(output);

    // Inject CSS: codemirror.css link + inline PLAYGROUND_CSS.
    this._linkEl = document.createElement('link');
    this._linkEl.rel = 'stylesheet';
    this._linkEl.href = `${BASE}/vendor/codemirror.css`;
    document.head.appendChild(this._linkEl);

    this._styleEl = document.createElement('style');
    this._styleEl.textContent = PLAYGROUND_CSS;
    document.head.appendChild(this._styleEl);

    // Boot the playground scripts non-blocking.
    void bootPlayground(this);
  }

  disconnectedCallback() {
    // Clean up injected styles if this element is removed.
    if (this._styleEl) {
      this._styleEl.remove();
      this._styleEl = null;
    }
    if (this._linkEl) {
      this._linkEl.remove();
      this._linkEl = null;
    }
  }
}

// Register the custom element.
customElements.define('datalog-playground', DatalogPlayground);
