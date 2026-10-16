# Widget profile conformance set

Each Telegram client draws widgets with its own renderer: Shadow DOM on the web,
WKWebView/WebView or a native engine (litehtml, TwidgetCore) on the others. A
widget looks the same everywhere only if every renderer draws the same profile
and the validator strips everything else the same way. This set checks that.
For any client it answers the question: which profile features does it support?

It is built around one **feature catalogue** and two widget sets:

| | |
|---|---|
| [`catalogue.json`](catalogue.json) / [`catalogue.md`](catalogue.md) | 244 profile features with stable IDs (`css.display.grid`, `canvas.filter`, `tw.profile`, …). Each is marked in / out / candidate, together with the decision it comes from (D3a, D3b, D4, D18, D23, the invariants, or `proposed` for this catalogue's own proposals), what the pipeline does with it today, and the keys a validator table would match. It is the seed for `profile.json`. The source is `scripts/conformance/catalogue.ts`. |
| [`auto/`](auto) | **Automatic set**, 21 dense widgets for runners and CI. Each one asserts layout, computed style, APIs and events itself and publishes the result machine-readably. Pixel stages are diffed against reference PNGs. |
| [`visual/`](visual) | **Visual set**, 13 readable widgets for people: the shared Telegram test channel. Each row shows a group of features drawn live next to an embedded reference picture, with a one-line caption saying what to compare. Interactive rows carry a one-line instruction. Files are numbered in reading order. |
| [`manifest.json`](manifest.json) | Maps each test to its catalogue IDs, kind (self / pixel / interactive), stages with rects and reference files, scripted actions, host-side checks, themes and widths, and what native Chromium/WebKit answer to each check. Also gives coverage per set. |
| [`refs/`](refs) | Reference PNGs for every stage: native headless Chromium (and WebKit where installed), DPR 2. Dark-theme variants are added for stages that depend on the theme. |
| [`results/`](results) | Reports of the renderers run so far in the common format, plus [`summary.md`](results/summary.md). |

Coverage: the automatic set covers 240 of the 244 catalogue IDs. That is every in-profile and candidate ID, and every out ID except the four listed under `coverage.external` in the manifest, which the attack suite and the server check. The visual set covers the 87 IDs that only a person can judge: gradients, shadows, filters, text, canvas, animations, SVG, images, interaction and theme.

## How a widget reports

Every widget is one self-contained HTML file: no external resources, `data:` images only, under 128 KB. It is valid content for a bot message as is: the server packs it into `.tgw`. An inlined harness (`scripts/conformance/harness/`) runs the checks and writes the result into the `data-conformance` attribute of `<html>`. Any host can read it from its own validated ops: it is the `attr` op on the element that the `el` op adopted as `html`. No renderer-specific hook is needed, so the same code path works on a canvas renderer.

```json
{"v":1,"id":"auto/03-layout","mode":"sandbox","status":"done",
 "n":{"pass":53,"fail":0,"xfail":2,"xpass":0,"skip":0,"pending":0},
 "checks":[["css.display.flex","row places children side by side","pass",""], …],
 "targets":{"knob":[x,y,w,h]}, "stages":{"flex":[8,8,80,60]}}
```

- `status`: `running` → `awaiting` (an interactive widget is ready for input) → `done`. Wait for `done`, or for `awaiting` before performing the actions.
- Check status:
  - `pass`, `fail`;
  - `pending`: input never came; count it as a fail;
  - `skip`: not applicable here;
  - `xfail`: expected to fail, because the feature is not implemented yet. This covers `CSS.supports` and `@supports` from the profile (D23), `TelegramWidget.profile`, the validator enforcing the profile (out features), the storage quota and so on;
  - `xpass`: expected to fail but passed, so the expectation is out of date.
- `targets`: rects, in widget CSS px, of the elements that the scripted actions tap or drag. The widget measures them in its own layout and re-measures them on every update. Runners tap where the widget says the element is.
- A line `[conformance] <id> {counts}` also goes to the console when the widget finishes.

## Running it

### Web (this repository)

```bash
corepack pnpm tsx scripts/conformance/build.ts
```

Regenerates the catalogue, inlines the harness, renders the references, records the native answers and writes the manifest. Run it after editing a widget.

```bash
corepack pnpm dev --port 5175
```

Starts the dev server (from a worktree, use a port that no other checkout uses).

```bash
corepack pnpm tsx scripts/conformance/run-web.ts --origin http://127.0.0.1:5175
```

Runs every widget on the iframe, Shadow DOM, Blitz and litehtml renderers of the dev app. Blitz and litehtml need `pnpm build:blitz-web` and `pnpm build:litehtml-web`. The defaults are the product configuration: `strict` policy, the `webAnimations` feature (D5), width 390 at DPR 2. Options: `--renderers shadow`, `--set auto`, `--only 12-canvas`, `--themes light,dark`, `--widths 360,390,430`.

```bash
corepack pnpm tsx scripts/conformance/report.ts
```

Writes `results/<platform>-<renderer>.json` and `results/summary.md`.

The runner loads each widget through `window.__tw.load` (the real worker → validator → renderer pipeline). It taps the renderer's `send` to read `data-conformance` from the validated ops. Then it:
- performs the actions from the manifest with real mouse input (canvas renderers only get taps, through `__tw.tap`);
- takes a viewport screenshot at DPR 2 and crops each stage of the auto set at the widget's snapped device-pixel origin;
- diffs each stage against the Chromium and WebKit references and keeps the better of the two;
- checks the host-side expectations: bridge calls, bubble height.

Raw output goes to `reports/conformance/` (gitignored), with stage crops and diff images for every run.

A pixel stage fails when the pixels that differ exceed `maxDiff` of its **ink**, meaning the pixels that are not white stage background in either image. This way a blank render fails on a sparse stage too. The colour threshold is pixelmatch's 0.12 and anti-aliased pixels are ignored. The default `maxDiff` is 5 %. Stages that draw text allow 50 %, because fonts differ between platforms: there the check is that the right thing is drawn at all. For text features the self-checks and the visual set decide.

### Native clients (iOS, macOS, Android, desktop, the twidget renderers)

A native runner produces the same **run file** and hands it to `report.ts`. Per widget of `auto/` (and `visual/`, which is only checked for loading):

1. Load the HTML with the production configuration: policy `strict`, `features` from the profile (`webAnimations`), and the client's own theme and width (390 by default).
2. Read `data-conformance` from the validated ops: the `attr` op on the id whose `el` op carried `adopt: "html"`. Wait for `status` `done`, or for `awaiting` when the widget has `actions`; give up after about 20 s.
3. Perform `actions`, translating the widget CSS px of `targets[name]` into view coordinates:
   - `tap`, `dblclick`, `rightclick`, `hover`;
   - `press` (hold for `ms`);
   - `drag` (from a target to a target, or `by` a delta);
   - `key`, `wheel`;
   - `theme` (switch the chat theme);
   - `visible` (put the bubble off screen and back);
   - `wait`.
   Then wait for `done` again. Report the actions the platform cannot deliver in `skippedActions`; the checks listed in their `ids` count as skipped.
4. Take a screenshot of the widget view at DPR 2 and crop each stage at `manifest.tests[].stages[].rect` (CSS px from the widget's top-left). Compare it with `refs/` using the rule above.
5. Check `host` expectations from the manifest: how many bridge calls with that method and first argument, the bubble height the host gave the widget, and (`focus: "chat"`) whether the chat's input field still has the keyboard focus after the widget booted, before any input (focus the input before loading the widget).
6. Write `{renderer, platform, engine, policy, features, date, runs: [...]}`. Each entry in `runs` is `{id, set, theme, width, loaded, dead, status, checks, stages:[{name, covers, diff, status, maxDiff}], host:[{id, name, status, detail}], errors, skippedActions}`, exactly what `run-web.ts` writes to `reports/conformance/<renderer>.json`. Then run:

```bash
corepack pnpm tsx scripts/conformance/report.ts path/to/run.json
```

The hooks that exist today:
- iOS/macOS demo: `--autorun <corpus>` with the WebView, Blitz or litehtml renderer. A `conformance` autorun needs steps 2–6 in `apps/ios`/`apps/macos`.
- Android demo: `--es autorun`.
- Desktop C++ core: `twidget-run <widget.html> --ops [--blitz|--litehtml] [--click X,Y] --png out.png` prints the validated ops and a raster, which is enough for steps 2–5.
- twidget repo renderers (TwidgetRenderer, TwidgetCore): consume the same validated ops and only need the screenshot and the attribute read.

None of these modes is in this commit.

## Reading the report

`results/summary.md` has one column per renderer and gives a verdict per catalogue ID over the automatic set:

| Mark | Meaning |
|---|---|
| ✅ | Every check and stage for the ID passed. |
| ◐ | Some passed, some failed. |
| ❌ | Everything failed. |
| ⏳ | Fails as expected: the feature is not implemented yet. |
| ❗ | Passes although expected to fail: update the expectation. |
| – | Not applicable on this renderer (for example, input it cannot receive). |

How to read the verdict depends on the catalogue status:
- **in**: the renderer must pass. A ❌ or ◐ is a renderer or pipeline bug, or a reason to reopen the decision.
- **candidate**: the feature joins the profile when every client's renderer passes (D3).
- **out**: ✅ means neutralised as the profile says. Today most out features show ⏳, because the validator does not enforce the profile yet; that is the later profile milestone.

The native Chromium/WebKit columns show what a plain browser engine answers. They are a sanity reference for the tests, not an expectation.

## Writing a test

- Start from an existing widget. Keep the header comment `<!-- conformance {…} -->` (title, hint, actions, host checks, covers) and the `<!-- conformance:harness -->` markers; `build.ts` fills them.
- Self-checks use the harness API:
  - `T.check / T.eq / T.near / T.ok(id, name, …, opts)` run at once;
  - `T.wait(id, name)` resolves later, from an event;
  - `T.step(fn)` runs after the first frames;
  - `T.run({awaitInput, deadline})` starts the run.

  Every check names one catalogue ID. `opts.xfail: 'reason'` marks the known gaps. `opts.sandboxOnly` marks things a plain browser cannot do: the SDK, network stubs.
- Pixel stages are `<div data-stage="name" data-covers="ids" [data-themes="light dark"] [data-max-diff] [data-xfail]>` with fixed-colour content on the white stage. In the auto set they sit on a fixed 80×60 grid at the top; in the visual set they are 150×90 in flow. A `<template data-ref="name">` at the end of the body gives the expected rendering when it differs from what Chromium draws, as for out features. Script-drawn stages go through `T.stage(name, draw)`, so that reference mode can skip them.
- Keep auto widgets under 900 px tall (the host cap) with no layout shift above the probes. Hit testing outside the bubble returns null, and runners tap the published targets.
- No external URLs except the CDN names of the bundled libraries.

## Known limits

- References come from headless Chromium on macOS. WebKit references appear when Playwright's WebKit is installed (`npx playwright install webkit`). The diff takes the better of the two engines.
- Text stages depend on platform fonts: see the threshold rule above.
- The canvas renderers (Blitz, litehtml wasm) get only synthetic taps in the web runner. Drags, hover, keys and wheel are reported as skipped, and taps there grant no user gesture, so the gesture-gated host checks are skipped as well.
- Interactive checks need a real pointer, so a touch-only client marks `:hover` and hover events as skipped.
