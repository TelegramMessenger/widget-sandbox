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
    /** What `onAction` got, in order (the first 1000). */
    val actions: ArrayList<WidgetAction> = ArrayList(),
) {
    fun copy() = WidgetDiagnostics(
        ArrayList(logs), LinkedHashMap(diags.mapValues { it.value.copy() }), ArrayList(bridge), stats.copy(), nodes, loaded, dead, height, lastError,
        rendererReadyMs, firstContentMs, loadedMs, prewarmed, ArrayList(actions),
    )
}

/**
 * The chat's theme for the bubble the widget sits in (docs/sdk-v1.md §2.3, §2.4, §5.1):
 * pass it at creation and call `setTheme` on every change.
 */
data class WidgetTheme(
    /** "light" or "dark"; anything but "dark" is light. */
    val colorScheme: String = "light",
    /** Telegram Mini Apps theme keys (`bg_color`, `text_color`, … [SdkPolicy.THEME_KEYS]) → `#rrggbb`; other keys and values are dropped, missing keys take the scheme's defaults. */
    val params: Map<String, String> = emptyMap(),
    /** The client's own button, for the widget's `<button>`. */
    val button: WidgetButtonTokens = WidgetButtonTokens(),
)

/** The client's button (docs/sdk-v1.md §2.4); every field optional. The renderer takes only values that match their grammar, the template's defaults stand for the rest. */
data class WidgetButtonTokens(
    /** `#rrggbb` */
    val bg: String? = null,
    /** `#rrggbb` */
    val text: String? = null,
    /** A CSS length: `10px`, `0.5em`, `1rem` */
    val radius: String? = null,
    /** A font shorthand with generic families only: `600 15px/1.2 system-ui` */
    val font: String? = null,
    /** One to four CSS lengths: `8px 14px` */
    val padding: String? = null,
    /** A CSS length */
    val height: String? = null,
    /** 0–1, at most three decimals */
    val pressedOpacity: Double? = null,
    /** 0–1, at most three decimals */
    val disabledOpacity: Double? = null,
)

/** Web Storage of the message as the client keeps it (docs/sdk-v1.md §2.8, §5.4): the stored `local` area and the in-memory `session` one. */
data class WidgetStorage(val local: Map<String, String> = emptyMap(), val session: Map<String, String> = emptyMap())

