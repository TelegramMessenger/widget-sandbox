// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "WidgetSandbox",
    platforms: [.iOS(.v16), .macOS(.v10_13)],
    products: [
        .library(name: "WidgetSandbox", targets: ["WidgetSandbox"]),
    ],
    targets: [
        // quickjs-ng v0.17.0 (MIT) vendored unmodified + a tiny shim for Swift.
        .target(
            name: "CQuickJS",
            path: "packages/ios/Sources/CQuickJS",
            exclude: ["quickjs/LICENSE"],
            sources: ["quickjs/quickjs.c", "quickjs/dtoa.c", "quickjs/libregexp.c", "quickjs/libunicode.c", "shim/qjs_shim.c"],
            publicHeadersPath: "include",
            cSettings: [
                .headerSearchPath("include"),
                .headerSearchPath("quickjs"),
                .define("_GNU_SOURCE"),
                .define("QUICKJS_NG_BUILD"),
            ]
        ),
        .target(
            name: "WidgetSandbox",
            dependencies: ["CQuickJS"],
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
