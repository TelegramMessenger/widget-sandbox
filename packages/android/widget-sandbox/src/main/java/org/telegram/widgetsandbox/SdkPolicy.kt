// SDK v1 checks (docs/sdk-v1.md §2.5, §2.6, §2.8, §2.9, §3): a line-for-line
// twin of the checked part of src/shared/sdk-policy.ts (see the comments
// there), SdkPolicy.swift and sdk_policy.cpp. The theme defaults and the
// renderer's CSS variables are TS only: the renderer core builds them from the
// `theme` message.
//
// Pure and side-effect free; checked against tests/fixtures/sdk-policy.json
// (copied to src/androidTest/assets by `pnpm build:android`), so a rule change
// goes to the four copies and the fixture together.
//
// Inputs are JSON values as org.json parses them (JSONObject, JSONArray,
// String, Number, Boolean, JSONObject.NULL) or the Kotlin equivalents (Map
// with String keys, List); anything else is "not an object" / "not an array".

package org.telegram.widgetsandbox

import org.json.JSONArray
import org.json.JSONObject
import kotlin.math.max
import kotlin.math.min

/** What the widget asked the client to do (docs/sdk-v1.md §3.2, §4.2), after the glue's checks. */
sealed class WidgetAction {
    /** The widget has rendered its initial state. */
    data object Ready : WidgetAction()
    /** An external https: page; the client confirms with the full URL. Scheme and host lowercase, non-ASCII percent-encoded. */
    data class OpenLink(val url: String) : WidgetAction()
    /** A t.me / telegram.me / telegram.dog link, for the client's own handling. */
    data class OpenTelegramLink(val url: String) : WidgetAction()
    /** `kind`: impact | notification | selection; `value`: the impact style or notification type, null for selection. */
    data class Haptic(val kind: String, val value: String?) : WidgetAction()
}

/** A storage commit or preload: the areas present, each in full (null = not part of it). */
data class StorageCommit(val local: Map<String, String>? = null, val session: Map<String, String>? = null)

enum class HapticDecision { PLAY, DROP, REPORT }

/**
 * Haptics need no gesture but a visible widget and room in the bucket (5 calls,
 * refilled at 10 per second). PLAY delivers; DROP drops silently; REPORT drops
 * and reports `haptic-limit` (at most once per second). Times in ms.
 */
class HapticGate {
    private var tokens = SdkPolicy.HAPTIC_BURST.toDouble()
    private var last: Double? = null
    private var reported: Double? = null

    fun decide(now: Double, visible: Boolean): HapticDecision {
        last?.let { tokens = min(SdkPolicy.HAPTIC_BURST.toDouble(), tokens + (max(0.0, now - it) * SdkPolicy.HAPTIC_PER_SECOND) / 1000) }
        last = now
        if (visible && tokens >= 1) {
            tokens -= 1
            return HapticDecision.PLAY
        }
        reported?.let { if (now - it < SdkPolicy.HAPTIC_REPORT_MS) return HapticDecision.DROP }
        reported = now
        return HapticDecision.REPORT
    }
}

object SdkPolicy {
    const val MAX_LINK_LENGTH = 256
    /** Bytes of keys and values (UTF-8) per storage area and message. */
    const val STORAGE_QUOTA_BYTES = 16384
    const val MAX_LOCALE_LENGTH = 35
    const val DEFAULT_LOCALE = "en-US"
    const val HAPTIC_BURST = 5
    const val HAPTIC_PER_SECOND = 10
    /** `haptic-limit` is reported at most this often. */
    const val HAPTIC_REPORT_MS = 1000
    /** The widget profile this glue enforces (`PROFILE_VERSION` in profile-tables.ts; docs/profile-v1.md). */
    const val PROFILE_VERSION = 1

    val THEME_KEYS = listOf(
        "bg_color", "text_color", "hint_color", "link_color", "button_color", "button_text_color",
        "secondary_bg_color", "accent_text_color", "subtitle_text_color", "destructive_text_color",
        "section_bg_color", "section_header_text_color", "section_separator_color", "header_bg_color", "bottom_bar_bg_color",
    )
    val HAPTIC_IMPACT_STYLES = listOf("light", "medium", "heavy", "rigid", "soft")
    val HAPTIC_NOTIFICATION_TYPES = listOf("error", "success", "warning")
    val TELEGRAM_HOSTS = listOf("t.me", "telegram.me", "telegram.dog")
    /** Methods that need the task's user gesture (docs/protocol.md, "User activation"). */
    val PRIVILEGED_METHODS = setOf("openLink", "openTelegramLink")