class WidgetHost(
    context: Context,
    html: String,
    /** The chat's theme; `setTheme` changes it. */
    theme: WidgetTheme = WidgetTheme(),
    /** Width of the widget in CSS px (= dp). */
    widthDp: Double = 360.0,
    options: Options = Options(),
    resources: SandboxResources = SandboxResources.get(context),
    /** The renderer; the WebView renderer when null. */
    rendererFactory: RendererFactory? = null,
) : SandboxEngine.Delegate {
    @Deprecated("Since SDK v1: pass a WidgetTheme", ReplaceWith("WidgetHost(context, html, WidgetTheme(theme), widthDp, options, resources, rendererFactory)"))
    constructor(
        context: Context,
        html: String,
        theme: String,
        widthDp: Double = 360.0,
        options: Options = Options(),
        resources: SandboxResources = SandboxResources.get(context),
        rendererFactory: RendererFactory? = null,
    ) : this(context, html, WidgetTheme(theme), widthDp, options, resources, rendererFactory)

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
        /** The client's UI language, BCP 47 (`ru`, `pt-BR`): `navigator.language` and the Intl default; `en-US` when malformed. */
        val locale: String = SdkPolicy.DEFAULT_LOCALE,
        /** Web Storage of this message as the client keeps it (the preload); over the 16 KB quota it is dropped whole (`storage-rejected`). */
        val storage: WidgetStorage = WidgetStorage(),
    )

    val view: View get() = renderer.view
    val diagnostics = WidgetDiagnostics()
    var onChange: ((WidgetDiagnostics) -> Unit)? = null
    /** What the widget asked the client to do, checked (URL rules, one link per tap, the haptic rate limit; docs/sdk-v1.md §5.2, §5.3). */
    var onAction: ((WidgetAction) -> Unit)? = null
    /** The new full content of a Web Storage area ("local" | "session") at the end of each task that changed it, in order (docs/sdk-v1.md §5.4). */
    var onStorageCommit: ((area: String, values: Map<String, String>) -> Unit)? = null
    /** On every primary pointerdown: whether the widget takes horizontal (`x`) and vertical (`y`) drags of this touch (docs/sdk-v1.md §5.5). The WebView renderer already keeps its parent from intercepting when `y`. */
    var onGestureClaim: ((x: Boolean, y: Boolean) -> Unit)? = null
    /** The raw method and arguments JSON of each call `onAction` gets, right after it. */
    @Deprecated("Since SDK v1: onAction")
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
    /** The client's theme, its scheme normalised and its palette checked. */
    private var theme = checkedTheme(theme)
    private val scheme get() = theme.colorScheme
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
        renderer = rendererFactory?.create(context, resources, scheme, widthDp) ?: RendererWebView(context, resources.rendererBundle, scheme)
        val diagSink: (String, String) -> Unit = { k, d -> main.post { if (!destroyed) diag(k, d) } }
        val pooled = if (options.pools) ValidatorPool.take(scheme) else null
        if (pooled != null) {
            pooled.onDiag = diagSink
            validator = pooled
            diagnostics.prewarmed = "validator"
        } else {
            validator = TrustedValidator(scheme, resources, if (options.pools || options.sharedValidatorThread) TrustedValidator.sharedThread else null, diagSink)
        }
        val config = SandboxConfig(
            innerWidth = viewport.first, innerHeight = viewport.second, devicePixelRatio = viewport.third, theme = scheme,
            memoryLimitBytes = options.memoryLimitBytes, cpuLimitMs = options.cpuLimitMs,
            cpuQuotaMs = options.cpuQuotaMs, cpuQuotaWindowMs = options.cpuQuotaWindowMs, cpuKillAfterMs = options.cpuKillAfterMs,
            maxTimers = options.maxTimers, visible = options.visible, policy = options.policy, features = options.features,
            themeParams = this.theme.params, locale = options.locale, storage = options.storage,
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
        // The theme goes first (queued until `ready`, which flushes it before the engine starts).
        toRenderer(themeMessage())
        // The engine starts once the renderer announced itself, so the very first
        // layout queries have something to measure.
        renderer.load()
    }

    /**
     * The host owns the theme: stylesheets are re-sanitized against the scheme
     * (prefers-color-scheme, color-scheme, light-dark()), matchMedia queries are
     * answered from it, the renderer gets the variables, and the sandbox gets the
     * scheme and the palette (`themeChanged` in the widget).
     */
    fun setTheme(theme: WidgetTheme) {
        this.theme = checkedTheme(theme)
        toRenderer(themeMessage())
        toRenderer("{\"type\":\"ops\",\"ops\":${validator.setTheme(scheme)}}")
        engine.theme(scheme, this.theme.params)
    }

    @Deprecated("Since SDK v1: setTheme(WidgetTheme)", ReplaceWith("setTheme(WidgetTheme(theme))"))
    fun setTheme(theme: String) = setTheme(WidgetTheme(theme))

    private fun checkedTheme(t: WidgetTheme) = t.copy(colorScheme = if (t.colorScheme == "dark") "dark" else "light", params = SdkPolicy.checkThemeParams(t.params))

    /** `{type:'theme', theme: WidgetTheme}` with the TS field names (docs/sdk-v1.md §3.4): the renderer core checks the tokens and sets the CSS variables. */
    private fun themeMessage(): String {
        val t = JSONObject().put("colorScheme", scheme).put("params", JSONObject(theme.params))
        val b = theme.button
        val button = JSONObject()
        b.bg?.let { button.put("bg", it) }
        b.text?.let { button.put("text", it) }
        b.radius?.let { button.put("radius", it) }
        b.font?.let { button.put("font", it) }
        b.padding?.let { button.put("padding", it) }
        b.height?.let { button.put("height", it) }
        b.pressedOpacity?.takeIf { it.isFinite() }?.let { button.put("pressedOpacity", it) }
        b.disabledOpacity?.takeIf { it.isFinite() }?.let { button.put("disabledOpacity", it) }
        if (button.length() > 0) t.put("button", button)
        return JSONObject().put("type", "theme").put("theme", t).toString()
    }

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
    override fun onAction(engine: SandboxEngine, action: WidgetAction) {
        main.post {
            if (destroyed) return@post
            if (diagnostics.actions.size < 1000) diagnostics.actions.add(action)
            onAction?.invoke(action)
            changed()
        }
    }
    override fun onBridge(engine: SandboxEngine, method: String, argsJson: String) {
        main.post {
            diagnostics.bridge.add(method to argsJson)
            @Suppress("DEPRECATION")
            onBridge?.invoke(method, argsJson)
            changed()
        }
    }
    override fun onStorageCommit(engine: SandboxEngine, area: String, values: Map<String, String>) {
        main.post { if (!destroyed) onStorageCommit?.invoke(area, values) }
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
            "gesture" -> {
                // `wants` is `y`; a renderer that sends only `wants` (the native prototypes) claims no horizontal drags.
                val wants = msg.optBoolean("wants", false)
                renderer.gestureDecision(wants)
                onGestureClaim?.invoke(msg.optBoolean("x", false), msg.optBoolean("y", wants))
            }
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
            // OS-level change inside the renderer: the host theme is authoritative, just re-notify.
            "media" -> engine.theme(scheme, theme.params)
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
