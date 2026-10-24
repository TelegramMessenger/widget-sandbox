# The widget payload: `messageMediaWidget` (layer 233)

A **widget is mini-HTML**: one self-contained HTML document, inline CSS and
JS, `data:` images only, no external resources, of at most **4 KB compressed**
and **32 KB uncompressed**. It travels as message media of its own, inline in
the message, since API layer 233 (shipped together with widget-sandbox
v0.1.0). The server never runs, parses or renders the HTML: it checks the
bytes and packs them.

```tl
inputMediaWidget   width:int height:int html:string    = InputMedia;
messageMediaWidget width:int height:int payload:bytes  = MessageMedia;
```

## Sending and delivery

- A bot sends `messages.sendMedia` / `messages.uploadMedia` with
  `inputMediaWidget`: `html` is the UTF-8 source of the widget (required);
  `width` and `height` are the optional size anchors in CSS px, `0` meaning
  "auto". Bots never send compressed bytes.
- The server checks the source (valid UTF-8, no NUL bytes, at most 32 KB),
  compresses it itself with zlib at level 9 (PHP `gzcompress($html, 9)`),
  checks the **packed** payload (at most 4 KB) and puts the bytes into the
  message. No document, no file id, no reuse by id.
- The client receives `messageMediaWidget`: `payload` is a **zlib stream**
  (RFC 1950: a 2-byte header, raw deflate, a 4-byte Adler-32), **not gzip**
  (RFC 1952, magic `1f 8b`) and not raw deflate (RFC 1951). The client inflates
  it (below) and hands the UTF-8 HTML to `WidgetHost` (`docs/integration.md`).
  The client keeps **no size limits of its own**: the server enforces both, so
  a payload the server produced always decodes to HTML within them, and the sandbox, the validator and the runtime limits
  are the security boundary, not the size.
- An edit by the bot replaces the payload as a whole, with the same checks.
  The widget's state lives only in the message's storage on the device and
  is deleted with the message (`docs/sdk-v1.md` §2.8).

| Error | Cause |
|---|---|
| `INPUT_MEDIA_WIDGET_TOO_LARGE` | HTML over 32 KB, or the compressed payload over 4 KB |
| `INPUT_MEDIA_WIDGET_HTML_INVALID` | not text HTML: broken UTF-8, binary data, compressed bytes sent by the bot |
| `INPUT_MEDIA_WIDGET_NOT_ALLOWED` | the sender is not a bot, the chat is not the bot's private chat with the user, or an inline, draft or ephemeral message |

### Inflating the payload

What reads RFC 1950 on each platform, and the traps:

| Platform | Inflate with | Not with |
|---|---|---|
| Web | `new Response(new Blob([payload]).stream().pipeThrough(new DecompressionStream('deflate'))).text()`: `'deflate'` is RFC 1950 | `'gzip'`, `'deflate-raw'` |
| Android, JVM | `java.util.zip.Inflater()` (the default, `nowrap = false`) or `InflaterInputStream` | `GZIPInputStream`, `Inflater(true)` |
| Apple | zlib's `uncompress` / `inflate` (libz) | `Compression`'s `COMPRESSION_ZLIB` and `NSData.decompressed(using: .zlib)`: raw deflate despite the name; with them, skip the 2-byte header and drop the 4-byte trailer |
| Qt (tdesktop) | zlib's `uncompress` / `inflate` | `qUncompress` alone: it expects a 4-byte big-endian length before the stream |
| C, C++ (zlib) | `inflateInit` / `uncompress`; `inflateInit2(&s, 15 + 32)` also accepts gzip, if a client has to read both | `inflateInit2(&s, 31)` (gzip only), `-15` (raw) |

A payload the server produced inflates to at most 32 KB of valid UTF-8, so a
32 KB output buffer is enough; a stream that fails to inflate is a broken
message: show the unsupported-media fallback.

## Who may send, and where it may go

Only bots, only into their private chat with the user: groups, channels,
inline mode, drafts and ephemeral messages are refused. **Forwarding is
allowed to any chat**, groups and channels included, and the forwarded copy
works exactly like the original (isolated, no network, no messages to the
bot); it is a different message, so it has its own storage, which starts
empty.

## Caption, preview, old clients

The message's ordinary media caption is the text for the preview, the chat
list and search; without a caption the client shows the localized "Widget"
stub, as for other media. Clients below layer 233 receive
`messageMediaUnsupported` with the caption text and show their usual
placeholder; a new client keeps that fallback for messages it decides not to
render.

The server has no flag for widgets: each client enables rendering itself, by
platform and version (a debug or rollout flag on the client side).

## Size anchors

`width` and `height` are always present in the media; `0` means auto, and
each axis is independent. The server passes them through unchanged; the client
keeps them within sensible bounds. How the client turns them into the bubble
before the widget has loaded is in `docs/integration.md` ("Bubble sizing").

## Writing a mini-widget

4 KB compressed is about 12–20 KB of ordinary HTML, CSS and JS: a card with a
few controls, a chart drawn on a canvas, an animation. What does not fit:
embedded images beyond tiny icons (a `data:` PNG compresses badly), bundled
libraries, long text. Widget profile 1 (`docs/profile-v1.md`) and SDK v1
(`docs/sdk-v1.md`) exist partly to keep widgets small: the client provides
the theme, the button style and the APIs every widget would otherwise embed,
and there are no bundled libraries.

The server compresses with zlib at level 9 (PHP `gzcompress($html, 9)`), so an
author can measure what it counts:
`python3 -c 'import sys,zlib; print(len(zlib.compress(sys.stdin.buffer.read(), 9)))' < widget.html`
(`gzip -9 -n` counts about 12 bytes more: its header and trailer are longer), the
harness's `size:` line or the dev app's stats panel (both warn above 3800 B,
the margin the conformance set keeps, and flag anything over a limit). A bot
that gets `INPUT_MEDIA_WIDGET_TOO_LARGE` for HTML under 32 KB has hit the
compressed limit.
How to write widgets that fit, and everything else an author needs:
`docs/widget-runtime-profile.md`.

## Testing

Widgets come only as `messageMediaWidget`: an `.html` a user uploads as a
document is a file, not a widget. Testing goes through real layer-233
messages, the conformance set included. Every conformance widget fits a mini-widget (`packages/conformance`
builds each one under 3800 bytes after zlib at level 9). A test bot sends
it to a tester's private chat with its reference picture as a photo next to
it, in the order of `packages/conformance/channel.json`, and the messages are
forwarded into the shared test channel. That also covers the corpus widgets
that fit, forward-copy storage, bot edits and the size anchors.

## Profile and versions

The HTML is written against widget profile 1 (`docs/profile-v1.md`;
`packages/core/profile.json` is the source). A widget learns the client's
profile at runtime through `TelegramWidget.profile`, `isProfileAtLeast(n)`,
`CSS.supports` and `@supports`, all answered from the same tables. The payload
itself has no version field: the profile is the framework build's, the media
layout is the layer's.
