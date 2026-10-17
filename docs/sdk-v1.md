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
host on each platform), `docs/tgw-format.md` (the widget payload: mini-HTML, and the
`.tgw` development format), `docs/protocol.md` (the full wire protocol), `docs/changes.md`
(per-release notes).

## 1. Who does what

| | the framework (widget-sandbox) | the client |
|---|---|---|
| widget API | `TelegramWidget`, Web Storage, `CSS.supports`, `navigator.language`, `visibilitychange`, the inert stubs | — |
| checks | argument schemas, user activation, rate limits, storage quota, profile tables, in all five glue copies | — |
| theme | CSS variables on the renderer root, `themeChanged`, the default `<button>` template | maps its chat theme to `themeParams` and button tokens, calls `setTheme` |
| links | validates the URL, lets one call through per pointer press | shows the confirmation with the full URL, opens it |
| haptics | validates, rate-limits, drops calls from hidden widgets | plays them (or ignores them on desktop and web) |
| storage | preload at start, per-task commits, the quota | persists per message, deletes with the message, chat, account, "Clear cache" |
| gestures | reports per press which drag axes the widget takes; never delivers a double tap or a long press | arbitrates drags with swipe-to-reply and list scroll; keeps double tap and long press for the bubble |
| size | reports the content height | sizes the bubble from the bot's hint before load, caps it, clips |
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
| `bg_color` | the bubble's background under the widget (incoming bubble of this chat's theme) |
| `text_color` | message text |
| `hint_color` | secondary text: time, meta |
| `link_color` | links in messages |
| `button_color`, `button_text_color` | the client's primary button; also the default `<button>` look (§2.4) |
| `secondary_bg_color` | a second surface on the bubble (quote, inline-button background) |
| `accent_text_color` | accent text |
| `subtitle_text_color` | subtitles |
| `destructive_text_color` | destructive actions |
| `section_bg_color`, `section_header_text_color`, `section_separator_color`, `header_bg_color`, `bottom_bar_bg_color` | no bubble counterpart; filled by the client from the closest colour so that Mini Apps code works |

Every key is always present. The same values are CSS custom properties on the
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

### 2.4 The default `<button>`

A `<button>` the widget did not style is drawn in the client's own button
style; any author CSS overrides it (`docs/decisions.md`, "Profile v1"). The
renderer inserts a user-agent-level stylesheet built from one template in
`packages/core`: `@layer tw-ua` declared first, `:where(button)` selectors,
values from the variables below (`uaStylesheet` in `packages/core/profile.json`).
The template fixes the states: base, `:active` (not disabled), `[disabled]`; no
hover state, no focus ring.

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
| double tap, long press | belong to the bubble (§5.5): `dblclick` and `contextmenu` are never delivered, and a touch the client takes for either ends for the widget with `pointercancel` and no `click` |

### 2.11 Errors and limits

| call | invalid arguments | outside the rules | other limits |
|---|---|---|---|
| `openLink`, `openTelegramLink` | `TypeError` | dropped, `activation-blocked` (no press, hidden, press used) | 256 characters; one call per press |
| haptics | `TypeError` | dropped, `haptic-limit` (hidden, over the bucket) | bucket 5, 10 per second |
| `localStorage`, `sessionStorage` | — | `QuotaExceededError` | 16 KB per area |
| `isProfileAtLeast` | `false` | — | — |
| `onEvent`, `offEvent` | ignored (unknown type or non-function) | — | 16 handlers per type; further ones are ignored |
| `ready` | — | — | — |

Diagnostics go to the host's diagnostics (`onDiag` / `WidgetDiagnostics`),
never to the widget.

A widget is killed, and its last frame kept (§5.7), for one of three reasons the
host sees in `died` / `onDead`: `CPU limit exceeded` (more than the task's slice
of continuous JS), `Layout query limit exceeded` (its synchronous layout queries
blocked the renderer for more than max(slice, 100 ms) in one task; the queries
also count toward the CPU quota) and `Memory limit exceeded (64 MB)`.

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

