package org.telegram.widgetsandbox

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** Where the widget is shown (docs/sdk-v1.md §2.19): the geometry of profile.json's `tables.surfaces`, as the glue applies it. */
class WidgetSurfaceTest {
    @Test
    fun aBubbleIsItsContentIn50To600() {
        assertEquals(50.0, WidgetSurface.MESSAGE.height(10.0, 844.0), 0.0)
        assertEquals(120.0, WidgetSurface.MESSAGE.height(120.0, 844.0), 0.0)
        assertEquals(600.0, WidgetSurface.MESSAGE.height(9000.0, 844.0), 0.0)
        assertEquals(400.0, WidgetSurface.MESSAGE.height(9000.0, 0.0, bubbleMax = 400.0), 0.0)
        assertEquals(50.0, WidgetSurface.MESSAGE.height(Double.NaN, 0.0), 0.0)
        assertEquals("a bubble has no viewport", 0.0, WidgetSurface.MESSAGE.checkViewportHeight(844.0), 0.0)
        assertTrue(WidgetSurface.MESSAGE.keepsResizeAnchor)
    }

    @Test
    fun aPageIsAtLeastItsViewportAndAtMostTheCeiling() {
        assertEquals(844.0, WidgetSurface.PROFILE.height(120.0, 844.0), 0.0)
        assertEquals(1500.0, WidgetSurface.PROFILE.height(1500.0, 844.0), 0.0)
        assertEquals(WidgetSurface.PAGE_MAX_HEIGHT, WidgetSurface.PROFILE.height(9000.0, 844.0), 0.0)
        assertEquals("no viewport: at least 50", 50.0, WidgetSurface.PROFILE.height(10.0, 0.0), 0.0)
        assertEquals(844.12, WidgetSurface.PROFILE.checkViewportHeight(844.123), 0.0)
        for (bad in listOf(0.0, -1.0, 4000.5, Double.NaN, Double.POSITIVE_INFINITY)) assertEquals("$bad", 0.0, WidgetSurface.PROFILE.checkViewportHeight(bad), 0.0)
        assertFalse(WidgetSurface.PROFILE.keepsResizeAnchor)
        assertEquals(listOf("message", "profile"), WidgetSurface.values().map { it.wire })
        assertEquals(4000.0, WidgetSurface.PAGE_MAX_HEIGHT, 0.0)
    }
}
