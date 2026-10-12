// The sandbox engine: the native counterpart of src/host/worker.ts (and a 1:1
// port of ios/.../SandboxEngine.swift). Owns one QuickJS runtime per widget on
// its own JSThread, exposes exactly one capability to the untrusted code
// (`__host(kind, arg)` + `__hostBinary()`), enforces CPU and memory limits,
// and drives timers / frames / scripts.
//
// Everything that comes out of the context is UNTRUSTED: the delegate must
// pass it through the validator before it reaches the renderer.

package org.telegram.widgetsandbox

import org.json.JSONArray
import org.json.JSONObject
import java.security.SecureRandom

/** Mirrors `SandboxConfig` / `LIMIT_DEFAULTS` in src/shared/protocol.ts. */
data class SandboxConfig(
    var innerWidth: Double,
    var innerHeight: Double,
    var devicePixelRatio: Double,
    var theme: String,
    /** QuickJS heap limit (JS_SetMemoryLimit); exceeding it terminates the widget. */
    var memoryLimitBytes: Long = 64L shl 20,
    /** Longest continuous run of widget JS (one task) before the widget is terminated. */
    var cpuLimitMs: Double = 500.0,
    /** JS time the widget may use per `cpuQuotaWindowMs` (sliding window); beyond it timers and frames are deferred, never killed. */
    var cpuQuotaMs: Double = 300.0,
    var cpuQuotaWindowMs: Double = 1000.0,
    /** Kill after this long of uninterrupted throttling; 0 disables (default). */
    var cpuKillAfterMs: Double = 0.0,
    /** Live timers a widget may hold; the shim throws RangeError beyond it, the engine refuses as a backstop. */
    var maxTimers: Int = 256,
    /** Initial visibility (see `setVisible`): hidden widgets get no frames and timers at 1 Hz. */
    var visible: Boolean = true,
)

data class SandboxStats(
    var jsMs: Double = 0.0,
    var tasks: Int = 0,
    var memoryBytes: Long = 0,
    var ops: Int = 0,
    var queries: Int = 0,
    var queryMs: Double = 0.0,
    var queryCacheHits: Int = 0,
    var missedMutations: Int = 0,
    /** Tasks deferred by the CPU quota. */
    var throttledTasks: Int = 0,
    /** JS time used in the current quota window at the time of the report. */
    var cpuWindowMs: Double = 0.0,
)

