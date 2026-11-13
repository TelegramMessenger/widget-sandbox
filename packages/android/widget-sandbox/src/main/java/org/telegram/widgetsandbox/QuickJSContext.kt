// One QuickJS runtime and context behind one interface, whichever build of the
// engine runs it: the native one (NativeQuickJS, through quickjs_jni.c → qjs_shim,
// the default everywhere) or the same engine commit inside a wasm2c box
// (BoxedQuickJS, through quickjs_box_jni.c → CQuickJSBox; docs/decisions.md,
// "QuickJS in a wasm2c box"). The twin of ios/.../QuickJSContext.swift.
//
// All methods must be called from the thread that owns the context (a JSThread
// in the product). On Android the box always runs in bounds-check mode: no guard
// pages (the 8 GiB reservation is not viable on a 64-bit Android process and
// would clash with ART's own SIGSEGV use).

package org.telegram.widgetsandbox

import android.util.Log
import androidx.annotation.Keep

class QuickJSException(message: String) : RuntimeException(message)

/** Value returned by a host function into the sandbox. */
sealed class HostReturn {
    object Undefined : HostReturn()
    @Keep object Null : HostReturn()
    class Str(val value: String) : HostReturn()
    class Num(val value: Double) : HostReturn()
    /** Read by JNI through the `bytes` field. */
    @Keep class ArrayBuffer(@JvmField val bytes: ByteArray) : HostReturn()
}

/** Result of calling a sandbox function from the host. */
sealed class JSResult {
    object Undefined : JSResult()
    class Str(val value: String) : JSResult()
    class Num(val value: Double) : JSResult()

    val string: String? get() = (this as? Str)?.value
    val number: Double? get() = (this as? Num)?.value
}

/** Which build of QuickJS runs a widget's code. */
enum class QuickJSBackend(val id: String) {
    /** The engine compiled into the process (`libtwquickjs`). The default on every platform. */
    NATIVE("native"),

    /**
     * The same engine commit compiled to wasm and back to C (`libtwquickjsbox`), in a linear memory of its own: a
     * memory bug in the engine reads and writes that memory only, or ends the process. Opt-in; on Android always the
     * bounds-check build (JS about 1.7 to 1.9 times slower). Where a build cannot run it, a boxed context fails closed:
     * the widget stops with "Engine fault", it never falls back to the native engine. The validator's engine stays native.
     */
    BOXED("boxed");

    companion object {
        fun fromId(id: String?): QuickJSBackend? = entries.firstOrNull { it.id == id }

        /**
         * Whether this build can run [BOXED]. False when `libtwquickjsbox` cannot be loaded or was compiled without
         * optimization (an unoptimized box is 11-48 times slower than the native engine and refuses to start).
         */
        val isBoxedAvailable: Boolean get() = BoxedQuickJS.isAvailable

        /**
         * The backend of hosts and engines that do not name one: [NATIVE]. A client that wants every widget boxed sets
         * it once at start, before the first host. The `WIDGET_SANDBOX_ENGINE=boxed` process-environment override (for the
         * test matrix, the twin of the Swift DEBUG override) is read once, and otherwise nothing changes.
         */
        @JvmStatic
        var default: QuickJSBackend = fromId(System.getenv("WIDGET_SANDBOX_ENGINE")) ?: NATIVE
    }
}

/**
 * What [QuickJSContext] needs from an engine. Internal: the rest of the library sees [QuickJSContext] only. The clocks
 * ([QuickJSContext.nativeNow], [QuickJSContext.threadCpuNanos]) are not here — they are the same for both engines.
 */
internal interface QuickJSEngine : AutoCloseable {
    var deadlineNanos: Long
    var cpuDeadlineNanos: Long
    val interrupted: Boolean
    fun resetInterrupted()
    val memoryUsed: Long
    val hasPendingJob: Boolean
    fun registerFunction(name: String, handler: (Array<String>) -> HostReturn)
    fun captureGlobalFunction(name: String): Boolean
    fun evaluate(code: String, filename: String, asModule: Boolean): JSResult
    fun evaluateBytecode(blob: ByteArray, source: String): JSResult
    fun compileBytecode(code: String, filename: String): ByteArray
    fun call(name: String, args: List<String>): JSResult
    fun executePendingJobs(onError: (QuickJSException) -> Unit)
}

