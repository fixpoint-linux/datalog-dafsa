# Documentation site

This directory is a static, dependency-free documentation site for the
`datalog-dafsa` engine. It is served by GitHub Pages directly from this
`docs/` folder on the `main` branch.

## Pages

- `index.html` — overview, quickstart, feature summary.
- `language.html` — the complete Datalog Language Reference.
- `cli.html` — the `dl` CLI subcommands.
- `api.html` — the full `dl.h` C API surface.
- `architecture.html` — storage thesis, lifecycle, join & evaluation strategies.
- `order-statistics.html` — rank / select / range_count / count, bound + perm
  variants, pull-iterator + merge-join, lazy range generator.
- `time-travel.html` — versioned snapshots, as-of queries, retention.
- `vector-search.html` — the semantic vector-search tier (MIH-over-ITQ,
  `dl vsearch`/`dl vhybrid`, pre-quantized vectors from an external encoder,
  in-store int8 re-rank).
- `style.css` — the shared theme.

All navigation is plain relative links; there is no build step.

## Language Server (IDE support)

A native LSP server for the Datalog language ships as `dl-lsp` (build with
`make lsp`). It speaks LSP (JSON-RPC 2.0 over stdio with Content-Length
framing) and reuses the real parser/compiler, so its diagnostics are exactly
what the engine rejects. Supported: diagnostics-as-you-type, hover (predicate
arity + IDB/EDB, variables, builtins), and completion (relations + builtins +
bound variables).

```sh
make lsp           # build ./dl-lsp
make test-lsp      # run the end-to-end LSP test harness (tests/lsp.sh)
```

To use it in an editor, point the editor's Datalog language server at
`./dl-lsp` (e.g. for Neovim's LSP config: `cmd = { "/abs/path/dl-lsp" }`).
`tests/lsp.sh` shows a complete JSON-RPC conversation you can replay against any
client.

## Enabling GitHub Pages

1. In the repository on GitHub, open **Settings**.
2. In the left sidebar, under **Code and automation**, select **Pages**.
3. Under **Build and deployment &rarr; Source**, choose **GitHub Actions**
   (this repo ships a `.github/workflows/pages.yml` that deploys `docs/` on
   every push to `main`).
4. Once the workflow has run, the site is published at
   `https://<user>.github.io/<repository>/` (rendering `docs/index.html`
   automatically). The exact URL depends on the repository owner and name.

## Notes

- Every syntax example is a real construct the parser and compiler accept;
  see the test suite under `../tests/` for the source of most examples.
- The content is deliberately conservative: no performance numbers are quoted
  (see `../tests/bench.c` for the benchmark harness).
