package org.telegram.widgetsandbox

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class UserActivationTest {
    @Test
    fun keyAndClickRules() {
        assertTrue(UserActivation.isUserActivation("click", null, false, 0))
        assertTrue(UserActivation.isUserActivation("keydown", "a", false, 0))
        assertFalse(UserActivation.isUserActivation("keydown", "Escape", false, 0))
        assertFalse(UserActivation.isUserActivation("keydown", "a", true, 0))
        assertFalse(UserActivation.isUserActivation("keydown", "c", false, 4))
        assertFalse(UserActivation.isUserActivation("mousemove", null, false, 0))
        assertFalse(UserActivation.isUserActivation("scroll", null, false, 0))
    }

    @Test
    fun onePhysicalPressIsOneGesture() {
        val c = GestureCounter()
        val press = listOf("pointerdown", "mousedown", "pointerup", "mouseup", "click").map { c.gestureFor(it, null, false, 0) }
        assertEquals(listOf(1, 1, 1, 1, 1), press)
        assertEquals(1, c.gestureFor("dblclick", null, false, 0))
        assertEquals(2, c.gestureFor("pointerdown", null, false, 0))
        assertEquals(2, c.gestureFor("click", null, false, 0))
        assertEquals(3, c.gestureFor("keydown", "a", false, 0))
        assertEquals(4, c.gestureFor("keydown", "b", false, 0))
        assertEquals(0, c.gestureFor("mousemove", null, false, 0))
        assertEquals(5, c.gestureFor("click", null, false, 0))
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
