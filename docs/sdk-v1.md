# SDK v1: the widget contract

Status: contract for the framework and the client teams, 2026-10-16; its open
questions were decided on 2026-10-17 (§6), and the framework implements it
(v0.1.0, 2026-10-17: the shim, all five glue copies and the Node harness, §7). It fixes
what a widget can call, what travels between sandbox, host and renderer, what
every host exposes and what every client implements, so that client teams can
build their side in parallel with the PoC. Decisions behind it: release plan
D4, D7a, D8, D9, D12, D14, D17, D18, D19, D22, D23, and the decisions of
2026-10-17 (no keyboard, no form controls, a client-styled `<button>`,
`docs/decisions.md`, "Profile v1").

What the framework did before v1 is noted where a client may meet it
(**before v1**: the API of the profile-0 builds).

Related documents, not repeated here: `docs/integration.md` (embedding the
host on each platform), `docs/tgw-format.md` (the widget payload: mini-HTML),
`docs/protocol.md` (the full wire protocol), `docs/changes.md`
(per-release notes).

## 1. Who does what

| | the framework (widget-sandbox) | the client |
|---|---|---|
| widget API | `TelegramWidget`, Web Storage, `CSS.supports`, `navigator.language`, `visibilitychange`, the inert stubs | — |
| checks | argument schemas, user activation, rate limits, storage quota, profile tables, in all five glue copies | — |
| theme | CSS variables on the renderer root, `themeChanged`, the defaults of the root, the text and `<button>` (the UA layer) | maps the bubble's colours to `themeParams`, its button and message text to the button and text tokens, calls `setTheme` |
| links | validates the URL, lets one call through per pointer press | shows the confirmation with the full URL, opens it |
| haptics | validates, rate-limits, drops calls from hidden widgets | plays them (or ignores them on desktop and web) |
| storage | preload at start, per-task commits, the quota | persists per message, deletes with the message, chat, account, "Clear cache" |
| gestures | reports per press what the widget claims with `touch-action` (horizontal drags, holds); the shared policy (thresholds, who wins); `dblclick` for every double tap, `contextmenu` for claimed holds; on the web, plays the bubble itself | drives its recognizers from the policy: menu and swipe-to-reply unless the widget claims the press, and then cancels the widget's touch; the list scroll always; no double-tap reaction over the widget (v0.1.2; vertical drags the chat's since v0.1.3, §5.5) |
| size | reports the content height and the widget's resize anchor (`setResizeAnchor`, v0.1.3) | sizes the bubble from the bot's hint before load, caps it, clips; keeps the anchored edge in place when the height changes (§5.6) |
| failures | freezes the last frame with a badge and reports `dead` | rebuilds it after a crash; after 2 crashes in a row leaves it stopped |

## 2. Widget-facing API

Everything below is what widget code sees inside the sandbox. The object is
`window.TelegramWidget`; member names follow `Telegram.WebApp` (Mini Apps)
where one exists, so that code and models that know Mini Apps carry over,
and so that a JSON-RPC facade for MCP Apps (`ui/open-link`,
`ui/notifications/host-context-changed`, `ui/notifications/initialized`) can be
laid over the same calls later (D19).

### 2.1 Members

| member | type | note |
|---|---|---|
| `profile` | `number`, read-only | |
| `isProfileAtLeast(n)` | `(n: number) => boolean` | |
| `colorScheme` | `'light' \| 'dark'`, read-only | before v1: `theme` |
| `themeParams` | `ThemeParams`, read-only, a fresh frozen object per change | |
| `isActive` | `boolean`, read-only (`!document.hidden`) | |
| `onEvent(type, handler)`, `offEvent(type, handler)` | `type`: `'themeChanged' \| 'activated' \| 'deactivated'`; the handler is called with `this` = `TelegramWidget` and no arguments | before v1: `on(event, cb)`, reserved; removed |
| `openLink(url)` | `(url: string) => void` | before v1 without the checks below |
| `openTelegramLink(url)` | `(url: string) => void` | |
| `HapticFeedback.impactOccurred(style)` | `style`: `'light' \| 'medium' \| 'heavy' \| 'rigid' \| 'soft'` | |
| `HapticFeedback.notificationOccurred(type)` | `type`: `'error' \| 'success' \| 'warning'` | |
| `HapticFeedback.selectionChanged()` | `() => HapticFeedback` | |
| `ready()` | `() => void` | |
| `setResizeAnchor(edge)` | `(edge: 'top' \| 'bottom') => void` | v0.1.3 (§2.12); detect it with `typeof TelegramWidget.setResizeAnchor === 'function'` |
| `theme`, `onTheme(cb)` | aliases of `colorScheme` and `onEvent('themeChanged')` (the callback gets the colour scheme, at once and on every change) | deprecated |
| `sendMessage` | `typeof TelegramWidget.sendMessage === 'undefined'` | removed |

All methods are synchronous and return `undefined`, except the three haptic
methods, which return `TelegramWidget.HapticFeedback` for chaining, as in Mini
Apps, and `isProfileAtLeast`. Nothing returns a promise, and no call gets an
answer from the client: whether the user confirmed a link is not reported back.

### 2.2 Profile detection (D23)

- `TelegramWidget.profile` is the integer version of the widget profile the
  client's framework build enforces (`profile` in `VERSION.json`, stamped into
  the bundles at build time; profile v1 is `1`). It is a property of the
  framework build, not of the client app: clients do not set it.
- `TelegramWidget.isProfileAtLeast(n)` is `profile >= n`; a non-number `n` gives
  `false`.
- `CSS.supports(property, value)` and `CSS.supports(conditionText)` answer from
  the profile tables compiled into the sandbox bundle: `true` only when the
  validator would keep that declaration (one evaluator for both,
  `packages/core/src/shared/css-supports.ts`).
- `@supports` rules are evaluated by the validator against the same tables:
  the block is kept or dropped before the CSS reaches the renderer, so the
  renderer's engine never decides.
- A widget that checks nothing still loads; on an older client the features
  outside that client's profile are dropped by its validator (accepted product
  behaviour). Clients below API layer 233 receive `messageMediaUnsupported`
  with the caption and show their usual placeholder; a client with widget
  rendering off does the same (`docs/tgw-format.md`).

### 2.3 Theme (D14)

`TelegramWidget.themeParams` carries the chat's palette as `#rrggbb` strings
(lowercase), with the keys of Mini Apps' `themeParams`:

| key | meaning inside a bubble |
|---|---|
| `bg_color` | the colour of this bubble, behind the widget: an incoming bubble's, or an outgoing bubble's when the user sent or forwarded the widget (§5.1). The client draws it under the transparent renderer: it is the widget's canvas (§2.4) |
| `text_color` | message text |
| `hint_color` | secondary text: time, meta |
| `link_color` | links in messages |
| `button_color`, `button_text_color` | the client's primary button; also the default `<button>` look (§2.4) |
| `secondary_bg_color` | a second surface on the bubble (quote, inline-button background) |
| `accent_text_color` | accent text |
| `subtitle_text_color` | subtitles |
| `destructive_text_color` | destructive actions |
| `section_bg_color`, `section_header_text_color`, `section_separator_color`, `header_bg_color`, `bottom_bar_bg_color` | no bubble counterpart; filled by the client from the closest colour so that Mini Apps code works (`section_separator_color` draws the lines of a table and `<hr>`, §2.4) |

