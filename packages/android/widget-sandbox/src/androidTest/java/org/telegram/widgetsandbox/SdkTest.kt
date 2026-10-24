package org.telegram.widgetsandbox

import android.os.Looper
import android.os.SystemClock
import android.view.InputDevice
import android.view.MotionEvent
import android.view.View
import androidx.test.core.app.ActivityScenario
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.util.Collections
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference

/**
 * SDK v1 (docs/sdk-v1.md §2–§4) through the real sandbox bundle in QuickJS: the glue's checks in
 * SandboxEngine (links and user activation, the haptic limit, storage preload and commits, theme
 * params and locale in the boot config), then WidgetHost end to end on the WebView renderer (a real
 * tap → onAction, onGesture and the deprecated onGestureClaim, onStorageCommit, the theme variables on the renderer's root).
 * `bridge-rejected` is covered by SdkPolicyTest: the shim only sends calls it has checked itself.
 */
@RunWith(AndroidJUnit4::class)
class SdkTest {
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private val resources = SandboxResources.get(instrumentation.targetContext)

    private fun run(html: String, configure: (SandboxConfig) -> Unit = {}): Pair<RecordingDelegate, SandboxEngine> {
        val d = RecordingDelegate()
        val config = SandboxConfig(innerWidth = 400.0, innerHeight = 300.0, devicePixelRatio = 2.0, theme = "light", cpuLimitMs = 2000.0)
        configure(config)
        val engine = SandboxEngine(html, config, resources)
        engine.delegate = d
        engine.start()
        assertTrue("engine did not load in time", d.loaded.await(20, TimeUnit.SECONDS))
        engine.thread.sync {}
        assertNull(d.died)
        return d to engine
    }

    /** The node id of the first `<tag>` in the sandbox's ops. */
    private fun idOf(d: RecordingDelegate, tag: String): Int {
        for (batch in d.ops.toList()) {
            val ops = try { JSONArray(batch) } catch (e: Exception) { continue }
            for (i in 0 until ops.length()) {
                val op = ops.optJSONObject(i) ?: continue
                if (op.optString("op") == "el" && op.optString("tag") == tag) return op.getInt("id")
            }
        }
        throw AssertionError("no <$tag> in the ops")
    }

    private fun click(id: Int) = "{\"type\":\"click\",\"targetId\":$id,\"detail\":1}"
    private fun RecordingDelegate.count(kind: String) = diags.count { it.first == kind }
    private fun RecordingDelegate.logged() = logs.filter { it.first == "log" }.map { it.second }

    @Test
    fun aTapOpensOneLinkNormalised() {
        val (d, engine) = run(
            "<button>go</button><script>document.querySelector('button').addEventListener('click', function () {" +
                "TelegramWidget.openLink('HTTPS://Example.com/поиск'); TelegramWidget.openLink('https://example.com/second'); });</script>",
        )
        val button = idOf(d, "button")
        engine.activationEvent(click(button), 1)
        engine.thread.sync {}
        assertEquals(listOf(WidgetAction.OpenLink("https://example.com/%D0%BF%D0%BE%D0%B8%D1%81%D0%BA")), d.actions.toList())
        assertEquals("the second call of the tap: ${d.diags}", 1, d.count("activation-blocked"))
        // A click that is no gesture (untrusted, or the same press again) opens nothing; the next press opens one link.
        engine.event(click(button))
        engine.activationEvent(click(button), 1)
        engine.activationEvent(click(button), 2)
        engine.thread.sync {}
        assertEquals(2, d.actions.size)
        assertEquals(6, d.count("activation-blocked"))
        assertEquals(listOf("openLink", "openLink"), d.bridge.map { it.first })
        assertEquals(0, d.count("bridge-rejected"))
        engine.terminate()
    }

    @Test
    fun hapticsAreLimitedNotGated() {
        val (d, engine) = run("<script>for (var i = 0; i < 7; i++) TelegramWidget.HapticFeedback.impactOccurred('medium');</script>")
        assertEquals(List(5) { WidgetAction.Haptic("impact", "medium") }, d.actions.toList())
        assertEquals("${d.diags}", 1, d.count("haptic-limit"))
        assertEquals(0, d.count("activation-blocked"))
        engine.terminate()

        val (hidden, engine2) = run("<script>TelegramWidget.HapticFeedback.selectionChanged();</script>") { it.visible = false }
        assertEquals(emptyList<WidgetAction>(), hidden.actions.toList())
        assertEquals(1, hidden.count("haptic-limit"))
        engine2.terminate()
    }