class SandboxEngine(
    private val html: String,
    private val config: SandboxConfig,
    private val resources: SandboxResources,
) {
    class QueryAnswer(val json: String, val binary: ByteArray?)

    /** All delegate calls happen on the engine's thread, never on the main thread. `query` is synchronous: it blocks the engine thread (and must not block the main thread). */
    interface Delegate {
        fun onOps(engine: SandboxEngine, json: String)
        fun onQuery(engine: SandboxEngine, json: String): QueryAnswer
        fun onListen(engine: SandboxEngine, json: String)
        fun onPreventDefault(engine: SandboxEngine, json: String)
        fun onRequestFrame(engine: SandboxEngine)
        fun onLog(engine: SandboxEngine, level: String, text: String)
        fun onDiag(engine: SandboxEngine, kind: String, detail: String)
        fun onBridge(engine: SandboxEngine, method: String, argsJson: String)
        fun onLoaded(engine: SandboxEngine)
        fun onStats(engine: SandboxEngine, stats: SandboxStats)
        fun onDied(engine: SandboxEngine, reason: String)
    }

    /** Everything below runs on this thread (see JSThread for why not an executor). */
    val thread = JSThread("widget.sandbox")
    @Volatile var delegate: Delegate? = null

    private var js: QuickJSContext? = null
    private var dead = false
    private val stats = SandboxStats()
    private val timers = HashMap<Int, JSThread.Scheduled>()
    private var lastBinary: ByteArray? = null
    private var lastStatsPost = 0L
    private var lastMemCheck = 0L
    private var timerLimitReported = false
    /** Timer delays are clamped to 24 h on every platform (MAX_TIMER_DELAY_MS in protocol.ts). */
    private val maxTimerDelayMs = 86_400_000.0

    // Visibility and the deferred-task queue (mirrors src/host/worker.ts).
    //
    // Hidden widget (`setVisible(false)`): no frames are requested from the
    // renderer (one is requested on return if the shim asked), timers fire at
    // most once per second, batched into one wake-up, and the shim gets
    // `visibilitychange`. Deferred tasks keep their order; one `fire` per timer
    // id, one frame at most.
    private var visible = config.visible
    private var frameWanted = false
    private class Deferred(val kind: String, var arg: String)
    private val deferred = ArrayList<Deferred>()
    private var wakeTimer: JSThread.Scheduled? = null
    private var wakeAtNs = Long.MAX_VALUE
    /** Next 1 Hz boundary at which hidden timers fire (0 = none scheduled); nanoNow() nanoseconds. */
    private var hiddenTickAtNs = 0L

    // CPU quota (mirrors worker.ts): decide before a task, record after it. Times in ms of nowNs().
    private val budget = CpuBudget(config)
    /** Slice given to the running task in ms (for the kill message); the boot task gets cpuLimitMs. */
    private var currentSliceMs = 0.0
    private var currentKind = "boot"
    /** JS time of the current task that is ours, not the widget's (evaluating the sandbox bundle), ms. */
    private var excludeMs = 0.0
    private var lastThrottleDiagMs = Double.NEGATIVE_INFINITY
    private val throttledSinceDiag = LinkedHashMap<String, Int>()
    private fun nowMs(): Double = nowNs() / 1e6
    /** Inputs that arrived before the runtime existed (the renderer reports its size before `ready`); replayed after boot. */
    private val pendingBeforeBoot = ArrayList<Pair<String, String>>()
    private val random = SecureRandom()

    private fun nowNs(): Long = QuickJSContext.nativeNow()
    private fun msSince(t0: Long): Double = (nowNs() - t0) / 1e6

    // MARK: lifecycle

    fun start() {
        thread.post { boot() }
    }

    /** Kills the widget, frees the runtime and stops the thread. */
    fun terminate() {
        thread.post {
            kill("terminated")
            js?.close()
            js = null
            thread.stop()
        }
    }

    private fun boot() {
        val ctx = QuickJSContext(config.memoryLimitBytes, JSThread.JS_STACK_LIMIT)
        js = ctx
        ctx.registerFunction("__host") { args -> hostCall(args.getOrElse(0) { "" }, args.getOrElse(1) { "" }) }
        ctx.registerFunction("__hostBinary") {
            val data = lastBinary ?: return@registerFunction HostReturn.Null
            lastBinary = null
            HostReturn.ArrayBuffer(data)
        }
        task("boot", config.cpuLimitMs) {
            val t0 = nowNs()
            // Generous one-off budget for our own bundle; its time is ours, not the widget's.
            ctx.deadlineNanos = nowNs() + (maxOf(config.cpuLimitMs, 5000.0) * 1e6).toLong()
            val how = evalBundle(resources.sandboxBundle, resources.sandboxBytecode, "sandbox-bundle.js")
            excludeMs = msSince(t0)
            delegate?.onDiag(this, "boot", "sandbox bundle eval ${excludeMs.toInt()} ms ($how)")
            resetDeadline()
            val bootArg = JSONObject()
                .put("html", html)
                .put(
                    "config",
                    JSONObject()
                        .put("memoryLimitBytes", config.memoryLimitBytes)
                        .put("cpuLimitMs", config.cpuLimitMs)
                        .put("cpuQuotaMs", config.cpuQuotaMs)
                        .put("cpuQuotaWindowMs", config.cpuQuotaWindowMs)
                        .put("cpuKillAfterMs", config.cpuKillAfterMs)
                        .put("maxTimers", config.maxTimers)
                        .put("visible", visible)
                        .put("viewport", JSONObject().put("innerWidth", config.innerWidth).put("innerHeight", config.innerHeight).put("dpr", config.devicePixelRatio))
                        .put("theme", config.theme),
                ).toString()
            callTw("boot", bootArg)
        }
        task("boot", config.cpuLimitMs) { callTw("loaded", "") }
        if (!dead) delegate?.onLoaded(this)
        postStats(force = true)
        val queued = ArrayList(pendingBeforeBoot)
        pendingBeforeBoot.clear()
        for ((kind, arg) in queued) if (!dead) dispatch(kind, arg)
    }

    // MARK: inputs from the host (all asynchronous)

    fun event(json: String) = enqueue("event", json)
    fun frame(time: Double) = enqueue("frame", time.toString())
    fun viewport(innerWidth: Double, innerHeight: Double, dpr: Double) =
        enqueue("viewport", JSONObject().put("innerWidth", innerWidth).put("innerHeight", innerHeight).put("dpr", dpr).toString())
    fun resize(entriesJson: String) = enqueue("resize", entriesJson)
    fun theme(theme: String) = enqueue("theme", JSONObject.quote(theme))
    fun bridgeEvent(event: String, dataJson: String) = enqueue("bridge", "[${JSONObject.quote(event)},$dataJson]")

    /** The host knows whether the widget is on screen; the sandbox does not. */
    fun setVisible(v: Boolean) {
        thread.post {
            if (dead || visible == v) return@post
            visible = v
            if (js == null) return@post // boot reads `visible` for the shim
            dispatch("visibility", "{\"visible\":$v}")
            if (v) {
                if (frameWanted) { frameWanted = false; delegate?.onRequestFrame(this) }
                if (deferred.isNotEmpty()) wake()
            }
        }
    }

    private fun enqueue(kind: String, arg: String) {
        thread.post {
            if (dead) return@post
            if (js == null) {
                pendingBeforeBoot.add(kind to arg)
                return@post
            }
            // A frame that arrives after the widget was hidden is dropped; the shim's callbacks wait for the next visible frame.
            if (kind == "frame" && !visible) { frameWanted = true; return@post }
            dispatch(kind, arg)
        }
    }

    /** Runs one task for an input (event, timer, frame, …) if the CPU quota allows, otherwise defers it (returns false). Events and visibility always run. */
    private fun dispatch(kind: String, arg: String): Boolean {
        if (dead) return true
        val now = nowMs()
        when (val d = budget.decide(kind, now)) {
            is CpuDecision.Defer -> {
                if (budget.shouldKill(now)) {
                    kill(String.format(java.util.Locale.ROOT, "CPU quota: throttled for %.1f s (%d ms per %d ms, cpuKillAfterMs=%d)", budget.throttledFor(now) / 1000, config.cpuQuotaMs.toInt(), config.cpuQuotaWindowMs.toInt(), config.cpuKillAfterMs.toInt()))
                    return true
                }
                defer(kind, arg)
                stats.throttledTasks += 1
                throttledSinceDiag[kind] = (throttledSinceDiag[kind] ?: 0) + 1
                if (now - lastThrottleDiagMs >= 1000) {
                    lastThrottleDiagMs = now
                    val parts = throttledSinceDiag.entries.sortedBy { it.key }.map { "${it.value} ${it.key}${if (it.value == 1) "" else "s"}" }
                    throttledSinceDiag.clear()
                    delegate?.onDiag(this, "cpu-throttled", "deferred ${parts.joinToString(", ")}: ${budget.used(now).toInt()} ms of the ${config.cpuQuotaMs.toInt()} ms per ${config.cpuQuotaWindowMs.toInt()} ms budget used")
                }
                scheduleWake((d.retryAt * 1e6).toLong())
                return false
            }
            is CpuDecision.Run -> {
                task(kind, d.sliceMs) { callTw(kind, arg) }
                return true
            }
        }
    }

    private fun defer(kind: String, arg: String) {
        if (kind == "frame") {
            val f = deferred.firstOrNull { it.kind == "frame" }
            if (f != null) { f.arg = arg; return }
        } else if (kind == "timer" && deferred.any { it.kind == "timer" && it.arg == arg }) {
            return // intervals do not accumulate missed ticks
        }
        deferred.add(Deferred(kind, arg))
    }

    private fun scheduleWake(atNs: Long) {
        if (wakeTimer != null && atNs >= wakeAtNs) return
        wakeTimer?.cancel()
        wakeAtNs = atNs
        wakeTimer = thread.after(maxOf(0L, (atNs - nowNs()) / 1_000_000L)) { wake() }
    }

    /** Runs deferred tasks in order; stops (and re-queues the rest) if one is deferred again. */
    private fun wake() {
        wakeTimer = null
        wakeAtNs = Long.MAX_VALUE
        hiddenTickAtNs = 0L
        val items = ArrayList(deferred)
        deferred.clear()
        for (i in items.indices) {
            if (dead) return
            if (!dispatch(items[i].kind, items[i].arg)) {
                deferred.addAll(0, items.subList(i, items.size))
                return
            }
        }
    }

    private fun onTimerFired(id: Int) {
        timers.remove(id)
        if (!visible) {
            defer("timer", id.toString())
            if (hiddenTickAtNs == 0L) hiddenTickAtNs = nowNs() + 1_000_000_000L
            scheduleWake(hiddenTickAtNs)
            return
        }
        dispatch("timer", id.toString())
    }

    // MARK: the single capability

    private fun hostCall(kind: String, arg: String): HostReturn {
        val delegate = delegate
        when (kind) {
            "ops" -> {
                stats.ops += countOccurrences("\"op\":", arg)
                delegate?.onOps(this, arg)
            }
            "query" -> return HostReturn.Str(syncQuery(arg))
            "now" -> return HostReturn.Num(nowNs() / 1e6)
            "timer" -> {
                val pair = try { JSONArray(arg) } catch (e: Exception) { return HostReturn.Undefined }
                if (pair.length() != 2) return HostReturn.Undefined
                val id = pair.optInt(0, Int.MIN_VALUE)
                if (id == Int.MIN_VALUE) return HostReturn.Undefined
                var delayMs = minOf(maxOf(0.0, pair.optDouble(1, 0.0)), maxTimerDelayMs)
                if (!visible) delayMs = maxOf(delayMs, 1000.0) // hidden: 1 Hz, like a background tab
                if (timers.size >= config.maxTimers) {
                    // Backstop: the shim should have thrown RangeError before asking. Reaching this means a shim bug (or a tampered shim).
                    if (!timerLimitReported) {
                        timerLimitReported = true
                        delegate?.onDiag(this, "timer-limit-enforced", "engine refused a timer beyond maxTimers=${config.maxTimers}; the shim did not stop it")
                    }
                    return HostReturn.Undefined
                }
                timers[id] = thread.after(delayMs.toLong()) {
                    if (dead) return@after
                    onTimerFired(id)
                }
            }
            "clearTimer" -> arg.toIntOrNull()?.let { timers.remove(it)?.cancel() }
            "raf" -> if (!visible) frameWanted = true else delegate?.onRequestFrame(this) // hidden: one frame is requested on return
            "log" -> {
                val (level, text) = splitPrefix(arg)
                delegate?.onLog(this, level, text.take(4000))
            }
            "diag" -> {
                val (k, detail) = splitPrefix(arg)
                delegate?.onDiag(this, k.take(64), detail.take(4000))
            }
            "listen" -> delegate?.onListen(this, arg)
            "pd" -> delegate?.onPreventDefault(this, arg)
            "bridge" -> {
                val pair = try { JSONArray(arg) } catch (e: Exception) { null }
                if (pair != null && pair.length() == 2 && pair.opt(0) is String) {
                    val args = pair.opt(1)
                    val argsJson = if (args == null || args == JSONObject.NULL) "null" else JSONObject.wrap(args)?.toString() ?: "null"
                    delegate?.onBridge(this, pair.getString(0).take(64), argsJson)
                }
            }
            "intl" -> return HostReturn.Str(IntlBridge.format(arg))
            "random" -> {
                val n = (arg.toIntOrNull() ?: 0).coerceIn(0, 65536)
                val bytes = ByteArray(n)
                random.nextBytes(bytes)
                return HostReturn.Str(bytes.joinToString(",", "[", "]") { (it.toInt() and 0xff).toString() })
            }
            "stat" -> {
                try {
                    val s = JSONObject(arg)
                    stats.queryCacheHits = s.optInt("cacheHits", 0)
                    stats.missedMutations = s.optInt("missed", 0)
                } catch (e: Exception) {
                }
            }
        }
        return HostReturn.Undefined
    }

    /** Blocks the engine thread until the host answers (the host must never wait for the engine). */
    private fun syncQuery(json: String): String {
        val delegate = delegate ?: return "{\"error\":\"no host\"}"
        val t0 = nowNs()
        val answer = delegate.onQuery(this, json)
        val dt = nowNs() - t0
        js?.let { it.deadlineNanos = it.deadlineNanos + dt } // time spent waiting is not widget CPU time
        stats.queries += 1
        stats.queryMs += dt / 1e6
        lastBinary = answer.binary
        return answer.json
    }

    // MARK: running code

    private fun kill(reason: String) {
        if (dead) return
        dead = true
        pendingBeforeBoot.clear()
        for (t in timers.values) t.cancel()
        timers.clear()
        deferred.clear()
        wakeTimer?.cancel()
        wakeTimer = null
        delegate?.onDied(this, reason)
    }

    private fun handleError(error: QuickJSException, location: String) {
        val text = error.message ?: error.toString()
        if (js?.interrupted == true) {
            // The slice is cpuLimitMs unless the CPU quota shrank it; say so, the fix differs (infinite loop vs. a widget that was already over budget).
            val quota = if (currentSliceMs < config.cpuLimitMs) "; CPU quota: ${maxOf(0.0, config.cpuQuotaMs - budget.used(nowMs())).toInt()} ms of ${config.cpuQuotaMs.toInt()} ms per ${config.cpuQuotaWindowMs.toInt()} ms left, so this $currentKind task got ${currentSliceMs.toInt()} ms" else ""
            return kill("CPU limit exceeded: more than ${currentSliceMs.toInt()} ms of continuous execution ($location$quota)")
        }
        if (text.lowercase().contains("out of memory")) return kill("Memory limit exceeded (${config.memoryLimitBytes shr 20} MB)")
        delegate?.onLog(this, "error", ("Uncaught $text").take(4000))
    }

    private fun drainJobs() {
        js?.executePendingJobs { handleError(it, "promise job") }
    }

    private fun callTw(kind: String, arg: String): String? {
        if (dead) return null
        val ctx = js ?: return null
        return try {
            ctx.call("__twDispatch", listOf(kind, arg)).string
        } catch (e: QuickJSException) {
            handleError(e, kind)
            null
        }
    }

    private fun settle() {
        for (i in 0 until 20) {
            if (dead) return
            drainJobs()
            if (dead) return
            val more = callTw("endTick", "")
            if (more != "1" && js?.hasPendingJob != true) break
        }
    }

    /**
     * Evaluates one of our own bundles: the precompiled `.qbc` when it is shipped and
     * matches the source, else the source. Returns "bytecode" or "source" for the diag.
     */
    private fun evalBundle(source: String, bytecode: ByteArray?, filename: String): String {
        val ctx = js
        if (ctx != null && bytecode != null) {
            try {
                ctx.evaluateBytecode(bytecode, source)
                return "bytecode"
            } catch (e: QuickJSException) {
                delegate?.onDiag(this, "bytecode", "$filename: falling back to source — ${e.message?.lineSequence()?.firstOrNull()}")
            }
        }
        evalScript(source, filename)
        return "source"
    }

    private fun evalScript(code: String, filename: String, asModule: Boolean = false) {
        val ctx = js ?: return
        try {
            ctx.evaluate(code, filename, asModule)
        } catch (e: QuickJSException) {
            handleError(e, filename)
        }
    }

    private fun pumpScripts() {
        for (i in 0 until 1000) {
            if (dead) return
            val next = callTw("nextScript", "")
            if (next.isNullOrEmpty()) break
            val s = try { JSONObject(next) } catch (e: Exception) { break }
            val id = s.optInt("id", 0)
            var ok = true
            resetDeadline()
            val src = s.optString("src", "")
            if (s.has("src") && src.isNotEmpty()) {
                val lib = resources.library(src)
                val source = lib?.let { resources.librarySource(it) }
                if (lib != null && source != null) {
                    val t0 = nowNs()
                    val how = evalBundle(source, resources.libraryBytecode(lib), lib.file)
                    delegate?.onDiag(this, "library", "$src → bundled ${lib.name} (eval ${msSince(t0).toInt()} ms, $how)")
                } else {
                    ok = false
                    delegate?.onDiag(this, "script-skipped", "external script not available: ${src.take(200)}")
                }
            } else if (s.has("code")) {
                evalScript(s.optString("code"), "inline-script-$id.js", s.optString("type") == "module")
            }
            if (dead) return
            settle()
            callTw("scriptDone", "[$id,$ok]")
            settle()
        }
    }

    private fun resetDeadline() {
        val ctx = js ?: return
        ctx.resetInterrupted()
        ctx.deadlineNanos = nowNs() + (currentSliceMs * 1e6).toLong()
    }

    /** One task of `kind` with `sliceMs` of continuous execution; its JS time is recorded against the quota. */
    private fun task(kind: String, sliceMs: Double, body: () -> Unit) {
        if (dead) return
        currentKind = kind
        currentSliceMs = sliceMs
        excludeMs = 0.0
        val t0 = nowNs()
        val q0 = stats.queryMs
        resetDeadline()
        body()
        if (!dead) settle()
        if (!dead) pumpScripts()
        stats.tasks += 1
        val cpuMs = msSince(t0) - (stats.queryMs - q0) - excludeMs
        stats.jsMs += cpuMs
        budget.record(kind, cpuMs, nowMs())
        stats.cpuWindowMs = budget.used(nowMs())
        val now = nowNs()
        if (now - lastMemCheck > 1_000_000_000L && !dead) {
            lastMemCheck = now
            stats.memoryBytes = js?.memoryUsed ?: 0
        }
        postStats(force = false)
    }

    private var trailingStats: JSThread.Scheduled? = null

    /** At most one stats message per 200 ms, but the last state always gets out. */
    private fun postStats(force: Boolean) {
        val now = nowNs()
        if (force || now - lastStatsPost > 200_000_000L) {
            lastStatsPost = now
            trailingStats?.cancel()
            trailingStats = null
            delegate?.onStats(this, stats.copy())
        } else if (trailingStats == null) {
            trailingStats = thread.after(200) {
                trailingStats = null
                postStats(force = true)
            }
        }
    }

    // MARK: helpers

    private fun splitPrefix(s: String): Pair<String, String> {
        val i = s.indexOf(':')
        return if (i < 0) s to "" else s.substring(0, i) to s.substring(i + 1)
    }

    private fun countOccurrences(needle: String, text: String): Int {
        var count = 0
        var i = text.indexOf(needle)
        while (i >= 0) {
            count++
            i = text.indexOf(needle, i + needle.length)
        }
        return count
    }
}
