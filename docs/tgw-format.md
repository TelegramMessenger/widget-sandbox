# The widget payload: mini-HTML

A **widget is mini-HTML**: one self-contained HTML document, inline CSS and
JS, `data:` images only, no external resources, of at most **4 KB after gzip**
and **32 KB uncompressed**, delivered right in the message (decided
2026-10-17). The bot sends plain HTML; the server gzips it and enforces both
limits. That is the whole format: there is no container, header or manifest
around the HTML.

| | |
|---|---|
| Content | gzip of one UTF-8 HTML document |
| Size | at most 4 KB compressed and 32 KB uncompressed, both checked by the server |
| Delivery | in the message itself: no document, no download, no file id. An edit brings new bytes with the edited message |
| Producer | the Telegram server only; bots never send gzip |
| Limits on the client | none. The limits live in the server's config and may move without a client update; the sandbox, the validator and the runtime limits are the security boundary, not the size |

How exactly the bytes sit in the message (a media type, a block of a rich
message, something else), who may send widgets and where, what forwarding
does, and what clients without widget support show are **under revision**
(`docs/server-task.md` lists the open points). Nothing in the framework
depends on these answers: a client gets bytes and two optional numbers,
`width` and `height`, and the rest is the sandbox.

## What a client does with it

1. Take the bytes from the message and `gunzip` them. The result is the UTF-8
   HTML text.
2. Hand the HTML to `WidgetHost` as the widget (`docs/integration.md`). There
   is no download and no loading state for a file; the only thing to show
   before the first frame is the bubble at the size the bot asked for
   (`docs/integration.md`, "Bubble sizing").
3. On an edit, the message carries new bytes: destroy the old host and create
   a new one with the new HTML, as for any new widget.

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
compresses.

## `.tgw`: the test-channel format

A `.tgw` file is gzip of a widget's HTML (MIME type `application/x-tgwidget`)
posted as a document. It is the **test-channel and development format**: a
debug flag in each client renders `.tgw` documents posted to the shared test
channel as widgets (off in release builds), so the clients can be developed
and compared before the server side exists. The conformance set
(`packages/conformance`) is posted there the same way; none of its widgets
fits the mini-widget limits yet (`docs/changes.md`, v0.1.0 known issues), and
the debug path applies no size limit. Whether `.tgw` ever becomes a product
format for larger widgets is not decided; nothing should be built on that
assumption. A `.tgw` or `.html` a user uploads as a document is a file, not a
widget.

## Profile and versions

The HTML is written against widget profile 1 (`docs/profile-v1.md`;
`packages/core/profile.json` is the source). A widget learns the client's
profile at runtime through `TelegramWidget.profile`, `isProfileAtLeast(n)`,
`CSS.supports` and `@supports`, all answered from the same tables. The payload
itself has no version field.