    @Test
    fun storagePreloadAndCommits() {
        val html = "<button>b</button><script>" +
            "console.log('seen=' + localStorage.getItem('count') + '/' + sessionStorage.getItem('s') + '/' + localStorage.length);" +
            "localStorage.setItem('count', String(Number(localStorage.getItem('count')) + 1)); localStorage.extra = 7;" +
            "document.querySelector('button').onclick = function () { sessionStorage.setItem('s', 'tapped'); localStorage.setItem('count', localStorage.count); };</script>"
        val (d, engine) = run(html) { it.storage = WidgetStorage(local = mapOf("count" to "41"), session = mapOf("s" to "kept")) }
        assertTrue("${d.logs}", d.logged().contains("seen=41/kept/1"))
        engine.event(click(idOf(d, "button")))
        engine.thread.sync {}
        assertEquals(
            "one commit per task, the changed areas in full; an unchanged value is no change",
            listOf("local" to mapOf("count" to "42", "extra" to "7"), "session" to mapOf("s" to "tapped")),
            d.storage.toList(),
        )
        assertEquals(0, d.count("storage-rejected"))
        engine.terminate()
    }

    @Test
    fun aPreloadOverTheQuotaIsDroppedWhole() {
        val (d, engine) = run("<script>console.log('n=' + (localStorage.length + sessionStorage.length));</script>") {
            it.storage = WidgetStorage(local = mapOf("a" to "x".repeat(20000)), session = mapOf("b" to "1"))
        }
        assertEquals("${d.diags}", 1, d.count("storage-rejected"))
        assertTrue("${d.logs}", d.logged().contains("n=0"))
        engine.terminate()
    }

    @Test
    fun themeParamsAndLocaleReachTheWidget() {
        val html = "<script>TelegramWidget.onEvent('themeChanged', function () { console.log('changed:' + TelegramWidget.colorScheme + ':' + TelegramWidget.themeParams.bg_color + ':' + TelegramWidget.themeParams.text_color); });" +
            "var p = TelegramWidget.themeParams; console.log([p.bg_color, p.text_color, 'made_up' in p, navigator.language, TelegramWidget.profile].join());</script>"
        val (d, engine) = run(html) {
            it.themeParams = mapOf("bg_color" to "#ABCDEF", "text_color" to "red", "made_up" to "#123456")
            it.locale = "pt-BR"
        }
        assertTrue("${d.logs}", d.logged().contains("#abcdef,#000000,false,pt-BR,1"))
        engine.theme("dark", mapOf("bg_color" to "#101010"))
        engine.thread.sync {}
        assertTrue("${d.logs}", d.logged().contains("changed:dark:#101010:#ffffff"))
        assertFalse("${d.diags}", d.diags.any { it.first == "profile-mismatch" })
        engine.terminate()

        val (d2, engine2) = run("<script>console.log('lang=' + navigator.language);</script>") { it.locale = "ru_RU" }
        assertTrue("${d2.logs}", d2.logged().contains("lang=en-US"))
        engine2.terminate()
    }

    /** `setResizeAnchor` (v0.1.3): the shim sends only changes, the glue takes them with no gesture and hidden, never as an action. */
    @Test
    fun resizeAnchorIsNoAction() {
        val html = "<script>TelegramWidget.setResizeAnchor('top'); TelegramWidget.setResizeAnchor('bottom');" +
            "for (var i = 0; i < 1000; i++) TelegramWidget.setResizeAnchor('bottom');" +
            "TelegramWidget.setResizeAnchor('top'); TelegramWidget.setResizeAnchor('bottom');" +
            "[undefined, 'left', 'Bottom', ' top', 1].forEach(function (v) { try { TelegramWidget.setResizeAnchor(v); } catch (e) { console.log(e.name); } });</script>"
        val (d, engine) = run(html) { it.visible = false }
        assertEquals(listOf("bottom", "top", "bottom"), d.resizeAnchors.toList())
        assertEquals(emptyList<WidgetAction>(), d.actions.toList())
        assertEquals(emptyList<Pair<String, String>>(), d.bridge.toList())
        assertEquals(List(5) { "TypeError" }, d.logged())
        assertEquals("${d.diags}", 0, d.count("activation-blocked") + d.count("bridge-rejected"))
        engine.terminate()
    }

    // ── WidgetHost end to end ──

    /** A tap (down, up) at a point of the widget in dp, dispatched into the WebView like a finger. */
    private fun tap(view: View, xDp: Float, yDp: Float) {
        val density = view.resources.displayMetrics.density
        val t0 = SystemClock.uptimeMillis()
        fun send(action: Int, dt: Long) {
            val e = MotionEvent.obtain(t0, t0 + dt, action, xDp * density, yDp * density, 0)
            e.source = InputDevice.SOURCE_TOUCHSCREEN
            instrumentation.runOnMainSync { view.dispatchTouchEvent(e) }
            e.recycle()
        }
        send(MotionEvent.ACTION_DOWN, 0)
        Thread.sleep(60)
        send(MotionEvent.ACTION_UP, 60)
    }