The palette is the bubble's: a widget in an outgoing bubble gets that bubble's
background, text, hint, link and separator colours, as the client draws a
message there. Every key is always present. The same values are CSS custom properties on the
widget's root element: `--tg-theme-bg-color`, `--tg-theme-text-color`, … (the
key with `_` turned into `-`), plus `--tg-color-scheme: light | dark`. The host
sets them in the renderer; a widget cannot overwrite the root's values, only
shadow them on its own elements. `prefers-color-scheme` media queries and
`color-scheme: light dark` follow the chat's scheme. Keys the client leaves
out take the framework's defaults for the scheme (`DEFAULT_THEME_PARAMS` in
`packages/core/src/shared/sdk-policy.ts`, Web K-like); clients pass every key.

A theme change (the user switches theme, the chat's theme changes, night mode)
updates `colorScheme`, `themeParams` and the variables in one task, then fires
`themeChanged` (and the legacy `onTheme` callbacks). The renderer sets the
variables inline and `!important` on the root, so neither a widget stylesheet
(`:root { … !important }`, any specificity) nor a style the widget sets on its
own root element replaces them.

### 2.4 The renderer's defaults: the root, the text, `<button>`

Every renderer inserts one user-agent-level stylesheet built from one template
in `packages/core` (`uaStylesheet` in `packages/core/profile.json`, printed in
`docs/profile-v1.md`): `@layer tw-ua` declared first, every selector inside
`:where()`, so any rule of the widget wins over it, whatever its specificity.
A widget without CSS therefore looks the same on every client and like the
client's own messages; the engine's own stylesheet (Times 16 px and an 8 px
body margin in a WebView, litehtml's master CSS) never shows through. Since
v0.1.1 the layer covers the root and the text elements, not only `<button>`: a
clarification of profile 1, not profile 2 (`docs/decisions.md`, "The
renderer's defaults").

**The root.** `html`: text `--tg-theme-text-color`, font
`var(--tg-text-font-size)/var(--tg-text-line-height) system-ui, sans-serif`,
no text-size adjustment, long words wrap, text not selectable (below). It has
no background of its own: the renderer is transparent and the bubble the
client draws under it, in `bg_color`, is the canvas. A background the widget
sets on its root or on its body paints the whole bubble, as in a browser (root
background propagation: the shadow-DOM renderer paints it on its host
element). `body`: the padding token as its margin
(`margin: var(--tg-text-padding)`), no padding; the first child's top margin
and the last child's bottom margin are dropped, so text starts at the token's
inset as in a message. A widget that writes `body { margin: 0 }` goes edge to
edge, as authors expect from a browser. The content height the renderer
reports, and so the bubble's height, includes the body's margins.

**Text selection** (D18 since v0.1.1). Text in a widget is not selectable by
default: the layer sets `user-select: none` on the root. A widget makes text
selectable with `user-select: text` (or `-webkit-user-select: text`) on an
element; its children follow (`auto`), and `none` turns it off again below.
`user-select: all` and `contain`, `::selection` and editing stay outside the
profile. The widget cannot read or change the selection (`getSelection()` is a
stub); the user selects and copies with the client's own gestures and copy
command (§5.10). A renderer that cannot select text yet keeps it
unselectable: the widget still works (an accepted exception to "every client
draws it", like `:hover`).

**The text elements**, in `em` of the parent's size, so they follow the
client's text size:

| element | default |
|---|---|
| `h1` … `h6` | 1.6, 1.35, 1.2, 1.07, 1, 0.87 em; weight 600; line height 1.25; 0.75 em above, 0.35 em below |
| `p`, `ul`, `ol`, `menu`, `dl`, `pre`, `blockquote`, `figure`, `table` | 0.6 em above and below |
| `ul`, `ol`, `menu` | 1.5 em of start padding; disc, decimal; a list inside a list item: circle, no margins |
| `dt`, `dd` | weight 600; 1.5 em of start margin |
| `table` | collapsed borders, no spacing; cells 0.3 em × 0.6 em of padding, start-aligned, top, a 1 px `--tg-theme-section-separator-color` line under each; `th` 600; no outer padding on the first and last cell of a row, so the text lines up with the paragraphs; `caption` in the hint colour, 0.87 em |
| `hr` | one 1 px line in `--tg-theme-section-separator-color`, 0.6 em margins |
| `pre`, `code`, `kbd`, `samp` | `ui-monospace, monospace` at 0.9 em (once: `code` inside `pre` keeps the size); `pre` wraps (`pre-wrap`, the bubble does not scroll) on a translucent grey, 0.5 em × 0.7 em of padding, 6 px radius |
| `blockquote` | no indent; a 3 px bar in `--tg-theme-accent-text-color` and 0.75 em before the text |
| `figure`, `figcaption` | no indent; `figcaption` in `--tg-theme-hint-color` at 0.87 em |
| `a[href]` | `--tg-theme-link-color`, not underlined |
| `b`, `strong`; `i`, `em`, `cite`, `dfn`, `var`; `small`; `sub`, `sup`; `mark` | 600; italic; 0.85 em; 0.75 em, shifted without changing the line height; a translucent yellow |
| `img` | at most the bubble's width (`max-width: 100%`; the `width`/`height` attributes still apply) |

Every property that WebKit's, Chromium's or litehtml's own stylesheet sets
differently is set here, so the three draw the same. RTL widgets get the list,
`dd`, table and quote indents on the right (logical properties after the
physical ones, which litehtml keeps).

**The text tokens.** The client passes them in the theme (`WidgetTheme.text`,
§4); the renderer checks each one and always sets the three variables on the
root, with the reference value for a token it did not get or refused:

| token (`text.…`) | variable | grammar | reference value |
|---|---|---|---|
| `fontSize` | `--tg-text-font-size` | CSS px (= points, dp) 8–64: a number, or a string `"15px"` | `15px` |
| `lineHeight` | `--tg-text-line-height` | a unitless number 1–3 | `1.35` |
| `padding` | `--tg-text-padding` (the body's margin) | one to four CSS lengths in `px`, `em`, `rem` | `8px 12px` (vertical, horizontal) |

`fontSize` is the chat's message text size: it follows the client's text-size
setting, and the client calls `setTheme` when it changes. A widget may use the
variables in its own CSS (`calc(var(--tg-text-font-size) * 1.2)`). Every
reference picture (the conformance set, the corpus reference mode) is drawn
with the reference set and the default palettes
(`packages/core/src/shared/reference.ts`).

**The `<button>`.** A `<button>` the widget did not style is drawn in the
client's own button style; any author CSS overrides it (`docs/decisions.md`,
"Profile v1"). The template fixes the states: base, `:active` (not disabled),
`[disabled]`; no hover state, no focus ring.

The client passes its tokens in the theme (`WidgetTheme.button`, §4); the
renderer sets each one it gets as the variable on the root and checks its
grammar (`themeVariables`): colours `#rrggbb`, lengths in `px`/`em`/`rem`, a
font shorthand naming generic families only, opacities 0–1. A token the client
does not pass keeps the template's default:

| token (`button.…`) | variable | the template's default |
|---|---|---|
| `bg` | `--tg-button-bg` | `rgba(51,144,236,.12)`: a tint, so that a widget that sets only its own `color` or only its own `background` stays readable (`docs/decisions.md`, "Profile v1") |
| `text` | `--tg-button-text` | `inherit` |
| `radius` | `--tg-button-radius` | `8px` |
| `font` | `--tg-button-font` | `500 14px/1.2 system-ui,sans-serif` (D3b) |
| `padding` | `--tg-button-padding` | `8px 14px` |
| `height` | `--tg-button-height` (minimum height) | `auto` |
| `pressedOpacity`, `disabledOpacity` | `--tg-button-pressed-opacity`, `--tg-button-disabled-opacity` | `.7`, `.5` |

A client whose buttons are solid (`bg` = its button colour, `text` = white)
accepts that a widget overriding only its button's `background` keeps the
client's text colour on it.

Other form controls are outside the profile and dropped by the validator.

### 2.5 Links (D8)

- `openLink(url)` opens an external page. `url` must be an absolute `https:`
  URL of at most 256 characters, without user name or password,
  whose host is not `t.me`, `telegram.me` or `telegram.dog`.
- `openTelegramLink(url)` opens a Telegram link: `https://t.me/…` (also
  `telegram.me`, `telegram.dog`), at most 256 characters, any path, as in Mini
  Apps: the client's own `t.me` handler confirms the dangerous ones (proxy,
  login, language and the like). `tg:` URLs are not accepted.
- An invalid argument throws `TypeError` in the widget synchronously (the shim
  checks the syntax; the glue checks it again). A valid call returns
  `undefined` and goes to the host.
- **User activation** (`docs/protocol.md`, "User activation"): the glue
  forwards a call only while the task of a real pointer press on the visible
  widget runs, and one press unlocks one call shared by both methods. A press
  is `pointerdown` … `click` from the renderer, marked `trusted` (v1 never
  delivers `dblclick`, §2.10);
  keys never count (D4: no keyboard input or focus at all), nor does a click a
  key produced (`detail` 0), a scripted `click()`, a timer, a frame or a promise
  job after the task. Blocked calls are dropped without an exception and
  reported as the diagnostic `activation-blocked` (at most 20 per widget).
- `<a href="https://…">` clicks and `window.open(url)` take the same path: the
  shim calls `openTelegramLink` for Telegram hosts and `openLink` otherwise
  (before v1 everything went to `openLink`); what neither accepts (`http:`,
  `javascript:`) is ignored with the diagnostic `navigation-blocked`, never an
  exception. `window.open` returns `null`.
- The URL the client gets is normalised: scheme and host lowercase (A–Z only),
  characters outside ASCII after the host percent-encoded as UTF-8. The host
  is ASCII (an IDN in its `xn--` form); ASCII outside RFC 3986's unreserved and
  reserved characters and `%` (space, `"<>\^\`{|}`, controls) and a `%` that does
  not start `%XX` are a `TypeError`. The 256-character limit applies to the
  normalised URL.
- The widget is not told whether the user confirmed.

### 2.6 Haptics

- `impactOccurred`, `notificationOccurred`, `selectionChanged` as in Mini
  Apps' `HapticFeedback`. An unknown style or type throws `TypeError`.
- Delivered only while the widget is visible. Rate limit per widget: a bucket
  of 5 calls refilled at 10 per second; calls over the limit or
  from a hidden widget are dropped without an exception and reported once per
  second as `haptic-limit`. No user gesture is required.
- Desktop and web clients accept the calls and play nothing.

### 2.7 Lifecycle

- `document.hidden`, `document.visibilityState` and `visibilitychange`: a widget scrolled off screen, in a background tab or app gets
  `hidden`; while hidden it gets no animation frames and its timers fire at
  most once per second. `isActive` and the `activated` / `deactivated` events
  mirror the same state (fired after `visibilitychange`).
- `ready()` tells the host the first state is drawn. It is optional:
  the host also notices the first content on its own.
- A widget can be destroyed and recreated at any time (scrolled far away,
  memory pressure, app restart). Only Web Storage survives (§2.8).
- When the runtime is killed (CPU, memory, a crash), the last frame stays
  frozen with a "Widget stopped" badge.

### 2.8 Storage (D17)

Why local storage, and why per message: the state belongs to the message
(a client already keeps a record per message, the cached bubble height); the
bot cannot hold state for the widget because v1 has no messages or callbacks
to the bot (D8); and the server checks only the payload's size (D15), so any
cloud synchronisation would be new server work. Local Web Storage keyed by
the message is what fits all three; a cloud or per-bot `DeviceStorage` is
out of v1 (§6).

- `localStorage` is stored by the client for this message on this device,
  under (account, chat, message id), where the chat is the one the message is
  in: the bot's private chat for the message it sent; a forwarded copy (allowed
  into any chat) is another message in another chat, another key, and starts
  empty. (D17 says (account, bot, `message_id`); message ids are unique only
  within a chat, so the chat is the exact key.) It
  survives the widget's
  destruction and app restarts, and is deleted with the message, the chat
  (or its history), the account and on "Clear cache". It is never sent to the
  bot or the server.
- `sessionStorage` has the same key and lives in the client's memory until the
  app restarts.
- Quota: 16 KB per area and message (16384 bytes of keys and values counted in
  UTF-8). `setItem` that would exceed it throws a `DOMException` named
  `QuotaExceededError` (the shim provides `DOMException`) and stores nothing.
  Keys and values are converted to strings, as in browsers.
- The data is in the widget at start (synchronous reads from the first line of
  script). Writes are local at once; at the end of the task in which an area
  changed, the shim commits that area to the glue (§3.2), which hands it to
  the client.
- No `storage` event: a message is shown in one place at a time; if it ever is
  in two, the last commit wins.
- An edit of the message with the same HTML keeps the running widget; with
  different HTML the widget restarts on the same storage. A bot that wants a
  clean start changes its key prefix or a data version in the new HTML.

### 2.9 Locale

`navigator.language` is the client's UI language as a BCP 47 tag (`ru`,
`en-US`, `pt-BR`), `navigator.languages` is `[language]`, and the `Intl`
default locale is the same (a locale argument the widget passes still wins). A
missing or malformed locale is `en-US` (before v1: always `en-US`).

### 2.10 Removed and inert

| API | v1 behaviour |
|---|---|
| `TelegramWidget.sendMessage` | removed; no message is ever sent on the user's behalf |
| `alert`, `confirm`, `prompt` | no dialog, nothing sent to the host; `confirm` returns `false`, `prompt` `null` (before v1 they reached the host as a bridge call `alert`) |
| `navigator.clipboard.writeText`, `readText`, `write`, `read`, `navigator.share` | reject with a `DOMException` named `NotAllowedError`, nothing sent to the host (before v1 `writeText` reached the host as `clipboard`); `navigator.canShare()` is `false` |
| `fetch`, `XMLHttpRequest`, `WebSocket`, … | throw `NetworkDisabledError` (invariant 5) |
| keyboard | no key events, no keyboard focus (D4; the renderers and glue drop key events in profile v1) |
| double tap | always the widget's, on every element: both clicks, then the renderer's own `dblclick` (since v0.1.2; before, never delivered) |
| long press | the bubble's over `touch-action: auto` and `pan-*` (§5.5): the touch the client takes ends for the widget with `pointercancel` and no `click`; over `touch-action: none` the widget's, with the renderer's own `contextmenu` (since v0.1.2), unless the press first leaves the slop vertically: then it is the list's scroll (since v0.1.3). The engine's own `dblclick` and `contextmenu` never reach the widget |
| vertical drag | always the chat's (the list scrolls), on every element and whatever `touch-action` says; the widget gets `pointercancel` (since v0.1.3; before, `pan-x` and `none` claimed it) |

### 2.11 Errors and limits

| call | invalid arguments | outside the rules | other limits |
|---|---|---|---|
| `openLink`, `openTelegramLink` | `TypeError` | dropped, `activation-blocked` (no press, hidden, press used) | 256 characters; one call per press |
| haptics | `TypeError` | dropped, `haptic-limit` (hidden, over the bucket) | bucket 5, 10 per second |
| `localStorage`, `sessionStorage` | — | `QuotaExceededError` | 16 KB per area |
| `isProfileAtLeast` | `false` | — | — |
| `onEvent`, `offEvent` | ignored (unknown type or non-function) | — | 16 handlers per type; further ones are ignored |
| `ready` | — | — | — |
| `setResizeAnchor` | `TypeError` (anything but exactly `'top'` or `'bottom'`) | — | only a change reaches the host |

Diagnostics go to the host's diagnostics (`onDiag` / `WidgetDiagnostics`),
never to the widget.

A widget is killed, and its last frame kept (§5.7), for one of three reasons the
host sees in `died` / `onDead`: `CPU limit exceeded` (more than the task's slice
of continuous JS), `Layout query limit exceeded` (its synchronous layout queries
blocked the renderer for more than max(slice, 100 ms) in one task; the queries
also count toward the CPU quota) and `Memory limit exceeded (64 MB)`.

### 2.12 Resize anchor (v0.1.3)

`TelegramWidget.setResizeAnchor(edge)` says which edge of the bubble stays put
on screen when the widget's height changes:

- `'top'` (the default, also before any call): the bubble grows and shrinks
  downward; the content below it moves. An FAQ whose answers expand, "show
  more" results, an order summary that opens.
- `'bottom'`: the bubble grows and shrinks upward; its bottom stays and the
  content above it moves. A chat log or a support thread where new messages
  appear at the bottom and the older ones move up.

It may be called at any time and applies to the height changes that follow,
until it is called again; call it before the change it should anchor, at the
start of the script for a widget that always grows one way (the anchor of a
re-created widget is what its new run sets). It needs no tap and works while
hidden. Anything but exactly `'top'` or `'bottom'` throws `TypeError`. A client
whose framework predates v0.1.3 has no such method: check
`typeof TelegramWidget.setResizeAnchor === 'function'` (the profile number does
not tell: it stays 1). The anchor is a request: a client keeps it for a bubble
on screen; for a bubble off screen it keeps what is on screen in place
(§5.6). The widget is not told which edge moved.

## 3. Protocol

What carries the API between the sandbox (QuickJS), the host glue and the
renderer. Every message from the sandbox is untrusted and is checked in all
five glue copies: the web worker and `WidgetHost` (TS), the Node harness,
Swift, Kotlin, C++ (`sandbox_engine.cpp`, `widget_host.cpp`). The checks live in
one policy with a shared JSON fixture, as `cpu-budget` does:
`packages/core/src/shared/sdk-policy.ts` and its twins `SdkPolicy.swift`,
`SdkPolicy.kt`, `sdk_policy.cpp`, all checked against
`packages/core/tests/fixtures/sdk-policy.json`.

### 3.1 Config (`SandboxConfig`, host → sandbox at start)

| field | type | set by | status |
|---|---|---|---|
| `theme` | `'light' \| 'dark'` | client (`colorScheme`) | |
| `themeParams` | `Record<ThemeKey, '#rrggbb'>` | client | |
| `locale` | BCP 47 string, at most 35 characters | client | |
| `profile` | integer | glue (its own constant; `VERSION.json` once it exists) | the shim reports `profile-mismatch` when it differs from the bundle's |
| `storage` | `{ local: Record<string, string>, session: Record<string, string> }` | client (the preload), glue checks the quota | |
| `policy` | `'strict'` | glue default | |

The client's values are trusted but still checked: unknown theme keys and
values that are not `#rrggbb` are dropped, a storage preload over the quota is
dropped whole with the diagnostic `storage-rejected` (the widget starts empty).

### 3.2 Sandbox → host

`__host('bridge', JSON.stringify([method, args]))`, one call per API call:

| method | args | glue checks | goes to the client as |
|---|---|---|---|
| `ready` | `[]` | — | `ready` |
| `openLink` | `[url]` | URL rules of §2.5; privileged: needs the task's gesture, consumes it | `openLink(url)` |
| `openTelegramLink` | `[url]` | URL rules of §2.5; privileged | `openTelegramLink(url)` |
| `haptic` | `['impact', style]`, `['notification', type]`, `['selection']` | enum values; visible; rate limit | `haptic(...)` |
| `setResizeAnchor` | `[edge]` | exactly `'top'` or `'bottom'` (v0.1.3); no gesture, no visibility gate | not an action: the host keeps the latest value and reports it with the height (§4) |

The glue checks in this order: the method and its arguments
(`checkBridgeCall`; any other method or a malformed call is dropped with
`bridge-rejected`, at most 20 reported); user activation for the two link
methods (`activation-blocked`); for `haptic` the gate of §2.6 (`haptic-limit`).
A call that passes goes to the client as a `WidgetAction` with the normalised
URL (and, deprecated, to `onBridge` with the raw method and arguments), except
`setResizeAnchor`, which only sets the host's anchor: the shim sends it only
when the value changes, and the host keeps one value, so a widget that calls it
in a loop costs the client nothing. The
method names `sendMessage`, `alert` and `clipboard` are gone from the wire.

Gesture tagging: on native hosts the engine runs a trusted event's task
with that press's gesture number and lets the first privileged call of the task
consume it. On the web the worker tags a `bridge` message with the gesture of
the task (`{type:'bridge', method, args, gesture}`) and the main-thread host
accepts each issued gesture once, within 5 s.

`__host('storage', JSON.stringify(commit))`, at most once per task, after the
task's `ops`: `commit` is `{ local?: Record<string, string>, session?:
Record<string, string> }`, the full content of each area that changed in the
task. The shim sends it once the task is over (after the MutationObserver
callbacks, when the glue's last `endTick` of the task returns). The glue
refuses an argument longer than any commit within the quota can be (2 × 12 ×
16384 + 64 characters) before parsing it, checks that the commit has only
`local` / `session`, each an object of strings within the quota, and hands
each area to the client (`onStorageCommit(area, values)`, `local` first); a
commit that fails is dropped whole with `storage-rejected` (at most 20
reported) and the client keeps the previous content. Commits reach the client
in order.

### 3.3 Host → sandbox

| message | payload |
|---|---|
| `theme` | `{ theme: 'light' \| 'dark', themeParams }` (before v1: the scheme only) |
| `visibility` | `{ visible }` |
| `event` | a renderer event, with `gesture: n` for a trusted press |

The `bridge` message (host → widget events such as a bot's answer) is gone with
`sendMessage`.

### 3.4 Host → renderer

| message | payload |
|---|---|
| `theme` | `{ type: 'theme', theme: { colorScheme, params, button, text } }`: the renderer core builds the variables of §2.3 and §2.4 (`themeVariables`) and sets them on the root (`:root`, or `[data-tw-root]` on the shadow-DOM renderer); the colour scheme itself still goes to the renderer element (`color-scheme`, the WebView's appearance) as before. A native renderer does the same with its own copy of `themeVariables` (`apps/renderer-litehtml/src/theme.cpp`) |

| `arbitrate` | `{ type: 'arbitrate', bubble, touch?, rtl? }` (v0.1.2): `bubble` true, the renderer plays the bubble itself with the gesture policy (the web hosts, `bubbleGestures`): a press the bubble takes ends for the widget with `pointercancel` and the host gets `bubble`. `touch`: a mouse's primary button arbitrates as a finger (and the widget sees `pointerType: 'touch'`). `rtl`: the edge strip is on the right. Native hosts leave it off: their own recognizers take the presses |

The renderer takes only variable names from the fixed list above and values
that match their grammar (`#rrggbb`, CSS lengths, a font shorthand with a
generic family, numbers 0–1, a font size of 8–64 px, a line height of 1–3).

