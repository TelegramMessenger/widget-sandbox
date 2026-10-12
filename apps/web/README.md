# Web dev app, chat feed demo and Playwright harness

Dev tooling for `packages/core`; nothing here is a product path.

- `index.html` + `src/main.ts` — the dev app: corpus picker, drop any `.html`,
  dark theme, reference mode (`?reference=1`, native rendering of the same file
  for comparison — never for `corpus/attacks`), renderer toggles (Shadow DOM,
  Blitz, litehtml wasm, compare all), stats / diagnostics / bridge / console
  panels. Exposes `window.__tw` — the Playwright suites in `tests/` drive it.
- `src/feed.ts` + `src/feed-bench.ts` — the chat feed (`?feed=1`): 72 bubbles
  with the list's own lifecycle, the worker pool on/off, a live renderer
  switch, one hostile bubble, a metrics overlay and the bench panel
  (`?bench=1`, `scripts/feed-report.ts` runs it headless). `docs/chat-feed-web.md`.
- `src/renderers/` — the Blitz and litehtml wasm renderers plugged into the
  `Renderer` seam (need `pnpm build:blitz-web` / `build:litehtml-web`).
- `bench/feed.html` — the raw renderer bench behind `scripts/feed-bench.ts`.
- `vite.config.ts` — the package's plugin plus dev middleware: `/api/corpus`,
  `/corpus/*`, `/reference/*`, `/blitz/*`, `/litehtml/*`.
- `tests/unit/` — the wasm renderers through the validator (skipped until built).

```bash
corepack pnpm dev            # from the repo root; http://127.0.0.1:5173/?file=compat/tabs.html
corepack pnpm dev --port 5174
```

Editing `packages/core/src` or this directory while a Playwright suite runs
reloads Vite and breaks the run.
