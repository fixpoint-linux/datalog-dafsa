# Documentation site

This directory is a static, dependency-free documentation site for the
`datalog-dafsa` engine. It is served by GitHub Pages directly from this
`docs/` folder on the `main` branch.

## Pages

- `index.html` — overview, quickstart, feature summary.
- `language.html` — the complete Datalog Language Reference.
- `cli.html` — the 8 `dl` CLI subcommands.
- `api.html` — the full `dl.h` C API surface.
- `architecture.html` — storage thesis, lifecycle, join & evaluation strategies.
- `order-statistics.html` — rank / select / range_count / count, bound + perm
  variants, pull-iterator + merge-join, lazy range generator.
- `time-travel.html` — versioned snapshots, as-of queries, retention.
- `style.css` — the shared theme (no JavaScript).

All navigation is plain relative links; there is no build step.

## Enabling GitHub Pages

1. In the repository on GitHub, open **Settings**.
2. In the left sidebar, under **Code and automation**, select **Pages**.
3. Under **Build and deployment &rarr; Source**, choose **Deploy from a branch**.
4. Set **Branch** to `main` and **Folder** to `/docs`.
5. Click **Save**.

The site is then published at
`https://<user>.github.io/<repository>/` (the site renders
`docs/index.html` automatically). The exact URL depends on the repository
owner and name.

## Notes

- Every syntax example is a real construct the parser and compiler accept;
  see the test suite under `../tests/` for the source of most examples.
- The content is deliberately conservative: no performance numbers are quoted
  (see `../tests/bench.c` for the benchmark harness).
