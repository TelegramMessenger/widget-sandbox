# Before you push to `main`

`main` is the release line: every client pins a tag cut from it, so a commit on `main`
must already be valid on every platform. One page of rules; the commands are in
`CLAUDE.md` ("Commands"), with pnpm run through corepack (`corepack pnpm …`).

## 1. Know what you are changing

| You touched | It is | What else must change in the same commit |
|---|---|---|
| `packages/core/src/sandbox`, `renderer`, `shared`, `host/validator*`, `allowlist`, `css-sanitizer`, `url-policy`, `profile.json` | the shared contract: one JS for every client | the rebuilt bundles with their `.qbc` (`pnpm build:ios`, `build:android`, `build:desktop`), a unit test; after `profile.json`, first `packages/core/scripts/profile-build.ts` |
| What a widget sees or may do, decided in glue: limits, timers, visibility, the order of ops / anim / queries, Intl, the SDK checks | parity glue, five copies: web worker, Node harness, Swift, Kotlin, C++ | the same change in every copy, or a line in the commit message saying why it is one platform only |
| `CpuBudget*`, `SdkPolicy*`, the `intl` capability | a policy checked against a fixture in `packages/core/tests/fixtures`: `cpu-budget.json`, `sdk-policy.json`, `intl-cases.json` | all four copies and the fixture together (`intl-cases.json` holds V8's answers: add the case to `packages/core/scripts/intl-cases.ts` and regenerate); `build:ios`, `build:android`, `build:desktop` copy the fixtures into the native tests |
| Your renderer, pools, demo, build files | platform only | nothing else |

If you cannot do every copy yourself, push a branch and ask for the rest; do not push the
one-platform half to `main`.

## 2. Put logic in the right place

- Logic that needs no platform data (option processing, defaults, ordering rules) goes into
  the shim once, not into five copies of glue. Example: ECMA-402's date defaults live in
  `packages/core/src/sandbox/globals.ts`, so every host, present and future, gets them.
- Locale data and native APIs stay in glue.
- A widget feature goes in or out only through `profile.json`; adding one is profile 2.

## 3. Test what you changed

- Always: `pnpm typecheck`, `pnpm test:unit`.
- Shim, validator or renderer JS: `pnpm test:attacks` on both renderers
  (`TW_RENDERER=shadow` too, 0 hits), the conformance run (`scripts/conformance/run-web.ts`,
  then `packages/conformance/tools/report.mjs` for `packages/conformance/results`),
  and the corpora the change can affect (`test:compat`, `test:showcase`, `test:defaults`, …).
- Glue: the tests of every platform you changed: `xcodebuild test` (macOS and iOS), Android
  JVM and `connectedDebugAndroidTest`, desktop `twidget-tests` and `twidget-run --attacks`.
  Code that never ran on its platform is not done.
- A new behaviour gets a test that fails without the change.
- Cost: measure anything that runs per element or per query on a large DOM (500+ elements).
  A widget has 300 ms of CPU per second and at most 500 ms in one task; a quadratic loop
  over a few hundred nodes is a dead widget.

## 4. Write it down

- One behaviour change per commit; the message says what changed and in which copies.
- `docs/changes.md`: a line under the next release's section; remove known issues you fixed.
- Test counts and commands in `CLAUDE.md` when you add tests or suites.

## 5. Push

- Rebase on the latest `main`; rebuild the bundles if the rebase touched `packages/core`.
- If a release is being prepared, say so before pushing: the release is rebased on top of
  you and re-tested.
- Never edit generated files by hand: `profile-tables.ts`, `docs/profile-v1.md`, the
  bundles and `.qbc`, `packages/conformance/{catalogue,manifest,channel}.json`,
  `packages/core/tests/fixtures/intl-cases.json` and the fixtures' copies in the native tests.
