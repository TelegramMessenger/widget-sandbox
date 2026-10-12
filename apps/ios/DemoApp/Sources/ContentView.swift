import SwiftUI
import WidgetSandbox

struct ContentView: View {
    var body: some View {
        NavigationStack {
            switch DemoAutorun.kind {
            case "attacks": AttackRunnerView(autostart: true)
            case "generated": GeneratedRunnerView(autostart: true)
            case "compat": GeneratedRunnerView(groups: ["compat"], reportName: "compat-report.json", autostart: true)
            case "htmlbubbles": GeneratedRunnerView(groups: Corpus.htmlbubblesGroups, reportName: "htmlbubbles-report.json", autostart: true)
            case "common": GeneratedRunnerView(groups: Corpus.commonGroups, reportName: "common-report.json", autostart: true)
            case "showcase": GeneratedRunnerView(groups: Corpus.showcaseGroups, reportName: "showcase-report.json", autostart: true)
            case "compare": CompareRunnerView(autostart: true)
            #if os(iOS)
            case "feed": ChatFeedView(config: FeedConfig.fromLaunchArguments(), autorun: true, dark: UserDefaults.standard.bool(forKey: "demo.dark"))
            #endif
            #if os(macOS)
            case "pause": PauseProbeView()
            #endif
            default: corpusList
            }
        }
    }

    private var corpusList: some View {
        List {
            #if os(iOS)
            NavigationLink("Chat feed: a widget in every bubble") { ChatFeedSetupView() }
            #endif
            NavigationLink("Run all attacks against the canary") { AttackRunnerView() }
            NavigationLink("Run the generated corpus") { GeneratedRunnerView() }
            NavigationLink("Run the compat corpus") { GeneratedRunnerView(groups: ["compat"], reportName: "compat-report.json") }
            NavigationLink("Run the Telegram demos (htmlbubbles-ported)") { GeneratedRunnerView(groups: Corpus.htmlbubblesGroups, reportName: "htmlbubbles-report.json") }
            NavigationLink("Run the common-profile corpus (renderer comparison)") { GeneratedRunnerView(groups: Corpus.commonGroups, reportName: "common-report.json") }
            NavigationLink("Run the showcase (interactive demos)") { GeneratedRunnerView(groups: Corpus.showcaseGroups, reportName: "showcase-report.json") }
            NavigationLink("Sandbox vs reference: speed comparison (dev tooling)") { CompareRunnerView() }
            NavigationLink("Custom HTML…") { CustomHtmlView() }
            ForEach(Corpus.showcaseGroups, id: \.self) { g in
                Section(g) {
                    ForEach(Corpus.files(in: g), id: \.self) { f in NavigationLink(f) { WidgetScreen(group: g, file: f) } }
                }
            }
            Section("compat") {
                ForEach(Corpus.files(in: "compat"), id: \.self) { f in NavigationLink(f) { WidgetScreen(group: "compat", file: f) } }
            }
            ForEach(Corpus.generatedGroups, id: \.self) { g in
                Section(g) {
                    ForEach(Corpus.files(in: g), id: \.self) { f in NavigationLink(f) { WidgetScreen(group: g, file: f) } }
                }
            }
            Section("attacks") {
                ForEach(Corpus.files(in: "attacks"), id: \.self) { f in NavigationLink(f) { WidgetScreen(group: "attacks", file: f) } }
            }
            ForEach(Corpus.commonGroups, id: \.self) { g in
                Section(g) {
                    ForEach(Corpus.files(in: g), id: \.self) { f in NavigationLink(f) { WidgetScreen(group: g, file: f) } }
                }
            }
            ForEach(Corpus.htmlbubblesGroups, id: \.self) { g in
                Section(g) {
                    ForEach(Corpus.files(in: g), id: \.self) { f in NavigationLink(f) { WidgetScreen(group: g, file: f) } }
                }
            }
        }
        .navigationTitle("Widget sandbox")
    }
}