### 3.5 Renderer → host

| message | payload |
|---|---|
| `gesture` | `{ wants, x, y, hold }` (booleans) on every primary `pointerdown`: what of this press the widget claims (`claimOf` of the gesture policy): horizontal drags (`x`), from the `touch-action` the widget gave the target and its ancestors and its `preventDefault` on `pointerdown`/`touchstart`; `hold` only from `touch-action: none` (v0.1.2). Taps and double taps are always the widget's, vertical drags always the chat's: `y` and `wants` (the field before v1, equal to `y`) are always false since v0.1.3, and hosts report `y` false whatever a renderer sends |
| `bubble` | `{ action, gesture, x, y, edge }` with `arbitrate` `bubble` on (v0.1.2): the bubble took a press: `action` `menu` \| `swipe` \| `scroll` \| `back`; (x, y) where the pointer was, widget coordinates; `edge`: it started in the leading edge strip |
| `size` | the content height |
| `event` | `trusted: true` only for real input |

## 4. Host interface per platform

The shapes the client wires up, as implemented (v0.1.0). Callbacks run on the
main thread (Swift, Kotlin), on the host thread (C++) or on the page's event
loop (web). On every platform the string-theme constructor, `setTheme(String)`
and `onBridge` are kept as deprecated forwards (no behaviour of their own;
`onBridge` gets the raw method and arguments of each call `onAction` got, right
after it); the host → widget bridge events (`bridgeEvent`) are removed.