class QuickJSContext private constructor(
    val backend: QuickJSBackend,
    private val engine: QuickJSEngine,
    /**
     * Why this context cannot run anything (a boxed engine this build cannot provide), or null. Every evaluation and
     * call of such a context throws [QuickJSException]; the caller is expected to kill the widget (fail closed).
     */
    val unavailableReason: String?,
) : AutoCloseable {
    companion object {
        /**
         * Must stay below the real stack of whichever thread enters JS. QuickJS only
         * *detects* overflow against this limit; the OS kills the process at the real
         * one. Product contexts live on a JSThread (8 MB) and use JSThread.JS_STACK_LIMIT.
         * The boxed engine derives its own, smaller limit from it (the box's generated C
         * uses several times more native stack than the stack QuickJS measures there).
         */
        const val DEFAULT_MAX_STACK = 384L shl 10

        init {
            System.loadLibrary("twquickjs")
        }

        /** CLOCK_BOOTTIME nanoseconds, the clock the native interrupt handler compares deadlines against. */
        @JvmStatic external fun nativeNow(): Long

        /** CPU nanoseconds consumed by the calling thread (CLOCK_THREAD_CPUTIME_ID); unaffected by how busy the device is. */
        @JvmStatic external fun nativeThreadCpuNanos(): Long
    }

    /**
     * `maxStackBytes` must stay below the real stack of whichever thread enters JS. Product contexts live on a JSThread
     * and use [JSThread.JS_STACK_LIMIT]. `backend` decides which build of QuickJS runs the code, for the context's life;
     * [QuickJSBackend.NATIVE] by default (the validator keeps it). A boxed context this build cannot provide reports
     * [unavailableReason] and fails every evaluation and call.
     */
    constructor(memoryLimitBytes: Long, maxStackBytes: Long = DEFAULT_MAX_STACK, backend: QuickJSBackend = QuickJSBackend.NATIVE) :
        this(backend, makeEngine(backend, memoryLimitBytes, maxStackBytes))

    // Kotlin cannot run code before a delegating constructor, so the engine and the reason are built together as a pair.
    private constructor(backend: QuickJSBackend, built: Pair<QuickJSEngine, String?>) : this(backend, built.first, built.second)

    /** "native QuickJS", or the boxed engine with the commit it was built from and how its memory is fenced (`bounds-checks`). */
    val engineDescription: String
        get() = when (backend) {
            QuickJSBackend.NATIVE -> "native QuickJS"
            QuickJSBackend.BOXED ->
                if (QuickJSBackend.isBoxedAvailable) "boxed QuickJS ${BoxedQuickJS.quickJSCommit} (${BoxedQuickJS.buildMode})" else "boxed QuickJS (unavailable)"
        }

    /** CPU time consumed by the calling thread, in nanoseconds; unaffected by how busy the device is. */
    fun threadCpuNanos(): Long = nativeThreadCpuNanos()

    /** Interrupt deadline in [nativeNow] nanoseconds; Long.MAX_VALUE disables it. */
    var deadlineNanos: Long
        get() = engine.deadlineNanos
        set(value) { engine.deadlineNanos = value }

    /** Interrupt deadline in CPU nanoseconds of the thread running JS ([threadCpuNanos]); Long.MAX_VALUE disables it. */
    var cpuDeadlineNanos: Long
        get() = engine.cpuDeadlineNanos
        set(value) { engine.cpuDeadlineNanos = value }

    /** Keeps a private reference to the global function [name]; [call] uses it from then on, even after the global is deleted. */
    fun captureGlobalFunction(name: String): Boolean = engine.captureGlobalFunction(name)

    /** Set by the interrupt handler when the deadline fired; reset by the caller. */
    val interrupted: Boolean get() = engine.interrupted
    fun resetInterrupted() = engine.resetInterrupted()

    val memoryUsed: Long get() = engine.memoryUsed
    val hasPendingJob: Boolean get() = engine.hasPendingJob

    /** Exposes `name(...)` to the sandbox. Arguments arrive as strings (numbers are formatted by JS). */
    fun registerFunction(name: String, handler: (Array<String>) -> HostReturn) = engine.registerFunction(name, handler)

    @Throws(QuickJSException::class)
    fun evaluate(code: String, filename: String, asModule: Boolean = false): JSResult = engine.evaluate(code, filename, asModule)

    /**
     * Runs one of our own precompiled bundles (`.qbc`, see qjs_shim.h), checked
     * against the source it was built from; throws on any mismatch so the caller
     * can fall back to [evaluate]. Never for widget code: bytecode is not verified.
     */
    @Throws(QuickJSException::class)
    fun evaluateBytecode(blob: ByteArray, source: String): JSResult = engine.evaluateBytecode(blob, source)

    /** Compiles `code` into the same container the build ships (tests). */
    @Throws(QuickJSException::class)
    fun compileBytecode(code: String, filename: String): ByteArray = engine.compileBytecode(code, filename)

    /** Calls the global function `name` with string arguments. */
    @Throws(QuickJSException::class)
    fun call(name: String, args: List<String> = emptyList()): JSResult = engine.call(name, args)

    /** Runs queued promise jobs. Errors thrown by jobs are reported through `onError` and do not stop the loop. */
    fun executePendingJobs(onError: (QuickJSException) -> Unit) = engine.executePendingJobs(onError)

    override fun close() = engine.close()
}