/// A chat-bubble-sized widget with the diagnostics panel below it.
struct WidgetScreen: View {
    let group: String
    let file: String
    /// Overrides the corpus file (the custom-HTML screen).
    var html: String? = nil
    @State private var model: WidgetModel?
    @State private var error: String?
    @State private var onScreen = true
    @State private var reference: ReferenceModel?
    // The toggles follow the user from widget to widget (and across launches).
    @AppStorage("demo.dark") private var dark = false
    /// Reference mode (dev tooling): the same file rendered natively, without the sandbox. Never for attacks.
    @AppStorage("demo.reference") private var showReference = false
    /// WebView, Blitz or litehtml (dev tooling; the sandbox and validator are the same). See DemoRenderer.
    @AppStorage(DemoRenderer.key) private var rendererRaw = ""
    @Environment(\.scenePhase) private var scenePhase
    private var referenceAllowed: Bool { group != "attacks" }

    var body: some View {
        GeometryReader { viewport in
            ScrollView {
                VStack(alignment: .leading, spacing: 12) {
                    #if os(iOS)
                    // The switches live above the bubble, like the web UI's checkbox row: in the iOS 26
                    // navigation bar they lose their captions and squeeze the title out (two bare knobs).
                    HStack(spacing: 18) {
                        if referenceAllowed { LabeledSwitch("Reference", isOn: $showReference) }
                        LabeledSwitch("Dark", isOn: $dark)
                        Spacer()
                    }
                    HStack {
                        RendererPicker(raw: $rendererRaw)
                        Spacer()
                    }
                    #endif
                    if let model {
                        // Like the web UI: sandbox and reference side by side where the window is wide enough.
                        #if os(macOS)
                        HStack(alignment: .top, spacing: 16) {
                            sandboxColumn(model, viewport: viewport.size).id(ObjectIdentifier(model))
                            if let reference { ReferenceBubble(model: reference).id(ObjectIdentifier(reference)).frame(width: 376) }
                        }
                        #else
                        sandboxColumn(model, viewport: viewport.size).id(ObjectIdentifier(model))
                        if let reference { ReferenceBubble(model: reference).id(ObjectIdentifier(reference)) }
                        #endif
                    } else if let error {
                        Text(error).foregroundStyle(.red)
                    }
                }
                .padding()
            }
            .coordinateSpace(name: "scroll")
        }
        // The demo's Dark switch is the chat's theme, not the system's: the captions and panels follow it too,
        // otherwise they are black on black.
        .environment(\.colorScheme, dark ? .dark : .light)
        .background(dark ? Color.black : DemoColor.screen)
        .navigationTitle(file)
        .inlineTitle()
        .toolbar {
            ToolbarItem(placement: .primaryAction) { Button("Restart") { load() } }
            #if os(macOS)
            // One toolbar item for both switches: as separate items macOS 26 fuses them into one capsule with no gap.
            ToolbarItem(placement: .primaryAction) {
                HStack(spacing: 18) {
                    RendererPicker(raw: $rendererRaw)
                    if referenceAllowed { LabeledSwitch("Reference", isOn: $showReference) }
                    LabeledSwitch("Dark", isOn: $dark)
                }
                .padding(.horizontal, 6)
            }
            #endif
        }
        .onChange(of: showReference) { _ in loadReference() }
        .onChange(of: rendererRaw) { _ in load() }
        .onChange(of: dark) { model?.setTheme($0 ? "dark" : "light"); reference?.setTheme($0 ? "dark" : "light") }
        .onAppear { load() }
        .onDisappear { model = nil; reference = nil }
        // Off-screen pause: the bubble scrolled out of the screen or the app in the background counts as hidden
        // (the view's window attachment — and on macOS the window's occlusion — is tracked by the host itself).
        .onChange(of: scenePhase) { _ in pushVisibility() }
    }

    private func sandboxColumn(_ model: WidgetModel, viewport: CGSize) -> some View {
        VStack(alignment: .leading, spacing: 12) {
            ChatBubble(model: model, edge: Corpus.isEdge(group), viewport: viewport) { onScreen = $0; pushVisibility() }
            DiagnosticsPanel(model: model)
        }
        #if os(macOS)
        .frame(width: 376)
        #endif
    }

