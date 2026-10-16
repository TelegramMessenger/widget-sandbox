// WidgetHost: one widget = one SandboxEngine (QuickJS on its own thread) + one
// TrustedValidator + one RendererWebView. The only place where the three meet;
// nothing from the engine reaches the renderer without passing the validator.
// Mirrors src/host/widget-host.ts and ios/.../WidgetHost.swift. All public API
// is main-thread.

package org.telegram.widgetsandbox

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.util.Base64
import android.view.View
import org.json.JSONObject
import java.util.concurrent.Semaphore
import java.util.concurrent.TimeUnit

class DiagEntry(var count: Int = 0, val details: ArrayList<String> = ArrayList()) {
    fun copy() = DiagEntry(count, ArrayList(details))
}

/** Snapshot of what the host knows about a widget; `onChange` delivers copies on the main thread. */
class WidgetDiagnostics(
    val logs: ArrayList<Pair<String, String>> = ArrayList(),
    val diags: LinkedHashMap<String, DiagEntry> = LinkedHashMap(),
    val bridge: ArrayList<Pair<String, String>> = ArrayList(),
    var stats: SandboxStats = SandboxStats(),
    var nodes: Int = 0,
    var loaded: Boolean = false,
    /** Fatal: the runtime is gone. The renderer keeps the last state (frozen); the owner removes the view with `destroy()`. */
    var dead: String? = null,
    /** Document height in CSS px (= dp). */
    var height: Double = 0.0,
    /** Last uncaught widget error (non-fatal), cleared once a later task completes without one. */
    var lastError: String? = null,
    /** Wall clock from WidgetHost creation to the renderer's `ready`, to the first `size` report that follows validated ops (first content on screen) and to `loaded`; same definitions as iOS. */
    var rendererReadyMs: Double? = null,
    var firstContentMs: Double? = null,
    var loadedMs: Double? = null,
    /** Which parts came prewarmed from the pools ("validator"), empty when none. */
    var prewarmed: String = "",
) {
    fun copy() = WidgetDiagnostics(
        ArrayList(logs), LinkedHashMap(diags.mapValues { it.value.copy() }), ArrayList(bridge), stats.copy(), nodes, loaded, dead, height, lastError,
        rendererReadyMs, firstContentMs, loadedMs, prewarmed,
    )
}

