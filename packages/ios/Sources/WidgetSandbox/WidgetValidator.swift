// What WidgetHost and ValidatorPool need from a widget's validator. `TrustedValidator` runs the validator bundle in
// this process; a client that runs no JavaScript itself (Telegram for macOS: Nova lives only in its widget service)
// installs `WidgetValidators.factory` and gets the same answers from a validator in another process. Everything the
// host asks for its renderer goes through one of these; the host never checks ops itself.

import Foundation
import WidgetSandboxProtocol

public protocol WidgetValidator: AnyObject {
    var onDiag: (String, String) -> Void { get set }
    /// Whether ValidatorPool may keep it for another widget after `destroy`.
    var recyclable: Bool { get }
    /// False once it can never answer again (its process ended): pools drop it.
    var isAlive: Bool { get }
    /// Runs `block` after everything queued before it, off the caller's thread; the methods below are called inside it.
    func perform(_ block: @escaping () -> Void)
    func setAssetResolver(_ resolver: WidgetAssetResolver?)
    func setPolicy(_ policy: SandboxPolicy)
    func setNativeScrolling(_ enabled: Bool)
    func useNativeModel(_ on: Bool)
    func setCpuSink(_ sink: ((Double) -> Void)?)
    func warmStylesheets(_ texts: [String])
    func reset(theme: String)
    func setTheme(_ theme: String) -> String
    func processOpsForRenderer(_ json: String, canvas: [Int: WidgetCanvasBatch], typed: Bool) -> TrustedValidator.RenderBatch
    func processBinary(_ payload: BinaryOpsPayload, canvas: [Int: WidgetCanvasBatch], typed: Bool) -> TrustedValidator.RenderBatch
    var nodeCount: Int { get }
    var stopped: String? { get }
    func validateQuery(_ json: String) -> String?
    func rewriteMediaQuery(_ query: String) -> String
    func validateListen(_ json: String) -> String
    func validatePreventDefault(_ json: String) -> String
    func validateAnim(_ json: String) -> String
    func markLoaded()
}

public enum WidgetValidators {
    /// Makes every validator of this process from now on: theme, resources, whether it may share a thread with other
    /// widgets' validators, diagnostics. Nil: `TrustedValidator`, in this process. Main thread, once at start.
    public static var factory: ((_ theme: String, _ resources: SandboxResources, _ shared: Bool, _ onDiag: @escaping (String, String) -> Void) -> WidgetValidator)?

    static func make(theme: String, resources: SandboxResources, shared: Bool, onDiag: @escaping (String, String) -> Void) -> WidgetValidator {
        if let factory {
            return factory(theme, resources, shared, onDiag)
        }
        return TrustedValidator.background(theme: theme, resources: resources, thread: shared ? TrustedValidator.sharedThreadForNextValidator() : nil, onDiag: onDiag)
    }
}

extension TrustedValidator: WidgetValidator {
    public var recyclable: Bool { Self.isShared(thread) }
    public var isAlive: Bool { true }
}