    /** UTF-8 length of a UTF-16 string; a lone surrogate counts 3 bytes (what U+FFFD takes). */
    fun utf8Length(s: String): Int {
        var n = 0
        var i = 0
        while (i < s.length) {
            val c = s[i].code
            if (c < 0x80) n += 1
            else if (c < 0x800) n += 2
            else if (c in 0xd800..0xdbff && i + 1 < s.length && (s[i + 1].code and 0xfc00) == 0xdc00) { n += 4; i++ }
            else n += 3
            i++
        }
        return n
    }

    private const val HEX = "0123456789ABCDEF"
    private fun isHex(c: Int) = (c in 48..57) || (c in 65..70) || (c in 97..102)
    /** ASCII that may stand unescaped after the authority: RFC 3986 unreserved, reserved and `%`. */
    private const val URL_ASCII = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~:/?#[]@!$&'()*+,;=%"

    /** Lowercases A–Z only: a Unicode mapping (U+212A KELVIN SIGN → `k`) must not turn a non-ASCII host into an ASCII one. */
    private fun asciiLower(s: String): String {
        val b = StringBuilder(s.length)
        for (ch in s) b.append(if (ch in 'A'..'Z') ch + 32 else ch)
        return b.toString()
    }

    private fun isTelegramHost(host: String) = TELEGRAM_HOSTS.any { host == it || host.endsWith(".$it") }
    private fun isLdhLabel(label: String): Boolean {
        if (label.isEmpty() || label.length > 63 || label[0] == '-' || label[label.length - 1] == '-') return false
        return label.all { it in 'a'..'z' || it in '0'..'9' || it == '-' }
    }

    /**
     * The URL a link call opens, or null when it breaks the rules: an absolute
     * `https:` URL, no user name or password, an ASCII host of LDH labels (an IDN
     * in its `xn--` form), an optional port, at most 256 characters once non-ASCII
     * characters after the host are percent-encoded as UTF-8. `openTelegramLink`
     * takes only t.me, telegram.me and telegram.dog (and their subdomains);
     * `openLink` every other host. Scheme and host come out lowercase.
     */
    fun checkLink(method: String, url: Any?): String? {
        if (url !is String || utf8Length(url) > MAX_LINK_LENGTH) return null
        if (asciiLower(url.take(8)) != "https://") return null
        var end = 8
        while (end < url.length && url[end] != '/' && url[end] != '?' && url[end] != '#') end++
        val authority = url.substring(8, end)
        if (authority.contains('@')) return null
        val colon = authority.indexOf(':')
        val host = asciiLower(if (colon < 0) authority else authority.substring(0, colon))
        val port = if (colon < 0) null else authority.substring(colon + 1)
        if (port != null && (port.length !in 1..5 || !port.all { it in '0'..'9' })) return null
        if (port != null && port.toInt() > 65535) return null
        if (host.isEmpty() || host.length > 253) return null
        for (label in host.split('.')) if (!isLdhLabel(label)) return null
        if (isTelegramHost(host) != (method == "openTelegramLink")) return null
        val rest = StringBuilder()
        var i = end
        while (i < url.length) {
            val c = url[i].code
            if (c < 0x80) {
                if (URL_ASCII.indexOf(url[i]) < 0) return null
                if (c == 37 && !(i + 2 < url.length && isHex(url[i + 1].code) && isHex(url[i + 2].code))) return null
                rest.append(url[i])
                i++
                continue
            }
            var cp = c
            if (c in 0xd800..0xdbff && i + 1 < url.length && (url[i + 1].code and 0xfc00) == 0xdc00) cp = 0x10000 + ((c - 0xd800) shl 10) + (url[++i].code - 0xdc00)
            else if (c in 0xd800..0xdfff) return null
            val bytes = when {
                cp < 0x800 -> intArrayOf(0xc0 or (cp shr 6), 0x80 or (cp and 63))
                cp < 0x10000 -> intArrayOf(0xe0 or (cp shr 12), 0x80 or ((cp shr 6) and 63), 0x80 or (cp and 63))
                else -> intArrayOf(0xf0 or (cp shr 18), 0x80 or ((cp shr 12) and 63), 0x80 or ((cp shr 6) and 63), 0x80 or (cp and 63))
            }
            for (b in bytes) rest.append('%').append(HEX[b shr 4]).append(HEX[b and 15])
            i++
        }
        val out = "https://" + host + (if (port == null) "" else ":$port") + rest
        return if (out.length > MAX_LINK_LENGTH) null else out
    }