### 4.1 Swift (iOS, macOS): `WidgetHost`

```swift
public struct WidgetTheme { var colorScheme: ColorScheme /* .light / .dark */; var params: [String: String]; var button: WidgetButtonTokens; var text: WidgetTextTokens }
public struct WidgetButtonTokens { var bg, text, radius, font, padding, height: String?; var pressedOpacity, disabledOpacity: Double? }
public struct WidgetTextTokens { var fontSize, lineHeight: Double?; var padding: String? }   // v0.1.1
public struct WidgetStorage { var local: [String: String]; var session: [String: String] }
public enum WidgetStorageArea: String { case local, session }
public enum WidgetHaptic { case impact(String), notification(String), selection }
public enum WidgetAction { case ready, openLink(URL), openTelegramLink(URL), haptic(WidgetHaptic) }
public struct WidgetGestureClaim { var x: Bool; var y: Bool; var hold: Bool }   // hold: v0.1.2; y: always false since v0.1.3
public enum WidgetResizeAnchor: String { case top, bottom }   // v0.1.3: WidgetDiagnostics.resizeAnchor, next to height

init(html: String, theme: WidgetTheme = WidgetTheme(), width: CGFloat = 400, options: Options = Options(), …) throws
// Options: maximumHeight (600), policy (.strict), features, pools, …, locale ("en-US"), storage (the preload)
var onChange: ((WidgetDiagnostics) -> Void)?           // height, resizeAnchor (v0.1.3), loaded, rendered, dead, diags, actions
var onError: ((String) -> Void)?
var onAction: ((WidgetAction) -> Void)?
var onStorageCommit: ((WidgetStorageArea, [String: String]) -> Void)?   // local before session
var onGestureClaim: ((WidgetGestureClaim) -> Void)?   // every primary pointerdown; drive the bubble's recognizers with GestureArbiter (§5.5)
func setTheme(_ theme: WidgetTheme)
func setVisible(_ visible: Bool); func snapshot(completion:); func destroy()
```