The glue checks in this order: the method and its arguments
(`checkBridgeCall`; any other method or a malformed call is dropped with
`bridge-rejected`, at most 20 reported); user activation for the two link
methods (`activation-blocked`); for `haptic` the gate of §2.6 (`haptic-limit`).
A call that passes goes to the client as a `WidgetAction` with the normalised
URL (and, deprecated, to `onBridge` with the raw method and arguments). The
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
| `theme` | `{ type: 'theme', theme: { colorScheme, params, button } }`: the renderer core builds the variables of §2.3 and §2.4 (`themeVariables`) and sets them on the root (`:root`, or `[data-tw-root]` on the shadow-DOM renderer); the colour scheme itself still goes to the renderer element (`color-scheme`, the WebView's appearance) as before |

The renderer takes only variable names from the fixed list above and values
that match their grammar (`#rrggbb`, CSS lengths, a font shorthand with a
generic family, numbers 0–1).

### 3.5 Renderer → host

| message | payload |
|---|---|
| `gesture` | `{ wants: boolean, x: boolean, y: boolean }` on every primary `pointerdown`: whether the widget takes horizontal (`x`) and vertical (`y`) drags of this touch, from the target's `touch-action` and the widget's `preventDefault` on `pointerdown`/`touchstart`. `wants` equals `y` (the field before v1) |
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
public struct WidgetTheme { var colorScheme: ColorScheme /* .light / .dark */; var params: [String: String]; var button: WidgetButtonTokens }
public struct WidgetButtonTokens { var bg, text, radius, font, padding, height: String?; var pressedOpacity, disabledOpacity: Double? }
public struct WidgetStorage { var local: [String: String]; var session: [String: String] }
public enum WidgetStorageArea: String { case local, session }
public enum WidgetHaptic { case impact(String), notification(String), selection }
public enum WidgetAction { case ready, openLink(URL), openTelegramLink(URL), haptic(WidgetHaptic) }
public struct WidgetGestureClaim { var x: Bool; var y: Bool }

init(html: String, theme: WidgetTheme = WidgetTheme(), width: CGFloat = 400, options: Options = Options(), …) throws
// Options: maximumHeight (600), policy (.strict), features, pools, …, locale ("en-US"), storage (the preload)
var onChange: ((WidgetDiagnostics) -> Void)?           // height, loaded, rendered, dead, diags, actions
var onError: ((String) -> Void)?
var onAction: ((WidgetAction) -> Void)?
var onStorageCommit: ((WidgetStorageArea, [String: String]) -> Void)?   // local before session
var onGestureClaim: ((WidgetGestureClaim) -> Void)?
func setTheme(_ theme: WidgetTheme)
func setVisible(_ visible: Bool); func snapshot(completion:); func destroy()
```

`WidgetBubble`, `WidgetModel` (`WidgetView`) and `WidgetPreparer` take a
`WidgetTheme` the same way. `URL(string:)` may escape a few characters the
check keeps (`[`, `]`, a second `#`); the checked string is
`SdkPolicy.checkLink`'s.

### 4.2 Kotlin (Android): `WidgetHost`

```kotlin
data class WidgetTheme(val colorScheme: String = "light", val params: Map<String, String> = emptyMap(), val button: WidgetButtonTokens = WidgetButtonTokens())
data class WidgetButtonTokens(val bg: String? = null, val text: String? = null, val radius: String? = null, val font: String? = null,
                              val padding: String? = null, val height: String? = null, val pressedOpacity: Double? = null, val disabledOpacity: Double? = null)
data class WidgetStorage(val local: Map<String, String> = emptyMap(), val session: Map<String, String> = emptyMap())
sealed class WidgetAction { Ready; OpenLink(url); OpenTelegramLink(url); Haptic(kind, value?) }   // kind: impact | notification | selection

WidgetHost(context, html, theme: WidgetTheme, widthDp, options, resources, rendererFactory)
// Options: minHeightDp (50), maxHeightDp (600), policy ("strict"), features, pools, …, locale ("en-US"), storage (the preload)
var onChange: ((WidgetDiagnostics) -> Unit)?               // … and actions
var onError: ((String) -> Unit)?
var onAction: ((WidgetAction) -> Unit)?
var onStorageCommit: ((area: String, values: Map<String, String>) -> Unit)?   // "local" | "session"
var onGestureClaim: ((x: Boolean, y: Boolean) -> Unit)?
fun setTheme(theme: WidgetTheme); fun setVisible(visible: Boolean); fun destroy()
```

The WebView renderer still applies `y` to its parent itself
(`requestDisallowInterceptTouchEvent`). The Blitz and litehtml prototypes send
only `wants`: the host reports it as `y`, with `x` false.

### 4.3 C++ (desktop core): `twidget::WidgetHost`

```cpp
struct WidgetButtonTokens { std::string bg, text, radius, font, padding, height; std::optional<double> pressedOpacity, disabledOpacity; };
struct WidgetTheme { std::string colorScheme; std::map<std::string, std::string> params; WidgetButtonTokens button; };
struct WidgetStorage { std::map<std::string, std::string> local, session; };
struct WidgetAction { enum Kind { Ready, OpenLink, OpenTelegramLink, Haptic } kind; std::string url, hapticKind, hapticValue; };

WidgetHost(std::string html, WidgetTheme theme, double width, WidgetHostOptions, std::shared_ptr<SandboxResources>, RendererFactory, …);
// WidgetHostOptions: …, std::string locale ("en-US"), WidgetStorage storage (the preload)
std::function<void(const WidgetDiagnostics&)> onChange;           // … actions, storageCommits (first 1000)
std::function<void(const std::string&)> onError;
std::function<void(const WidgetAction&)> onAction;
std::function<void(const std::string& area, const std::map<std::string, std::string>&)> onStorageCommit;
std::function<void(bool x, bool y)> onGestureClaim;
void start(); void setTheme(const WidgetTheme&); void setVisible(bool); void destroy();
```

Storage strings keep a lone surrogate as WTF-8 (what QuickJS hands over), so
a value round-trips exactly; it is not strict UTF-8 in that case. Commits
already on their way reach `onStorageCommit` before `destroy()` returns.

### 4.4 Web: `WidgetHost` (`@telegram/widget-sandbox/host/widget-host`)

```ts
interface WidgetTheme { colorScheme: 'light' | 'dark'; params?: Partial<ThemeParams>; button?: ButtonTokens }
type WidgetAction =
  | { kind: 'ready' }
  | { kind: 'openLink'; url: string }
  | { kind: 'openTelegramLink'; url: string }
  | { kind: 'haptic'; haptic: 'impact' | 'notification' | 'selection'; value?: string };
new WidgetHost(container, html, {
  theme,                                   // WidgetTheme, or a plain 'light' | 'dark'
  locale, storage: { local, session },     // the client's language and Web Storage preload
  onAction(action), onStorageCommit(area, values), onGestureClaim({ x, y }),
  policy, visible, onDead, onError, onLoaded, onResize, onDiag, rendererFactory, workerPool, features, …
})
host.setTheme(theme); host.setVisible(visible); host.destroy();
```

The web client's page must also carry the CSP of invariant 6 (`connect-src`
only Telegram's endpoints, `form-action 'none'`, no third-party
`img-src`/`media-src`/`font-src`/`frame-src`).