    private func pushVisibility() {
        #if os(iOS)
        model?.host.setVisible(onScreen && scenePhase == .active)
        #else
        // A desktop window that merely lost focus is still watched; minimised / covered windows are the host's job.
        model?.host.setVisible(onScreen)
        #endif
    }

    private func load() {
        // Restart = a new WidgetHost from the same HTML (the previous one is destroyed by WidgetModel.deinit).
        model = nil
        do {
            model = try WidgetModel(html: sourceHtml, theme: dark ? "dark" : "light", width: 360, rendererFactory: DemoRenderer.factory)
        } catch {
            self.error = "\(error)"
        }
        loadReference()
    }

    private func loadReference() {
        reference = nil
        guard showReference, referenceAllowed else { return }
        reference = ReferenceModel(html: sourceHtml, theme: dark ? "dark" : "light")
    }

    private var sourceHtml: String { html ?? Corpus.html(group: group, file: file) }
}

/// Paste any HTML and render it like a corpus file (sandbox, and reference if the toggle is on).
struct CustomHtmlView: View {
    @AppStorage("demo.customHtml") private var text = "<p>Hello from a pasted widget</p>\n<script>document.querySelector('p').textContent += ' at ' + new Date().toLocaleTimeString();</script>"
    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("The HTML goes through the same path as a corpus file; the draft is kept between launches.").font(.caption).foregroundStyle(.secondary)
            TextEditor(text: $text).font(.system(size: 12, design: .monospaced)).frame(minHeight: 200)
            HStack {
                NavigationLink("Render") { WidgetScreen(group: "custom", file: "custom.html", html: text) }
                Button("Clear") { text = "" }
            }
        }
        .padding()
        .navigationTitle("Custom HTML")
    }
}

/// The natively rendered twin of ChatBubble (dev tooling): WebKit runs the widget's own scripts here.
struct ReferenceBubble: View {
    @ObservedObject var model: ReferenceModel
    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text("reference — native WebKit, no sandbox, dev tooling only").font(.caption2).foregroundStyle(.secondary).padding(.leading, 8)
            ReferenceView(model: model)
                .frame(width: 360, height: max(model.height, 24))
                .padding(8)
                .background(DemoColor.bubble)
                .clipShape(RoundedRectangle(cornerRadius: 14))
            // Same definition as the sandbox panel's "loaded in": wall clock from creating the web view.
            Text(model.loadedAfterMs.map { String(format: "● loaded in %.0f ms", $0) } ?? "… loading")
                .font(.caption.bold()).foregroundStyle(model.loadedAfterMs == nil ? Color.secondary : Color.green).padding(.leading, 8)
            if let loadMs = model.loadMs {
                Text(String(format: "page's own clock: load event at %.0f ms, DOMContentLoaded at %.0f ms (from navigation start, excludes creating the web view)", loadMs, model.domContentLoadedMs ?? 0))
                    .font(.caption2).foregroundStyle(.secondary).padding(.leading, 8)
            }
            ForEach(Array(model.errors.suffix(5).enumerated()), id: \.offset) { _, e in Text(e).font(.caption2).foregroundStyle(.red).lineLimit(2).padding(.leading, 8) }
        }
    }
}

struct ChatBubble: View {
    @ObservedObject var model: WidgetModel
    /// Edge to edge (corpus/showcase): no padding, the bubble clips the widget at its corner radius.
    var edge = false
    /// Size of the enclosing scroll view (coordinate space "scroll"), to tell whether the bubble is on screen.
    var viewport: CGSize = .zero
    /// Called when the bubble enters or leaves the screen (the owner's side of the off-screen pause).
    var onScreenChange: (Bool) -> Void = { _ in }
    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(DemoRenderer.label).font(.caption2).foregroundStyle(.secondary).padding(.leading, 8)
            WidgetView(host: model.host)
                .frame(width: 360, height: max(model.diagnostics.height, 24))
                .padding(edge ? 0 : 8)
                .background(DemoColor.bubble)
                .clipShape(RoundedRectangle(cornerRadius: edge ? 18 : 14))
                .onScreen(viewport: viewport, onScreenChange)
            // What a product shows under the bubble: the widget stays on screen (frozen) with a one-line reason.
            if let dead = model.diagnostics.dead {
                Text("Widget stopped: \(dead.split(separator: "\n").first.map(String.init) ?? dead)").font(.caption2).foregroundStyle(.red).lineLimit(1).padding(.leading, 8)
            } else if let err = model.diagnostics.lastError {
                Text(err.split(separator: "\n").first.map(String.init) ?? err).font(.caption2).foregroundStyle(.red).lineLimit(1).padding(.leading, 8)
            }
        }
    }
}

