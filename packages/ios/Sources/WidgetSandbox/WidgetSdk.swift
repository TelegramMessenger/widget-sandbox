// The client-facing types of SDK v1 (docs/sdk-v1.md §4.1): what WidgetHost takes
// (the theme, the storage preload, the bot's data and where the widget is) and
// what it reports (actions, storage commits, gesture claims, requests to the
// bot). The checks behind them are SdkPolicy.swift.

import Foundation

/// The client's own `<button>` (docs/sdk-v1.md §2.4); every field optional, the template's defaults stand for the rest.
/// Passed to the renderer as given: the renderer core keeps only values that match each token's grammar
/// (`#rrggbb`, CSS lengths, a `font` shorthand with a generic family, opacities 0–1).
public struct WidgetButtonTokens: Equatable {
    public var bg: String?
    public var text: String?
    public var radius: String?
    public var font: String?
    public var padding: String?
    public var height: String?
    public var pressedOpacity: Double?
    public var disabledOpacity: Double?

    public init(bg: String? = nil, text: String? = nil, radius: String? = nil, font: String? = nil, padding: String? = nil, height: String? = nil, pressedOpacity: Double? = nil, disabledOpacity: Double? = nil) {
        self.bg = bg
        self.text = text
        self.radius = radius
        self.font = font
        self.padding = padding
        self.height = height
        self.pressedOpacity = pressedOpacity
        self.disabledOpacity = disabledOpacity
    }

    /// The fields that are set, with the TS `ButtonTokens` names.
    var json: [String: Any] {
        var out: [String: Any] = [:]
        if let bg { out["bg"] = bg }
        if let text { out["text"] = text }
        if let radius { out["radius"] = radius }
        if let font { out["font"] = font }
        if let padding { out["padding"] = padding }
        if let height { out["height"] = height }
        if let pressedOpacity, pressedOpacity.isFinite { out["pressedOpacity"] = pressedOpacity }
        if let disabledOpacity, disabledOpacity.isFinite { out["disabledOpacity"] = disabledOpacity }
        return out
    }
}

/// The client's message text (docs/sdk-v1.md §2.4): every field optional, the reference set stands for the rest
/// (15 px, 1.35, `8px 12px`). Passed to the renderer as given: the renderer core keeps a font size of 8–64 (CSS px =
/// points), a unitless line height of 1–3 and one to four CSS lengths of padding, and sets them as `--tg-text-*`.
public struct WidgetTextTokens: Equatable {
    /// The chat's message font size in points (follows the client's text-size setting).
    public var fontSize: Double?
    /// Unitless, a multiple of the font size.
    public var lineHeight: Double?
    /// Between the bubble's edge and the widget's content, CSS padding: `8px 12px`.
    public var padding: String?

    public init(fontSize: Double? = nil, lineHeight: Double? = nil, padding: String? = nil) {
        self.fontSize = fontSize
        self.lineHeight = lineHeight
        self.padding = padding
    }

    /// The fields that are set, with the TS `TextTokens` names.
    var json: [String: Any] {
        var out: [String: Any] = [:]
        if let fontSize, fontSize.isFinite { out["fontSize"] = fontSize }
        if let lineHeight, lineHeight.isFinite { out["lineHeight"] = lineHeight }
        if let padding { out["padding"] = padding }
        return out
    }
}

/// The chat's theme for the bubble the widget sits in (docs/sdk-v1.md §2.3, §2.4, §5.1): its own colours (an
/// outgoing bubble's when the user sent or forwarded the widget), the client's button and its message text.
public struct WidgetTheme: Equatable {
    public enum ColorScheme: String {
        case light
        case dark
    }

    public var colorScheme: ColorScheme
    /// `themeParams` keys (`bg_color`, `text_color`, …) → `#rrggbb`. Clients pass every key; unknown keys and
    /// other values are dropped (`SdkPolicy.checkThemeParams`), a missing key takes the scheme's default.
    public var params: [String: String]
    public var button: WidgetButtonTokens
    public var text: WidgetTextTokens

    public init(colorScheme: ColorScheme = .light, params: [String: String] = [:], button: WidgetButtonTokens = WidgetButtonTokens(), text: WidgetTextTokens = WidgetTextTokens()) {
        self.colorScheme = colorScheme
        self.params = params
        self.button = button
        self.text = text
    }

    /// A plain colour scheme as the API before v1 took it: `"dark"` is dark, anything else light.
    public init(colorScheme name: String) {
        self.init(colorScheme: name == "dark" ? .dark : .light)
    }

    /// The palette the glue passes on: the 15 keys with `#rrggbb` values, lowercased.
    public var checkedParams: [String: String] { SdkPolicy.checkThemeParams(params) }

    /// The host → renderer message (docs/sdk-v1.md §3.4), the TS `WidgetTheme` shape; the renderer core builds the CSS variables.
    var rendererMessage: String {
        var theme: [String: Any] = ["colorScheme": colorScheme.rawValue, "params": checkedParams]
        let tokens = button.json
        if !tokens.isEmpty { theme["button"] = tokens }
        let textTokens = text.json
        if !textTokens.isEmpty { theme["text"] = textTokens }
        return SandboxEngine.json(["type": "theme", "theme": theme])
    }
}

