# corpus — the HTML files every demo and test suite loads

Single source of truth; `packages/core/scripts/build-native-bundles.ts` copies `compat`,
`attacks` and `generated/*` into the iOS/macOS, Android and desktop demo apps.
Run one file in the sandbox without a demo:
`corepack pnpm tsx packages/core/scripts/sandbox-node.ts corpus/compat/tabs.html --verbose`.

| Folder | What | Used by |
|---|---|---|
| `compat/` | 12 hand-written widgets (calculator, tabs, SVG chart, drag, animations, WebGL fallback…) | `test:compat`, demos |
| `out-of-profile/` | 9 former `compat/` widgets that need what widget profile 1 leaves out: bundled Chart.js and D3 (`chartjs-*`, `d3-*`) and form controls (`form-validation`, `inputs`, `local-storage`, `quiz`, `sortable-table`); kept to show what the profile removes (`?file=out-of-profile/<name>` in the dev app), run by no suite (`docs/profile-v1.md`) | dev app |
| `attacks/` | 89 files that each try to reach the network or run code (`markup-*`, `css-*`, `api-*`, `nav-*`, `cpu-*`, `fallback-*`, `timer-flood`) | `test:attacks`, demos; must be 0 canary hits |
| `generated/with-profile/`, `generated/without-profile/` | 20 LLM-generated widgets each, same prompts, with/without `docs/widget-runtime-profile.md` in context; `manifest.json` has the prompts | `test:generated`, demos |
| `htmlbubbles-ported/` | 116 chat-demo messages (12 conversations) written as classic widgets (plain HTML+CSS+JS, no `render()`/`onAction`); conventions and status in its README | `test:htmlbubbles`, web picker, demos (`--autorun htmlbubbles`) |
| `showcase/travel`, `showcase/commerce`, `showcase/life`, `showcase/work` | 21 interactive, animated demos on the **full web profile** (transitions, keyframes, grid, 3D, SVG, canvas, pointer drag/swipe, live clocks), one realistic chat case each: boarding pass, ride tracking, smart home, kanban, live poll …; conventions and status in `showcase/README.md` | `test:showcase`, web picker, the web chat feed (`?feed=1`, its default corpus) |
| `common/cards`, `common/interactive`, `common/live`, `common/heavy` | 32 widgets on the **common profile** — only what WebView, Blitz and litehtml all render the same way (flex, tables, gradients, `var()`; no grid/SVG/canvas/transforms/shadows/forms/emoji), so renderer timings compare like for like; profile and status in `common/README.md` | `test:common`, web picker, demos (`--autorun common`), Android bench (`CORPUS=common`) |

