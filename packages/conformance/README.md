# Widget profile conformance set

Each Telegram client draws widgets with its own renderer: Shadow DOM on the web,
WKWebView/WebView or a native engine (litehtml, TwidgetCore) on the others. A
widget looks the same everywhere only if every renderer draws the same profile
and the validator strips everything else the same way. This set checks that.
For any client it answers the question: which profile features does it support?

**Client teams: start with [CLIENTS.md](CLIENTS.md).** It covers how to run the set in your client on your renderer and read the report. This README is for the maintainers of the set.

| | |
|---|---|
| [`catalogue.json`](catalogue.json) / [`catalogue.md`](catalogue.md) | 244 profile features with stable IDs (`css.display.grid`, `canvas.filter`, `tw.profile`, …). Each is marked in / out / candidate, with the decision it comes from (D3a, D3b, D4, D18, D23, the invariants, or `proposed` for the catalogue's own proposals), what the pipeline does with it today, and the keys a validator table would match. Profile 1 is frozen in `packages/core/profile.json` (2026-10-17), which is the source: the catalogue takes its statuses, decisions and neutralisations, so profile 1 has no candidates. The catalogue adds the SDK v1 limits and client behaviours that `profile.json` does not list yet. The source is `scripts/conformance/catalogue.ts`; `emit-catalogue.ts` overlays `profile.json` when the branch has it. |
| [`auto/`](auto) | **Automatic set**, 21 dense widgets for runners and CI. Each one asserts layout, computed style, APIs and events itself and publishes the result. Pixel stages are diffed against reference PNGs. |
| [`visual/`](visual) | **Visual set**, 13 readable widgets for people (the shared Telegram test channel). Each row shows a group of features drawn live next to an embedded reference picture, with a one-line caption. Interactive rows carry a one-line instruction. Numbered in reading order. |
| [`visual/sdk/`](visual/sdk) | **Client SDK group**, 17 mini-widgets (≤ 4 KB gzipped each, the v1 delivery limit) for what each client implements of SDK v1. Their reference pictures are separate files (`refs/visual/sdk/`), posted next to them; [`visual/sdk/README.md`](visual/sdk/README.md) is the post list. They use the mini harness (`harness/mini.js`). |
| [`manifest.json`](manifest.json) | Per test: catalogue IDs, kind (self / pixel / interactive), stages with rects and reference files, scripted actions, host checks, themes, and what native Chromium answers to each check. Also coverage per set. |
| [`refs/`](refs) | Reference PNGs for every stage: native headless Chromium (and WebKit when installed), DPR 2, dark variants for theme-dependent stages. |
| [`run.schema.json`](run.schema.json) | The run file every runner writes. |
| [`tools/`](tools) | `report.mjs`, the report (run files → per-renderer JSON + `summary.md`); `diff.mjs`, the pixel rule; `png.mjs`. No dependencies, Node ≥ 18. |
| [`harness/`](harness) | The harness inlined into every widget (checks, result attribute, stage swap for references). |
| [`results/`](results) | Reports of the renderers run so far, plus [`summary.md`](results/summary.md). |

Coverage: the automatic set covers 240 of the 244 catalogue IDs. That is every in-profile and candidate ID, and every out ID except the four listed under `coverage.external` in the manifest, which the attack suite and the server check. The visual set covers the 87 IDs that only a person can judge: gradients, shadows, filters, text, canvas, animations, SVG, images, interaction and theme.

## How a widget reports

Every widget is one self-contained HTML file: no external resources, `data:` images only, under 128 KB. A bot can send it as a message as is.

The harness writes the result as JSON into the `data-conformance` attribute of `<html>`. Any host reads it from its own validated ops: it is the `attr` op on the element that the `el` op adopted as `html`.

Every check names a catalogue ID and gets one of six statuses:

| Status | Meaning |
|---|---|
| `pass` | The check passed. |
| `fail` | The check failed. |
| `pending` | Input never came; counts as a fail. |
| `skip` | Not applicable on this renderer. |
| `xfail` | Expected to fail: the framework does not implement it yet. |
| `xpass` | Expected to fail but passed, so the expectation is out of date. |

Interactive widgets publish `targets`, the rects of what to tap or drag, measured in their own layout. See CLIENTS.md for the full format.

## Maintainers: building and running here

The steps are in this order:

1. Rebuild the set after editing a widget, the harness or the catalogue. This regenerates the catalogue, inlines the harness, renders the references and records the native answers in the manifest:

   ```bash
   corepack pnpm tsx scripts/conformance/build.ts
   ```

2. Start the dev server. From a worktree, use a port no other checkout uses:

   ```bash
   corepack pnpm dev --port 5175
   ```

3. Run the web renderers. This covers iframe, Shadow DOM, and Blitz/litehtml when their wasm is built (`pnpm build:blitz-web`, `pnpm build:litehtml-web`):

   ```bash
   corepack pnpm tsx scripts/conformance/run-web.ts --origin http://127.0.0.1:5175
   ```

   The defaults are the product configuration: `strict`, `webAnimations`, width 390, DPR 2. Options: `--renderers shadow`, `--set auto`, `--only 12-canvas`, `--themes light,dark`, `--widths 360,390,430`.

4. Judge the run files into the reports. This is the same tool the clients run:

   ```bash
   node packages/conformance/tools/report.mjs --out packages/conformance/results reports/conformance/{iframe,shadow,blitz,litehtml}.json
   ```

`run-web.ts` is the reference runner. It loads each widget through `window.__tw.load` (the real worker → validator → renderer pipeline) and wraps the renderer's `send` to read the result from the validated ops. It focuses a stand-in chat input first and performs the manifest actions with real mouse input (the canvas renderers get taps only, through `__tw.tap`). It crops the stages from a DPR 2 viewport screenshot at the widget's snapped device-pixel origin and writes the run files to `reports/conformance/`, which is gitignored. The dev app lists the set as the `conformance/` group in its corpus panel.

## Writing a test

- Start from an existing widget. Keep the header comment `<!-- conformance {…} -->` (title, hint, actions with the catalogue `ids` they feed, host checks, covers) and the `<!-- conformance:harness -->` markers; `build.ts` fills the markers.
- Self-checks use the harness API:

  | Call | Use |
  |---|---|
  | `T.check`, `T.eq`, `T.near`, `T.ok(id, name, …, opts)` | Checks that run at once. |
  | `T.wait(id, name)` | A check resolved later, by an event. |
  | `T.step(fn)` | Runs after the first frames. |
  | `T.target(name, el)` | Publishes a tap or drag target. |
  | `T.run({awaitInput, deadline})` | Starts the widget. |

  Options: `opts.xfail: 'reason'` for known gaps, `opts.sandboxOnly` for what a plain browser cannot do.
- Pixel stages are `<div data-stage="name" data-covers="ids" [data-themes] [data-max-diff] [data-colors] [data-xfail]>` with fixed colours on the white stage:
  - auto set: an 80×60 grid at the top;
  - visual set: 150×90, in flow.

  A `<template data-ref="name">` at the end of the body gives the expected rendering when it differs from what Chromium draws. Script-drawn stages go through `T.stage(name, draw)`.
- Keep auto widgets under 900 px tall (the host cap) and keep layout above the probes stable.
- No external URLs except the CDN names of the bundled libraries.

The client SDK group's pictures come from the same build. `scripts/conformance/sdk-mock.js` is a stand-in for `TelegramWidget` that follows the contract and uses a reference palette. Only the build injects it, so the pictures show the "SDK present" state. The build also flags any widget of the group over 4 KB gzipped or 32 KB.

## Known limits

- The visual set (`visual/01…13`) does not fit a mini-widget: 5–55 KB gzipped, with embedded pictures. The test channel posts it as `.tgw` documents behind the debug flag until it is split.
- References come from headless Chromium on macOS. WebKit references appear once Playwright's WebKit is installed, and the rule then takes the better of the two.
- Text stages depend on platform fonts. They allow 50 % of the ink but check the feature's colours.
- The web runner gives the canvas renderers (Blitz and litehtml wasm prototypes, which are not product renderers) only synthetic taps. Drags, hover, keys and wheel are skipped there, and so are gesture-gated host checks.
