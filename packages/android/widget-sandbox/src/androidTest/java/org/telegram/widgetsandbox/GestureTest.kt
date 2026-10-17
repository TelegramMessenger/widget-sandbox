package org.telegram.widgetsandbox

import android.os.SystemClock
import android.view.MotionEvent
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Space
import androidx.test.core.app.ActivityScenario
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference

/**
 * A widget inside a scrolling container (the chat list): a vertical swipe
 * that starts on a plain widget scrolls the list, one that starts on an
 * element with `touch-action: none` stays with the widget (the renderer's
 * `gesture` message tells RendererWebView to keep the parent from intercepting).
 */
@RunWith(AndroidJUnit4::class)
class GestureTest {
    private val instrumentation = InstrumentationRegistry.getInstrumentation()

    private class Fixture(val host: WidgetHost, val scroll: ScrollView, val latest: AtomicReference<WidgetDiagnostics>)

    private fun mount(scenario: ActivityScenario<TestActivity>, html: String): Fixture {
        lateinit var f: Fixture
        scenario.onActivity { a ->
            val d = a.resources.displayMetrics.density
            val host = WidgetHost(a, html, "light", 360.0)
            val latest = AtomicReference(WidgetDiagnostics())
            host.onChange = { latest.set(it) }
            val column = LinearLayout(a).apply { orientation = LinearLayout.VERTICAL }
            column.addView(host.view, LinearLayout.LayoutParams((360 * d).toInt(), (300 * d).toInt()))
            column.addView(Space(a), LinearLayout.LayoutParams(1, (4000 * d).toInt()))
            val scroll = ScrollView(a)
            scroll.addView(column)
            a.container.removeAllViews()
            a.container.addView(scroll, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.MATCH_PARENT))
            f = Fixture(host, scroll, latest)
        }
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(15)
        while (System.nanoTime() < deadline && !f.latest.get().loaded) Thread.sleep(50)
        assertTrue("widget never loaded", f.latest.get().loaded)
        Thread.sleep(300)
        return f
    }

    /**
     * Vertical swipe, 200 px up over ~160 ms, starting at the given point inside the
     * widget (dp, widget coordinates). Dispatched straight into the view hierarchy
     * from the ScrollView down, which is exactly the interception path under test
     * (system-level injection is refused on a device when another app's window
     * overlaps).
     */
    private fun swipeUp(f: Fixture, xDp: Float, yDp: Float) {
        val d = f.host.view.resources.displayMetrics.density
        val x = xDp * d
        val y0 = yDp * d
        val t0 = SystemClock.uptimeMillis()
        fun send(action: Int, y: Float, dt: Long) {
            val e = MotionEvent.obtain(t0, t0 + dt, action, x, y, 0)
            e.source = android.view.InputDevice.SOURCE_TOUCHSCREEN
            instrumentation.runOnMainSync { f.scroll.dispatchTouchEvent(e) }
            e.recycle()
        }
        send(MotionEvent.ACTION_DOWN, y0, 0)
        for (i in 1..10) {
            Thread.sleep(16)
            send(MotionEvent.ACTION_MOVE, y0 - i * 20f, i * 16L)
        }
        send(MotionEvent.ACTION_UP, y0 - 200f, 176)
        Thread.sleep(400)
    }

    private fun scrollY(f: Fixture): Int {
        var y = 0
        instrumentation.runOnMainSync { y = f.scroll.scrollY }
        return y
    }

    @Test
    fun plainWidgetLetsTheListScroll() {
        ActivityScenario.launch(TestActivity::class.java).use { scenario ->
            val f = mount(scenario, "<div style='height:280px;background:#eee'>plain content</div>")
            swipeUp(f, 180f, 150f)
            assertTrue("list should have scrolled, scrollY=${scrollY(f)}", scrollY(f) > 0)
            instrumentation.runOnMainSync { f.host.destroy() }
        }
    }

    @Test
    fun touchActionNoneKeepsTheGestureInTheWidget() {
        val html = """
            <div id="area" style="height:280px;touch-action:none;background:#dde"><div id="box">drag</div></div>
            <script>
              var n = 0;
              document.getElementById('area').addEventListener('pointerdown', function (e) { e.preventDefault(); console.log('down'); });
              document.getElementById('area').addEventListener('pointermove', function (e) { if (++n === 5) console.log('moved5'); });
              document.getElementById('area').addEventListener('pointercancel', function () { console.log('cancelled'); });
            </script>
        """.trimIndent()
        ActivityScenario.launch(TestActivity::class.java).use { scenario ->
            val f = mount(scenario, html)
            swipeUp(f, 180f, 150f)
            val logs = f.latest.get().logs.map { it.second }
            assertEquals("list must not scroll, logs=$logs", 0, scrollY(f))
            assertTrue("widget should have seen the moves: $logs", logs.contains("moved5"))
            assertTrue("gesture must not be cancelled: $logs", !logs.contains("cancelled"))
            instrumentation.runOnMainSync { f.host.destroy() }
        }
    }

    /**
     * Profile 1 has no range sliders (form controls are dropped) and no scrolling inside a widget
     * (`overflow: auto` becomes `hidden`), so neither claims the swipe any more: the list scrolls.
     */
    @Test
    fun outOfProfileSlidersAndScrollersLeaveTheSwipeToTheList() {
        val cases = listOf(
            "range" to "<div style='padding:40px 0'><input type=range min=0 max=100 value=50 style='width:300px'></div>",
            "scroller" to "<div style='height:200px;overflow-y:auto'><div style='height:2000px'>tall</div></div>",
        )
        ActivityScenario.launch(TestActivity::class.java).use { scenario ->
            for ((name, html) in cases) {
                val f = mount(scenario, html)
                swipeUp(f, 150f, if (name == "range") 50f else 100f)
                assertTrue("$name: the list scrolls", scrollY(f) > 0)
                instrumentation.runOnMainSync { f.host.destroy() }
            }
        }
    }
}
