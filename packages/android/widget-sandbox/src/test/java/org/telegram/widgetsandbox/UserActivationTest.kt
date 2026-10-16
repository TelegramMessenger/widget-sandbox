package org.telegram.widgetsandbox

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class UserActivationTest {
    @Test
    fun keyAndClickRules() {
        assertTrue(UserActivation.isUserActivation("click", null, false, 0))
        assertTrue(UserActivation.isUserActivation("keydown", "Enter", false, 0))
        assertTrue(UserActivation.isUserActivation("keydown", " ", false, 0))
        for (key in listOf("a", "Escape", "Tab", "ArrowDown", "PageDown", "Shift", "enter", null)) assertFalse(key.toString(), UserActivation.isUserActivation("keydown", key, false, 0))
        assertFalse(UserActivation.isUserActivation("keydown", "Enter", true, 0))
        assertFalse(UserActivation.isUserActivation("keydown", "Enter", false, 4))
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
        assertEquals(2, c.gestureFor("click", null, false, 0, 1.0))
        assertEquals(3, c.gestureFor("keydown", "Enter", false, 0))
        assertEquals(4, c.gestureFor("keydown", " ", false, 0))
        assertEquals(0, c.gestureFor("keydown", "Tab", false, 0))
        assertEquals(0, c.gestureFor("mousemove", null, false, 0))
        assertEquals(5, c.gestureFor("click", null, false, 0, 1.0))
    }

    @Test
    fun anEnterOrSpacePressAndItsClickAreOneGesture() {
        val c = GestureCounter()
        assertEquals(1, c.gestureFor("keydown", "Enter", false, 0))
        assertEquals(1, c.gestureFor("click", null, false, 0, 0.0))
        assertEquals(0, c.gestureFor("keydown", "Enter", true, 0))
        assertEquals(1, c.gestureFor("click", null, false, 0, 0.0))
        assertEquals(2, c.gestureFor("keydown", " ", false, 0))
        assertEquals(0, c.gestureFor("keyup", " ", false, 0))
        assertEquals(2, c.gestureFor("click", null, false, 0, 0.0))
        assertEquals(3, c.gestureFor("click", null, false, 0, 0.0))
        assertEquals(4, c.gestureFor("keydown", "Enter", false, 0))
        assertEquals(5, c.gestureFor("click", null, false, 0, 1.0))
        assertEquals(6, c.gestureFor("keydown", "Enter", false, 0))
        assertEquals(0, c.gestureFor("keydown", "Tab", false, 0))
        assertEquals(6, c.gestureFor("click", null, false, 0, 0.0))
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
