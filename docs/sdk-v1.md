# SDK v1: the widget contract

Status: contract for the framework and the client teams, 2026-10-16. It fixes
what a widget can call, what travels between sandbox, host and renderer, what
every host exposes and what every client implements, so that client teams can
build their side in parallel with the PoC. Decisions behind it: release plan
D4, D7a, D8, D9, D12, D14, D17, D18, D19, D22, D23, and the decisions of
2026-10-17 (no keyboard, no form controls, a client-styled `<button>`,
`docs/decisions.md`, "Profile v1"). Background: `docs/bridge-discussion.md`
("Решено"), `docs/widget-state.md` §7.

Each item is marked **today** (in the framework now) or **v1** (to be built in
the framework's SDK milestone). Values marked *proposed* are this document's
choice where no decision exists yet; they are listed again in §6.

Related documents, not repeated here: `docs/integration.md` (embedding the
host on each platform), `docs/tgw-format.md` (the `.tgw` file and the rich
block), `docs/protocol.md` (the full wire protocol), `docs/changes.md`
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
| gestures | reports per press which drag axes the widget takes | arbitrates with swipe-to-reply, list scroll, long press, double tap |
| size | reports the content height | sizes the bubble from the bot's hint before load, caps it, clips |
| failures | freezes the last frame with a badge and reports `dead` | restarts at most 2–3 times in a row, then leaves it stopped |

## 2. Widget-facing API

Everything below is what widget code sees inside the sandbox. The object is
`window.TelegramWidget`; member names follow `Telegram.WebApp` (Mini Apps)
where one exists, so that code and models that know Mini Apps carry over,
and so that a JSON-RPC facade for MCP Apps (`ui/open-link`,
`ui/notifications/host-context-changed`, `ui/notifications/initialized`) can be
laid over the same calls later (D19).

### 2.1 Members

| member | type | status |
|---|---|---|
| `profile` | `number`, read-only | v1 |
| `isProfileAtLeast(n)` | `(n: number) => boolean` | v1 |
| `colorScheme` | `'light' \| 'dark'`, read-only | v1 (today: `theme`) |
| `themeParams` | `ThemeParams`, read-only, a fresh frozen object per change | v1 |
| `isActive` | `boolean`, read-only (`!document.hidden`) | v1 |
| `onEvent(type, handler)`, `offEvent(type, handler)` | `type`: `'themeChanged' \| 'activated' \| 'deactivated'` | v1 (today: `on(event, cb)`, reserved) |
| `openLink(url)` | `(url: string) => void` | today without the checks below; v1 |
| `openTelegramLink(url)` | `(url: string) => void` | v1 |
| `HapticFeedback.impactOccurred(style)` | `style`: `'light' \| 'medium' \| 'heavy' \| 'rigid' \| 'soft'` | v1 |
| `HapticFeedback.notificationOccurred(type)` | `type`: `'error' \| 'success' \| 'warning'` | v1 |
| `HapticFeedback.selectionChanged()` | `() => HapticFeedback` | v1 |
| `ready()` | `() => void` | today |
| `theme`, `onTheme(cb)` | kept as aliases of `colorScheme` and `onEvent('themeChanged')` (the callback gets the colour scheme) | today, deprecated in v1 |
| `sendMessage` | removed in v1 (`typeof TelegramWidget.sendMessage === 'undefined'`) | today; removed |

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
  validator would keep that declaration. Today `CSS.supports` always returns
  `true`.
- `@supports` rules are evaluated by the validator against the same tables:
  the block is kept or dropped before the CSS reaches the renderer, so the
  renderer's engine never decides. Today the condition passes through and the
  renderer engine evaluates it.
- A widget that checks nothing still loads; on an older client the features
  outside that client's profile are dropped by its validator (accepted product
  behaviour). Clients without widget support show the existing
  unsupported-rich-block stub.

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
`color-scheme: light dark` already follow the chat's scheme (today).

