# The widget payload: `messageMediaWidget` (layer 233)

A **widget is mini-HTML**: one self-contained HTML document, inline CSS and
JS, `data:` images only, no external resources, of at most **4 KB after gzip**
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
  "auto". Bots never send gzip.
- The server checks the source (valid UTF-8, no NUL bytes, at most 32 KB),
  gzips it itself, checks the **packed** payload (at most 4 KB) and puts the
  bytes into the message. No document, no file id, no reuse by id.
- The client receives `messageMediaWidget`: `payload` is the raw gzip bytes;
  the client `gunzip`s them and hands the HTML to `WidgetHost`
  (`docs/integration.md`). The client keeps **no size limits of its own**:
  the server enforces both, so a payload the server produced always decodes
  to HTML within them, and the sandbox, the validator and the runtime limits
  are the security boundary, not the size.
- An edit by the bot replaces the payload as a whole, with the same checks.
  The widget's state lives only in the message's storage on the device and
  is deleted with the message (`docs/sdk-v1.md` §2.8).

| Error | Cause |
|---|---|
| `INPUT_MEDIA_WIDGET_TOO_LARGE` | HTML over 32 KB, or the gzip payload over 4 KB |
| `INPUT_MEDIA_WIDGET_HTML_INVALID` | not text HTML: broken UTF-8, binary data, gzip sent by the bot |
| `INPUT_MEDIA_WIDGET_NOT_ALLOWED` | the sender is not a bot, the chat is not the bot's private chat with the user, or an inline, draft or ephemeral message |

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

4 KB of gzip is about 12–20 KB of ordinary HTML, CSS and JS: a card with a
few controls, a chart drawn on a canvas, an animation. What does not fit:
embedded images beyond tiny icons (a `data:` PNG compresses badly), bundled
libraries, long text. Widget profile 1 (`docs/profile-v1.md`) and SDK v1
(`docs/sdk-v1.md`) exist partly to keep widgets small: the client provides
the theme, the button style and the APIs every widget would otherwise embed,
and there are no bundled libraries.

Widget authors see one number, the uncompressed limit; the compressed one is
the server's to check, because the author cannot know how the server's gzip
compresses. A bot that gets `INPUT_MEDIA_WIDGET_TOO_LARGE` for HTML under
32 KB has hit the gzip limit.

## `.tgw`: the development format

A `.tgw` file is gzip of a widget's HTML (MIME type `application/x-tgwidget`)
as a document. It is **never product delivery**: product widgets come only as
`messageMediaWidget`, and a `.tgw` or `.html` a user uploads as a document is
a file, not a widget. A client supports `.tgw` behind its debug flag, through
the same path as `messageMediaWidget` but from the gunzipped file, for two
uses:

1. **Local tests without a bot**: any widget HTML from disk.
2. **The oversized conformance tests in the shared test channel.** The
   conformance widgets do not fit 4 KB of gzip (the automatic set is
   5.1–8.4 KB, the visual set 5.4–54.6 KB), so the server rejects them with
   `INPUT_MEDIA_WIDGET_TOO_LARGE` and no bot can send them; they stay in the
   channel as `.tgw` files until they are split into pieces of at most 4 KB
   (`docs/changes.md`, known issues).

Everything that fits 4 KB of gzip goes through real layer-233 messages: a test
bot sends it to a tester's private chat and the messages are forwarded into
the channel. That covers the corpus widgets that fit, the client-SDK visual
cases (`packages/conformance/visual/sdk`), forward-copy storage, bot edits and
the size anchors.

## Profile and versions

The HTML is written against widget profile 1 (`docs/profile-v1.md`;
`packages/core/profile.json` is the source). A widget learns the client's
profile at runtime through `TelegramWidget.profile`, `isProfileAtLeast(n)`,
`CSS.supports` and `@supports`, all answered from the same tables. The payload
itself has no version field: the profile is the framework build's, the media
layout is the layer's.
