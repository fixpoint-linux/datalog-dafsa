// shell/mfe/dafsa-page.js — the datalog-dafsa docs pages (all 9 content pages)
// as ONE @mfe MFE module backed by a single Elm bundle.
//
// Nine data-mfe slots (dafsa-landing, dafsa-language, dafsa-cli, dafsa-api,
// dafsa-architecture, dafsa-time-travel, dafsa-vector-search,
// dafsa-order-statistics, dafsa-typed-projects) all resolve to THIS module via
// the import map. The browser dedupes the module by its resolved URL, so there
// is ONE shared module instance; each slot mount calls mount() with a different
// element. The Elm app selects WHICH page to render from the `pathname` flag
// (the shell router pushes the path via history.pushState, so
// window.location.pathname always reflects the active sub-page).
//
// This mirrors fxstore-page.js's seam (fetch + scoped-eval of dist/elm.js), the
// only difference being the flags payload { pathname }.
//
// IMPORTANT (reconcile subtlety — see handoff-dafsa-mfe-plan "KEY CRITICAL
// FINDING"): @mfe/core's registry is keyed by slot NAME but makeRef is
// STRUCTURAL, so a shared slot name across the 9 pages would send reconcile
// down its TRANSPLANT + update(prev===next) bail path and show stale content.
// That is why each page has its OWN slot name — reconcile then performs a clean
// NEW mount + Pass-2 unmount of the previous slot on cross-page nav.

const ELM_URL = new URL('../../elm.js', import.meta.url).href;

// Cache the load promise so the bundle is fetched and evaluated exactly once.
let elmPromise = null;

// Every compiled Elm bundle is `(function(scope){ ... })(this)`, registering
// its modules on `scope` (defaulting to globalThis). Loading a SECOND bundle
// via indirect eval would overwrite the shared globalThis.Elm, so
// _Platform_export -> _Platform_mergeExportsProd would hit the existing
// `Main` -> `init` key and _Debug_crash(6) ("name clash"). Each MFE therefore
// evaluates its own bundle into a PRIVATE scope object, giving every Elm app
// its own `Elm` and avoiding the global collision entirely — crucial when two
// sites' MFEs load in the same shell page.
function evalBundle(code) {
  const scope = {};
  // new Function body is the bundle; calling it with .call(scope) binds the
  // bundle's `(this)` to `scope`, so Elm lands on scope.Elm, not globalThis.
  // eslint-disable-next-line no-new-func -- CSP constraints match indirect eval.
  new Function(code).call(scope);
  return scope.Elm;
}

/** Fetch + scoped-eval the compiled Elm bundle and return the Elm object. */
function loadElm() {
  if (!elmPromise) {
    elmPromise = (async () => {
      const res = await fetch(ELM_URL);
      if (!res.ok) {
        throw new Error(`dafsa-page: failed to fetch ${ELM_URL} (HTTP ${res.status})`);
      }
      const code = await res.text();
      const Elm = evalBundle(code);
      if (!Elm || !Elm.Main || typeof Elm.Main.init !== 'function') {
        throw new Error('dafsa-page: dist/elm.js did not expose Elm.Main.init');
      }
      return Elm;
    })().catch((err) => {
      // Reset so a later mount can retry instead of being permanently poisoned.
      elmPromise = null;
      throw err;
    });
  }
  return elmPromise;
}

// Tracks slot elements that already hold a live Elm app, so re-entrant mounts
// (e.g. an SSR rehydration pass) never double-initialize the same node.
const live = new WeakMap();

function clearChildren(element) {
  while (element.firstChild) {
    element.removeChild(element.firstChild);
  }
}

/** The MFE lifecycle, per @mfe/core types.ts. */
export default {
  async mount(element, ctx) {
    if (live.has(element)) return; // already inited into this node
    const Elm = await loadElm();
    // On a fresh template the slot is empty; after SSR rehydration it already
    // holds the pre-rendered markup. Clearing first guarantees a single,
    // drift-free render from Elm regardless of which case we're in.
    clearChildren(element);
    // Elm.Main.init({node, flags}) replaces the given node with its rendered root via
    // parentNode.replaceChild. Initializing into the [data-mfe] slot element
    // ITSELF would therefore strip the slot's data-mfe attribute and break
    // @mfe/core's reconcile slot-tracking. Mount into a fresh INNER wrapper div
    // instead: the slot element stays in the DOM (data-mfe intact) and Elm
    // replaces only the wrapper, leaving the Elm root (including the <style>
    // emitted by Fixpoint.Style.stylesheet) as the slot's child.
    //
    // Pass the current pathname as a flag so the Elm app knows which page to render.
    const inner = document.createElement('div');
    element.appendChild(inner);
    const app = Elm.Main.init({ node: inner, flags: { pathname: window.location.pathname } });
    live.set(element, app);
  },

  async unmount(element, ctx) {
    if (!live.has(element)) return;
    clearChildren(element);
    live.delete(element);
  },

  async update(prev, next, ctx) {
    // The slot moved structurally (reconcile's UPDATE path): move Elm's live
    // rendered subtree from prev to next, preserving in-app state with no
    // re-init. @mfe/core's transplant covers the same-ref case; we only handle
    // the moved-ref case here.
    //
    // Reconcile can also call update(prev, next) where prev === next (same
    // ref, props changed, after it already transplanted). In that case the DOM
    // is already in place and clearing would wipe it, so bail out.
    if (prev === next) return;
    const app = live.get(prev);
    clearChildren(next);
    for (const child of Array.from(prev.childNodes)) {
      next.appendChild(child);
    }
    if (app) {
      live.delete(prev);
      live.set(next, app);
    }
  },
};
