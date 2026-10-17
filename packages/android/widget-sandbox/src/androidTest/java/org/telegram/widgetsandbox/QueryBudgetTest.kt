package org.telegram.widgetsandbox

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

/**
 * The layout-query budget (CpuBudget.queryBudgetMs): a widget whose loop forces
 * layout on every iteration burns no JS CPU of its own but keeps the host
 * answering queries; it is killed with the layout-query reason instead of
 * holding the renderer, and the host is never blocked for long. The twin of
 * QueryBudgetTests.swift.
 */
@RunWith(AndroidJUnit4::class)
class QueryBudgetTest {
    private val resources = SandboxResources.get(InstrumentationRegistry.getInstrumentation().targetContext)

    /**
     * Every query costs the host [delayMs] of wall time, like a slow renderer. The delay is
     * large against one loop iteration's CPU on the emulator's debug interpreter, so the
     * query budget (not the CPU slice of the same 200 ms) is the limit that fires.
     */
    private class SlowHost(private val delayMs: Long) : SandboxEngine.Delegate {
        @Volatile var died: String? = null
        @Volatile var loaded = false
        @Volatile var queryCount = 0
        @Volatile var firstQueryAt = 0L
        @Volatile var diedAt = 0L
        override fun onOps(engine: SandboxEngine, json: String) {}
        override fun onQuery(engine: SandboxEngine, json: String): SandboxEngine.QueryAnswer {
            if (queryCount++ == 0) firstQueryAt = System.currentTimeMillis()
            Thread.sleep(delayMs)
            return SandboxEngine.QueryAnswer("null", null)
        }
        override fun onListen(engine: SandboxEngine, json: String) {}
        override fun onPreventDefault(engine: SandboxEngine, json: String) {}
        override fun onRequestFrame(engine: SandboxEngine) {}
        override fun onLog(engine: SandboxEngine, level: String, text: String) {}
        override fun onDiag(engine: SandboxEngine, kind: String, detail: String) {}
        override fun onLoaded(engine: SandboxEngine) { loaded = true }
        override fun onStats(engine: SandboxEngine, stats: SandboxStats) {}
        override fun onDied(engine: SandboxEngine, reason: String) { diedAt = System.currentTimeMillis(); died = reason }
    }

    /** Boots the widget and waits until it is loaded or dead (the emulator boots slowly under load), at most [waitMs]. */
    private fun run(script: String, delayMs: Long, waitMs: Long): Pair<SlowHost, SandboxEngine> {
        val config = SandboxConfig(innerWidth = 300.0, innerHeight = 200.0, devicePixelRatio = 2.0, theme = "light", cpuLimitMs = 200.0)
        val host = SlowHost(delayMs)
        val engine = SandboxEngine("<div id=a></div><script>$script</script>", config, resources)
        engine.delegate = host
        engine.start()
        val deadline = System.currentTimeMillis() + waitMs
        while (host.died == null && !host.loaded && System.currentTimeMillis() < deadline) Thread.sleep(10)
        if (host.died == null) engine.thread.sync {} // the loading task's queries are in
        return host to engine
    }

    @Test
    fun forcedLayoutLoopIsKilledInsteadOfBlockingTheHost() {
        val started = System.currentTimeMillis()
        // 20 ms per answer: 10 queries pass the 200 ms budget while the loop has burnt far less CPU than the 200 ms slice.
        val (host, engine) = run("const a = document.getElementById('a'); let i = 0; for (;;) { a.style.width = (i++ % 300) + 'px'; a.offsetHeight; }", delayMs = 20, waitMs = 30_000)
        val died = host.died
        assertNotNull("alive after ${System.currentTimeMillis() - started} ms, ${host.queryCount} queries", died)
        assertTrue(died!!, died.startsWith("Layout query limit exceeded"))
        // From the first query (the boot itself is slow on a loaded emulator) to the kill: the host was never held for long.
        assertTrue("${host.diedAt - host.firstQueryAt} ms from the first query to the kill", host.diedAt - host.firstQueryAt < 5_000)
        assertTrue("${host.queryCount} queries", host.queryCount < 40)
        engine.terminate()
    }

    @Test
    fun ordinaryQueriesKeepTheWidgetAlive() {
        val (host, engine) = run("const a = document.getElementById('a'); for (let i = 0; i < 10; i++) { a.style.width = i + 'px'; a.offsetHeight; }", delayMs = 2, waitMs = 10_000)
        assertNull(host.died, host.died)
        assertTrue(host.queryCount > 0)
        engine.terminate()
    }
}
