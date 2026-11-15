import Foundation

/// The sandbox, validator and renderer bundles, in a target of their own so that a client that runs no widget
/// JavaScript itself (an app whose engines and validators are in another process) does not ship them.
public enum WidgetSandboxResourceBundle {
    public static var bundle: Bundle {
        return Bundle.module
    }
}
