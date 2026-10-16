package org.telegram.widgetsandbox

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class UserActivationTest {
    @Test
    fun pointerRulesNoKeys() {
        assertTrue(UserActivation.isUserActivation("click", 1.0))
        assertTrue(UserActivation.isUserActivation("pointerdown"))
        assertFalse("the click Enter or Space activates", UserActivation.isUserActivation("click", 0.0))
        assertFalse(UserActivation.isUserActivation("keydown"))
        assertFalse(UserActivation.isUserActivation("mousemove"))
        assertFalse(UserActivation.isUserActivation("scroll"))
    }

    @Test
    fun onePhysicalPressIsOneGesture() {
        val c = GestureCounter()
        val press = listOf("pointerdown", "mousedown", "pointerup", "mouseup", "click").map { c.gestureFor(it) }
        assertEquals(listOf(1, 1, 1, 1, 1), press)
        assertEquals(1, c.gestureFor("dblclick"))
        assertEquals(2, c.gestureFor("pointerdown"))
        assertEquals(2, c.gestureFor("click", 1.0))
        assertEquals(0, c.gestureFor("mousemove"))
        assertEquals(3, c.gestureFor("click", 1.0))
    }

    @Test
    fun keysAndKeyboardClicksAreNeverAGesture() {
        val c = GestureCounter()
        assertEquals(0, c.gestureFor("keydown"))
        assertEquals(0, c.gestureFor("click", 0.0))
        assertEquals(0, c.gestureFor("dblclick", 0.0))
        assertEquals(1, c.gestureFor("click", 1.0))
        assertEquals(0, c.gestureFor("click", 0.0))
    }

    @Test
    fun gateLetsOneCallThroughPerGestureInsideItsTask() {
        val g = ActivationGate()
        assertFalse(g.consume(true))
        g.begin(1, true)
        assertTrue(g.consume(true))
        assertFalse(g.consume(true))
        g.end()
        g.begin(1, true)
        assertFalse(g.consume(true))
        g.end()
        g.begin(2, false)
        assertFalse(g.consume(true))
        g.end()
        g.begin(3, true)
        g.end()
        assertFalse(g.consume(true))
        g.begin(4, true)
        assertFalse(g.consume(false))
        assertTrue(g.consume(true))
    }
}
