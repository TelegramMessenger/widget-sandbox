// The boxed QuickJS engine: the same fork commit as the native one, compiled to
// wasm and translated back to C (packages/quickjs-box), behind the scalar C
// surface of qjsbox_host.h, reached through quickjs_box_jni.c (libtwquickjsbox).
// The twin of ios/.../QuickJSBoxed.swift. One box is one runtime and one context
// in a linear memory of its own; a memory bug in the engine reads and writes that
// memory only, or ends the process ("contain and crash", docs/decisions.md).
//
// On Android the box is always the bounds-check build: no guard pages (the 8 GiB
// address reservation is not viable on a 64-bit Android process and would clash
// with ART's own SIGSEGV use). The deadlines live in the box's host layer, in C;
// the interrupt check runs no Kotlin and no JNI. All methods must be called from
// the one thread that owns the context (a JSThread), as with the native engine.

package org.telegram.widgetsandbox

import android.util.Log
import androidx.annotation.Keep

internal class BoxedQuickJS(memoryLimitBytes: Long, maxStackBytes: Long) : QuickJSEngine {
    companion object {
        /** Why `libtwquickjsbox` could not be loaded, or null when it is in and its natives are bound. */
        private val loadError: String? = try {
            System.loadLibrary("twquickjsbox")
            null
        } catch (t: Throwable) {
            t.message ?: "libtwquickjsbox could not be loaded"
        }

        /** True when the box is loaded and compiled optimized (an unoptimized box is 11-48x slower and refuses to start). */
        val isAvailable: Boolean = loadError == null && runCatching { nativeOptimized() }.getOrDefault(false)

        /** Why a boxed context would fail closed on this build; meaningful only when [isAvailable] is false. */
        val unavailableReason: String =
            loadError
                ?: if (runCatching { nativeOptimized() }.getOrDefault(false)) "the QuickJS box is not available on this build"
                else "the QuickJS box was compiled without optimization (compile it with -O2 in every configuration)"

        /** "bounds-checks" on Android (the memory mode), or "unavailable" when the box could not be loaded. */
        val buildMode: String = if (loadError == null) runCatching { nativeBuildMode() }.getOrDefault("unavailable") else "unavailable"

        /** The first hex digits of the QuickJS fork commit the box was built from (e.g. "b526a81"). */
        val quickJSCommit: String = if (loadError == null) runCatching { nativeQuickJSCommit() }.getOrDefault("unknown") else "unknown"

        @JvmStatic private external fun nativeOptimized(): Boolean
        @JvmStatic private external fun nativeBuildMode(): String
        @JvmStatic private external fun nativeQuickJSCommit(): String
    }

    // Throws QuickJSException from JNI (ThrowNew) when the box cannot be created (unoptimized build, the engine did not
    // start). makeEngine() turns that into an UnavailableQuickJS, so the widget fails closed with the reason.
    private var handle: Long = nativeCreate(memoryLimitBytes, maxStackBytes)
    private val functions = ArrayList<(Array<String>) -> HostReturn>()

    init {
        if (handle == 0L) throw QuickJSException("the QuickJS box could not be created")
    }

    // The deadlines are stored in the Kotlin clock domain (CLOCK_BOOTTIME ns / thread CPU ns, as the native engine uses)
    // and translated to the box's own clocks (CLOCK_MONOTONIC / CLOCK_THREAD_CPUTIME_ID seconds) inside the JNI setters.
    override var deadlineNanos: Long = Long.MAX_VALUE
        set(value) {
            field = value
            nativeSetDeadline(handle, value)
        }

    override var cpuDeadlineNanos: Long = Long.MAX_VALUE
        set(value) {
            field = value
            nativeSetCpuDeadline(handle, value)
        }

    override val interrupted: Boolean get() = nativeInterrupted(handle)
    override fun resetInterrupted() = nativeResetInterrupted(handle)

    override val memoryUsed: Long get() = nativeMemoryUsed(handle)
    override val hasPendingJob: Boolean get() = nativeIsJobPending(handle)

    override fun captureGlobalFunction(name: String): Boolean = nativeCaptureGlobalFunction(handle, name)

    override fun registerFunction(name: String, handler: (Array<String>) -> HostReturn) {
        functions.add(handler)
        nativeRegisterFunction(handle, name, functions.size - 1)
    }

    /** Called from quickjs_box_jni.c (box_call). Never throws. Identical to NativeQuickJS.dispatch. */
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
            Log.e("twquickjsbox", "host function threw", t)
            null
        }
    }

    /** Called from quickjs_box_jni.c to raise a QuickJSException with a UTF-8 description. */
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
    private external fun nativeSetCpuDeadline(handle: Long, nanos: Long)
    private external fun nativeCaptureGlobalFunction(handle: Long, name: String): Boolean
    private external fun nativeInterrupted(handle: Long): Boolean
    private external fun nativeResetInterrupted(handle: Long)
}
