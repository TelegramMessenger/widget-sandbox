package org.telegram.widgetsandbox

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import org.junit.runner.RunWith

/**
 * The boxed QuickJS engine on Android: the same fork commit inside a wasm2c box, in bounds-check mode
 * (BoxedQuickJS.kt, quickjs_box_jni.c, packages/quickjs-box). The twin of BoxedQuickJSTests.swift.
 *
 * Every box runs on a JSThread: the box's generated C uses several times more native stack than the stack
 * QuickJS measures inside it, so it needs the JSThread's 8 MB, not the instrumentation thread's.
 */
@RunWith(AndroidJUnit4::class)
class BoxedQuickJSTest {
    private fun <T> onBox(memoryMb: Long = 16, block: (QuickJSContext) -> T): T {
        val t = JSThread("box.test")
        try {
            return t.sync {
                QuickJSContext(memoryMb shl 20, JSThread.JS_STACK_LIMIT, QuickJSBackend.BOXED).use { js ->
                    block(js)
                }
            }
        } finally {
            t.stop()
        }
    }

    @Test
    fun boxIsAvailableInBoundsCheckMode() {
        assertTrue("QuickJSBackend.isBoxedAvailable", QuickJSBackend.isBoxedAvailable)
        assertEquals("bounds-checks", BoxedQuickJS.buildMode)
        assertTrue("commit ${BoxedQuickJS.quickJSCommit}", BoxedQuickJS.quickJSCommit.isNotEmpty() && BoxedQuickJS.quickJSCommit != "unknown")
        val t = JSThread("box.desc")
        try {
            val desc = t.sync { QuickJSContext(8L shl 20, JSThread.JS_STACK_LIMIT, QuickJSBackend.BOXED).use { it.engineDescription } }
            assertTrue(desc, desc.startsWith("boxed QuickJS") && desc.contains("bounds-checks"))
            assertTrue(desc, desc.contains(BoxedQuickJS.quickJSCommit))
        } finally {
            t.stop()
        }
    }

    @Test
    fun evaluatesAndCallsWithRealUtf8() = onBox { js ->
        assertEquals(3.0, js.evaluate("1 + 2", "t.js").number)
        assertEquals("héllo 🎉", js.evaluate("'h' + 'éllo ' + '\\u{1F389}'", "t.js").string)
        js.evaluate("globalThis.echo = (a, b) => a + '|' + b + '|' + a.length", "t.js")
        assertEquals("🎉|ß|2", js.call("echo", listOf("🎉", "ß")).string)
        assertTrue(js.evaluate("undefined", "t.js") is JSResult.Undefined)
    }

    @Test
    fun hostFunctionRoundTrip() = onBox { js ->
        val seen = ArrayList<List<String>>()
        js.registerFunction("__host") { args ->
            seen.add(args.toList())
            when (args[0]) {
                "str" -> HostReturn.Str("→ " + args[1])
                "num" -> HostReturn.Num(42.5)
                "null" -> HostReturn.Null
                "buf" -> HostReturn.ArrayBuffer(byteArrayOf(1, 2, 3))
                else -> HostReturn.Undefined
            }
        }
        val out = js.evaluate(
            "[__host('str', 'émoji 🎉'), __host('num', 7), String(__host('null')), new Uint8Array(__host('buf')).join(','), String(__host('x'))].join(';')",
            "t.js",
        ).string
        assertEquals("→ émoji 🎉;42.5;null;1,2,3;undefined", out)
        assertEquals(listOf("num", "7"), seen[1])
    }

    @Test
    fun bytecodeRoundTripAndStaleBlobRefused() = onBox { js ->
        val blob = js.compileBytecode("globalThis.answer = 6 * 7;", "t.js")
        assertTrue(blob.size > 24)
        js.evaluateBytecode(blob, "globalThis.answer = 6 * 7;")
        assertEquals(42.0, js.evaluate("answer", "t.js").number)
        try {
            js.evaluateBytecode(blob, "globalThis.answer = 6 * 8;")
            fail("a blob compiled from another source must be refused")
        } catch (e: QuickJSException) {
        }
    }

    @Test
    fun exceptionsCarryNameMessageAndStack() = onBox { js ->
        try {
            js.evaluate("function f(){ throw new TypeError('bad ✗') } f()", "boom.js")
            fail("expected an exception")
        } catch (e: QuickJSException) {
            assertTrue(e.message, e.message!!.startsWith("TypeError: bad ✗"))
            assertTrue(e.message, e.message!!.contains("boom.js"))
        }
        try {
            js.call("nope")
            fail("expected an exception")
        } catch (e: QuickJSException) {
            assertEquals("nope is not a function", e.message)
        }
        assertEquals(2.0, js.evaluate("1+1", "t.js").number)
    }

    @Test
    fun deadlineInterruptsAndIsReported() = onBox { js ->
        js.deadlineNanos = QuickJSContext.nativeNow() + 150_000_000L
        try {
            js.evaluate("for(;;){}", "loop.js")
            fail("expected an interrupt")
        } catch (e: QuickJSException) {
            assertTrue(e.message, e.message!!.contains("interrupt"))
        }
        assertTrue(js.interrupted)
        js.resetInterrupted()
        js.deadlineNanos = Long.MAX_VALUE
        assertFalse(js.interrupted)
        assertEquals(2.0, js.evaluate("1+1", "t.js").number)
    }

    @Test
    fun memoryLimitThrowsOutOfMemory() = onBox(8) { js ->
        try {
            js.evaluate("var a = []; for (;;) a.push(new Array(100000).fill(1));", "mem.js")
            fail("expected out of memory")
        } catch (e: QuickJSException) {
            assertTrue(e.message, e.message!!.lowercase().contains("out of memory"))
        }
    }

    @Test
    fun promiseJobsRunAndReportErrors() = onBox { js ->
        js.evaluate("globalThis.log = []; Promise.resolve(1).then(v => log.push(v)); Promise.resolve().then(() => { throw new Error('in job') }).catch(e => log.push(e.message));", "p.js")
        assertTrue(js.hasPendingJob)
        val errors = ArrayList<String>()
        js.executePendingJobs { errors.add(it.message ?: "") }
        assertFalse(js.hasPendingJob)
        assertEquals("1,in job", js.evaluate("log.join()", "t.js").string)
        assertTrue(errors.toString(), errors.isEmpty())
    }

    /** Runaway recursion ends in a catchable RangeError inside the box (its guest stack limit), not a native crash. */
    @Test
    fun deepRecursionIsCaughtAsRangeError() = onBox { js ->
        val depth = js.evaluate(
            "(function(){ var n = 0; function r(){ n++; r(); } try { r(); } catch (e) { if (!(e instanceof RangeError)) throw e; } return n; })()",
            "t.js",
        ).number ?: -1.0
        assertTrue("box recursion depth $depth", depth > 200)
    }
}