/// Reports whether a view's frame intersects the visible part of the enclosing
/// ScrollView (coordinate space "scroll", `viewport` = the scroll view's size);
/// a list of bubbles would use the same thing per cell. Works on both platforms,
/// unlike a comparison with the device screen.
private struct OnScreenKey: PreferenceKey {
    static var defaultValue = true
    static func reduce(value: inout Bool, nextValue: () -> Bool) { value = nextValue() }
}

extension View {
    func onScreen(viewport: CGSize, _ change: @escaping (Bool) -> Void) -> some View {
        background(GeometryReader { g in
            Color.clear.preference(key: OnScreenKey.self, value: viewport == .zero || CGRect(origin: .zero, size: viewport).intersects(g.frame(in: .named("scroll"))))
        })
        .onPreferenceChange(OnScreenKey.self, perform: change)
    }
}

struct DiagnosticsPanel: View {
    @ObservedObject var model: WidgetModel
    var body: some View {
        let d = model.diagnostics
        VStack(alignment: .leading, spacing: 6) {
            Text(statusLine(d))
                .font(.caption.bold()).foregroundStyle(d.dead != nil ? .red : d.loaded ? .green : .secondary)
            Text(String(format: "JS %.0f ms in %d tasks · %.1f MB · %d ops · %d nodes · %d sync queries (%.0f ms, %d cache hits) · missed %d",
                        d.stats.jsMs, d.stats.tasks, Double(d.stats.memoryBytes) / 1048576, d.stats.ops, d.nodes, d.stats.queries, d.stats.queryMs, d.stats.queryCacheHits, d.stats.missedMutations))
                .font(.caption2).foregroundStyle(.secondary)
            ForEach(d.diags.keys.sorted(), id: \.self) { k in
                let e = d.diags[k]!
                Text("\(k) ×\(e.count): \(e.details.suffix(2).joined(separator: " · "))").font(.caption2).lineLimit(2)
                    .foregroundStyle(["url-blocked", "network-blocked", "navigation-blocked", "protocol-violation", "renderer-violation"].contains(k) ? .red : .orange)
            }
            ForEach(Array(d.bridge.suffix(10).enumerated()), id: \.offset) { _, b in Text("bridge: \(b.method)(\(b.argsJson))").font(.caption2) }
            ForEach(Array(d.logs.suffix(30).enumerated()), id: \.offset) { _, l in
                Text("[\(l.level)] \(l.text)").font(.system(size: 10, design: .monospaced)).foregroundStyle(l.level == "error" ? .red : .primary).lineLimit(4)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(8)
        .background(DemoColor.panel)
        .clipShape(RoundedRectangle(cornerRadius: 8))
    }

    /// Wall clock from WidgetHost creation, measured by the host itself (the compare runner's definition):
    /// `loaded` = renderer ready + QuickJS boot + the widget's scripts; the renderer's share is the second number.
    private func statusLine(_ d: WidgetDiagnostics) -> String {
        if let dead = d.dead { return "✖ terminated: \(dead)" }
        if !d.loaded { return "… loading" }
        var line = "● loaded"
        if let ms = d.loadedMs { line += String(format: " in %.0f ms", ms) }
        if let ms = d.rendererReadyMs { line += String(format: " (renderer ready at %.0f ms)", ms) }
        return line
    }
}
