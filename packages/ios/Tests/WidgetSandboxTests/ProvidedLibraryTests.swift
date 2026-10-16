import XCTest
@testable import WidgetSandbox

final class ProvidedLibraryTests: XCTestCase {
    static let threeSource = "globalThis.THREE = { REVISION: '160', Scene: function () { this.children = []; } };"

    static let threePatterns = [
        "^app:three$",
        "^https?:\\/\\/cdn\\.jsdelivr\\.net\\/npm\\/three@0\\.1[5-6]\\d(?:\\.\\d+)?\\/build\\/three(?:\\.min)?\\.js$",
        "^https?:\\/\\/unpkg\\.com\\/three@0\\.1[5-6]\\d(?:\\.\\d+)?\\/build\\/three(?:\\.min)?\\.js$",
        "^https?:\\/\\/cdnjs\\.cloudflare\\.com\\/ajax\\/libs\\/three\\.js\\/r?1[5-6]\\d(?:\\.\\d+)?\\/three(?:\\.min)?\\.js$",
    ]

    final class Counter {
        let lock = NSLock()
        var calls = 0
    }

    private func resources(source: @escaping () -> String? = { ProvidedLibraryTests.threeSource }, counter: Counter = Counter()) -> SandboxResources {
        let resources = SandboxResources()
        XCTAssertTrue(resources.provide(ProvidedLibrary(name: "three", file: "three.js", patterns: Self.threePatterns, source: {
            counter.lock.withLock { counter.calls += 1 }
            return source()
        })))
        return resources
    }

    private func run(_ html: String, resources: SandboxResources) -> RecordingDelegate {
        let d = RecordingDelegate()
        let config = SandboxConfig(innerWidth: 400, innerHeight: 300, devicePixelRatio: 2, theme: "light")
        let engine = SandboxEngine(html: html, config: config, resources: resources)
        engine.delegate = d
        engine.start()
        wait(for: [d.loaded], timeout: 20)
        engine.queue.sync {}
        engine.terminate()
        return d
    }

    private func logs(_ d: RecordingDelegate) -> String {
        d.lock.withLock { d.logs.map { $0.1 }.joined(separator: "\n") }
    }

    func testCDNCopiesResolveToTheProvidedCopyOnly() {
        let resources = resources()
        XCTAssertEqual(resources.library(for: "app:three")?.name, "three")
        XCTAssertEqual(resources.library(for: "APP:THREE")?.name, "three")
        XCTAssertEqual(resources.library(for: "  app:three  ")?.name, "three")
        XCTAssertEqual(resources.library(for: "https://cdn.jsdelivr.net/npm/three@0.160.0/build/three.min.js")?.name, "three")
        XCTAssertEqual(resources.library(for: "//cdn.jsdelivr.net/npm/three@0.160.0/build/three.min.js?v=2#x")?.name, "three")
        XCTAssertEqual(resources.library(for: "https://unpkg.com/three@0.158.0/build/three.js")?.name, "three")
        XCTAssertEqual(resources.library(for: "https://cdnjs.cloudflare.com/ajax/libs/three.js/r160/three.min.js")?.name, "three")
        XCTAssertEqual(resources.library(for: "https://cdn.jsdelivr.net/npm/d3@7")?.name, "d3")
        XCTAssertNotNil(resources.library(for: "app:three").flatMap { resources.librarySource($0) })
        XCTAssertNil(SandboxResources().library(for: "app:three"))
    }

