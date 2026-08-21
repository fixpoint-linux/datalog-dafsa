// shell/pages.js — canonical page definitions for datalog-dafsa MFE site.
//
// Single source of truth for all routes, templates, slots, and output paths.
// Imported by both shell/shell.js (browser ESM) and scripts/ssg.mjs (Node ESM).
//
// CANONICAL ROUTE TABLE (10 dafsa pages):
//   '/datalog-dafsa'              → template 'dafsa-landing'
//   '/datalog-dafsa/language'     → template 'dafsa-language'
//   '/datalog-dafsa/cli'          → template 'dafsa-cli'
//   '/datalog-dafsa/api'          → template 'dafsa-api'
//   '/datalog-dafsa/architecture'  → template 'dafsa-architecture'
//   '/datalog-dafsa/time-travel'  → template 'dafsa-time-travel'
//   '/datalog-dafsa/vector-search' → template 'dafsa-vector-search'
//   '/datalog-dafsa/order-statistics' → template 'dafsa-order-statistics'
//   '/datalog-dafsa/typed-projects' → template 'dafsa-typed-projects'
//   '/datalog-dafsa/playground'  → template 'dafsa-playground'
//
// SLOT NAME == TEMPLATE NAME for all dafsa pages.
// The landing page's `dir` is '' so its output is dist/index.html.
// All other content pages have dir == slug, output to dist/<slug>/index.html.
// The playground is NOT Elm-rendered (WASM + CodeMirror), so it's type 'playground'.
// The cross-nav home route '/' → 'fixpoint' is handled separately (main site owns
// /shell/templates/fixpoint.html and the importmap key 'fixpoint-landing').

export const PAGES = [
  {
    slug: 'datalog-dafsa',
    path: '/datalog-dafsa',
    slot: 'dafsa-landing',
    template: 'dafsa-landing',
    dir: '',
    title: 'datalog-dafsa — a DAFSA-backed Datalog engine that time-travels',
    type: 'content',
  },
  {
    slug: 'language',
    path: '/datalog-dafsa/language',
    slot: 'dafsa-language',
    template: 'dafsa-language',
    dir: 'language',
    title: 'Language — datalog-dafsa',
    type: 'content',
  },
  {
    slug: 'cli',
    path: '/datalog-dafsa/cli',
    slot: 'dafsa-cli',
    template: 'dafsa-cli',
    dir: 'cli',
    title: 'CLI — datalog-dafsa',
    type: 'content',
  },
  {
    slug: 'api',
    path: '/datalog-dafsa/api',
    slot: 'dafsa-api',
    template: 'dafsa-api',
    dir: 'api',
    title: 'API — datalog-dafsa',
    type: 'content',
  },
  {
    slug: 'architecture',
    path: '/datalog-dafsa/architecture',
    slot: 'dafsa-architecture',
    template: 'dafsa-architecture',
    dir: 'architecture',
    title: 'Architecture — datalog-dafsa',
    type: 'content',
  },
  {
    slug: 'time-travel',
    path: '/datalog-dafsa/time-travel',
    slot: 'dafsa-time-travel',
    template: 'dafsa-time-travel',
    dir: 'time-travel',
    title: 'Time Travel — datalog-dafsa',
    type: 'content',
  },
  {
    slug: 'vector-search',
    path: '/datalog-dafsa/vector-search',
    slot: 'dafsa-vector-search',
    template: 'dafsa-vector-search',
    dir: 'vector-search',
    title: 'Vector Search — datalog-dafsa',
    type: 'content',
  },
  {
    slug: 'order-statistics',
    path: '/datalog-dafsa/order-statistics',
    slot: 'dafsa-order-statistics',
    template: 'dafsa-order-statistics',
    dir: 'order-statistics',
    title: 'Order Statistics — datalog-dafsa',
    type: 'content',
  },
  {
    slug: 'typed-projects',
    path: '/datalog-dafsa/typed-projects',
    slot: 'dafsa-typed-projects',
    template: 'dafsa-typed-projects',
    dir: 'typed-projects',
    title: 'Typed Projects — datalog-dafsa',
    type: 'content',
  },
  {
    slug: 'playground',
    path: '/datalog-dafsa/playground',
    slot: 'dafsa-playground',
    template: 'dafsa-playground',
    dir: 'playground',
    title: 'Playground — datalog-dafsa',
    type: 'playground',
  },
];

// All content pages (Elm-rendered) — excludes playground
export const CONTENT_PAGES = PAGES.filter((p) => p.type === 'content');

// Just the dafsa pages (all of them)
export const DAFSA_PAGES = PAGES;
