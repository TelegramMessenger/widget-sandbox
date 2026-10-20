// The client-facing types of SDK v1 (docs/sdk-v1.md §4.1): what WidgetHost takes
// (the theme, the storage preload) and what it reports (actions, storage
// commits, gesture claims). The checks behind them are SdkPolicy.swift.

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
        }
    }
}

/// On every primary pointerdown: whether the widget takes horizontal (`x`) and vertical (`y`) drags of this touch (docs/sdk-v1.md §5.5).
public struct WidgetGestureClaim: Equatable {
    public var x: Bool
    public var y: Bool

    public init(x: Bool, y: Bool) {
        self.x = x
        self.y = y
    }
}
