// The JavaScript shipped with the library (built by `pnpm build:android` into
// src/main/assets/WidgetJS): the three bundles and their bytecode.

package org.telegram.widgetsandbox

import android.content.Context
import android.content.res.AssetManager

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
}
