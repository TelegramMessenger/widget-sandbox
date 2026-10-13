import Foundation
import CoreGraphics

public final class WidgetBubble {
    public let slot = WidgetFeedSlot()
    public let html: String
    public let options: WidgetHost.Options
    public private(set) var width: CGFloat
    public private(set) var theme: String
    public private(set) var host: WidgetHost?
    public private(set) var height: CGFloat = 0
    public private(set) var hasContent = false
    public private(set) var generation = 0
    public private(set) var isAwaitingRender = false
    public private(set) var hostHasContent = false

    public var onHeightChange: (() -> Void)?
    public var onHostChange: (() -> Void)?
    public var onVisibilityChange: ((Bool) -> Void)?
    public var onPauseChange: ((Bool) -> Void)?
    public var onPrepareChange: ((Bool) -> Void)?
    public var onContent: (() -> Void)?
    public var onHostContent: (() -> Void)?
    public var configure: ((WidgetHost) -> Void)?
    /// A host booted off the main thread for this widget (see WidgetPreparer); adopted instead of booting a new one.
    public var preparedHost: (() -> WidgetHost?)?
    public private(set) var adoptedHosts = 0
    private var adopting: WidgetHost?
    public var captureSnapshot: (() -> (snapshot: AnyObject, bytes: Int)?)?
    public var onSnapshotChange: (() -> Void)?
    public private(set) var snapshot: AnyObject?

    private let rendererFactory: RendererFactory?
    private weak var feed: WidgetFeed?

    public init(html: String, theme: String, width: CGFloat, options: WidgetHost.Options = WidgetHost.Options(), feed: WidgetFeed? = .shared, rendererFactory: RendererFactory? = nil) {
        self.html = html
        self.theme = theme
        self.width = width
        self.options = options
        self.rendererFactory = rendererFactory
        self.feed = feed
        slot.create = { [weak self] in
            return self?.makeHost(bootVisible: false) != nil
        }
        slot.drop = { [weak self] in
            self?.dropHost()
        }
        slot.visibilityChanged = { [weak self] visible in
            guard let self = self else {
                return
            }
            if visible {
                self.adoptPending()
            } else {
                self.takeSnapshot()
            }
            self.host?.setVisible(visible)
            self.onVisibilityChange?(visible)
        }
        slot.pauseChanged = { [weak self] paused in
            self?.onPauseChange?(paused)
        }
        slot.prepareChanged = { [weak self] prepared in
            self?.onPrepareChange?(prepared)
        }
        slot.snapshotEvicted = { [weak self] in
            self?.snapshot = nil
            self?.onSnapshotChange?()
        }
        feed?.register(slot)
    }

    public func takeSnapshot() {
        guard hostHasContent, let captured = captureSnapshot?() else {
            return
        }
        snapshot = captured.snapshot
        slot.snapshotStored(bytes: max(1, captured.bytes))
        onSnapshotChange?()
    }

    public func clearSnapshot() {
        guard snapshot != nil else {
            return
        }
        snapshot = nil
        slot.snapshotRemoved()
        onSnapshotChange?()
    }

    public func snapshotShown() {
        slot.snapshotUsed()
    }

    deinit {
        feed?.unregister(slot)
        host?.destroy()
        adopting?.destroy()
        pendingPrepared?.discard()
    }

    public var isVisible: Bool {
        return slot.isVisible
    }

    public var isPrepared: Bool {
        return slot.isPrepared
    }

    public var displayHeight: CGFloat {
        return height
    }

    public func setTheme(_ theme: String) {
        guard theme != self.theme else {
            return
        }
        self.theme = theme
        host?.setTheme(theme)
    }

    public func setWidth(_ width: CGFloat) {
        self.width = width
    }

    @discardableResult
    public func ensureHost() -> WidgetHost? {
        if let host = host {
            return host
        }
        guard makeHost(bootVisible: true) != nil else {
            return nil
        }
        slot.markLive(true)
        adoptPending()
        return host ?? adopting
    }