class WidgetHost(
    context: Context,
    html: String,
    theme: String = "light",
    /** Width of the widget in CSS px (= dp). */
    widthDp: Double = 360.0,
    options: Options = Options(),
    resources: SandboxResources = SandboxResources.get(context),
    /** The renderer; the WebView renderer when null. */
    rendererFactory: RendererFactory? = null,
) : SandboxEngine.Delegate {
    /** Limits for the widget's QuickJS runtime (see SandboxConfig). */
    data class Options(
        val memoryLimitBytes: Long = 64L shl 20,
        val cpuLimitMs: Double = 500.0,
        val cpuQuotaMs: Double = 300.0,
        val cpuQuotaWindowMs: Double = 1000.0,
        val cpuKillAfterMs: Double = 0.0,
        val maxTimers: Int = 256,
        /** Initial visibility; change it later with `setVisible`. */
        val visible: Boolean = true,
        val minHeightDp: Double = 24.0,
        val maxHeightDp: Double = 900.0,
        /** Badge text shown by the renderer over a dead widget (user-facing, no error details). */
        val stoppedLabel: String = "Widget stopped",
        /** One JSThread for every validator instead of a thread per widget (TrustedValidator.sharedThread). */
        val sharedValidatorThread: Boolean = false,
        /** Take a prewarmed validator from ValidatorPool when it has one, and recycle it on destroy (implies the shared thread). */
        val pools: Boolean = false,
        /** "strict" (default; anything but "compat") or "compat": the gesture gate, 64 KB text caps and the text budgets. */
        val policy: String = "strict",
        /** Opt-in sandbox features, e.g. "webAnimations"; see [SandboxConfig.features]. */
        val features: Set<String> = emptySet(),
    )

    val view: View get() = renderer.view
    val diagnostics = WidgetDiagnostics()
    var onChange: ((WidgetDiagnostics) -> Unit)? = null
    var onBridge: ((String, String) -> Unit)? = null
    /** Non-fatal uncaught exception in widget code (also in `diagnostics.logs`). */
    var onError: ((String) -> Unit)? = null
    private var tasksAtError = 0

    private val main = Handler(Looper.getMainLooper())
    private val engine: SandboxEngine
    private val gestures = GestureCounter()
    private val validator: TrustedValidator
    private val renderer: Renderer
    private val options = options
    private var theme = theme
    private var rendererReady = false
    private val pendingToRenderer = ArrayList<String>()
    @Volatile private var destroyed = false
    private var viewport = Triple(widthDp, 300.0, context.resources.displayMetrics.density.toDouble())
    private var ownerVisible = options.visible
    private var inWindow = false
    private var qid = 0
    private var pendingQuery: PendingQuery? = null
    private val queryLock = Any()
    private val createdAt = System.nanoTime()
    private var opsSent = 0
    private fun sinceCreatedMs() = (System.nanoTime() - createdAt) / 1e6

    init {
        renderer = rendererFactory?.create(context, resources, theme, widthDp) ?: RendererWebView(context, resources.rendererBundle, theme)
        val diagSink: (String, String) -> Unit = { k, d -> main.post { if (!destroyed) diag(k, d) } }
        val pooled = if (options.pools) ValidatorPool.take(theme) else null
        if (pooled != null) {
            pooled.onDiag = diagSink
            validator = pooled
            diagnostics.prewarmed = "validator"
        } else {
            validator = TrustedValidator(theme, resources, if (options.pools || options.sharedValidatorThread) TrustedValidator.sharedThread else null, diagSink)
        }
        val config = SandboxConfig(
            innerWidth = viewport.first, innerHeight = viewport.second, devicePixelRatio = viewport.third, theme = theme,
            memoryLimitBytes = options.memoryLimitBytes, cpuLimitMs = options.cpuLimitMs,
            cpuQuotaMs = options.cpuQuotaMs, cpuQuotaWindowMs = options.cpuQuotaWindowMs, cpuKillAfterMs = options.cpuKillAfterMs,
            maxTimers = options.maxTimers, visible = options.visible, policy = options.policy, features = options.features,
        )
        if (isStrictPolicy(options.policy)) validator.setPolicy("strict")
        engine = SandboxEngine(html, config, resources)
        engine.delegate = this
        renderer.onDiag = { k, d -> diag(k, d) }
        renderer.onMessage = { json -> onRenderer(json) }
        renderer.view.addOnAttachStateChangeListener(object : View.OnAttachStateChangeListener {
            override fun onViewAttachedToWindow(v: View) { inWindow = true; pushVisibility() }
            override fun onViewDetachedFromWindow(v: View) { inWindow = false; pushVisibility() }
        })
        // The engine starts once the renderer announced itself, so the very first
        // layout queries have something to measure.
        renderer.load()
    }

    fun setTheme(theme: String) {
        this.theme = theme
        toRenderer("{\"type\":\"ops\",\"ops\":${validator.setTheme(theme)}}")
        engine.theme(theme)
    }

    fun bridgeEvent(event: String, dataJson: String = "null") = engine.bridgeEvent(event, dataJson)

    /**
     * Off-screen pause. The owner reports whether the bubble is on screen (scrolled away, activity
     * stopped, …); the view's own window attachment is combined with it. Hidden: no animation frames,
     * timers at 1 Hz, `document.hidden` / `visibilitychange` inside the widget.
     */
    fun setVisible(visible: Boolean) {
        ownerVisible = visible
        pushVisibility()
    }

    private fun pushVisibility() = engine.setVisible(ownerVisible && inWindow)

    /** Trusted script evaluated in the renderer (tests and diagnostics only). */
    fun evaluateInRenderer(script: String, callback: (String) -> Unit) = renderer.evaluate(script) { callback(it ?: "null") }

    fun destroy() {
        if (destroyed) return
        destroyed = true
        engine.terminate()
        renderer.destroy()
        if (options.pools) ValidatorPool.recycle(validator) else validator.close()
        resolveQuery("{\"error\":\"dead\"}", null)
    }

    // MARK: engine → host (engine thread)

    override fun onOps(engine: SandboxEngine, json: String) {
        main.post {
            if (destroyed) return@post
            val clean = validator.processOps(json)
            if (clean != "[]") {
                toRenderer("{\"type\":\"ops\",\"ops\":$clean}")
                opsSent += 1
            }
            diagnostics.nodes = validator.nodeCount
        }
    }

    override fun onQuery(engine: SandboxEngine, json: String): SandboxEngine.QueryAnswer {
        val pending = PendingQuery()
        synchronized(queryLock) { pendingQuery = pending }
        main.post {
            if (destroyed) return@post resolveQuery("{\"error\":\"dead\"}", null)
            var clean = validator.validateQuery(json) ?: return@post resolveQuery("{\"error\":\"invalid query\"}", null)
            if (clean.contains("\"matchMedia\"")) {
                try {
                    val obj = JSONObject(clean)
                    if (obj.has("query")) {
                        obj.put("query", validator.rewriteMediaQuery(obj.getString("query")))
                        clean = obj.toString()
                    }
                } catch (e: Exception) {
                }
            }
            qid += 1
            pending.id = qid
            toRenderer("{\"type\":\"query\",\"qid\":$qid,\"query\":$clean}")
        }
        if (!pending.semaphore.tryAcquire(3, TimeUnit.SECONDS)) {
            synchronized(queryLock) { if (pendingQuery === pending) pendingQuery = null }
            return SandboxEngine.QueryAnswer("{\"error\":\"timeout\"}", null)
        }
        return SandboxEngine.QueryAnswer(pending.json, pending.binary)
    }

    private fun resolveQuery(json: String, binary: ByteArray?) {
        val p = synchronized(queryLock) { pendingQuery.also { pendingQuery = null } } ?: return
        p.json = json
        p.binary = binary
        p.semaphore.release()
    }

    override fun onListen(engine: SandboxEngine, json: String) {
        main.post { if (!destroyed) toRenderer("{\"type\":\"listen\",\"types\":${validator.validateListen(json)}}") }
    }
    override fun onPreventDefault(engine: SandboxEngine, json: String) {
        main.post { if (!destroyed) toRenderer("{\"type\":\"preventDefault\",\"keys\":${validator.validatePreventDefault(json)}}") }
    }
    override fun onAnim(engine: SandboxEngine, json: String) {
        main.post {
            if (destroyed) return@post
            val anims = validator.validateAnim(json)
            if (anims != "[]") toRenderer("{\"type\":\"anim\",\"now\":${QuickJSContext.nativeNow() / 1e6},\"anims\":$anims}")
        }
    }
    override fun onRequestFrame(engine: SandboxEngine) {
        main.post { toRenderer("{\"type\":\"requestFrame\"}") }
    }
    override fun onLog(engine: SandboxEngine, level: String, text: String) {
        main.post {
            if (diagnostics.logs.size < 5000) diagnostics.logs.add(level to text)
            if (level == "error" && text.startsWith("Uncaught ")) {
                diagnostics.lastError = text
                tasksAtError = diagnostics.stats.tasks
                onError?.invoke(text)
            }
            changed()
        }
    }
    override fun onDiag(engine: SandboxEngine, kind: String, detail: String) {
        main.post { diag(kind, detail) }
    }
    override fun onBridge(engine: SandboxEngine, method: String, argsJson: String) {
        main.post {
            diagnostics.bridge.add(method to argsJson)
            onBridge?.invoke(method, argsJson)
            changed()
        }
    }
    override fun onLoaded(engine: SandboxEngine) {
        val loadedMs = sinceCreatedMs()
        main.post { diagnostics.loaded = true; diagnostics.loadedMs = loadedMs; changed() }
    }
    override fun onStats(engine: SandboxEngine, stats: SandboxStats) {
        main.post {
            diagnostics.stats = stats
            if (diagnostics.lastError != null && stats.tasks > tasksAtError + 1) diagnostics.lastError = null
            changed()
        }
    }
    override fun onDied(engine: SandboxEngine, reason: String) {
        main.post {
            if (diagnostics.dead != null) return@post
            diagnostics.dead = reason
            changed()
            resolveQuery("{\"error\":\"dead\"}", null)
            // The WebView stays: last render as fallback, badge from the renderer, no more events or frames.
            toRenderer(JSONObject().put("type", "frozen").put("reason", options.stoppedLabel).put("fatal", true).toString())
        }
    }

    // MARK: renderer → host (main thread)

    private fun onRenderer(json: String) {
        if (destroyed) return
        val msg = try { JSONObject(json) } catch (e: Exception) { return }
        when (msg.optString("type")) {
            "ready" -> {
                if (rendererReady) return
                rendererReady = true
                diagnostics.rendererReadyMs = sinceCreatedMs()
                val queued = ArrayList(pendingToRenderer)
                pendingToRenderer.clear()
                queued.forEach(renderer::send)
                engine.start()
            }
            "event" -> (msg.opt("ev") as? JSONObject)?.let { ev ->
                // Only input the renderer saw as real (the DOM event's isTrusted; every input event of a native renderer) is a gesture.
                val gesture = if (msg.opt("trusted") == true) gestures.gestureFor(ev.optString("type"), ev.optDouble("detail", 0.0)) else 0
                if (gesture > 0) engine.activationEvent(ev.toString(), gesture) else engine.event(ev.toString())
            }
            "gesture" -> renderer.gestureDecision(msg.optBoolean("wants", false))
            "result" -> {
                val id = msg.optInt("qid", -1)
                val match = synchronized(queryLock) { pendingQuery?.id == id }
                if (!match) return
                val binary = msg.optString("binaryBase64", "").takeIf { it.isNotEmpty() }?.let { try { Base64.decode(it, Base64.DEFAULT) } catch (e: Exception) { null } }
                val result = msg.opt("result")
                resolveQuery(if (result == null) "null" else JSONObject.wrap(result)?.toString() ?: "null", binary)
            }
            "size" -> {
                val height = msg.optDouble("height", 0.0).coerceIn(options.minHeightDp, options.maxHeightDp)
                if (diagnostics.firstContentMs == null && opsSent > 0) {
                    diagnostics.firstContentMs = sinceCreatedMs()
                    changed()
                }
                if (height != diagnostics.height) {
                    diagnostics.height = height
                    changed()
                }
                val vp = Triple(msg.optDouble("innerWidth", viewport.first), msg.optDouble("innerHeight", viewport.second), msg.optDouble("dpr", viewport.third))
                if (vp != viewport) {
                    viewport = vp
                    engine.viewport(vp.first, vp.second, vp.third)
                }
            }
            "resize" -> msg.optJSONArray("entries")?.let { engine.resize(it.toString()) }
            "frame" -> engine.frame(msg.optDouble("time", 0.0))
            "media" -> engine.theme(theme)
            "violation" -> diag("renderer-violation", msg.optString("detail"))
        }
    }

    private fun toRenderer(json: String) {
        if (destroyed) return
        if (rendererReady) renderer.send(json) else pendingToRenderer.add(json)
    }

    private fun diag(kind: String, detail: String) {
        val entry = diagnostics.diags.getOrPut(kind) { DiagEntry() }
        entry.count += 1
        if (entry.details.size < 50) entry.details.add(detail)
        changed()
    }

    private var changeScheduled = false
    private fun changed() {
        // Coalesce UI updates: a misbehaving widget can emit thousands of diagnostics per second.
        if (changeScheduled) return
        changeScheduled = true
        main.post {
            changeScheduled = false
            if (!destroyed) onChange?.invoke(diagnostics.copy())
        }
    }

    private class PendingQuery {
        val semaphore = Semaphore(0)
        var id = 0
        var json = "null"
        var binary: ByteArray? = null
    }
}
