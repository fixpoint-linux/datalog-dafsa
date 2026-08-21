// shell/shell.js — @mfe/framework thin-shell entry for datalog-dafsa MFE site.
//
// Boots the datalog-dafsa docs app with 11 routes:
//   '/'                    → template 'fixpoint'        (cross-nav home, main site)
//   '/datalog-dafsa'       → template 'dafsa-landing'
//   '/datalog-dafsa/language'     → template 'dafsa-language'
//   '/datalog-dafsa/cli'         → template 'dafsa-cli'
//   '/datalog-dafsa/api'         → template 'dafsa-api'
//   '/datalog-dafsa/architecture' → template 'dafsa-architecture'
//   '/datalog-dafsa/time-travel' → template 'dafsa-time-travel'
//   '/datalog-dafsa/vector-search' → template 'dafsa-vector-search'
//   '/datalog-dafsa/order-statistics' → template 'dafsa-order-statistics'
//   '/datalog-dafsa/typed-projects' → template 'dafsa-typed-projects'
//   '/datalog-dafsa/playground' → template 'dafsa-playground'
//
// Matching the main site means a data-mfe-route like '/datalog-dafsa' or '/'
// resolves the same way on either page, so cross-site MFE nav links agree.
//
// The pages ship statically pre-rendered (see scripts/ssg.mjs): the #app root
// carries an `ssr` attribute, so createApp rehydrates the existing DOM in
// place instead of wiping it and re-fetching the template on first paint.
//
// Rehydrate only when the current pathname (trailing-slash-stripped) matches
// a pre-rendered dafsa page (i.e. starts with /datalog-dafsa and is NOT
// /datalog-dafsa/playground, since playground isn't pre-rendered).

import { createApp } from '@mfe/framework';

const app = await createApp({
  root: document.getElementById('app'),
  routes: [
    { path: '/', template: 'fixpoint', name: 'home' },
    { path: '/datalog-dafsa', template: 'dafsa-landing', name: 'dafsa-landing' },
    { path: '/datalog-dafsa/language', template: 'dafsa-language', name: 'dafsa-language' },
    { path: '/datalog-dafsa/cli', template: 'dafsa-cli', name: 'dafsa-cli' },
    { path: '/datalog-dafsa/api', template: 'dafsa-api', name: 'dafsa-api' },
    { path: '/datalog-dafsa/architecture', template: 'dafsa-architecture', name: 'dafsa-architecture' },
    { path: '/datalog-dafsa/time-travel', template: 'dafsa-time-travel', name: 'dafsa-time-travel' },
    { path: '/datalog-dafsa/vector-search', template: 'dafsa-vector-search', name: 'dafsa-vector-search' },
    { path: '/datalog-dafsa/order-statistics', template: 'dafsa-order-statistics', name: 'dafsa-order-statistics' },
    { path: '/datalog-dafsa/typed-projects', template: 'dafsa-typed-projects', name: 'dafsa-typed-projects' },
    { path: '/datalog-dafsa/playground', template: 'dafsa-playground', name: 'dafsa-playground' },
  ],
  basePath: '/',
  // dafsa's templates are served from /datalog-dafsa/shell/templates
  // (the main site owns /shell/templates). Pin the baseURL here so both route
  // templates resolve under this site's shell regardless of the deep-link subpath.
  baseURL: '/datalog-dafsa/shell/templates',
  // The SSG output pre-renders all content pages except playground.
  // Rehydrate only when the current pathname matches a pre-rendered route.
  ssr: (() => {
    const path = (window.location.pathname.replace(/\/+$/, '') || '/');
    // Pre-rendered: all dafsa routes EXCEPT playground
    return path.startsWith('/datalog-dafsa') && path !== '/datalog-dafsa/playground';
  })(),
});

// Expose the app handle so the shell/host can inspect or drive it later.
window.__datalogDafsaApp = app;