    private fun waitFor(timeoutS: Long = 10, condition: () -> Boolean): Boolean {
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(timeoutS)
        while (System.nanoTime() < deadline) {
            if (condition()) return true
            Thread.sleep(50)
        }
        return condition()
    }

    @Test
    fun hostDeliversActionsClaimsCommitsAndThemeVariables() {
        val html = """
            <style>body { margin: 0 }</style>
            <div id="b" style="height:100px;background:#ccc">open</div>
            <div id="area" style="height:100px;touch-action:none">drag</div>
            <script>
              document.getElementById('b').addEventListener('click', function () {
                TelegramWidget.openLink('https://example.com/a');
                TelegramWidget.openLink('https://example.com/b');
                TelegramWidget.HapticFeedback.selectionChanged();
                localStorage.setItem('taps', String(Number(localStorage.getItem('taps')) + 1));
              });
            </script>
        """.trimIndent()
        val probe = "(function(){ var s = document.documentElement.style; return ['--tg-theme-bg-color', '--tg-theme-text-color', '--tg-color-scheme', '--tg-button-radius', '--tg-button-pressed-opacity', '--tg-text-font-size', '--tg-text-padding'].map(function (n) { return s.getPropertyValue(n).trim(); }).concat([getComputedStyle(document.documentElement).fontSize, getComputedStyle(document.body).marginTop]).join('|'); })()"
        ActivityScenario.launch(TestActivity::class.java).use { scenario ->
            lateinit var host: WidgetHost
            val latest = AtomicReference(WidgetDiagnostics())
            val actions = Collections.synchronizedList(ArrayList<WidgetAction>())
            val claims = Collections.synchronizedList(ArrayList<WidgetGestureClaim>())
            val drags = Collections.synchronizedList(ArrayList<Pair<Boolean, Boolean>>())
            val commits = Collections.synchronizedList(ArrayList<Pair<String, Map<String, String>>>())
            val bridge = Collections.synchronizedList(ArrayList<String>())
            val offMain = Collections.synchronizedList(ArrayList<String>())
            fun onMain(what: String) { if (Looper.myLooper() != Looper.getMainLooper()) offMain.add(what) }
            scenario.onActivity { activity ->
                host = WidgetHost(
                    activity, html, WidgetTheme("light", mapOf("bg_color" to "#ABCDEF", "text_color" to "nope")), 360.0,
                    WidgetHost.Options(storage = WidgetStorage(local = mapOf("taps" to "41"))),
                )
                host.onChange = { latest.set(it) }
                host.onAction = { onMain("action"); actions.add(it) }
                host.onGesture = { onMain("claim"); claims.add(it) }
                @Suppress("DEPRECATION")
                host.onGestureClaim = { x, y -> onMain("claim"); drags.add(x to y) }
                host.onStorageCommit = { area, values -> onMain("storage"); commits.add(area to values) }
                @Suppress("DEPRECATION")
                host.onBridge = { method, _ -> bridge.add(method) }
                activity.show(host.view)
            }
            assertTrue("widget never loaded", waitFor(15) { latest.get().loaded })
            Thread.sleep(300)
            fun eval(): String {
                val done = CountDownLatch(1)
                val out = AtomicReference("")
                instrumentation.runOnMainSync { host.evaluateInRenderer(probe) { out.set(it); done.countDown() } }
                assertTrue(done.await(5, TimeUnit.SECONDS))
                return out.get().trim('"')
            }
            assertEquals("the scheme's defaults and the reference text tokens stand for what the client did not pass; the widget's body margin beats the layer's", "#abcdef|#000000|light|||15px|8px 12px|15px|0px", eval())

            tap(host.view, 50f, 50f)
            assertTrue("actions: $actions, diags: ${latest.get().diags.keys}", waitFor { actions.size >= 2 && commits.isNotEmpty() })
            Thread.sleep(200)
            assertEquals(listOf(WidgetAction.OpenLink("https://example.com/a"), WidgetAction.Haptic("selection", null)), actions.toList())
            assertEquals(actions.toList(), latest.get().actions)
            assertEquals("${latest.get().diags.mapValues { it.value.details }}", 1, latest.get().diags["activation-blocked"]?.count)
            assertEquals(listOf("local" to mapOf("taps" to "42")), commits.toList())
            assertEquals(listOf("openLink", "haptic"), bridge.toList())
            assertEquals("a plain element claims nothing", listOf(WidgetGestureClaim(x = false, y = false)), claims.toList())

            tap(host.view, 50f, 150f)
            assertTrue("$claims", waitFor { claims.size >= 2 && drags.size >= 2 })
            assertEquals("touch-action: none takes horizontal drags and holds, never vertical ones (v0.1.3)", WidgetGestureClaim(x = true, y = false, hold = true), claims[1])
            assertEquals("the deprecated onGestureClaim gets the drags of each claim", listOf(false to false, true to false), drags.toList())

            instrumentation.runOnMainSync { host.setTheme(WidgetTheme("dark", mapOf("bg_color" to "#101010"), WidgetButtonTokens(radius = "10px", pressedOpacity = 0.7), WidgetTextTokens(fontSize = 17.0, padding = "4px 6px"))) }
            val dark = "#101010|#ffffff|dark|10px|0.7|17px|4px 6px|17px|0px"
            assertTrue(waitFor(5) { eval() == dark })
            assertEquals(dark, eval())
            assertEquals("callbacks run on the main thread", emptyList<String>(), offMain.toList())
            assertNull(latest.get().dead)
            instrumentation.runOnMainSync { host.destroy() }
        }
    }

