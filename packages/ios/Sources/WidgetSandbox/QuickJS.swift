// What every engine behind `QuickJSContext` (QuickJSContext.swift) returns and throws. The native QuickJS engine
// that used to live here is in the WidgetSandboxQuickJS target (NativeQuickJS.swift), which WidgetSandbox does not link.

import Foundation

public enum QuickJSError: Error, CustomStringConvertible {
    case exception(String)
    case notAFunction(String)
    /// The engine itself cannot run or cannot be trusted any more (the boxed engine is not part of this build, it was
    /// compiled without optimization, a call into it was refused): the widget stops, as on a CPU or memory limit.
    case engineFault(String)

    public var description: String {
        switch self {
        case .exception(let text): return text
        case .notAFunction(let name): return "\(name) is not a function"
        case .engineFault(let reason): return "engine fault: \(reason)"
        }
    }
}

/// Value returned by a host function into the sandbox.
public enum HostReturn {
    case undefined
    case string(String)
    case number(Double)
    case arrayBuffer(Data)
    case null
}

/// Result of calling a sandbox function from the host.
public enum JSResult {
    case undefined
    case string(String)
    case number(Double)

    public var string: String? { if case .string(let s) = self { return s } else { return nil } }
    public var number: Double? { if case .number(let n) = self { return n } else { return nil } }
}
