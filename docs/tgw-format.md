# The `.tgw` widget document

A widget travels inside a Telegram message as a `.tgw` document, the way an
animated sticker travels as `.tgs`.

| | |
|---|---|
| Content | gzip of one UTF-8 HTML document: the whole widget, inline CSS and JS, `data:` images only, no external resources |
| MIME type | `application/x-tgwidget` |
| Producer | the Telegram server only. A bot sends plain HTML (a string or a file) or reuses a widget it uploaded before by file id; the server checks it, packs it and stores it as an ordinary document (file reference, CDN, deduplication by hash). Bots never send a ready `.tgw` or any gzip. |
| Size limit | one limit, **128 KB of HTML**, enforced by the server and kept in its config so it can move without a client update. There is no limit on the gzip size: the bot sees only the HTML. |
| Where it renders | only as the `widget` block of a bot's message in the bot's private chat with the user; forwarding that message anywhere, groups and channels included, keeps the widget working. A `.tgw` or `.html` a user uploads as a document is a file, not a widget. |

## What a client does with it

1. Download the document like any other and `gunzip` it. The result is the
   UTF-8 HTML text. Nothing else is in the container: no header, no manifest,
   no second member.
2. Hand the HTML to `WidgetHost` as the widget (`docs/integration.md`). The
   client keeps no HTML size limit of its own and trusts the server's check;
   the sandbox, the validator and the runtime limits are the security boundary,
   not the size.
3. Keep the bubble geometry from the message's optional `width` and `height`
   (`docs/integration.md`, "Bubble sizing"); the document itself carries no
   size.

A debug flag in each client renders `.tgw` documents posted to the shared test
channel as widgets, so the clients can be developed and compared before the
server side exists (PoC). The flag is off in release builds; without a server
nothing but the `widget` block ever becomes a widget.

## What the server checks

Only that the payload is text HTML in valid UTF-8 without binary data and
within the size limit; then it sets the MIME type and packs. It runs no JS,
sanitises nothing and validates no content: every client validates the widget
itself (`docs/overview.md`, the invariants in `CLAUDE.md`). The sender must be
a bot and the chat its private chat with the user; editing a widget message
repeats the same checks. Details and the test list are in `docs/server-task.md`.

## Profile and versions

The HTML is written against the widget profile (`packages/conformance/catalogue.json`
is the current feature list). A widget learns the client's profile at runtime
through the SDK (`TelegramWidget.profile`, `CSS.supports` and `@supports`
answered from the profile): that API belongs to the profile v1 and SDK v1
milestones and is not in v0.1.0. The container format itself has no version
field; a format change would get a new MIME type.
