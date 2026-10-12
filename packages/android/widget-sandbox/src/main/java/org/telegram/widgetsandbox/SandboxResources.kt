// The JavaScript shipped with the library (built by `pnpm build:android` into
// src/main/assets/WidgetJS): the three bundles, the bundled libraries and the
// CDN URL patterns that map to them.

package org.telegram.widgetsandbox

import android.content.Context
import android.content.res.AssetManager
import org.json.JSONArray

class BundledLibrary(val name: String, val file: String, val patterns: List<Regex>)

class SandboxResources(private val assets: AssetManager) {
    companion object {
        @Volatile private var shared: SandboxResources? = null

        /** One instance per process; the bundles are read once. */
        fun get(context: Context): SandboxResources =
            shared ?: synchronized(this) { shared ?: SandboxResources(context.applicationContext.assets).also { shared = it } }
    }

    private fun read(path: String): String =
        try {
            assets.open("WidgetJS/$path").bufferedReader(Charsets.UTF_8).use { it.readText() }
        } catch (e: Exception) {
            throw IllegalStateException("WidgetSandbox: missing asset WidgetJS/$path — run `pnpm build:android`", e)
        }

    val sandboxBundle: String by lazy { read("sandbox-bundle.js") }
    val rendererBundle: String by lazy { read("renderer-bundle.js") }
    val validatorBundle: String by lazy { read("validator-bundle.js") }

    private fun readBytes(path: String): ByteArray? =
        try { assets.open("WidgetJS/$path").use { it.readBytes() } } catch (e: Exception) { null }

    /** Precompiled bytecode of the bundles (`.qbc` next to the `.js`, built together; null when not shipped). */
    val sandboxBytecode: ByteArray? by lazy { readBytes("sandbox-bundle.qbc") }
    val validatorBytecode: ByteArray? by lazy { readBytes("validator-bundle.qbc") }
    private val bytecodeCache = HashMap<String, ByteArray?>()
    fun libraryBytecode(lib: BundledLibrary): ByteArray? = synchronized(bytecodeCache) {
        bytecodeCache.getOrPut(lib.file) { readBytes("libs/" + lib.file.replace(Regex("\\.js$"), ".qbc")) }
    }

    val libraries: List<BundledLibrary> by lazy {
        val list = JSONArray(read("libraries.json"))
        (0 until list.length()).map { i ->
            val item = list.getJSONObject(i)
            val patterns = item.getJSONArray("patterns")
            BundledLibrary(
                item.getString("name"), item.getString("file"),
                (0 until patterns.length()).map { Regex(patterns.getString(it), RegexOption.IGNORE_CASE) },
            )
        }
    }

    private val libraryCache = HashMap<String, String>()

    /** Same resolution as src/host/loader.ts: known CDN URLs → bundled copy; anything else → null. */
    fun library(url: String): BundledLibrary? {
        var u = url.trim()
        if (u.startsWith("//")) u = "https:$u"
        val cut = u.indexOfFirst { it == '?' || it == '#' }
        if (cut >= 0) u = u.substring(0, cut)
        return libraries.firstOrNull { lib -> lib.patterns.any { it.containsMatchIn(u) } }
    }

    fun librarySource(lib: BundledLibrary): String? = synchronized(libraryCache) {
        libraryCache[lib.file] ?: try {
            read("libs/${lib.file}").also { libraryCache[lib.file] = it }
        } catch (e: IllegalStateException) {
            null
        }
    }
}