### 4.5 Node harness (`packages/core/scripts/sandbox-node.ts`)

For tests and tooling, not for clients. `RunOptions` takes `theme`, `locale`,
`themeParams`, `storage` and `script` steps `{ theme, themeParams }`;
`RunResult` has `actions` (what `onAction` would get) and `storageCommits`.
Gestures are injected per event (`gesture: n`).

## 5. What the client implements

### 5.1 Theme

- Map the chat's theme to every key of §2.3, for the bubble the widget sits
  in (incoming bubble colours, custom chat themes, night mode), and the
  button tokens of §2.4 from the client's own button. Pass them at creation
  and call `setTheme` on every change.
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

- PoC: taps only. v1: drag, with arbitration per touch.
- On every primary `pointerdown` the host reports `onGestureClaim(x, y)`:
  `x` means the widget takes horizontal drags of this touch (the client
  suppresses swipe-to-reply and other horizontal chat gestures for it), `y`
  vertical ones (the client keeps the list from scrolling). With neither, the
  touch belongs to the chat as usual, and the widget still gets its taps.
- Double tap and long press always belong to the bubble: the quick reaction
  and the message menu (a right click on desktop). The widget never gets
  `dblclick` or `contextmenu`; a touch the client takes for either ends for the
  widget with `pointercancel` and no `click`. The widget gets single taps and,
  in v1, drags. The Android WebView renderer already applies `y` to its parent
  itself.
- No keyboard: the client never routes key events to the widget, and the
  chat's input field keeps the focus.

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
- No text selection in the bubble (D18); the renderers disable it in profile
  v1.

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

- The widget is `messageMediaWidget` (layer 233): gunzip `payload`, no size
  limit of your own, hand the HTML to the host (`docs/tgw-format.md`,
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
  `docs/integration.md`, "Testing"); `.tgw` is only the local development
  format.

## 6. Out of v1 and the decisions of 2026-10-17

Out of v1: sending messages or data to the bot and bot callbacks; inline mode
and sharing; payments; the viewer's identity; popups; `MainButton`; per-bot
`DeviceStorage` and cloud storage; keyboard input; form controls other than
`<button>`; double taps and long presses on the widget; `TelegramWidget.platform`;
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
   message menu): the widget gets single taps and, in v1, drags (§5.5).
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
