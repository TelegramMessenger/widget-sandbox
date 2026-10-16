# The widget payload: mini-widgets, and `.tgw` later

v1 starts with **mini-widgets** (decided 2026-10-17): the widget travels
inside the message itself, as a small gzip of its HTML, and renders as soon as
the message arrives. Big widgets carried as `.tgw` documents are deferred.

## The inline payload

| | |
|---|---|
| Content | gzip of one UTF-8 HTML document: the whole widget, inline CSS and JS, `data:` images only, no external resources |
| Size | at most **4 KB compressed** and **32 KB uncompressed** |
| Where | inline in the `widget` block of the rich message: no document, no download, no file id. Edits bring new bytes in the edited message; forwarding copies the bytes with the message |
| Producer | the Telegram server only. A bot sends plain HTML; the server checks it, gzips it and enforces both limits. Bots never send gzip |
| Who may send | only bots, only in their private chat with the user (forwarding is allowed anywhere, groups and channels included) |

Both limits live in the server's config, so they can move without a client
update: a client keeps **no limits of its own**. Decompressing a payload the
server produced always yields HTML under the uncompressed limit, and the
sandbox, the validator and the runtime limits are the security boundary, not
the size.

## What a client does with it

1. Take the bytes from the block and `gunzip` them. The result is the UTF-8
   HTML text; the container holds nothing else: no header, no manifest, no
   second member.
2. Hand the HTML to `WidgetHost` as the widget (`docs/integration.md`). There
   is no download and no loading state for a file; the only thing to show
   before the first frame is the bubble at the size the bot asked for
   (`docs/integration.md`, "Bubble sizing").
3. On an edit, the block carries new bytes: destroy the old host and create a
   new one with the new HTML, as for any new widget.

## Writing a mini-widget

4 KB of gzip is about 12–20 KB of ordinary HTML, CSS and JS: a card with a
few controls, a chart drawn on a canvas, an animation. What does not fit:
embedded images beyond tiny icons (a `data:` PNG compresses badly), bundled
libraries, long text. The SDK (SDK v1 milestone) and the client-styled
`<button>` (profile v1) exist partly to keep widgets small: the client provides
what every widget would otherwise embed.

Widget authors see one number, the uncompressed limit; the compressed one is
the server's to check, because the author cannot know how the server's gzip
compresses.

## `.tgw` documents (deferred)

A `.tgw` document is gzip of a UTF-8 HTML document with MIME type
`application/x-tgwidget`, stored and served like a sticker file (`.tgs` is gzip
of Lottie JSON), made by the server from the bot's plain HTML or reused by file
id, with one limit of 128 KB of HTML on the server. It is the path for big
widgets and comes **after** mini-widgets; its server work is described in
`docs/server-task.md`, which predates the mini-widget decision and still
describes this model.

Until then `.tgw` exists only as the **test-channel format**: a debug flag in
each client renders `.tgw` documents posted to the shared test channel as
widgets (off in release builds), so the clients can be developed and compared
with widgets of any size before the server side exists. The conformance set
(`packages/conformance`) is posted there the same way; none of its widgets fits
the mini-widget limits yet (`docs/changes.md`, v0.1.0 known issues). A `.tgw`
or `.html` a user uploads as a document is a file, not a widget.

## Profile and versions

The HTML is written against the widget profile (`packages/conformance/catalogue.json`
is the current feature list). A widget learns the client's profile at runtime
through the SDK (`TelegramWidget.profile`, `CSS.supports` and `@supports`
answered from the profile): that API belongs to the profile v1 and SDK v1
milestones and is not in v0.1.0. The payload itself has no version field; a
format change would be a new block field or a new MIME type.