A theme change (the user switches theme, the chat's theme changes, night mode)
updates `colorScheme`, `themeParams` and the variables in one task, then fires
`themeChanged` (and the legacy `onTheme` callbacks). Today the widget gets only
`'light' | 'dark'`.

### 2.4 The default `<button>`

A `<button>` the widget did not style is drawn in the client's own button
style; any author CSS overrides it (`docs/decisions.md`, "Profile v1"). The
renderer inserts a user-agent-level stylesheet built from one template in
`packages/core`: `@layer tw-ua` declared first, `:where(button)` selectors,
values from the variables below. The template fixes the states: base,
`:active`, `:disabled`, `:hover` on desktop; no focus ring.

| variable | meaning |
|---|---|
| `--tg-button-bg`, `--tg-button-text` | default `var(--tg-theme-button-color)`, `var(--tg-theme-button-text-color)` |
| `--tg-button-radius` | corner radius, CSS length |
| `--tg-button-font` | CSS `font` shorthand, a system generic family only (D3b) |
| `--tg-button-padding` | CSS padding |
| `--tg-button-height` | minimum height |
| `--tg-button-pressed-opacity`, `--tg-button-disabled-opacity` | 0–1 |

Other form controls are outside the profile and dropped by the validator.

### 2.5 Links (D8)

- `openLink(url)` opens an external page. `url` must be an absolute `https:`
  URL of at most 2048 characters (*proposed*), without user name or password,
  whose host is not `t.me`, `telegram.me` or `telegram.dog`.
- `openTelegramLink(url)` opens a Telegram link: `https://t.me/…` (also
  `telegram.me`, `telegram.dog`), at most 2048 characters. `tg:` URLs are not
  accepted.
- An invalid argument throws `TypeError` in the widget synchronously (the shim
  checks the syntax; the glue checks it again). A valid call returns
  `undefined` and goes to the host.
- **User activation** (today, `docs/protocol.md`, "User activation"): the glue
  forwards a call only while the task of a real pointer press on the visible
  widget runs, and one press unlocks one call shared by both methods. A press
  is `pointerdown` … `click` / `dblclick` from the renderer, marked `trusted`;
  keys never count (D4: no keyboard input or focus at all), nor does a click a
  key produced (`detail` 0), a scripted `click()`, a timer, a frame or a promise
  job after the task. Blocked calls are dropped without an exception and
  reported as the diagnostic `activation-blocked` (at most 20 per widget).
- `<a href="https://…">` clicks and `window.open(url)` take the same path: the
  shim calls `openTelegramLink` for Telegram hosts and `openLink` otherwise
  (v1; today everything goes to `openLink`). `javascript:` links are ignored.
- The widget is not told whether the user confirmed.

### 2.6 Haptics

- `impactOccurred`, `notificationOccurred`, `selectionChanged` as in Mini
  Apps' `HapticFeedback`. An unknown style or type throws `TypeError`.
- Delivered only while the widget is visible. Rate limit per widget: a bucket
  of 5 calls refilled at 10 per second (*proposed*); calls over the limit or
  from a hidden widget are dropped without an exception and reported once per
  second as `haptic-limit`. No user gesture is required (*proposed*, §6).
- Desktop and web clients accept the calls and play nothing.

### 2.7 Lifecycle

- `document.hidden`, `document.visibilityState` and `visibilitychange`
  (today): a widget scrolled off screen, in a background tab or app gets
  `hidden`; while hidden it gets no animation frames and its timers fire at
  most once per second. `isActive` and the `activated` / `deactivated` events
  mirror the same state (v1).
- `ready()` tells the host the first state is drawn (today). It is optional:
  the host also notices the first content on its own.
- A widget can be destroyed and recreated at any time (scrolled far away,
  memory pressure, app restart). Only Web Storage survives (§2.8).
- When the runtime is killed (CPU, memory, a crash), the last frame stays
  frozen with a "Widget stopped" badge (today).

### 2.8 Storage (D17, `docs/widget-state.md` §7)

- `localStorage` is stored by the client for this message on this device,
  under (account, chat, message id), where the chat is the one the message is
  in: the bot's private chat for the message it sent, the destination chat for
  a forwarded copy, which therefore starts empty. (D17 says (account, bot,
  `message_id`); message ids are unique only within a chat, so the chat is the
  exact key, and for the bot's own message it is the bot's private chat.) It
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
default locale is the same (v1; today always `en-US`).

### 2.10 Removed and inert

| API | v1 behaviour |
|---|---|
| `TelegramWidget.sendMessage` | removed; no message is ever sent on the user's behalf |
| `alert`, `confirm`, `prompt` | no dialog, nothing sent to the host; `confirm` returns `false`, `prompt` `null` (today they reach the host as a bridge call `alert`, v1 stops that) |
| `navigator.clipboard.writeText`, `readText`, `navigator.share` | reject with `NotAllowedError`, nothing sent to the host (today `writeText` reaches the host as `clipboard`, v1 stops that) |
| `fetch`, `XMLHttpRequest`, `WebSocket`, … | throw `NetworkDisabledError` (invariant 5, today) |
| keyboard | no key events, no keyboard focus (D4; the renderers and glue drop key events in profile v1) |

### 2.11 Errors and limits

| call | invalid arguments | outside the rules | other limits |
|---|---|---|---|
| `openLink`, `openTelegramLink` | `TypeError` | dropped, `activation-blocked` (no press, hidden, press used) | 2048 characters; one call per press |
| haptics | `TypeError` | dropped, `haptic-limit` (hidden, over the bucket) | bucket 5, 10 per second |
| `localStorage`, `sessionStorage` | — | `QuotaExceededError` | 16 KB per area |
| `isProfileAtLeast` | `false` | — | — |
| `onEvent`, `offEvent` | ignored (unknown type or non-function) | — | 64 handlers per type (*proposed*) |
| `ready` | — | — | — |

Diagnostics go to the host's diagnostics (`onDiag` / `WidgetDiagnostics`),
never to the widget.

## 3. Protocol

What carries the API between the sandbox (QuickJS), the host glue and the
renderer. Every message from the sandbox is untrusted and is checked in all
five glue copies: the web worker and `WidgetHost` (TS), the Node harness,
Swift, Kotlin, C++ (`sandbox_engine.cpp`, `widget_host.cpp`). The checks live in
one policy per concern with a shared JSON fixture, as `cpu-budget` does today.

### 3.1 Config (`SandboxConfig`, host → sandbox at start)

| field | type | set by | status |
|---|---|---|---|
| `theme` | `'light' \| 'dark'` | client (`colorScheme`) | today |
| `themeParams` | `Record<ThemeKey, '#rrggbb'>` | client | v1 |
| `locale` | BCP 47 string, at most 35 characters | client | v1 |
| `profile` | integer | glue (its own constant, from `VERSION.json`) | v1 |
| `storage` | `{ local: Record<string, string>, session: Record<string, string> }` | client (the preload), glue checks the quota | v1 |
| `policy` | `'strict'` | glue default | today |

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

Any other method or a malformed call is dropped with `bridge-rejected` (at most
20). The method names `sendMessage`, `alert` and `clipboard` disappear from the
wire in v1.

Gesture tagging (today): on native hosts the engine runs a trusted event's task
with that press's gesture number and lets the first privileged call of the task
consume it. On the web the worker tags a `bridge` message with the gesture of
the task (`{type:'bridge', method, args, gesture}`) and the main-thread host
accepts each issued gesture once, within 5 s.

`__host('storage', JSON.stringify(commit))`, at most once per task, after the
task's `ops`: `commit` is `{ local?: Record<string, string>, session?:
Record<string, string> }`, the full content of each area that changed in the
task. The glue checks that it is an object of strings within the quota and
replaces its copy of that area; a commit that fails is dropped with
`storage-rejected` and the client keeps the previous content. Commits reach the
client in order.

### 3.3 Host → sandbox

| message | payload | status |
|---|---|---|
| `theme` | `{ theme: 'light' \| 'dark', themeParams }` | today with `theme` only; v1 adds `themeParams` |
| `visibility` | `{ visible }` | today |
| `event` | a renderer event, with `gesture: n` for a trusted press | today |

### 3.4 Host → renderer

| message | payload | status |
|---|---|---|
| theme | colour scheme plus the variables of §2.3 and §2.4, set on the root (`:root`, or `[data-tw-root]` on the shadow-DOM renderer) | today: colour scheme only; v1: the variables |

The renderer takes only variable names from the fixed list above and values
that match their grammar (`#rrggbb`, CSS lengths, a font shorthand with a
generic family, numbers 0–1).

