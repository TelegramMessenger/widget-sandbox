// Intl delegation (pure formatting, no I/O): the sandbox shim forwards
// Intl.* / toLocale*String calls as `__host('intl', json)`; the request shape
// is `{kind, locale, opts, value, method, unit, a, b}` (see src/sandbox).
// Same kinds and fallbacks as the iOS IntlBridge; `*Parts` kinds return `[]`.

package org.telegram.widgetsandbox

import android.icu.text.Collator
import android.icu.text.DateFormat
import android.icu.text.ListFormatter
import android.icu.text.NumberFormat
import android.icu.text.PluralRules
import android.icu.text.RelativeDateTimeFormatter
import android.icu.util.Currency
import org.json.JSONArray
import org.json.JSONObject
import java.util.Date
import java.util.Locale

object IntlBridge {
    fun format(requestJson: String): String {
        val req = try { JSONObject(requestJson) } catch (e: Exception) { return "" }
        val kind = req.optString("kind")
        val locale = localeOf(req.opt("locale"))
        val opts = req.optJSONObject("opts") ?: JSONObject()
        val value = req.optDouble("value", 0.0)
        return try {
            when (kind) {
                "number" -> {
                    val f = when (opts.optString("style")) {
                        "currency" -> NumberFormat.getCurrencyInstance(locale).also { c ->
                            opts.optString("currency").takeIf { it.isNotEmpty() }?.let { code -> runCatching { c.currency = Currency.getInstance(code) } }
                        }
                        "percent" -> NumberFormat.getPercentInstance(locale)
                        else -> NumberFormat.getNumberInstance(locale)
                    }
                    if (opts.has("minimumFractionDigits")) f.minimumFractionDigits = opts.optInt("minimumFractionDigits")
                    if (opts.has("maximumFractionDigits")) f.maximumFractionDigits = opts.optInt("maximumFractionDigits")
                    if (opts.has("useGrouping")) f.isGroupingUsed = opts.optBoolean("useGrouping")
                    f.format(value)
                }
                "date", "dateMethod" -> {
                    val date = Date(value.toLong())
                    val method = req.optString("method")
                    val wantsDate = listOf("weekday", "year", "month", "day", "dateStyle").any { opts.has(it) }
                    val wantsTime = listOf("hour", "minute", "second", "timeStyle").any { opts.has(it) }
                    val f: DateFormat = if (wantsDate || wantsTime) {
                        val skeleton = StringBuilder()
                        opts.optString("weekday").takeIf { it.isNotEmpty() }?.let { skeleton.append(if (it == "long") "EEEE" else "EEE") }
                        opts.optString("month").takeIf { it.isNotEmpty() }?.let { skeleton.append(when (it) { "long" -> "MMMM"; "short" -> "MMM"; "numeric" -> "M"; else -> "MM" }) }
                        if (opts.has("day")) skeleton.append("d")
                        if (opts.has("year")) skeleton.append("y")
                        if (opts.has("hour")) skeleton.append("j")
                        if (opts.has("minute")) skeleton.append("mm")
                        if (opts.has("second")) skeleton.append("ss")
                        if (skeleton.isEmpty()) {
                            val ds = style(opts.optString("dateStyle"))
                            val ts = style(opts.optString("timeStyle"))
                            when {
                                ds != null && ts != null -> DateFormat.getDateTimeInstance(ds, ts, locale)
                                ds != null -> DateFormat.getDateInstance(ds, locale)
                                ts != null -> DateFormat.getTimeInstance(ts, locale)
                                else -> DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.MEDIUM, locale)
                            }
                        } else {
                            DateFormat.getInstanceForSkeleton(skeleton.toString(), locale)
                        }
                    } else {
                        when (method) {
                            "toLocaleTimeString" -> DateFormat.getTimeInstance(DateFormat.MEDIUM, locale)
                            "toLocaleDateString" -> DateFormat.getDateInstance(DateFormat.MEDIUM, locale)
                            else -> DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.MEDIUM, locale)
                        }
                    }
                    f.format(date)
                }
                "plural" -> PluralRules.forLocale(locale).select(value)
                "relative" -> {
                    val f = RelativeDateTimeFormatter.getInstance(locale)
                    val unit = req.optString("unit", "second").removeSuffix("s")
                    val u = when (unit) {
                        "minute" -> RelativeDateTimeFormatter.RelativeUnit.MINUTES
                        "hour" -> RelativeDateTimeFormatter.RelativeUnit.HOURS
                        "day" -> RelativeDateTimeFormatter.RelativeUnit.DAYS
                        "week" -> RelativeDateTimeFormatter.RelativeUnit.WEEKS
                        "month" -> RelativeDateTimeFormatter.RelativeUnit.MONTHS
                        "year" -> RelativeDateTimeFormatter.RelativeUnit.YEARS
                        else -> RelativeDateTimeFormatter.RelativeUnit.SECONDS
                    }
                    f.format(Math.abs(value), if (value < 0) RelativeDateTimeFormatter.Direction.LAST else RelativeDateTimeFormatter.Direction.NEXT, u)
                }
                "list" -> {
                    val arr = req.optJSONArray("value") ?: JSONArray()
                    ListFormatter.getInstance(locale).format((0 until arr.length()).map { arr.opt(it).toString() })
                }
                "compare" -> Integer.signum(Collator.getInstance(locale).compare(req.optString("a"), req.optString("b"))).toString()
                "upper" -> req.optString("value").uppercase(locale)
                "lower" -> req.optString("value").lowercase(locale)
                else -> ""
            }
        } catch (e: Exception) {
            ""
        }
    }

    private fun localeOf(v: Any?): Locale {
        val tag = when (v) {
            is String -> v
            is JSONArray -> v.optString(0, "")
            else -> ""
        }
        return if (tag.isEmpty()) Locale.getDefault() else Locale.forLanguageTag(tag)
    }

    private fun style(s: String): Int? = when (s) {
        "full" -> DateFormat.FULL
        "long" -> DateFormat.LONG
        "medium" -> DateFormat.MEDIUM
        "short" -> DateFormat.SHORT
        else -> null
    }
}