    /** The haptic of a `haptic` bridge call's args, or null: `['impact', style]`, `['notification', type]`, `['selection']`. */
    fun checkHaptic(args: Any?): WidgetAction.Haptic? {
        val a = items(args) ?: return null
        val first = a.getOrNull(0)
        val second = a.getOrNull(1)
        if (first == "impact" && a.size == 2 && second is String && second in HAPTIC_IMPACT_STYLES) return WidgetAction.Haptic("impact", second)
        if (first == "notification" && a.size == 2 && second is String && second in HAPTIC_NOTIFICATION_TYPES) return WidgetAction.Haptic("notification", second)
        if (first == "selection" && a.size == 1) return WidgetAction.Haptic("selection", null)
        return null
    }

    /** The action of a bridge call, or null when the method is unknown or its arguments are wrong (`bridge-rejected`). */
    fun checkBridgeCall(method: Any?, args: Any?): WidgetAction? {
        val a = items(args) ?: return null
        return when (method) {
            "ready" -> if (a.isEmpty()) WidgetAction.Ready else null
            "openLink" -> (if (a.size == 1) checkLink("openLink", a[0]) else null)?.let { WidgetAction.OpenLink(it) }
            "openTelegramLink" -> (if (a.size == 1) checkLink("openTelegramLink", a[0]) else null)?.let { WidgetAction.OpenTelegramLink(it) }
            "haptic" -> checkHaptic(a)
            else -> null
        }
    }

    private fun isColor(v: String) = v.length == 7 && v[0] == '#' && (1..6).all { isHex(v[it].code) }

    /** The theme keys of `params` whose values are `#rrggbb` (lowercased); everything else is dropped. */
    fun checkThemeParams(params: Any?): Map<String, String> {
        val out = LinkedHashMap<String, String>()
        val obj = entries(params) ?: return out
        for (key in THEME_KEYS) {
            val v = obj[key]
            if (v is String && isColor(v)) out[key] = asciiLower(v)
        }
        return out
    }

    private val LOCALE = Regex("[A-Za-z]{2,8}(-[A-Za-z0-9]{1,8})*")

    /** A BCP 47 tag of at most 35 characters (`ru`, `en-US`, `pt-BR`, `zh-Hant-TW`), or null. */
    fun checkLocale(locale: Any?): String? {
        if (locale !is String || locale.isEmpty() || locale.length > MAX_LOCALE_LENGTH) return null
        return if (LOCALE.matches(locale)) locale else null
    }

    /** UTF-8 bytes of an area's keys and values. */
    fun storageBytes(area: Map<String, String>): Int {
        var n = 0
        for ((k, v) in area) n += utf8Length(k) + utf8Length(v)
        return n
    }

    /** An object of strings within the quota, or null. */
    fun checkStorageArea(area: Any?): Map<String, String>? {
        val obj = entries(area) ?: return null
        val out = LinkedHashMap<String, String>()
        for ((k, v) in obj) {
            if (v !is String) return null
            out[k] = v
        }
        return if (storageBytes(out) <= STORAGE_QUOTA_BYTES) out else null
    }

    /** A commit (or a preload) of `local` and/or `session`, each checked whole; null when anything is wrong (`storage-rejected`). */
    fun checkStorageCommit(commit: Any?): StorageCommit? {
        val obj = entries(commit) ?: return null
        var local: Map<String, String>? = null
        var session: Map<String, String>? = null
        for ((k, v) in obj) {
            val area = checkStorageArea(v) ?: return null
            when (k) {
                "local" -> local = area
                "session" -> session = area
                else -> return null
            }
        }
        return StorageCommit(local, session)
    }

    // ── JSON shapes ──

    /** The members of a JSON object (insertion order), or null when `v` is not one; JSONObject.NULL becomes null. */
    private fun entries(v: Any?): Map<String, Any?>? {
        val m = LinkedHashMap<String, Any?>()
        when (v) {
            is JSONObject -> for (k in v.keys()) m[k] = v.opt(k).takeUnless { it == JSONObject.NULL }
            is Map<*, *> -> for ((k, x) in v) m[k as? String ?: return null] = x.takeUnless { it == JSONObject.NULL }
            else -> return null
        }
        return m
    }

    /** The items of a JSON array, or null when `v` is not one; JSONObject.NULL becomes null. */
    private fun items(v: Any?): List<Any?>? = when (v) {
        is JSONArray -> List(v.length()) { i -> v.opt(i).takeUnless { it == JSONObject.NULL } }
        is List<*> -> v.map { x -> x.takeUnless { it == JSONObject.NULL } }
        else -> null
    }
}
