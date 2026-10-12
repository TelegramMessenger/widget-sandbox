// The renderer as WidgetHost sees it: a view plus the JSON message contract of
// docs/protocol.md (host → renderer: ops / query / listen / preventDefault /
// requestFrame / frozen; renderer → host: ready / event / gesture / result /
// size / resize / frame / media / violation). RendererWebView is the product
// renderer; other implementations (the Blitz prototype in the demo) plug in
// through WidgetHost's `rendererFactory`. All members are main-thread.
// Mirrors Renderer.kt on Android.

import Foundation

public protocol Renderer: AnyObject {
    var view: PlatformView { get }
    /// Renderer → host messages (JSON), delivered on the main thread.
    var onMessage: ((String) -> Void)? { get set }
    var onDiag: ((String, String) -> Void)? { get set }

    /// Starts the renderer; it announces itself with `{type:'ready'}`.
    func load()
    /// Host → renderer message (JSON).
    func send(_ json: String)
    /// The chat theme changed (`prefers-color-scheme` inside the renderer).
    func setTheme(_ theme: String)
    /// Trusted script evaluated in the renderer (tests / diagnostics only); renderers without script answer `nil`.
    func evaluate(_ script: String, completion: @escaping (String?) -> Void)
    /// The last render as an image (a bubble about to drop its runtime keeps it); nil when the renderer cannot.
    func snapshot(completion: @escaping (PlatformImage?) -> Void)
    func destroy()
}

public extension Renderer {
    func snapshot(completion: @escaping (PlatformImage?) -> Void) { completion(nil) }
}

/// Builds a renderer for one widget: the shared resources, the chat theme and the width in CSS px (= points).
public typealias RendererFactory = (_ resources: SandboxResources, _ theme: String, _ width: CGFloat) -> Renderer

extension RendererWebView: Renderer {
    public var view: PlatformView { webView }

    public func evaluate(_ script: String, completion: @escaping (String?) -> Void) {
        webView.evaluateJavaScript(script) { value, _ in completion(value.map { "\($0)" }) }
    }
}