/// Web Storage of one message as the client keeps it (docs/sdk-v1.md §2.8, §5.4), passed as the preload.
public struct WidgetStorage: Equatable {
    public var local: [String: String]
    public var session: [String: String]

    public init(local: [String: String] = [:], session: [String: String] = [:]) {
        self.local = local
        self.session = session
    }
}

public enum WidgetStorageArea: String {
    case local
    case session
}

public enum WidgetHaptic: Equatable {
    /// `light`, `medium`, `heavy`, `rigid` or `soft`.
    case impact(String)
    /// `error`, `success` or `warning`.
    case notification(String)
    case selection
}

/// The edge of the bubble that stays put on screen when the widget's height changes (`TelegramWidget.setResizeAnchor`,
/// docs/sdk-v1.md §2.12 and §5.6): `top` (the default: the bubble grows and shrinks downward, the content below moves)
/// or `bottom` (it grows and shrinks upward, its bottom stays, the content above moves). Not an action: the host keeps
/// the widget's latest one next to the height (`WidgetDiagnostics.resizeAnchor`).
public enum WidgetResizeAnchor: String, Equatable {
    case top, bottom
}

/// What the widget asked the client to do, checked: the URL rules, user activation for the links, the haptic rate limit.
public enum WidgetAction: Equatable {
    case ready
    /// An external page: show a confirmation with the full URL, then open it (docs/sdk-v1.md §5.2).
    case openLink(URL)
    /// `https://t.me/…` (or telegram.me, telegram.dog): through the client's own t.me handling.
    case openTelegramLink(URL)
    case haptic(WidgetHaptic)

    /// The client's form of a checked call; nil when Foundation cannot represent the checked URL (never seen: it is ASCII).
    /// `URL(string:)` may percent-encode a few more characters (`[`, `]`, a second `#`) than the check leaves.
    init?(_ checked: SdkPolicy.Action) {
        switch checked {
        case .ready:
            self = .ready
        case .openLink(let s):
            guard let url = URL(string: s) else { return nil }
            self = .openLink(url)
        case .openTelegramLink(let s):
            guard let url = URL(string: s) else { return nil }
            self = .openTelegramLink(url)
        case .haptic(let h):
            switch h.haptic {
            case "impact": self = .haptic(.impact(h.value ?? ""))
            case "notification": self = .haptic(.notification(h.value ?? ""))
            default: self = .haptic(.selection)
            }
        case .setResizeAnchor, .watchPublicData:
            // Not actions: the host keeps them in its state (WidgetDiagnostics.resizeAnchor, .watchesPublicData).
            return nil
        case .request, .copyText:
            // Not actions: a request goes through the request gate to `onRequest`, a text to `onCopyText`.
            return nil
        }
    }
}

// MARK: the bot's data and requests (docs/sdk-v1.md §2.13–§2.15)

/// The bot's two strings for this message (docs/sdk-v1.md §2.13), the full state each time: `chatData` (this message
/// only, never forwarded) and `publicData` (a forward gets it too: a snapshot, or a value shared with the original).
/// nil or empty is none; a field over 1024 bytes (UTF-8) or with a NUL is none for the widget and reported
/// (`data-rejected`).
public struct WidgetData: Equatable {
    public var chatData: String?
    public var publicData: String?

    public init(chatData: String? = nil, publicData: String? = nil) {
        self.chatData = chatData
        self.publicData = publicData
    }

    /// The fields that are set, with the TS `WidgetData` names (`SdkPolicy.checkWidgetData` takes it).
    var json: [String: Any] {
        var out: [String: Any] = [:]
        if let chatData { out["chatData"] = chatData }
        if let publicData { out["publicData"] = publicData }
        return out
    }
}

/// The kind of chat the widget is in (`Chat.type` of the Bot API).
public enum WidgetChatType: String, CaseIterable {
    case `private`, group, supergroup, channel
}

/// Who looks at the widget (the widget sees it as Mini Apps' `WebAppUser`: `id`, `first_name`, `last_name`,
/// `username`, `is_premium`): the account's Telegram user id, the one the bot gets with a request, and, when it has
/// them, its names, username and Premium. An id outside 1…2^53 − 1 is no viewer for the widget; a field that breaks its
/// rule (a name of 1–64 UTF-16 code units, a username of 5–32 characters of A–Z, a–z, 0–9 and _) is left out and
/// reported (`data-rejected`).
public struct WidgetViewer: Equatable {
    public var id: Int64
    public var firstName: String?
    public var lastName: String?
    public var username: String?
    public var isPremium: Bool

    public init(id: Int64, firstName: String? = nil, lastName: String? = nil, username: String? = nil, isPremium: Bool = false) {
        self.id = id
        self.firstName = firstName
        self.lastName = lastName
        self.username = username
        self.isPremium = isPremium
    }

