package org.telegram.widgetsandbox.demo

import android.content.Context

/**
 * Corpus files copied into the app assets by `pnpm build:android` (Corpus/compat, Corpus/attacks,
 * Corpus/generated/{with,without}-profile, Corpus/htmlbubbles-ported/<chat> — Telegram's demos as classic widgets,
 * Corpus/common/<batch> — the renderer-comparison corpus on the common CSS profile).
 */
object Corpus {
    val generatedGroups = listOf("generated/with-profile", "generated/without-profile")

    /** One group per demo chat (corpus/htmlbubbles-ported/README.md). */
    fun htmlbubblesGroups(context: Context): List<String> = subGroups(context, "htmlbubbles-ported")

    /** The renderer-comparison corpus, one group per batch (corpus/common/README.md). */
    fun commonGroups(context: Context): List<String> = subGroups(context, "common")

    private fun subGroups(context: Context, parent: String): List<String> =
        (context.assets.list("Corpus/$parent") ?: emptyArray()).filter { !it.endsWith(".md") }.sorted().map { "$parent/$it" }

    fun files(context: Context, group: String): List<String> =
        (context.assets.list("Corpus/$group") ?: emptyArray()).filter { it.endsWith(".html") }.sorted()

    fun html(context: Context, group: String, file: String): String =
        try {
            context.assets.open("Corpus/$group/$file").bufferedReader(Charsets.UTF_8).use { it.readText() }
        } catch (e: Exception) {
            "<p>missing $group/$file — run pnpm build:android</p>"
        }
}