### 3.5 Renderer → host

| message | payload | status |
|---|---|---|
| `gesture` | `{ wants: boolean, x: boolean, y: boolean }` on every primary `pointerdown`: whether the widget takes horizontal (`x`) and vertical (`y`) drags of this touch, from the target's `touch-action` and the widget's `preventDefault` on `pointerdown`/`touchstart`. `wants` equals `y` (today's field) | today: `wants`; v1: `x`, `y` |
| `size` | the content height | today |
| `event` | `trusted: true` only for real input | today |

## 4. Host interface per platform

The shapes the client wires up. Existing members are listed as they are;
**v1** members are the contract the framework milestone implements. Callbacks
run on the main thread (Swift, Kotlin), on the host thread (C++) or on the
page's event loop (web).

### 4.1 Swift (iOS, macOS): `WidgetHost`

```swift
// today
init(html: String, theme: String, width: CGFloat, options: Options, …) throws
var onChange: ((WidgetDiagnostics) -> Void)?      // height, loaded, rendered, dead, diags
var onError: ((String) -> Void)?
var onBridge: ((String, String) -> Void)?         // raw method + args JSON; deprecated in v1
func setVisible(_ visible: Bool)
func snapshot(completion: @escaping (PlatformImage?) -> Void)
func destroy()
// Options: maximumHeight (900), policy (.strict), features, pools, …

// v1
public struct WidgetTheme {
    public var colorScheme: ColorScheme                // .light / .dark
    public var params: [String: String]                // themeParams keys → "#rrggbb"
    public var button: WidgetButtonTokens               // §2.4
}
public struct WidgetStorage { public var local: [String: String]; public var session: [String: String] }
public enum WidgetHaptic { case impact(String), notification(String), selection }
public enum WidgetAction { case ready, openLink(URL), openTelegramLink(URL), haptic(WidgetHaptic) }
public enum WidgetStorageArea { case local, session }
public struct WidgetGestureClaim { public var x: Bool; public var y: Bool }

init(html: String, theme: WidgetTheme, width: CGFloat, options: Options, …) throws
// Options: + locale: String, + storage: WidgetStorage (the preload)
var onAction: ((WidgetAction) -> Void)?
var onStorageCommit: ((WidgetStorageArea, [String: String]) -> Void)?
var onGestureClaim: ((WidgetGestureClaim) -> Void)?
func setTheme(_ theme: WidgetTheme)                    // replaces setTheme(String)
```

### 4.2 Kotlin (Android): `WidgetHost`

```kotlin
// today
WidgetHost(context, html, theme = "light", widthDp, options, resources, rendererFactory)
var onChange: ((WidgetDiagnostics) -> Unit)?
var onError: ((String) -> Unit)?
var onBridge: ((String, String) -> Unit)?        // deprecated in v1
fun setVisible(visible: Boolean); fun destroy()
// Options: minHeightDp (24), maxHeightDp (900), policy ("strict"), features, pools, …
// The WebView renderer already applies the gesture decision to its parent
// (requestDisallowInterceptTouchEvent).

// v1
data class WidgetTheme(val colorScheme: String, val params: Map<String, String>, val button: WidgetButtonTokens)
data class WidgetStorage(val local: Map<String, String> = emptyMap(), val session: Map<String, String> = emptyMap())
sealed class WidgetAction {
    object Ready : WidgetAction()
    data class OpenLink(val url: String) : WidgetAction()
    data class OpenTelegramLink(val url: String) : WidgetAction()
    data class Haptic(val kind: String, val value: String?) : WidgetAction()   // impact|notification|selection
}
// constructor takes theme: WidgetTheme; Options: + locale: String, + storage: WidgetStorage
var onAction: ((WidgetAction) -> Unit)?
var onStorageCommit: ((area: String, values: Map<String, String>) -> Unit)?   // "local" | "session"
var onGestureClaim: ((x: Boolean, y: Boolean) -> Unit)?
fun setTheme(theme: WidgetTheme)
```

### 4.3 C++ (desktop core): `twidget::WidgetHost`

```cpp
// today
WidgetHost(std::string html, std::string theme, double width, WidgetHostOptions, std::shared_ptr<SandboxResources>, RendererFactory, …);
std::function<void(const WidgetDiagnostics&)> onChange;
std::function<void(const std::string& method, const std::string& argsJson)> onBridgeCall;   // deprecated in v1
std::function<void(const std::string&)> onError;
void start(); void setVisible(bool); void destroy();

// v1
struct WidgetTheme { std::string colorScheme; std::map<std::string, std::string> params; WidgetButtonTokens button; };
struct WidgetStorage { std::map<std::string, std::string> local, session; };
struct WidgetAction { enum Kind { Ready, OpenLink, OpenTelegramLink, Haptic } kind; std::string url, hapticKind, hapticValue; };
// WidgetHostOptions: + std::string locale, + WidgetStorage storage; the constructor takes WidgetTheme
std::function<void(const WidgetAction&)> onAction;
std::function<void(const std::string& area, const std::map<std::string, std::string>&)> onStorageCommit;
std::function<void(bool x, bool y)> onGestureClaim;
void setTheme(const WidgetTheme&);
```

### 4.4 Web: `WidgetHost` (`@telegram/widget-sandbox/host/widget-host`)

```ts
// today
new WidgetHost(container, html, { theme, policy, visible, onBridge, onDead, onError, onLoaded, onResize, onDiag, rendererFactory, workerPool, features, … })
host.setTheme(theme); host.setVisible(visible); host.destroy();

// v1
interface WidgetTheme { colorScheme: 'light' | 'dark'; params: ThemeParams; button: ButtonTokens }
type WidgetAction =
  | { kind: 'ready' }
  | { kind: 'openLink'; url: string }
  | { kind: 'openTelegramLink'; url: string }
  | { kind: 'haptic'; haptic: 'impact' | 'notification' | 'selection'; value?: string };
// options: theme: WidgetTheme (a plain 'light' | 'dark' still accepted), locale, storage: { local, session },
//          onAction(action), onStorageCommit(area, values), onGestureClaim({ x, y })
host.setTheme(theme: WidgetTheme)
```

The web client's page must also carry the CSP of invariant 6 (`connect-src`
only Telegram's endpoints, `form-action 'none'`, no third-party
`img-src`/`media-src`/`font-src`/`frame-src`).