    /// In the widget's field names (`WebAppUser`; `is_premium` only when true): what `SdkPolicy.checkViewer` takes and
    /// what the shim gets at boot.
    var json: [String: Any] {
        var out: [String: Any] = ["id": id]
        if let firstName { out["first_name"] = firstName }
        if let lastName { out["last_name"] = lastName }
        if let username { out["username"] = username }
        if isPremium { out["is_premium"] = true }
        return out
    }
}

/// Where the widget is and what it may do (docs/sdk-v1.md §4.4): the message's flags as the server computes them
/// (`messageMediaWidget`), the chat and the viewer; fixed for the host's life. The framework derives nothing itself:
/// with the defaults (no flags) the widget never sends a request.
public struct WidgetContext: Equatable {
    /// `is_copy`: the message is a forwarded copy: never any `chatData` (the widget reads it as `TelegramWidget.isForward`).
    public var isCopy = false
    /// `can_query`: requests are allowed, on a tap. Without it every request is refused (`NOT_ALLOWED`).
    public var canQuery = false
    /// `background_query`: requests without a tap too (the original in the private chat with the bot): visible only,
    /// one when the widget appears on screen, then one per 5 s.
    public var backgroundQuery = false
    /// The kind of chat the message is in. A copy gets none (null in the widget): the bot learns nothing about where its
    /// widget was forwarded.
    public var chatType: WidgetChatType?
    /// The account that looks at the widget. Only a widget with `canQuery` gets it (null elsewhere, previews included).
    public var viewer: WidgetViewer?
    /// An inline result shown before it is sent (no message yet; docs/sdk-v1.md §2.16): the widget gets its data and
    /// runs as usual, but no input (a tap selects the result), no requests or other bridge calls but `ready` and
    /// `setResizeAnchor`, no chat type or viewer, and Web Storage that is never committed. The flags are ignored.
    public var preview = false

    public init(isCopy: Bool = false, canQuery: Bool = false, backgroundQuery: Bool = false, chatType: WidgetChatType? = nil, viewer: WidgetViewer? = nil, preview: Bool = false) {
        self.isCopy = isCopy
        self.canQuery = canQuery
        self.backgroundQuery = backgroundQuery
        self.chatType = chatType
        self.viewer = viewer
        self.preview = preview
    }

    /// The flags as the glue applies them: a preview has none, whatever the client passed.
    var effective: WidgetContext {
        preview ? WidgetContext(chatType: chatType, viewer: viewer, preview: true) : self
    }
}

/// What the client sends to the bot (`TelegramWidget.request`, checked: docs/sdk-v1.md §2.14): `data` (1–64 bytes,
/// UTF-8), as a callback button's `callback_data`. Neither the server nor the bot can tell a tap from a background
/// request, so the request says nothing about it (the glue's request gate decided).
public struct WidgetRequest: Equatable {
    public let data: String

    public init(data: String) {
        self.data = data
    }
}

/// The bot's answer as the client hands it to the host: the server's `result` (at most 1024 bytes, UTF-8, no NUL; nil:
/// an empty answer). An answer carries no data: the bot edits its data with separate calls, which reach the host as
/// `setData`.
public struct WidgetRequestAnswer: Equatable {
    public var result: String?

    public init(result: String? = nil) {
        self.result = result
    }

    /// The TS `WidgetRequestAnswer` shape, absent for nil (`SdkPolicy.checkRequestAnswer` takes it).
    var json: [String: Any] {
        var out: [String: Any] = [:]
        if let result { out["result"] = result }
        return out
    }
}

/// A request the client could not complete: the bot did not answer in time (`timeout`), a flood wait
/// (`rateLimited`), anything else (`failed`). The widget's promise rejects with `TIMEOUT`, `RATE_LIMITED` or `FAILED`.
public enum WidgetRequestFailure: Error, Equatable {
    case timeout, rateLimited, failed

    var code: SdkPolicy.RequestError {
        switch self {
        case .timeout: return .timeout
        case .rateLimited: return .rateLimited
        case .failed: return .failed
        }
    }
}

/// On every primary pointerdown: what of this press the widget claims with CSS `touch-action` (docs/sdk-v1.md §5.5,
/// GesturePolicy.swift): `none` claims horizontal drags and holds, `pan-y` horizontal drags, `auto` and `pan-x`
/// nothing. Taps and double taps over the widget are always its own, vertical drags always the chat's (v0.1.3).
/// A client's recognizers feed it to a GestureArbiter.
public struct WidgetGestureClaim: Equatable {
    /// Horizontal drags (no swipe-to-reply or back).
    public var x: Bool
    /// Always false since v0.1.3: vertical drags are the chat's (the list scrolls), and the arbiter ignores it.
    /// Kept so that code written for v0.1.2 still builds.
    public var y: Bool
    /// Holds, and a right click with a mouse: no message menu; the widget gets `contextmenu`.
    public var hold: Bool

    public init(x: Bool, y: Bool, hold: Bool = false) {
        self.x = x
        self.y = y
        self.hold = hold
    }
}
