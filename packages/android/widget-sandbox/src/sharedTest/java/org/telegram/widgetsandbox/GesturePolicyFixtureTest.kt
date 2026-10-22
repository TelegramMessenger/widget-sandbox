package org.telegram.widgetsandbox

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

/**
 * GesturePolicy.kt against the fixture shared with gesture-policy.ts, GesturePolicy.swift and gesture_policy.cpp
 * (tests/fixtures/gesture-policy.json, copied to androidTest/assets by `pnpm build:android`): every section.
 * Written once for both runs (src/sharedTest): `GesturePolicyTest` in src/test reads the asset from the module
 * directory on the JVM, the one in src/androidTest from the test APK's assets.
 */
abstract class GesturePolicyFixtureTest {
    /** The fixture's text. */
    protected abstract fun fixtureJson(): String

    private val fixture by lazy { JSONObject(fixtureJson()) }

    private fun section(name: String): List<JSONObject> {
        val a = fixture.getJSONArray(name)
        assertTrue(name, a.length() > 0)
        return List(a.length()) { a.getJSONObject(it) }
    }

    private fun strings(a: JSONArray) = List(a.length()) { a.getString(it) }

    private fun claim(o: JSONObject) = WidgetGestureClaim(o.getBoolean("x"), o.getBoolean("y"), o.getBoolean("hold"))

    private val verdictKeys = setOf("owner", "gesture", "cancel", "action", "event", "edge")

    /** A fixture verdict (`action`, `event`, `edge` only when set), or null for JSON null. */
    private fun verdict(v: Any?): GestureVerdict? {
        if (v == JSONObject.NULL) return null
        val o = v as JSONObject
        assertTrue("$o", o.keys().asSequence().all { it in verdictKeys })
        return GestureVerdict(
            owner = GestureOwner.entries.single { it.value == o.getString("owner") },
            gesture = Gesture.entries.single { it.value == o.getString("gesture") },
            cancel = o.getBoolean("cancel"),
            action = if (o.has("action")) BubbleAction.entries.single { it.value == o.getString("action") } else null,
            event = if (o.has("event")) GestureEvent.entries.single { it.value == o.getString("event") } else null,
            edge = o.optBoolean("edge", false),
        )
    }

    @Test
    fun constants() {
        val c = fixture.getJSONObject("constants")
        val ours = mapOf(
            "TOUCH_SLOP_PX" to GesturePolicy.TOUCH_SLOP_PX,
            "HOLD_MS" to GesturePolicy.HOLD_MS,
            "DOUBLE_TAP_MS" to GesturePolicy.DOUBLE_TAP_MS,
            "DOUBLE_TAP_SLOP_PX" to GesturePolicy.DOUBLE_TAP_SLOP_PX,
            "EDGE_STRIP_PX" to GesturePolicy.EDGE_STRIP_PX,
        )
        assertEquals(c.keys().asSequence().toSet(), ours.keys)
        for ((name, value) in ours) assertEquals(name, if (c.get(name) is Boolean) (if (c.getBoolean(name)) 1.0 else 0.0) else c.getDouble(name), value, 0.0)
    }

    @Test
    fun claims() {
        for (c in section("claims")) {
            val actions = strings(c.getJSONArray("touchActions"))
            assertEquals("$actions prevented ${c.getBoolean("prevented")}", claim(c.getJSONObject("expect")), GesturePolicy.claimOf(actions, c.getBoolean("prevented")))
        }
    }

    @Test
    fun cases() {
        for (c in section("cases")) {
            val a = GestureArbiter()
            val steps = c.getJSONArray("steps")
            for (i in 0 until steps.length()) {
                val s = steps.getJSONObject(i)
                val where = "${c.getString("name")}: step $i ${s.toString().take(80)}"
                fun point(name: String) = s.getJSONArray(name).let { Triple(it.getDouble(0), it.getDouble(1), it.getDouble(2)) }
                when {
                    s.has("down") -> {
                        val (t, x, y) = point("down")
                        val claim = GesturePolicy.claimOf(listOf(s.optString("claim", "auto")))
                        val pointer = PointerKind.entries.single { it.value == s.optString("pointer", "touch") }
                        assertEquals(where, verdict(s.get("expect")), a.down(t, x, y, claim, pointer, s.optInt("button", 0)))
                    }
                    s.has("move") -> point("move").let { (t, x, y) -> assertEquals(where, verdict(s.get("expect")), a.move(t, x, y)) }
                    s.has("up") -> point("up").let { (t, x, y) -> assertEquals(where, verdict(s.get("expect")), a.up(t, x, y)) }
                    s.has("tick") -> assertEquals(where, verdict(s.get("expect")), a.tick(s.getDouble("tick")))
                    s.optBoolean("cancel", false) -> a.cancel()
                    s.has("holdDeadline") -> assertEquals(where, if (s.isNull("holdDeadline")) null else s.getDouble("holdDeadline"), a.holdDeadline())
                    else -> fail("$where: unknown step")
                }
            }
        }
    }

    /**
     * Inputs the fixture does not carry, with V8's answers: `trim` is String.prototype.trim (U+FEFF and U+00A0
     * go, U+001C stays) and `\b` is JavaScript's ASCII word boundary (`é` is not a word character).
     */
    @Test
    fun javaScriptStringRules() {
        val all = WidgetGestureClaim(x = true, y = true, hold = true)
        assertEquals(all, GesturePolicy.claimOf(listOf("﻿none ")))
        assertEquals(WidgetGestureClaim(x = true, y = false), GesturePolicy.claimOf(listOf("\u001cpan-y")))
        assertEquals(WidgetGestureClaim(x = true, y = false), GesturePolicy.claimOf(listOf("pan-y ")))
        assertEquals(WidgetGestureClaim(x = false, y = true), GesturePolicy.claimOf(listOf("pan-xé")))
        assertEquals(WidgetGestureClaim(x = false, y = true), GesturePolicy.claimOf(listOf("pan-left-x")))
        assertEquals(WidgetGestureClaim(x = true, y = true), GesturePolicy.claimOf(listOf("pan-xy")))
        assertEquals(WidgetGestureClaim(x = true, y = true), GesturePolicy.claimOf(listOf("xpan-y")))
        assertEquals(WidgetGestureClaim(x = true, y = true), GesturePolicy.claimOf(listOf("pan-y_")))
        assertEquals(WidgetGestureClaim(x = true, y = true), GesturePolicy.claimOf(listOf("pan-İ")))
    }
}