    func testSpoofedURLsDoNotResolve() {
        let resources = resources()
        let spoofed = [
            "https://evil.example/three@0.160.0/build/three.min.js",
            "https://cdn.jsdelivr.net.evil.example/npm/three@0.160.0/build/three.min.js",
            "https://cdn.jsdelivr.net@evil.example/npm/three@0.160.0/build/three.min.js",
            "https://evil.example/npm/three.js?https://cdn.jsdelivr.net/npm/three@0.160.0/build/three.min.js",
            "https://evil.example/#https://cdn.jsdelivr.net/npm/three@0.160.0/build/three.min.js",
            "https://cdn.jsdelivr.net/npm/three@0.160.0/build/../../../evil/x.js",
            "https://cdn.jsdelivr.net/npm/three@0.160.0/build/three.min.js/../x.js",
            "https://cdn.jsdelivr.net/npm/three@0.160.0/examples/jsm/controls/OrbitControls.js",
            "https://cdn.jsdelivr.net/npm/three@0.199.0/build/three.min.js",
            "https://cdn.jsdelivr.net/npm/three@0.160.0/build/three.min.js\nhttps://evil.example/x.js",
            "https://cdn.jsdelivr.net/npm/three@0.160.0/build/three.min.jsx",
            "https://cdn\u{2024}jsdelivr\u{2024}net/npm/three@0.160.0/build/three.min.js",
            "https://ｃｄｎ.jsdelivr.net/npm/three@0.160.0/build/three.min.js",
            "app:three/../x", "app:three.js", "app:threes", "xapp:three", "app:three\u{0}", "javascript:THREE", "data:text/javascript,globalThis.THREE=1",
            "https://cdn.jsdelivr.net/npm/three@0.160.0/build/three.min.js?" + String(repeating: "a", count: 3000),
        ]
        for url in spoofed {
            XCTAssertNil(resources.library(for: url), url)
        }
    }

    func testProvidersCannotRegisterUnsafeEntries() {
        let resources = SandboxResources()
        let source: () -> String? = { "1" }
        XCTAssertFalse(resources.provide(ProvidedLibrary(name: "d3", file: "x.js", patterns: ["^app:x$"], source: source)))
        XCTAssertFalse(resources.provide(ProvidedLibrary(name: "x", file: "d3.min.js", patterns: ["^app:x$"], source: source)))
        XCTAssertFalse(resources.provide(ProvidedLibrary(name: "x", file: "../x.js", patterns: ["^app:x$"], source: source)))
        XCTAssertFalse(resources.provide(ProvidedLibrary(name: "x", file: "x.txt", patterns: ["^app:x$"], source: source)))
        XCTAssertFalse(resources.provide(ProvidedLibrary(name: "X Y", file: "x.js", patterns: ["^app:x$"], source: source)))
        XCTAssertFalse(resources.provide(ProvidedLibrary(name: "x", file: "x.js", patterns: [], source: source)))
        // An unbalanced `)` would close the anchoring group: `\A(?:p)|(q)\z` matches any URL that starts with p.
        for pattern in [".*", "^app:x$|.*", "https?://.*", "", "(", "^app:x$|^$", "^(?:https?://[^/]+/)?x\\.js$", String(repeating: "a", count: 2000),
                        "https://cdn\\.example\\.com/lib)|(zzz", "app:x)|(.*", "a)(b", "\\Qapp:x)|(.*"] {
            XCTAssertFalse(resources.provide(ProvidedLibrary(name: "x", file: "x.js", patterns: [pattern], source: source)), pattern)
        }
        XCTAssertNil(resources.library(for: "https://cdn.example.com/lib-evil.js"))
        XCTAssertTrue(resources.provide(ProvidedLibrary(name: "x", file: "x.js", patterns: ["app:x"], source: source)))
        XCTAssertTrue(resources.provide(ProvidedLibrary(name: "z", file: "z.js", patterns: ["app:z|https://cdn\\.example\\.com/z(?:\\.min)?\\.js", "app:\\(z\\)|[)(]z"], source: source)))
        for url in ["app:z", "https://cdn.example.com/z.min.js", "app:(z)", ")z"] { XCTAssertEqual(resources.library(for: url)?.name, "z", url) }
        for url in ["app:zz", "https://cdn.example.com/z.js/x", "xapp:z"] { XCTAssertNil(resources.library(for: url), url) }
        XCTAssertNil(resources.library(for: "app:x/y"))
        XCTAssertNil(resources.library(for: "zapp:x"))
        XCTAssertEqual(resources.library(for: "app:x")?.name, "x")
        XCTAssertFalse(resources.provide(ProvidedLibrary(name: "y", file: "x.js", patterns: ["^app:y$"], source: source)))
    }