`WidgetBubble`, `WidgetModel` (`WidgetView`) and `WidgetPreparer` take a
`WidgetTheme` the same way. `URL(string:)` may escape a few characters the
check keeps (`[`, `]`, a second `#`); the checked string is
`SdkPolicy.checkLink`'s.

### 4.2 Kotlin (Android): `WidgetHost`

```kotlin
data class WidgetTheme(val colorScheme: String = "light", val params: Map<String, String> = emptyMap(), val button: WidgetButtonTokens = WidgetButtonTokens(),
                       val text: WidgetTextTokens = WidgetTextTokens())
data class WidgetButtonTokens(val bg: String? = null, val text: String? = null, val radius: String? = null, val font: String? = null,
                              val padding: String? = null, val height: String? = null, val pressedOpacity: Double? = null, val disabledOpacity: Double? = null)
data class WidgetTextTokens(val fontSize: Double? = null, val lineHeight: Double? = null, val padding: String? = null)   // v0.1.1
data class WidgetStorage(val local: Map<String, String> = emptyMap(), val session: Map<String, String> = emptyMap())
sealed class WidgetAction { Ready; OpenLink(url); OpenTelegramLink(url); Haptic(kind, value?) }   // kind: impact | notification | selection

WidgetHost(context, html, theme: WidgetTheme, widthDp, options, resources, rendererFactory)
// Options: minHeightDp (50), maxHeightDp (600), policy ("strict"), features, pools, …, locale ("en-US"), storage (the preload)
var onChange: ((WidgetDiagnostics) -> Unit)?               // … height, resizeAnchor ("top" | "bottom", v0.1.3), actions
var onError: ((String) -> Unit)?
var onAction: ((WidgetAction) -> Unit)?
var onStorageCommit: ((area: String, values: Map<String, String>) -> Unit)?   // "local" | "session"
var onGesture: ((WidgetGestureClaim) -> Unit)?             // v0.1.2: x, y, hold (y always false since v0.1.3); drive the bubble's recognizers with GestureArbiter (§5.5)
var onGestureClaim: ((x: Boolean, y: Boolean) -> Unit)?    // deprecated since v0.1.2: called right after onGesture
fun setTheme(theme: WidgetTheme); fun setVisible(visible: Boolean); fun destroy()
```