/** Builds the engine for a backend, fail-closed: a boxed engine this build cannot provide is an [UnavailableQuickJS]. */
private fun makeEngine(backend: QuickJSBackend, memoryLimitBytes: Long, maxStackBytes: Long): Pair<QuickJSEngine, String?> =
    when (backend) {
        QuickJSBackend.NATIVE -> NativeQuickJS(memoryLimitBytes, maxStackBytes) to null
        QuickJSBackend.BOXED ->
            if (!QuickJSBackend.isBoxedAvailable) {
                UnavailableQuickJS(BoxedQuickJS.unavailableReason) to BoxedQuickJS.unavailableReason
            } else {
                try {
                    BoxedQuickJS(memoryLimitBytes, maxStackBytes) to null
                } catch (e: QuickJSException) {
                    val reason = e.message ?: "the QuickJS box could not be created"
                    UnavailableQuickJS(reason) to reason
                }
            }
    }

/** The engine of a context that cannot have one: nothing runs, every evaluation and call fails with the reason. */
private class UnavailableQuickJS(private val reason: String) : QuickJSEngine {
    override var deadlineNanos: Long = Long.MAX_VALUE
    override var cpuDeadlineNanos: Long = Long.MAX_VALUE
    override val interrupted: Boolean get() = false
    override fun resetInterrupted() {}
    override val memoryUsed: Long get() = 0
    override val hasPendingJob: Boolean get() = false
    override fun registerFunction(name: String, handler: (Array<String>) -> HostReturn) {}
    override fun captureGlobalFunction(name: String): Boolean = false
    override fun evaluate(code: String, filename: String, asModule: Boolean): JSResult = throw QuickJSException("Engine fault: $reason")
    override fun evaluateBytecode(blob: ByteArray, source: String): JSResult = throw QuickJSException("Engine fault: $reason")
    override fun compileBytecode(code: String, filename: String): ByteArray = throw QuickJSException("Engine fault: $reason")
    override fun call(name: String, args: List<String>): JSResult = throw QuickJSException("Engine fault: $reason")
    override fun executePendingJobs(onError: (QuickJSException) -> Unit) {}
    override fun close() {}
}

/**
 * The native QuickJS engine: quickjs-ng (the fork) through quickjs_jni.c → qjs_shim, compiled into `libtwquickjs`.
 * Unchanged behaviour from before the box was wired in; the default on every platform.
 */
internal class NativeQuickJS(memoryLimitBytes: Long, maxStackBytes: Long) : QuickJSEngine {
    private var handle: Long = nativeCreate(memoryLimitBytes, maxStackBytes)
    private val functions = ArrayList<(Array<String>) -> HostReturn>()

    init {
        check(handle != 0L) { "QuickJS runtime allocation failed" }
    }

    override var deadlineNanos: Long
        get() = nativeGetDeadline(handle)
        set(value) = nativeSetDeadline(handle, value)

    override var cpuDeadlineNanos: Long = Long.MAX_VALUE
        set(value) {
            field = value
            nativeSetCpuDeadline(handle, value)
        }