    private func makeHost(bootVisible: Bool) -> WidgetHost? {
        if let host = host ?? adopting ?? pendingPrepared?.host {
            return host
        }
        if let prepared = preparedHost?() {
            return adopt(prepared, bootVisible: bootVisible)
        }
        if !bootVisible, options.backgroundBoot, let prepared = WidgetPreparer.prepare(html: html, theme: theme, width: width, options: options, tracked: false) {
            backgroundBoots += 1
            pendingPrepared = prepared
            onPendingHost?(prepared.host)
            prepared.onReady = { [weak self, weak prepared] _ in
                guard let self = self, let prepared = prepared, self.pendingPrepared === prepared else {
                    return
                }
                self.adoptPending()
            }
            return prepared.host
        }
        var options = self.options
        options.visible = bootVisible && options.visible
        var rendererFactory = self.rendererFactory
        var executor = WidgetExecutor()
        var shell: WidgetShellPool.Shell?
        if WidgetExecutor.stayOffMain, let made = rendererFactory?(.shared, theme, width) {
            rendererFactory = { _, _, _ in made }
            if made is ScheduledRenderer {
                shell = WidgetShellPool.Shell(container: WidgetShellPool.makeContainer(made, width: width), renderer: made)
                executor = WidgetExecutor(queue: WidgetPreparer.sharedQueue())
            }
        }
        guard let host = try? WidgetHost(html: html, theme: theme, width: width, options: options, rendererFactory: rendererFactory, executor: executor, shell: shell) else {
            return nil
        }
        generation += 1
        isAwaitingRender = hasContent
        hostHasContent = false
        let generation = self.generation
        host.onChange = { [weak self] diagnostics in
            guard let self = self, self.generation == generation else {
                return
            }
            self.changed(diagnostics)
        }
        self.host = host
        configure?(host)
        onHostChange?()
        DispatchQueue.main.asyncAfter(deadline: .now() + bootGrace) { [weak self] in
            self?.settleBootVisibility(generation)
        }
        return host
    }

    public private(set) var backgroundBoots = 0
    /// A host booting on a background queue for this bubble (not adopted yet): owners may configure its renderer through the host's executor.
    public var onPendingHost: ((WidgetHost) -> Void)?
    private var pendingPrepared: WidgetPrepared?

    private func adoptPending() {
        guard let prepared = pendingPrepared else {
            return
        }
        pendingPrepared = nil
        guard prepared.adopt() != nil else {
            return
        }
        _ = adopt(prepared.host, bootVisible: true)
    }

    private func adopt(_ prepared: WidgetHost, bootVisible: Bool) -> WidgetHost {
        generation += 1
        isAwaitingRender = false
        hostHasContent = false
        let generation = self.generation
        adopting = prepared
        prepared.moveToMain { [weak self, weak prepared] in
            guard let self = self, let prepared = prepared, self.generation == generation, self.adopting === prepared else {
                return
            }
            self.adopting = nil
            self.adoptedHosts += 1
            prepared.onChange = { [weak self] diagnostics in
                guard let self = self, self.generation == generation else {
                    return
                }
                self.changed(diagnostics)
            }
            self.host = prepared
            prepared.setVisible(bootVisible && self.options.visible && self.slot.isVisible)
            self.configure?(prepared)
            self.onHostChange?()
            self.changed(prepared.diagnostics)
        }
        return prepared
    }

    public var bootGrace: TimeInterval = 2

    private func settleBootVisibility(_ generation: Int) {
        guard generation == self.generation, let host = host, !slot.isVisible else {
            return
        }
        host.setVisible(false)
    }

    private func dropHost() {
        if let pending = pendingPrepared {
            pendingPrepared = nil
            pending.discard()
        }
        if let adopting = adopting {
            self.adopting = nil
            adopting.destroy()
        }
        guard let host = host else {
            return
        }
        self.host = nil
        host.destroy()
        isAwaitingRender = false
        hostHasContent = false
        onHostChange?()
    }

    private func changed(_ diagnostics: WidgetDiagnostics) {
        if diagnostics.firstContentMs != nil && !hostHasContent {
            hostHasContent = true
            settleBootVisibility(generation)
            onHostContent?()
        }
        if isAwaitingRender {
            guard diagnostics.rendered || diagnostics.dead != nil else {
                return
            }
            isAwaitingRender = false
        }
        let newHeight = diagnostics.height
        if newHeight > 0 && abs(newHeight - height) > 0.5 {
            height = newHeight
            onHeightChange?()
        }
        if diagnostics.firstContentMs != nil && !hasContent {
            hasContent = true
            onContent?()
        }
    }
}