### 4.5 Node harness (`packages/core/scripts/sandbox-node.ts`)

For tests and tooling, not for clients. `RunOptions` gains `locale`,
`themeParams`, `storage`; `RunResult` gains `actions` (what `onAction` would get)
and `storageCommits`. Gestures are injected per event (`gesture: n`), as
today.

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
  area. Write `local` to disk with a short delay (*proposed*: 1 s) and at once
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
- Long press, double tap and swipe-to-reply on a bubble with a widget follow
  the designers' rules (§6). The Android WebView renderer already applies `y` to
  its parent itself.
- No keyboard: the client never routes key events to the widget, and the
  chat's input field keeps the focus.

### 5.6 Bubble size

- Before the widget loads: if the bot gave `width` and `height`, draw the
  bubble with that aspect ratio at the bubble's width; with `width = 0` and
  `height = H`, at height `H`; with neither, the default placeholder (height
  from the designers) (D12b; the rich block's fields are in
  `docs/tgw-format.md`).
- After the first render, use the height the host reports (`onChange` →
  `height`, web `onResize`), capped at the client's maximum (`maximumHeight`,
  `maxHeightDp`, the web host's 900 px today), and cache it per message for the
  next time the bubble is built.
- Content beyond the cap is clipped: no inner scrolling (outside the
  profile), no fullscreen button (D12a).