    override fun captureGlobalFunction(name: String): Boolean = nativeCaptureGlobalFunction(handle, name)

    override val interrupted: Boolean get() = nativeInterrupted(handle)
    override fun resetInterrupted() = nativeResetInterrupted(handle)

    override val memoryUsed: Long get() = nativeMemoryUsed(handle)
    override val hasPendingJob: Boolean get() = nativeIsJobPending(handle)

    override fun registerFunction(name: String, handler: (Array<String>) -> HostReturn) {
        functions.add(handler)
        nativeRegisterFunction(handle, name, functions.size - 1)
    }

    /** Called from quickjs_jni.c (host_trampoline). Never throws. */
    @Keep
    @Suppress("unused")
    private fun dispatch(magic: Int, args: Array<ByteArray>): Any? {
        return try {
            when (val r = functions[magic](Array(args.size) { String(args[it], Charsets.UTF_8) })) {
                HostReturn.Undefined -> null
                HostReturn.Null -> r
                is HostReturn.Str -> r.value.toByteArray(Charsets.UTF_8)
                is HostReturn.Num -> r.value
                is HostReturn.ArrayBuffer -> r
            }
        } catch (t: Throwable) {
            Log.e("twquickjs", "host function threw", t)
            null
        }
    }

    /** Called from quickjs_jni.c to raise a QuickJSException with a UTF-8 description. */
    @Keep
    @Suppress("unused")
    private fun raise(description: ByteArray) {
        throw QuickJSException(String(description, Charsets.UTF_8))
    }

    override fun evaluate(code: String, filename: String, asModule: Boolean): JSResult =
        toResult(nativeEval(handle, code.toByteArray(Charsets.UTF_8), filename, asModule))

    override fun evaluateBytecode(blob: ByteArray, source: String): JSResult =
        toResult(nativeEvalBytecode(handle, blob, source.toByteArray(Charsets.UTF_8)))

    override fun compileBytecode(code: String, filename: String): ByteArray =
        nativeCompileBytecode(handle, code.toByteArray(Charsets.UTF_8), filename)

    override fun call(name: String, args: List<String>): JSResult =
        toResult(nativeCall(handle, name, Array(args.size) { args[it].toByteArray(Charsets.UTF_8) }))

    override fun executePendingJobs(onError: (QuickJSException) -> Unit) {
        for (i in 0 until 100) {
            val r = try {
                nativeExecutePendingJob(handle)
            } catch (e: QuickJSException) {
                onError(e)
                continue
            }
            if (r == 0) break
        }
    }

    private fun toResult(raw: Any?): JSResult = when (raw) {
        null -> JSResult.Undefined
        is ByteArray -> JSResult.Str(String(raw, Charsets.UTF_8))
        is Double -> JSResult.Num(raw)
        else -> JSResult.Undefined
    }

    override fun close() {
        if (handle != 0L) {
            nativeDestroy(handle)
            handle = 0
        }
    }

    private external fun nativeCreate(memoryLimit: Long, maxStack: Long): Long
    private external fun nativeDestroy(handle: Long)
    private external fun nativeRegisterFunction(handle: Long, name: String, magic: Int)
    private external fun nativeEval(handle: Long, code: ByteArray, filename: String, asModule: Boolean): Any?
    private external fun nativeEvalBytecode(handle: Long, blob: ByteArray, source: ByteArray): Any?
    private external fun nativeCompileBytecode(handle: Long, code: ByteArray, filename: String): ByteArray
    private external fun nativeCall(handle: Long, name: String, args: Array<ByteArray>): Any?
    private external fun nativeIsJobPending(handle: Long): Boolean
    private external fun nativeExecutePendingJob(handle: Long): Int
    private external fun nativeMemoryUsed(handle: Long): Long
    private external fun nativeSetDeadline(handle: Long, nanos: Long)
    private external fun nativeGetDeadline(handle: Long): Long
    private external fun nativeSetCpuDeadline(handle: Long, nanos: Long)
    private external fun nativeCaptureGlobalFunction(handle: Long, name: String): Boolean
    private external fun nativeInterrupted(handle: Long): Boolean
    private external fun nativeResetInterrupted(handle: Long)
}
