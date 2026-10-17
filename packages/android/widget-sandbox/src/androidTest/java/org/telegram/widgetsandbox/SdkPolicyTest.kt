package org.telegram.widgetsandbox

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

/**
 * SdkPolicy.kt against the fixture shared with sdk-policy.ts, SdkPolicy.swift and sdk_policy.cpp
 * (tests/fixtures/sdk-policy.json, copied to androidTest/assets by `pnpm build:android`): every section.
 */
@RunWith(AndroidJUnit4::class)
class SdkPolicyTest {
    private val fixture = JSONObject(InstrumentationRegistry.getInstrumentation().context.assets.open("sdk-policy.json").bufferedReader().readText())

    private fun cases(section: String): List<JSONObject> {
        val a = fixture.getJSONArray(section)
        assertTrue(section, a.length() > 0)
        return List(a.length()) { a.getJSONObject(it) }
    }

    /** The fixture's JSON null as Kotlin null. */
    private fun JSONObject.value(name: String): Any? = opt(name).takeUnless { it == JSONObject.NULL }

    /** `{"repeat": [s, n]}` stands for s repeated n times. */
    private fun expand(v: Any?): Any? = when (v) {
        is JSONArray -> JSONArray().also { out -> for (i in 0 until v.length()) out.put(expand(v.opt(i))) }
        is JSONObject -> {
            val r = v.optJSONArray("repeat")
            if (v.length() == 1 && r != null) r.getString(0).repeat(r.getInt(1))
            else JSONObject().also { out -> for (k in v.keys()) out.put(k, expand(v.opt(k))) }
        }
        else -> v
    }

    private fun strings(o: JSONObject): Map<String, String> = o.keys().asSequence().associateWith { o.getString(it) }

    private fun action(o: JSONObject?): WidgetAction? = when (o?.getString("kind")) {
        null -> null
        "ready" -> WidgetAction.Ready
        "openLink" -> WidgetAction.OpenLink(o.getString("url"))
        "openTelegramLink" -> WidgetAction.OpenTelegramLink(o.getString("url"))
        "haptic" -> WidgetAction.Haptic(o.getString("haptic"), o.value("value") as String?)
        else -> error("unknown kind ${o.getString("kind")}")
    }

    @Test
    fun links() {
        for (c in cases("links")) {
            val url = c.value("url")
            assertEquals("${c.getString("method")} ${url.toString().take(80)}", c.value("expect"), SdkPolicy.checkLink(c.getString("method"), url))
        }
    }

    @Test
    fun haptics() {
        for (c in cases("haptics")) {
            val expect = (c.value("expect") as JSONObject?)?.let { WidgetAction.Haptic(it.getString("haptic"), it.value("value") as String?) }
            assertEquals("${c.opt("args")}", expect, SdkPolicy.checkHaptic(c.value("args")))
        }
    }

    @Test
    fun bridgeCalls() {
        for (c in cases("bridge")) {
            assertEquals("${c.opt("method")} ${c.opt("args")}", action(c.value("expect") as JSONObject?), SdkPolicy.checkBridgeCall(c.value("method"), c.value("args")))
        }
    }

    @Test
    fun hapticGate() {
        for (c in cases("hapticGate")) {
            val gate = HapticGate()
            val steps = c.getJSONArray("steps")
            for (i in 0 until steps.length()) {
                val s = steps.getJSONObject(i)
                assertEquals("${c.getString("name")}, step $i", HapticDecision.valueOf(s.getString("expect").uppercase()), gate.decide(s.getDouble("at"), s.getBoolean("visible")))
            }
        }
    }

    @Test
    fun themeParams() {
        for (c in cases("themeParams")) assertEquals("${c.opt("input")}", strings(c.getJSONObject("expect")), SdkPolicy.checkThemeParams(c.value("input")))
    }

    @Test
    fun locales() {
        for (c in cases("locales")) assertEquals("${c.opt("input")}", c.value("expect"), SdkPolicy.checkLocale(c.value("input")))
    }

    @Test
    fun storage() {
        for (c in cases("storage")) {
            val where = c.optString("note").ifEmpty { "${c.opt("input")}".take(120) }
            val got = SdkPolicy.checkStorageCommit(expand(c.value("input")))
            val expect = expand(c.value("expect")) as JSONObject?
            if (expect == null) {
                assertNull(where, got)
                continue
            }
            assertEquals(where, StorageCommit(expect.optJSONObject("local")?.let(::strings), expect.optJSONObject("session")?.let(::strings)), got)
        }
    }

    @Test
    fun utf8() {
        for (c in cases("utf8")) assertEquals(c.getString("s"), c.getInt("bytes"), SdkPolicy.utf8Length(c.getString("s")))
    }

    /** Kotlin-only inputs: the same checks over Kotlin collections (what WidgetHost passes), and a lone surrogate, which the fixture cannot carry. */
    @Test
    fun kotlinCollectionsAndLoneSurrogates() {
        assertEquals(WidgetAction.Haptic("impact", "soft"), SdkPolicy.checkBridgeCall("haptic", listOf("impact", "soft")))
        assertEquals(StorageCommit(local = mapOf("a" to "1")), SdkPolicy.checkStorageCommit(mapOf("local" to mapOf("a" to "1"))))
        assertNull(SdkPolicy.checkStorageCommit(mapOf("local" to mapOf("a" to 1))))
        assertEquals(mapOf("bg_color" to "#abcdef"), SdkPolicy.checkThemeParams(mapOf("bg_color" to "#ABCDEF", "text_color" to "red", "x" to "#000000")))
        assertNull(SdkPolicy.checkLink("openLink", "https://example.com/\ud800"))
        assertNull(SdkPolicy.checkLink("openLink", "https://example.com/\udc00x"))
        assertEquals(3, SdkPolicy.utf8Length("\udc00"))
        assertEquals(3, SdkPolicy.utf8Length("\ud800"))
    }
}
