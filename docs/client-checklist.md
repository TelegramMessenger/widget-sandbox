# Client checklist for widgets v0.1.0

One page for a client team. Each box links to where the detail is:
`docs/integration.md` (the host), `docs/sdk-v1.md` §4–5 (the SDK and what the
client implements), `packages/conformance/CLIENTS.md` (the conformance set),
`docs/tgw-format.md` (the message media).

## a. Adopt the framework

- [ ] Pin the tag `v0.1.0`, never a branch ([integration.md, "Pinning a release"](integration.md#pinning-a-release)).
- [ ] Create one `WidgetHost` per widget on your platform; policy `strict`
      (the default), `features` as the profile sets them, today `["webAnimations"]`
      ([integration.md, "Policy and features"](integration.md#policy-and-features); the per-platform
      API in [sdk-v1.md §4](sdk-v1.md#4-host-interface-per-platform)).
- [ ] `setVisible(false)` off screen, `setTheme` on theme change, `destroy()` on
      recycle ([integration.md](integration.md)).

## b. PoC on real messages (layer 233) from a test bot

- [ ] Render `messageMediaWidget`: gunzip `payload`, no size limit of your own
      ([tgw-format.md](tgw-format.md), [integration.md, "The widget payload"](integration.md#the-widget-payload)).
- [ ] Enable rendering with your own platform/version flag; without it, and on
      old layers, the message shows as unsupported media with its caption.
- [ ] Bubble sizing from the anchors: width 0 = the bubble's width, W = a fixed width in
      [200, max]; height 0 = placeholder, then the content height in [50, 600] cached per
      message, H = fixed clamp(H, 50, 600); hard clip at the bubble's height, no fullscreen
      ([integration.md, "Bubble sizing"](integration.md#bubble-sizing), [sdk-v1.md §5.6](sdk-v1.md#56-bubble-size)).
- [ ] Taps only reach the widget; no keyboard input, no focus; text is not selectable
      unless the widget sets `user-select: text`, and then it is selected the way your
      client selects message text and copied with your copy command
      ([integration.md, "Input"](integration.md#input), [sdk-v1.md §5.10](sdk-v1.md#510-text-selection-v011)).
- [ ] Off-screen pause: frames stop, timers at 1 Hz when the bubble leaves the screen.
- [ ] A killed widget keeps its last frame with the "Widget stopped" badge; the
      bubble does not collapse ([sdk-v1.md §5.7](sdk-v1.md#57-failures-and-restarts-d22)).
- [ ] Caption as the preview text; the localized "Widget" stub without one.
- [ ] A test bot sends widgets (≤ 4 KB gzip, every conformance widget included) to a
      tester's private chat; forward them into the shared test channel (bots may send
      widgets only to private chats).

## c. SDK v1

- [ ] Theme: the palette of the bubble the widget sits in (an outgoing bubble's when
      the user sent or forwarded it), the button tokens and the text tokens into
      `WidgetTheme`, updated on theme change; the widget view stays transparent over
      the bubble, which is its canvas ([sdk-v1.md §5.1](sdk-v1.md#51-theme)).
- [ ] Text tokens (v0.1.1): `fontSize` follows the chat's text-size setting and
      `setTheme` runs when the user changes it; `lineHeight` and `padding` match your
      message text. `visual/14-unstyled-document` and `corpus/defaults` look like your
      own messages ([sdk-v1.md §2.4](sdk-v1.md#24-the-renderers-defaults-the-root-the-text-button)).
- [ ] A native renderer (not a WebView) puts the profile's UA layer into its master CSS,
      sets the theme variables on the root, propagates a body background to the whole
      bubble and honours `user-select: text` (`docs/decisions.md`, "The renderer's
      defaults", "Text selection").
- [ ] Links: `openLink` / `openTelegramLink` only from a real tap; confirmation
      showing the full URL; then open ([sdk-v1.md §5.2](sdk-v1.md#52-links)).
- [ ] Haptics with the rate limit ([sdk-v1.md §5.3](sdk-v1.md#53-haptics)).
- [ ] Storage per (account, chat, message id) on disk, preloaded at creation,
      committed from `onStorageCommit`, deleted with the message, the chat, the
      account and on "Clear cache" ([sdk-v1.md §5.4](sdk-v1.md#54-storage)).
- [ ] Drag arbitration against swipe-to-reply and the list scroll; double tap and
      long press stay with the bubble ([sdk-v1.md §5.5](sdk-v1.md#55-gestures-and-drag-d4)).
- [ ] A widget stays stopped after 2 crashes in a row ([sdk-v1.md §5.7](sdk-v1.md#57-failures-and-restarts-d22)).
- [ ] A forwarded copy works like the original with its own, empty storage; a bot
      edit replaces the payload and restarts the widget on the same storage.

## d. Test

- [ ] The conformance automatic set on your renderer, with the report
      ([CLIENTS.md](../packages/conformance/CLIENTS.md)).
- [ ] The attack corpus (`corpus/attacks`): 0 leaks, 0 outside requests.
- [ ] The whole conformance set in the test channel as real layer-233 messages from
      the test bot, in the order of `packages/conformance/channel.json`, each widget
      with its reference picture posted next to it (`visual/` and `visual/sdk/`;
      the automatic widgets report their own checks). Go through the visual ones with
      the manual checklist: light and dark themes, rotation, system font size, an RTL
      locale, reduce motion, bubble re-creation on scroll, forward, edit.
- [ ] A feed bench on a low-end device: many widgets in a list, scroll, memory.

## e. Send back

- [ ] The conformance report (`<platform>-<renderer>.json` and `summary.md`) and
      every discrepancy you found, to the widget-sandbox maintainers or as a PR
      into `packages/conformance/results/`.