    /** The renderer with a look at each `size` report just before WidgetHost handles it. */
    private class SizeProbe(private val inner: Renderer, private val onSize: (JSONObject) -> Unit) : Renderer by inner {
        override var onMessage: ((String) -> Unit)? = null
            set(value) {
                field = value
                inner.onMessage = { json ->
                    if (json.contains("\"size\"")) runCatching { JSONObject(json) }.getOrNull()?.takeIf { it.optString("type") == "size" }?.let(onSize)
                    value?.invoke(json)
                }
            }
    }

    /**
     * `setResizeAnchor` through WidgetHost on the WebView renderer (v0.1.3): the latest anchor is in the diagnostics next to
     * the height, never in `actions` / `bridge`, and it is there before the size report of the content its task changed
     * (bridge → ops → renderer → size: the engine posts the anchor during the task, the ops at its end).
     */
    @Test
    fun hostKeepsTheResizeAnchorNextToTheHeight() {
        val html = """
            <style>body { margin: 0 }</style>
            <script>TelegramWidget.setResizeAnchor('bottom');</script>
            <div id="box" style="height:100px;background:#ccc">tap</div>
            <script>
              document.getElementById('box').addEventListener('click', function () {
                TelegramWidget.setResizeAnchor('top');
                document.getElementById('box').style.height = '220px';
              });
            </script>
        """.trimIndent()
        ActivityScenario.launch(TestActivity::class.java).use { scenario ->
            lateinit var host: WidgetHost
            lateinit var plain: WidgetHost
            val latest = AtomicReference(WidgetDiagnostics())
            val plainLatest = AtomicReference(WidgetDiagnostics())
            /** Each size report the renderer sent: (its height, the host's anchor when the host got it). */
            val atSize = Collections.synchronizedList(ArrayList<Pair<Double, String>>())
            val actions = Collections.synchronizedList(ArrayList<WidgetAction>())
            scenario.onActivity { activity ->
                val probe = RendererFactory { context, res, theme, _ ->
                    SizeProbe(RendererWebView(context, res.rendererBundle, theme)) { atSize.add(it.optDouble("height") to host.diagnostics.resizeAnchor) }
                }
                host = WidgetHost(activity, html, WidgetTheme(), 360.0, rendererFactory = probe)
                host.onChange = { latest.set(it) }
                host.onAction = { actions.add(it) }
                activity.show(host.view)
            }
            assertTrue("widget never loaded", waitFor(15) { latest.get().loaded && latest.get().height >= 100 })
            Thread.sleep(300)
            assertEquals("bottom", latest.get().resizeAnchor)
            assertEquals("the first size report of the content finds the anchor its task set: $atSize", "bottom", atSize.first { it.first >= 100 }.second)

            tap(host.view, 50f, 50f)
            assertTrue("$atSize", waitFor { latest.get().height >= 220 })
            Thread.sleep(200)
            assertEquals("top", latest.get().resizeAnchor)
            assertEquals("the grown height's size report finds the anchor of the tap that grew it: $atSize", "top", atSize.first { it.first >= 220 }.second)
            assertEquals(emptyList<WidgetAction>(), actions.toList())
            assertEquals(emptyList<WidgetAction>(), latest.get().actions)
            assertEquals(emptyList<Pair<String, String>>(), latest.get().bridge)
            assertNull("${latest.get().diags.keys}", latest.get().diags["bridge-rejected"])

            // A widget that never calls it: top.
            scenario.onActivity { activity ->
                plain = WidgetHost(activity, "<p>plain</p>", WidgetTheme(), 360.0)
                plain.onChange = { plainLatest.set(it) }
                activity.show(plain.view)
            }
            assertTrue("plain widget never loaded", waitFor(15) { plainLatest.get().loaded })
            assertEquals("top", plainLatest.get().resizeAnchor)
            instrumentation.runOnMainSync { host.destroy(); plain.destroy() }
        }
    }
}