- No text selection in the bubble (D18); the renderers disable it in profile
  v1.

### 5.7 Failures and restarts (D22)

- When the host reports `dead`, the last frame stays with the badge
  (`stoppedLabel`, localised by the client).
- The client may build a new host for the message the next time the bubble is
  shown, at most 2–3 times in a row (*proposed*: 3); after that the widget stays
  stopped until the app restarts or the message is edited with new HTML.

### 5.8 Visibility

Call `setVisible(false)` when the bubble leaves the screen and when the app
goes to the background, `setVisible(true)` when it is back.

## 6. Out of v1 and open questions

Out of v1: sending messages or data to the bot and bot callbacks; inline mode
and sharing; payments; the viewer's identity; popups; `MainButton`; per-bot
`DeviceStorage` and cloud storage; keyboard input; form controls other than
`<button>`; fullscreen; accessibility (D21); MCP Apps transport (D19, later).

Open, with this document's proposal:

1. Haptics without a gesture: allowed while visible, with the rate limit
   (proposed), or only within the task of a pointer press?
2. Rate limit numbers: haptics bucket 5 at 10 per second; URL length 2048;
   64 handlers per event type; 3 restarts.
3. `openTelegramLink` paths: any `t.me` path through the client's own handler
   (proposed), or a deny-list (`proxy`, `socks`, `login`, `setlanguage`, …) in the
   glue?
4. `TelegramWidget.platform` (`ios`, `android`, `macos`, `desktop`, `web`): useful
   for layout tweaks, a little fingerprinting; not in v1 (proposed).
5. Bubble gestures: does a double tap on the widget's area react, does a long
   press open the menu while the widget takes a drag (designers).
6. Should `openLink` report back whether the user opened the link (an
   `linkOpened` event)? Not in v1 (proposed).
