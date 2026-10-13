import XCTest
@testable import WidgetSandbox

/// Canvas words (`config.features` = canvasWords): the payloads the shim hands to
/// `__hostCanvas` are spliced back into the ops JSON byte for byte as JSON.stringify
/// would have written the commands, and a hostile payload is dropped, not trusted.
final class CanvasWordsTests: XCTestCase {
    private static let widget = """
        <canvas id="c" width="200" height="100"></canvas><script>
        var c = document.getElementById('c').getContext('2d'), f = 0;
        var g = c.createLinearGradient(0, 0, 100, 0); g.addColorStop(0, 'red'); g.addColorStop(0.5, 'rgba(1,2,3,0.25)');
        var p = new Path2D('M0 0 L10 10'); p.lineTo(20.5, 5e-7);
        function draw() {
          c.clearRect(0, 0, 200, 100); c.save(); c.fillStyle = g; c.fillRect(f * 1.5, -0, 1e21, 0.1 + 0.2);
          c.font = '12px "Helvetica Neue"'; c.fillText('h\\u00e9llo \\ud83d\\ude00 ' + f, 5, 80); c.setLineDash([2, 3.25]); c.lineWidth = 2;
          c.stroke(p); c.arc(50, 50, 10, 0, Math.PI * 2, false); c.drawImage(document.getElementById('c'), 0, 0);
          c.imageSmoothingEnabled = false; c.restore();
          if (++f < 4) requestAnimationFrame(draw);
        }
        draw();
        var img = c.createImageData(2, 2); img.data[0] = 255;
        setTimeout(function () { c.putImageData(img, 0, 0); c.fillRect(0, 0, 1, 1); }, 1);
        </script>
        """

    private var lastPayloads = 0

    private func run(_ html: String, features: Set<String>) -> [String] {
        let d = RecordingDelegate()
        var config = SandboxConfig(innerWidth: 400, innerHeight: 300, devicePixelRatio: 2, theme: "light")
        config.features = features
        let engine = SandboxEngine(html: html, config: config)
        engine.delegate = d
        engine.start()
        wait(for: [d.loaded], timeout: 20)
        for frame in 0..<8 {
            engine.frame(time: Double(frame) * 16)
            engine.queue.sync {}
        }
        RunLoop.current.run(until: Date().addingTimeInterval(0.1))
        engine.queue.sync {}
        lastPayloads = engine.queue.sync { engine.canvasWordPayloads }
        engine.terminate()
        return d.lock.withLock { d.ops }
    }

    func testWordPayloadsSpliceToTheSameJSONAsTheJSONPath() {
        let json = run(Self.widget, features: [])
        XCTAssertEqual(lastPayloads, 0)
        let words = run(Self.widget, features: ["canvasWords"])
        XCTAssertGreaterThanOrEqual(lastPayloads, 4, "the canvas went as words")
        XCTAssertFalse(json.joined().contains("\"bin\":"))
        XCTAssertFalse(words.joined().contains("\"bin\":"), "every placeholder was spliced")
        XCTAssertGreaterThan(json.filter { $0.contains("\"op\":\"canvas\"") }.count, 3)
        XCTAssertEqual(words, json)
    }

    func testHostilePayloadsAreDropped() {
        let attacks: [String] = [
            "__hostCanvas(0, new Float64Array([0, -1, 0, 1, 1, 99, 0]), '')",
            "__hostCanvas(0, new Float64Array([1e300]), 'x')",
            "__hostCanvas(1e9, new Float64Array([0]), '')",
            "__hostCanvas(0, new Float64Array([3, 1]), 'ab')",
            "__hostCanvas(0, [0], '')",
            "__hostCanvas(0, new Int32Array([0]), '')",
        ]
        for attack in attacks {
            let html = "<canvas id='c'></canvas><script>var h = typeof __hostCanvas; console.log('hostCanvas ' + h); try { \(attack) } catch (e) {} document.getElementById('c').getContext('2d').fillRect(0, 0, 1, 1);</script>"
            let ops = run(html, features: ["canvasWords"])
            XCTAssertFalse(ops.joined().contains("\"bin\":"), attack)
        }
    }

    func testBatchDecoderRejectsMalformedWords() {
        func batch(_ words: [Double], _ strings: String) -> Bool {
            words.withUnsafeBufferPointer { CanvasWords.batch(words: $0, strings: strings) != nil }
        }
        XCTAssertTrue(batch([0, 0, 0, 0, 1, 1], "a"))
        XCTAssertFalse(batch([0, 0, 0, 0, 2, 1], "a"), "lengths must cover the joined string")
        XCTAssertFalse(batch([0, 0, 0, 0, -1], ""), "negative count")
        XCTAssertFalse(batch([0, 0, 0, 0, 0.5], ""), "fractional count")
        XCTAssertFalse(batch([], ""))
    }
}