The WebView renderer keeps its parent from intercepting a press only while the
widget may still take it as a hold (a `hold` claim, until the press leaves the
slop vertically: `requestDisallowInterceptTouchEvent`). The Blitz and litehtml
prototypes send only `wants`, always false now: they claim nothing.

### 4.3 C++ (desktop core): `twidget::WidgetHost`

```cpp
struct WidgetButtonTokens { std::optional<std::string> bg, text, radius, font, padding, height; std::optional<double> pressedOpacity, disabledOpacity; };
struct WidgetTextTokens { std::optional<double> fontSize, lineHeight; std::optional<std::string> padding; };   // v0.1.1
struct WidgetTheme { std::string colorScheme; std::map<std::string, std::string> params; WidgetButtonTokens button; WidgetTextTokens text; };
struct WidgetStorage { std::map<std::string, std::string> local, session; };
struct WidgetAction { enum Kind { Ready, OpenLink, OpenTelegramLink, Haptic } kind; std::string url, hapticKind, hapticValue; };

WidgetHost(std::string html, WidgetTheme theme, double width, WidgetHostOptions, std::shared_ptr<SandboxResources>, RendererFactory, …);
// WidgetHostOptions: …, std::string locale ("en-US"), WidgetStorage storage (the preload)
std::function<void(const WidgetDiagnostics&)> onChange;           // … height, resizeAnchor ("top" | "bottom", v0.1.3), actions, storageCommits (first 1000)
std::function<void(const std::string&)> onError;
std::function<void(const WidgetAction&)> onAction;
std::function<void(const std::string& area, const std::map<std::string, std::string>&)> onStorageCommit;
std::function<void(const WidgetGestureClaim&)> onGesture;          // v0.1.2: x, y, hold (y always false since v0.1.3; twidget/gesture_policy.h, GestureArbiter)
std::function<void(bool x, bool y)> onGestureClaim;                 // deprecated since v0.1.2: called right after onGesture
void start(); void setTheme(const WidgetTheme&); void setVisible(bool); void destroy();
```

Storage strings keep a lone surrogate as WTF-8 (what QuickJS hands over), so
a value round-trips exactly; it is not strict UTF-8 in that case. Commits
already on their way reach `onStorageCommit` before `destroy()` returns.

### 4.4 Web: `WidgetHost` (`@telegram/widget-sandbox/host/widget-host`)

```ts
interface WidgetTheme { colorScheme: 'light' | 'dark'; params?: Partial<ThemeParams>; button?: ButtonTokens; text?: TextTokens }
interface TextTokens { fontSize?: number | string; lineHeight?: number | string; padding?: string }   // v0.1.1
type WidgetAction =
  | { kind: 'ready' }
  | { kind: 'openLink'; url: string }
  | { kind: 'openTelegramLink'; url: string }
  | { kind: 'haptic'; haptic: 'impact' | 'notification' | 'selection'; value?: string };
new WidgetHost(container, html, {
  theme,                                   // WidgetTheme, or a plain 'light' | 'dark'
  locale, storage: { local, session },     // the client's language and Web Storage preload
  onAction(action), onStorageCommit(area, values), onGestureClaim({ x, y, hold }),
  bubbleGestures, onBubbleGesture({ action, gesture, x, y, edge }),   // v0.1.2: the renderer plays the bubble (§5.5)
  onResize(height, anchor),                // anchor: 'top' | 'bottom', the widget's setResizeAnchor (v0.1.3)
  policy, visible, onDead, onError, onLoaded, onDiag, rendererFactory, workerPool, features, …
})
host.resizeAnchor;                         // v0.1.3: the latest anchor, 'top' until the widget sets one
host.setTheme(theme); host.setVisible(visible); host.setBubbleGestures(mode); host.destroy();
// @telegram/widget-sandbox/shared/gesture-policy: GestureArbiter, claimOf and the thresholds, for a client's own recognizers
```

The web client's page must also carry the CSP of invariant 6 (`connect-src`
only Telegram's endpoints, `form-action 'none'`, no third-party
`img-src`/`media-src`/`font-src`/`frame-src`).

### 4.5 Node harness (`packages/core/scripts/sandbox-node.ts`)

For tests and tooling, not for clients. `RunOptions` takes `theme`, `locale`,
`themeParams`, `button`, `text`, `storage` and `script` steps `{ theme, themeParams }`;
`RunResult` has `actions` (what `onAction` would get), `storageCommits`,
`resizeAnchor` and `resizeAnchors` (the latest anchor and each one the glue
accepted, v0.1.3; the CLI prints a `resize anchor:` line) and `theme` (the
renderer's `theme` message, which `--ops-file` writes before the ops).
Gestures are injected per event (`gesture: n`). Since v0.1.2 `width` (the bubble's,
default 400) and `gestures` (`tap`, `hold`, `double-tap`, `swipe`, `right-click`) run the
widget on the real renderer core in headless Chromium after load: `RunResult.gestures`
reports per press the element, its `touch-action` claim, the policy's outcome, the events
the widget received and a hint; `height` is the laid-out content height. From a shell:
`sandbox-node.ts widget.html --width 360 --hold 120,40 --double-tap 120,40 --swipe 60,80,120,0`
(`--desktop` for a desktop client's bubble, `--json` for the full result).

## 5. What the client implements

### 5.1 Theme

- Map the chat's theme to every key of §2.3 for the bubble the widget sits
  in: `bg_color` is that bubble's own fill, the colour the UA layer paints
  behind the widget. A widget the user sent or forwarded sits in an outgoing
  bubble: pass the outgoing bubble's colours (fill, text, hint, link,
  separator), not the incoming ones; custom chat themes and night mode the
  same way. A bubble with a gradient or a translucent fill has no single
  colour: pass the one the widget's area shows most (the client draws the
  bubble under a transparent widget view either way).
