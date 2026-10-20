// Intl delegation (pure formatting, no I/O): the sandbox shim forwards
// Intl.* / toLocale*String calls as `__host('intl', json)`; the request shape
// is `{kind, locale, opts, value, method, unit, a, b}` (see src/sandbox).
// Same kinds and fallbacks as the iOS IntlBridge; `numberParts` / `dateParts`
// (formatToParts, in the widget profile) come from ICU's field iterators.

package org.telegram.widgetsandbox

import android.icu.text.Collator
import android.icu.text.DateFormat
import android.icu.text.ListFormatter
import android.icu.text.NumberFormat
import android.icu.text.PluralRules
import android.icu.text.RelativeDateTimeFormatter
import android.icu.util.Currency
import android.icu.util.TimeZone
import org.json.JSONArray
import org.json.JSONObject
import java.text.AttributedCharacterIterator
import java.text.CharacterIterator
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
                "number" -> numberFormat(locale, opts).format(value)
                "numberParts" -> parts(numberFormat(locale, opts).formatToCharacterIterator(value), ::numberPart).toString()
                "date", "dateMethod" -> dateFormat(req, locale, opts).format(Date(value.toLong()))
                "dateParts" -> parts(dateFormat(req, locale, opts).formatToCharacterIterator(Date(value.toLong())), ::datePart).toString()
                "plural" -> PluralRules.forLocale(locale, if (opts.optString("type") == "ordinal") PluralRules.PluralType.ORDINAL else PluralRules.PluralType.CARDINAL).select(value)
                "dateOptions" -> {
                    val f = dateFormat(req, locale, opts)
                    val out = JSONObject()
                    for (key in opts.keys()) opts.opt(key).takeIf { it is String || it is Number || it is Boolean }?.let { out.put(key, it) }
                    out.put("locale", locale.toLanguageTag()).put("calendar", "gregory").put("numberingSystem", "latn").put("timeZone", f.timeZone.id).toString()
                }
                "relative" -> {
                    val style = when (opts.optString("style")) {
                        "short" -> RelativeDateTimeFormatter.Style.SHORT
                        "narrow" -> RelativeDateTimeFormatter.Style.NARROW
                        else -> RelativeDateTimeFormatter.Style.LONG
                    }
                    val f = RelativeDateTimeFormatter.getInstance(android.icu.util.ULocale.forLocale(locale), null, style, android.icu.text.DisplayContext.CAPITALIZATION_NONE)
                    val unit = req.optString("unit", "second").removeSuffix("s")
                    if (android.os.Build.VERSION.SDK_INT >= 28) {
                        val ru = when (unit) {
                            "minute" -> RelativeDateTimeFormatter.RelativeDateTimeUnit.MINUTE
                            "hour" -> RelativeDateTimeFormatter.RelativeDateTimeUnit.HOUR
                            "day" -> RelativeDateTimeFormatter.RelativeDateTimeUnit.DAY
                            "week" -> RelativeDateTimeFormatter.RelativeDateTimeUnit.WEEK
                            "month" -> RelativeDateTimeFormatter.RelativeDateTimeUnit.MONTH
                            "quarter" -> RelativeDateTimeFormatter.RelativeDateTimeUnit.QUARTER
                            "year" -> RelativeDateTimeFormatter.RelativeDateTimeUnit.YEAR
                            else -> RelativeDateTimeFormatter.RelativeDateTimeUnit.SECOND
                        }
                        return if (opts.optString("numeric") == "auto") f.format(value, ru) else f.formatNumeric(value, ru)
                    }
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

    private fun numberFormat(locale: Locale, opts: JSONObject): NumberFormat {
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
        return f
    }

    /** JS `formatToParts`: one `{type, value}` per run of characters the iterator gives the same field. */
    private fun parts(it: AttributedCharacterIterator, type: (Set<AttributedCharacterIterator.Attribute>, Char) -> String): JSONArray {
        val out = JSONArray()
        var current: String? = null
        val text = StringBuilder()
        var c = it.first()
        while (c != CharacterIterator.DONE) {
            val t = type(it.attributes.keys, c)
            if (t != current && current != null) {
                out.put(JSONObject().put("type", current).put("value", text.toString()))
                text.setLength(0)
            }
            current = t
            text.append(c)
            c = it.next()
        }
        if (current != null) out.put(JSONObject().put("type", current).put("value", text.toString()))
        return out
    }

    private fun numberPart(keys: Set<AttributedCharacterIterator.Attribute>, c: Char): String = when {
        NumberFormat.Field.GROUPING_SEPARATOR in keys -> "group"
        NumberFormat.Field.DECIMAL_SEPARATOR in keys -> "decimal"
        NumberFormat.Field.FRACTION in keys -> "fraction"
        NumberFormat.Field.EXPONENT_SYMBOL in keys -> "exponentSeparator"
        NumberFormat.Field.EXPONENT_SIGN in keys -> "exponentMinusSign"
        NumberFormat.Field.EXPONENT in keys -> "exponentInteger"
        NumberFormat.Field.INTEGER in keys -> "integer"
        NumberFormat.Field.SIGN in keys -> if (c == '+') "plusSign" else "minusSign"
        NumberFormat.Field.CURRENCY in keys -> "currency"
        NumberFormat.Field.PERCENT in keys -> "percentSign"
        else -> "literal"
    }

    private fun datePart(keys: Set<AttributedCharacterIterator.Attribute>, c: Char): String = when {
        DateFormat.Field.ERA in keys -> "era"
        DateFormat.Field.YEAR in keys || DateFormat.Field.EXTENDED_YEAR in keys -> "year"
        DateFormat.Field.MONTH in keys -> "month"
        DateFormat.Field.DAY_OF_MONTH in keys -> "day"
        DateFormat.Field.DAY_OF_WEEK in keys -> "weekday"
        DateFormat.Field.AM_PM in keys -> "dayPeriod"
        DateFormat.Field.HOUR0 in keys || DateFormat.Field.HOUR1 in keys || DateFormat.Field.HOUR_OF_DAY0 in keys || DateFormat.Field.HOUR_OF_DAY1 in keys -> "hour"
        DateFormat.Field.MINUTE in keys -> "minute"
        DateFormat.Field.SECOND in keys -> "second"
        DateFormat.Field.MILLISECOND in keys -> "fractionalSecond"
        DateFormat.Field.TIME_ZONE in keys -> "timeZoneName"
        else -> "literal"
    }

    private fun dateFormat(req: JSONObject, locale: Locale, opts: JSONObject): DateFormat {
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
        opts.optString("timeZone").takeIf { it.isNotEmpty() }?.let { f.timeZone = TimeZone.getTimeZone(it) }
        return f
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
