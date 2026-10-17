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

    /** Every query costs the host [delayMs] of wall time, like a slow renderer. */
    private class SlowHost(private val delayMs: Long) : SandboxEngine.Delegate {
        @Volatile var died: String? = null
        @Volatile var queryCount = 0
        override fun onOps(engine: SandboxEngine, json: String) {}
        override fun onQuery(engine: SandboxEngine, json: String): SandboxEngine.QueryAnswer {
            queryCount++
            Thread.sleep(delayMs)
            return SandboxEngine.QueryAnswer("null", null)
        }
        override fun onListen(engine: SandboxEngine, json: String) {}
        override fun onPreventDefault(engine: SandboxEngine, json: String) {}
        override fun onRequestFrame(engine: SandboxEngine) {}
        override fun onLog(engine: SandboxEngine, level: String, text: String) {}
        override fun onDiag(engine: SandboxEngine, kind: String, detail: String) {}
        override fun onLoaded(engine: SandboxEngine) {}
        override fun onStats(engine: SandboxEngine, stats: SandboxStats) {}
        override fun onDied(engine: SandboxEngine, reason: String) { died = reason }
    }

    private fun run(script: String, delayMs: Long, waitMs: Long): Pair<SlowHost, SandboxEngine> {
        val config = SandboxConfig(innerWidth = 300.0, innerHeight = 200.0, devicePixelRatio = 2.0, theme = "light", cpuLimitMs = 200.0)
        val host = SlowHost(delayMs)
        val engine = SandboxEngine("<div id=a></div><script>$script</script>", config, resources)
        engine.delegate = host
        engine.start()
        val deadline = System.currentTimeMillis() + waitMs
        while (host.died == null && System.currentTimeMillis() < deadline) Thread.sleep(10)
        return host to engine
    }

    @Test
    fun forcedLayoutLoopIsKilledInsteadOfBlockingTheHost() {
        val started = System.currentTimeMillis()
        val (host, engine) = run("const a = document.getElementById('a'); let i = 0; for (;;) { a.style.width = (i++ % 300) + 'px'; a.offsetHeight; }", delayMs = 5, waitMs = 10_000)
        val died = host.died
        assertNotNull("alive after ${System.currentTimeMillis() - started} ms, ${host.queryCount} queries", died)
        assertTrue(died!!, died.startsWith("Layout query limit exceeded"))
        assertTrue("took ${System.currentTimeMillis() - started} ms", System.currentTimeMillis() - started < 5_000)
        assertTrue("${host.queryCount} queries", host.queryCount < 120)
        engine.terminate()
    }

    @Test
    fun ordinaryQueriesKeepTheWidgetAlive() {
        val (host, engine) = run("const a = document.getElementById('a'); for (let i = 0; i < 10; i++) { a.style.width = i + 'px'; a.offsetHeight; }", delayMs = 2, waitMs = 1_000)
        assertNull(host.died, host.died)
        assertTrue(host.queryCount > 0)
        engine.terminate()
    }
}