    func testOversizedModulesAreNotLoaded() {
        let saved = SandboxResources.maxProvidedSourceBytes
        SandboxResources.maxProvidedSourceBytes = 1 << 16
        defer { SandboxResources.maxProvidedSourceBytes = saved }
        let counter = Counter()
        let big = "globalThis.THREE = { REVISION: '160' };" + String(repeating: " ", count: 1 << 17)
        let resources = resources(source: { big }, counter: counter)
        let lib = resources.library(for: "app:three")!
        XCTAssertNil(resources.librarySource(lib))
        XCTAssertNil(resources.libraryBytecode(lib))
        XCTAssertNil(resources.librarySource(lib))
        XCTAssertEqual(counter.lock.withLock { counter.calls }, 1)
        let d = run("<script src=\"app:three\"></script><script>console.log('three:' + typeof THREE)</script>", resources: resources)
        XCTAssertTrue(logs(d).contains("three:undefined"), logs(d))
        XCTAssertTrue(d.lock.withLock { d.diags.contains { $0.0 == "script-skipped" } })
        XCTAssertEqual(resources.compiledLibraries, 0)
    }

    func testOversizedProvidedBytecodeFallsBackToSource() {
        let saved = SandboxResources.maxProvidedBytecodeBytes
        SandboxResources.maxProvidedBytecodeBytes = 16
        defer { SandboxResources.maxProvidedBytecodeBytes = saved }
        let resources = SandboxResources()
        XCTAssertTrue(resources.provide(ProvidedLibrary(name: "three", file: "three.js", patterns: Self.threePatterns, source: { Self.threeSource }, bytecode: { Data(count: 1 << 20) })))
        let lib = resources.library(for: "app:three")!
        XCTAssertNil(resources.libraryBytecode(lib))
        let d = run("<script src=\"app:three\"></script><script>console.log('three:' + THREE.REVISION)</script>", resources: resources)
        XCTAssertTrue(logs(d).contains("three:160"), logs(d))
    }

    func testProvidedLibraryIsCompiledOnceAndLaterWidgetsLoadItsBytecode() {
        let counter = Counter()
        let resources = resources(counter: counter)
        var details: [String] = []
        for _ in 0 ..< 3 {
            let d = run("<script src=\"app:three\"></script><script>console.log('three:' + THREE.REVISION + ' ' + typeof new THREE.Scene().children)</script>", resources: resources)
            XCTAssertTrue(logs(d).contains("three:160 object"), logs(d))
            details += d.lock.withLock { d.diags.filter { $0.0 == "library" }.map { $0.1 } }
        }
        XCTAssertEqual(resources.compiledLibraries, 1)
        XCTAssertEqual(counter.lock.withLock { counter.calls }, 1)
        XCTAssertEqual(details.count, 3)
        XCTAssertTrue(details.allSatisfy { $0.contains("bytecode") }, "\(details)")
    }

    func testAWidgetSpoofingTheCDNGetsNothing() {
        let resources = resources()
        let d = run("""
        <script src="https://cdn.jsdelivr.net.evil.example/npm/three@0.160.0/build/three.min.js"></script>
        <script>
        const s = document.createElement('script');
        s.src = 'https://evil.example/x.js?https://cdn.jsdelivr.net/npm/three@0.160.0/build/three.min.js';
        document.head.appendChild(s);
        </script>
        <script>console.log('three:' + typeof THREE)</script>
        """, resources: resources)
        XCTAssertTrue(logs(d).contains("three:undefined"), logs(d))
        XCTAssertEqual(d.lock.withLock { d.diags.filter { $0.0 == "script-skipped" }.count }, 2)
        XCTAssertEqual(resources.compiledLibraries, 0)
    }

    func testProvidersRunOutsideTheResourcesLock() {
        let resources = SandboxResources()
        var seen: String?
        XCTAssertTrue(resources.provide(ProvidedLibrary(name: "x", file: "x.js", patterns: ["app:x"], source: {
            seen = resources.library(for: "https://cdn.jsdelivr.net/npm/d3@7")?.name
            return "globalThis.X = 1;"
        })))
        let lib = resources.library(for: "app:x")!
        XCTAssertEqual(resources.librarySource(lib), "globalThis.X = 1;")
        XCTAssertNotNil(resources.libraryBytecode(lib))
        XCTAssertEqual(seen, "d3")
        XCTAssertEqual(resources.compiledLibraries, 1)
    }
}
