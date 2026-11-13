// swift-tools-version:5.9
import PackageDescription

let localCheckout = !#filePath.contains("/checkouts/")
let optimizedC: [CSetting] = localCheckout ? [.unsafeFlags(["-O3"])] : []
let optimizesDebug = localCheckout && Context.environment["WIDGET_SANDBOX_DEBUG_UNOPTIMIZED"] == nil
// The QuickJS box fences its memory with guard pages on macOS and with bounds checks everywhere else
// (packages/quickjs-box/src/qjsbox_config.h). WIDGET_SANDBOX_BOX_BOUNDS_CHECKS=1 builds the second mode
// on macOS too, so the tests can run it.
let boxBoundsChecks: [CSetting] = Context.environment["WIDGET_SANDBOX_BOX_BOUNDS_CHECKS"] != nil ? [.define("QJSBOX_BOUNDS_CHECKS", to: "1")] : []

let package = Package(
    name: "WidgetSandbox",
    platforms: [.iOS(.v16), .macOS(.v10_13)],
    products: [
        .library(name: "WidgetSandbox", targets: ["WidgetSandbox"]),
        .library(name: "WidgetSandboxProtocol", targets: ["WidgetSandboxProtocol"]),
        .library(name: "CQuickJS", targets: ["CQuickJS"]),
        .library(name: "CQuickJSBox", targets: ["CQuickJSBox"]),
    ],
    targets: [
        // QuickJS from the TelegramMessenger/quickjs fork (quickjs-ng 0.17 with fixes; a git
        // submodule, MIT) + a tiny shim for Swift. The same sources build the Android library,
        // the desktop core and qjsc; the web/Node wasm is built from it in packages/quickjs-wasm.
        .target(
            name: "CQuickJS",
            path: "packages/ios/Sources/CQuickJS",
            exclude: [
                "quickjs/LICENSE", "quickjs/docs", "quickjs/examples", "quickjs/tests", "quickjs/test262", "quickjs/gen",
                "quickjs/CMakeLists.txt", "quickjs/Makefile", "quickjs/README.md", "quickjs/SECURITY.md", "quickjs/meson.build",
                "quickjs/meson_options.txt", "quickjs/amalgam.js", "quickjs/api-test.c", "quickjs/ctest.c", "quickjs/cxxtest.cc",
                "quickjs/fuzz.c", "quickjs/gc-stress-skip.txt", "quickjs/lre-test.c", "quickjs/qjs-wasi-reactor.c", "quickjs/qjs.c",
                "quickjs/qjs.man", "quickjs/qjsc.c", "quickjs/qjsc.man", "quickjs/quickjs-libc.c", "quickjs/repl.js",
                "quickjs/run-test262.c", "quickjs/standalone.js", "quickjs/test262-fast.conf", "quickjs/test262.conf",
                "quickjs/test262_errors.txt", "quickjs/tests.conf", "quickjs/unicode_download.sh", "quickjs/unicode_gen.c",
                "quickjs/unicode_gen_def.h", "quickjs/update-version.sh", "quickjs/builtin-array-fromasync.js",
                "quickjs/builtin-iterator-zip.js", "quickjs/builtin-iterator-zip-keyed.js",
            ],
            sources: ["quickjs/quickjs.c", "quickjs/dtoa.c", "quickjs/libregexp.c", "quickjs/libunicode.c", "shim/qjs_shim.c"],
            publicHeadersPath: "include",
            cSettings: [
                .headerSearchPath("include"),
                .headerSearchPath("quickjs"),
                .define("_GNU_SOURCE"),
                .define("QUICKJS_NG_BUILD"),
                .define("QJS_DISABLE_DUMPS"),
            ] + optimizedC
        ),
        // The same QuickJS commit inside a wasm2c box: compiled to wasm and translated back to C by
        // packages/quickjs-box/build.sh (generated/, committed), with WABT's runtime (wabt/) and a C host
        // layer over opaque handles (src/, include/). Only src/ is compiled: its files include the
        // generated code and the runtime after qjsbox_config.h. Opt-in (QuickJSBackend.boxed), macOS only
        // for now; it must be compiled optimized (an unoptimized box refuses to start).
        .target(
            name: "CQuickJSBox",
            path: "packages/quickjs-box",
            exclude: ["build.sh", "CLIENTS.md", "manifest.sha256", "guest", "wasm", "generated", "wabt", "tools"],
            sources: ["src"],
            publicHeadersPath: "include",
            cSettings: [
                .headerSearchPath("generated"),
                .headerSearchPath("wabt"),
            ] + boxBoundsChecks + optimizedC
        ),
        // Typed host -> renderer ops for in-process native renderers; no dependencies.
        .target(
            name: "WidgetSandboxProtocol",
            path: "packages/ios/Sources/WidgetSandboxProtocol"
        ),
        .target(
            name: "WidgetSandbox",
            dependencies: ["CQuickJS", "WidgetSandboxProtocol"],
            path: "packages/ios/Sources/WidgetSandbox",
            resources: [.copy("WidgetJS")]
        ),
        .testTarget(
            name: "WidgetSandboxTests",
            dependencies: ["WidgetSandbox"],
            path: "packages/ios/Tests/WidgetSandboxTests",
            resources: [.copy("Fixtures")]
        ),
    ]
)

if optimizesDebug {
    for target in package.targets where target.type == .regular && target.name != "CQuickJS" {
        target.swiftSettings = (target.swiftSettings ?? []) + [.unsafeFlags(["-O"], .when(configuration: .debug))]
    }
}