- Pass the button tokens of §2.4 from the client's own button, and the text
  tokens from its message text: `fontSize` = the chat's message font size,
  following the text-size setting; `lineHeight` and `padding` as the client's
  text bubble lays out text (the reference set 15 / 1.35 / `8px 12px` when
  unsure). Pass them at creation and call `setTheme` on every change.
- Pass the UI language as `locale`.

### 5.2 Links

- On `openLink(url)` and `openTelegramLink(url)`: show a native confirmation
  that displays the **full URL** (not only the domain; the host in its ASCII
  form, as the framework's URL parser gives it), with Open and Cancel.
- On Open: `openLink` goes to the external browser (or the client's in-app
  browser, by its usual setting); `openTelegramLink` goes through the client's
  own `t.me` handling, which keeps its own confirmations (joining a chat,
  starting a bot).
- One dialog at a time; drop actions that arrive while one is shown.
- The framework guarantees at most one link action per pointer press.

### 5.3 Haptics

Play `impact`, `notification` and `selection` with the platform's generators
(iOS `UIImpactFeedbackGenerator`, `UINotificationFeedbackGenerator`,
`UISelectionFeedbackGenerator`; Android `View.performHapticFeedback` or
`VibrationEffect`); respect the system's haptics setting. Desktop and web
ignore them.

### 5.4 Storage

- Key: (account, chat, message id) of the message the widget belongs to (§2.8).
- At creation, pass the stored `local` area and the in-memory `session` area as
  `storage`.
- On `onStorageCommit(area, values)`: keep `values` as the new content of that
  area. Write `local` to disk with a short delay (for example 1 s) and at once
  when the widget is hidden or destroyed and when the app goes to the
  background or terminates. Keep `session` in memory only.
- Delete both areas with the message, with the chat or its history, with the
  account, and on "Clear cache". There are no per-bot quotas and no age-based
  eviction.
- The framework enforces the 16 KB quota; the client stores what it gets.

### 5.5 Gestures and drag (D4)

Who gets a press is decided by one shared policy, so every client and the sandbox
behave the same (v0.1.2, `docs/decisions.md` "Gestures: `touch-action` is the
claim" and, v0.1.3, "Vertical drags are the chat's"). Taps and **double taps
over the widget are always the widget's**, on every element: a double tap
reaches it as both clicks and then `dblclick`, and the client's double-tap
reaction stays on the bubble outside the widget. **Vertical drags are always
the chat's**: the list scrolls over every widget, whatever `touch-action` says,
and the widget gets `pointercancel`. Horizontal drags and holds are claimed per
element at `pointerdown` by CSS `touch-action`:

| over an element with | hold | horizontal drag | vertical drag |
|---|---|---|---|
| `touch-action: auto` (default), `manipulation` | bubble: message menu | bubble: swipe-to-reply / back | list scroll |
| `pan-y` | bubble | widget | list scroll |
| `pan-x` (claims nothing since v0.1.3) | bubble | bubble | list scroll |
| `none` | widget (`contextmenu`) | widget | list scroll (since v0.1.3) |

The first move past the slop locks the press's axis: a press that starts
horizontally on a claimed element is the widget's on both axes until the
finger lifts; one that starts vertically is the list's. A hold the widget has
won (on `none`) is locked the same way at its first move past the slop:
sideways it stays the widget's, up or down the list takes it and the widget
gets `pointercancel` after its `contextmenu`.

`touch-action` on a container covers its children, as in CSS (the ancestor
chain runs up to the widget's `<html>`). The leading 20 px of the widget (the
right edge in a right-to-left UI) take no claim (the system back gesture), and the bubble
outside the widget (caption, time, edges) is always the place for the menu and
the reaction. With a mouse: a
right click is the menu, or `contextmenu` for the widget on `none`; clicks,
double clicks, holds and drags are the widget's.

The policy is `GestureArbiter` (`packages/core/src/shared/gesture-policy.ts`;
`GesturePolicy.swift`, `GesturePolicy.kt`, `twidget/gesture_policy.h`), checked on
every platform against `packages/core/tests/fixtures/gesture-policy.json`:

| | value |
|---|---|
| touch slop (a tap or a hold stays within it) | 10 px |
| hold | 500 ms within the slop |
| double tap (`dblclick`) | the 2nd `pointerdown` ≤ 300 ms after the 1st `pointerup`, ≤ 40 px from the 1st `pointerdown` |
| axis lock | the dominant axis when the press leaves the slop (a tie is vertical) |
| edge strip | 20 px from the widget's leading edge |

What the client does:

1. On every primary `pointerdown` the host reports `onGestureClaim` (Kotlin and
   C++: `onGesture`): `x` (horizontal drags) and `hold`; `y` is always false
   since v0.1.3 (the arbiter ignores it). It arrives a few
   milliseconds after the touch starts; hold the bubble's recognizers until it
   does (or 150 ms pass, then treat it as no claim).
2. Feed the touch over the widget to a `GestureArbiter`: `down(t, x, y, claim,
   pointer, button)` with x from the widget's leading edge (`width - x` in a
   right-to-left UI) and y from its top, in CSS px (points, dp), then `move`,
   `up`, `cancel` (the platform cancelled it), and `tick` at `holdDeadline()`.
3. On a verdict with `cancel`: take the touch from the widget view the
   platform's way (iOS: a recognizer that begins with `cancelsTouchesInView`;
   Android: intercept in the parent, the WebView gets `ACTION_CANCEL`; desktop:
   end the press for the view), so the widget gets `pointercancel` and no
   `click`, and do `action`: `menu` (the message menu), `swipe` (swipe-to-reply,
   or back by its direction), `scroll` (the list), `back` (the system's or the
   chat's back gesture).
4. On a verdict for the widget: stand back for the rest of the press (no menu,
   no swipe, no list scroll from it: the axis is locked). Until then the list's
   own scroll must still be able to start on a vertical move: never keep the
   list from scrolling because of a claim. The double-tap reaction recognizer must not
   act over the widget at all. The renderer sends the widget its own `dblclick`
   / `contextmenu`; the client sends nothing.
5. No keyboard: the client never routes key events to the widget, and the
   chat's input field keeps the focus.

Platform notes, from the demo bubbles: on iOS a recognizer with
`cancelsTouchesInView` does not cancel a touch WKWebView already handles (WebKit
reads touches through its own recognizers); disabling and re-enabling the web
view's enabled gesture recognizers once, when the bubble takes the press, makes
WebKit send `pointercancel` (public API; the iOS demo's `BubbleGestures`). On
Android, intercept in the parent and send the child an `ACTION_CANCEL` (the
Android demo's `BubbleGestureLayout`). The Android WebView renderer keeps its
parent from intercepting only while a `hold` claim may still win (until the
press leaves the slop: vertically, the list takes it). The Blitz and litehtml
prototypes report only `wants`, always false: they claim nothing. On the web
there are no recognizers over the widget: `WidgetHost` with `bubbleGestures`
lets the renderer play the bubble with the same policy and reports what it took
(`onBubbleGesture`); the dev app and the feed demo do that by default
(`gestures`: phone, desktop or off). The browser's own panning is the list
scroll there: the renderer gives the engine every `touch-action` widened to
allow vertical panning (`none` → `pan-y`), so a touch screen scrolls the page
over any widget, while the claim reads the widget's own value.

### 5.6 Bubble size

- The size anchors `width` and `height` of `messageMediaWidget` (D12b; `0` =
  auto, each axis independent, `docs/tgw-format.md`). Width `0`: the widget
  takes the bubble's width; `W > 0`: a fixed-width widget, `W` clamped to
  [200, the bubble's maximum width], and the host is created with that width.
  Height `0`: the default placeholder before load, then the content height the
  host reports (`onChange` → `height`, web `onResize`) clamped to [50, 600] and
  cached per message; `H > 0`: a fixed height clamp(H, 50, 600) from the start,
  unchanged by the content. The four combinations are tabulated in
  `docs/integration.md` ("Bubble sizing"). 200 / 50 / 600 are the shared
  v0.1.0 constants, the same on every client; the hosts clamp to [50, 600] by
  default. Test it with a test bot sending `inputMediaWidget` with the anchors
  set.
- Content beyond the bubble's height (600 in auto mode, or the fixed height) is
  clipped: no inner scrolling (outside the profile), no fullscreen button
  (D12a).
- The widget view fills the bubble: no padding of the bubble's own around it
  (the widget's body margin, the `padding` text token, is the inset, §2.4), and
  the bubble clips the view at its corner radius, so a background the widget
  paints reaches the rounded edges.
- Text is not selectable unless the widget enables it with `user-select: text`
  (D18 since v0.1.1; §2.4, §5.10).
- **The resize anchor** (v0.1.3, §2.12): with every height the host reports the
  edge that stays put (`WidgetDiagnostics.resizeAnchor` next to `height`; web
  `onResize(height, anchor)`), `top` until the widget sets one. When an auto
  height changes, the list keeps that edge where it is on screen: for `top` the
  content below the bubble moves; for `bottom` the content above moves (in a
  list laid out from the top, add the height difference to the scroll offset;
  in an inverted chat list that grows from the bottom it is the natural
  behaviour). The same holds when the bubble opens at its cached height and the
  first report differs, and after the bubble is re-created (the anchor is the
  one the new run reports with its height). A bubble off screen keeps the
  visible content in place, as for any item that changes height. Keep the
  anchor with the cached height per message.
- During an animated height change, align the old pixels to the anchored
  edge: the widget view (or its snapshot) is pinned to the bubble's bottom for
  `bottom` and to its top for `top`, so the edge that stays put does not move
  while the bubble's other edge animates. A fixed height (`H > 0`) never
  changes, whatever the anchor.

### 5.7 Failures and restarts (D22)

- When the host reports `dead`, the last frame stays with the badge
  (`stoppedLabel`, localised by the client).
- The client may build a new host for the message the next time the bubble is
  shown; after 2 crashes in a row the widget stays stopped until the app
  restarts or the message is edited with new HTML (D22).

### 5.8 Visibility

Call `setVisible(false)` when the bubble leaves the screen and when the app
goes to the background, `setVisible(true)` when it is back.

### 5.9 The message: media, caption, flag

- The widget is `messageMediaWidget` (layer 233): inflate `payload` (zlib,
  RFC 1950, not gzip), no size limit of your own, hand the HTML to the host (`docs/tgw-format.md`,
  `docs/integration.md`). An edit replaces the payload: new host, same storage.
- Rendering is enabled by the client's own platform/version flag; off, or
  below layer 233 (`messageMediaUnsupported`), the message shows as
  unsupported media with its caption. Keep that fallback.
- The media caption is the preview and search text; without one, the localized
  "Widget" stub.
- Test with a test bot in a tester's private chat and forward into the shared
  test channel (bots may send widgets only to private chats). Every
  conformance widget is sent that way as a layer-233 message, with its
  reference picture posted next to it (`packages/conformance/channel.json`,
  `docs/integration.md`, "Testing").

### 5.10 Text selection (v0.1.1)

Text a widget marked `user-select: text` is selectable; everything else is not
(§2.4).

- Touch: a long press over text (`touch-action: auto`) is the bubble's (§5.5),
  so selection inside a widget starts the way the client
  selects text in an ordinary message (its bubble menu's Select, or its own
  selection mode), and the renderer view then takes the selection gestures.
- Mouse (desktop, web): a drag over selectable text selects it; a press
  anywhere else still selects nothing and keeps the chat input's focus.
- Copy uses the client's normal copy command and menu.
- A native renderer (litehtml, TwidgetCore, the Swift renderer) must honour
  `user-select: text` to be conformant; until it does, the text stays
  unselectable and the widget still works.

## 6. Out of v1 and the decisions of 2026-10-17

Out of v1: sending messages or data to the bot and bot callbacks; inline mode
and sharing; payments; the viewer's identity; popups; `MainButton`; per-bot
`DeviceStorage` and cloud storage; keyboard input; form controls other than
`<button>`; `TelegramWidget.platform`;
a `linkOpened` event; fullscreen; accessibility (D21); MCP Apps transport (D19,
later).

The questions this contract left open, as decided on 2026-10-17:

1. Haptics need no gesture: allowed while the widget is visible, with the rate
   limit of §2.6 (a bucket of 5, refilled at 10 per second).
2. Limits: URLs at most 256 characters, at most 16 handlers per event type, and
   the widget stays stopped after 2 crashes in a row (D22, which said 2–3).
3. `openTelegramLink` takes any `t.me` path, as in Mini Apps; the client's own
   handler confirms the dangerous ones. No deny-list in the glue.
4. No `TelegramWidget.platform` in v1.
5. Double tap and long press always stay with the bubble (the reaction, the
   message menu): the widget gets single taps and, in v1, drags. *Replaced in
   v0.1.2*: double taps over the widget are always the widget's, and
   `touch-action: none` gives it holds too (§5.5, `docs/decisions.md` "Gestures:
   `touch-action` is the claim").
6. No `linkOpened` event in v1: the widget is not told whether the user opened
   the link.
7. The bubble size from the bot's `width` and `height` is tested end to end
   with a test bot (layer 233 ships with v0.1.0).

## 7. The framework milestone

SDK v1 is implemented in the framework as part of v0.1.0 (2026-10-17): the
shim, all five glue copies and the Node harness, with tests on every platform
(the policy fixture `packages/core/tests/fixtures/sdk-policy.json` in each
suite; `packages/core/tests/unit/sdk.test.ts`, `tests/sdk.spec.ts` —
`pnpm test:sdk` — and the platform suites through the real engine). The corpus
widgets that called `sendMessage` keep their state in `localStorage` or link
out with `openLink`, and the corpus READMEs say so; the attack files keep
calling it to check that it is gone.
