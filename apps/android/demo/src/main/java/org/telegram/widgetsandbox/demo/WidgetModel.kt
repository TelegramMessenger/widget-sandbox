package org.telegram.widgetsandbox.demo

import android.content.Context
import android.view.HapticFeedbackConstants
import android.widget.Toast
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.ui.layout.boundsInWindow
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import org.telegram.widgetsandbox.WidgetAction
import org.telegram.widgetsandbox.WidgetDiagnostics
import org.telegram.widgetsandbox.WidgetHost
import org.telegram.widgetsandbox.WidgetTheme
import org.telegram.widgetsandbox.blitz.BlitzRenderer
import org.telegram.widgetsandbox.litehtml.LitehtmlRenderer
import org.telegram.widgetsandbox.RendererFactory

/**
 * The renderer the demo's widgets use (the **Renderer** picker on a widget screen, persisted as the
 * `renderer` preference): the product WebView renderer, or one of the native prototypes — Blitz
 * (renderer-blitz module, Rust) or litehtml (renderer-litehtml module, C++). The sandbox and the validator
 * are the same either way; dev tooling. `--es renderer webview|blitz|blitz_canvas|litehtml` sets it from adb.
 * `blitz_canvas` is Blitz's layout painted by android.graphics from a display list (CanvasScene.kt).
 */
enum class DemoRenderer(val title: String, val factory: RendererFactory?) {
    WEBVIEW("WebView", null), BLITZ("Blitz", BlitzRenderer.factory), BLITZ_CANVAS("Blitz·Canvas", BlitzRenderer.canvasFactory), LITEHTML("litehtml", LitehtmlRenderer.factory);

    val label get() = if (this == WEBVIEW) "sandbox" else "sandbox · $title renderer"

    companion object {
        const val PREF = "renderer"
        fun of(name: String?) = entries.firstOrNull { it.name.equals(name, ignoreCase = true) }
        /** The persisted choice; `blitz` was the Blitz switch before the picker existed. */
        fun load(prefs: android.content.SharedPreferences) = of(prefs.getString(PREF, null)) ?: if (prefs.getBoolean("blitz", false)) BLITZ else WEBVIEW
    }
}

/**
 * The demo's chat theme: the colour scheme only, so the renderer fills the palette from the
 * Telegram defaults. A client passes the bubble's own colours and button (docs/sdk-v1.md §5.1).
 */
fun demoTheme(dark: Boolean) = WidgetTheme(if (dark) "dark" else "light")

/** Observable façade over WidgetHost for Compose screens (= WidgetModel.swift). */
class WidgetModel(context: Context, html: String, theme: WidgetTheme, widthDp: Double, options: WidgetHost.Options = WidgetHost.Options(), val renderer: DemoRenderer = DemoRenderer.WEBVIEW, rendererFactory: RendererFactory? = null) {
    /** `rendererFactory` overrides the picker's renderer (the bench's `none` renderer, Bench.kt). */
    val host = WidgetHost(context, html, theme, widthDp, options, rendererFactory = rendererFactory ?: renderer.factory)
    var diagnostics by mutableStateOf(WidgetDiagnostics())
        private set

    init {
        host.onChange = { diagnostics = it }
        // What a client does with the actions (docs/sdk-v1.md §5.2, §5.3), reduced to dev tooling:
        // haptics play, links are shown and not opened (the panel lists every action).
        host.onAction = { action ->
            when (action) {
                is WidgetAction.Haptic -> host.view.performHapticFeedback(
                    when (action.kind) {
                        "impact" -> HapticFeedbackConstants.VIRTUAL_KEY
                        "notification" -> HapticFeedbackConstants.LONG_PRESS
                        else -> HapticFeedbackConstants.CLOCK_TICK
                    },
                )
                is WidgetAction.OpenLink -> Toast.makeText(context, "openLink: ${action.url}", Toast.LENGTH_SHORT).show()
                is WidgetAction.OpenTelegramLink -> Toast.makeText(context, "openTelegramLink: ${action.url}", Toast.LENGTH_SHORT).show()
                WidgetAction.Ready -> {}
            }
        }
    }

    fun setTheme(theme: WidgetTheme) = host.setTheme(theme)
    fun destroy() = host.destroy()

    // The owner's side of the off-screen pause: the bubble scrolled out of the window, or the
    // activity stopped, counts as hidden (the view's own attach state is tracked by the host).
    private var onScreen = true
    private var started = true
    fun setOnScreen(v: Boolean) { onScreen = v; host.setVisible(onScreen && started) }
    fun setStarted(v: Boolean) { started = v; host.setVisible(onScreen && started) }
}

/** WebView | Blitz | litehtml, the renderer picker above the bubble (= RendererPicker in DemoPlatform.swift). */
@Composable
fun RendererPicker(selected: DemoRenderer, onSelect: (DemoRenderer) -> Unit) {
    SingleChoiceSegmentedButtonRow {
        DemoRenderer.entries.forEachIndexed { i, r ->
            SegmentedButton(
                selected = r == selected,
                onClick = { onSelect(r) },
                shape = SegmentedButtonDefaults.itemShape(index = i, count = DemoRenderer.entries.size),
                label = { Text(r.title, fontSize = 12.sp) },
            )
        }
    }
}

/** The widget at chat-bubble width; its height follows the renderer's size reports. */
@Composable
fun WidgetView(model: WidgetModel, widthDp: Int = 360) {
    // key: a new model (Restart) must get a new AndroidView, or the factory is never re-run and the
    // destroyed host's view stays on screen.
    key(model) {
        AndroidView(
            factory = { model.host.view },
            modifier = Modifier.width(widthDp.dp).height(maxOf(model.diagnostics.height, 24.0).dp)
                // boundsInWindow is clipped by the scrolling parents: empty means scrolled out of view.
                .onGloballyPositioned { val b = it.boundsInWindow(); model.setOnScreen(b.width > 0f && b.height > 0f) },
        )
    }
    @Suppress("DEPRECATION")
    val lifecycle = androidx.compose.ui.platform.LocalLifecycleOwner.current.lifecycle
    DisposableEffect(lifecycle, model) {
        val observer = LifecycleEventObserver { _, event ->
            when (event) {
                Lifecycle.Event.ON_START -> model.setStarted(true)
                Lifecycle.Event.ON_STOP -> model.setStarted(false)
                else -> {}
            }
        }
        lifecycle.addObserver(observer)
        onDispose { lifecycle.removeObserver(observer) }
    }
}

@Composable
fun ChatBubble(model: WidgetModel) {
    Column {
        Text(model.renderer.label, fontSize = 10.sp, color = MaterialTheme.colorScheme.onSurfaceVariant, modifier = Modifier.padding(start = 8.dp))
        Column(
            modifier = Modifier.clip(RoundedCornerShape(14.dp)).background(MaterialTheme.colorScheme.surface).padding(8.dp),
        ) { WidgetView(model) }
        // What a product shows under the bubble: the widget stays on screen (frozen) with a one-line reason.
        val d = model.diagnostics
        val line = d.dead?.let { "Widget stopped: " + it.lineSequence().first() } ?: d.lastError?.lineSequence()?.first()
        if (line != null) Text(line, fontSize = 10.sp, color = Color.Red, maxLines = 1, modifier = Modifier.padding(start = 8.dp, top = 4.dp))
    }
}

val blockedKinds = listOf("url-blocked", "network-blocked", "navigation-blocked", "tag-dropped", "script-skipped", "css-dropped")
private val redKinds = setOf("url-blocked", "network-blocked", "navigation-blocked", "protocol-violation", "renderer-violation")

fun blockedCount(d: WidgetDiagnostics) = blockedKinds.sumOf { d.diags[it]?.count ?: 0 }

@Composable
fun DiagnosticsPanel(model: WidgetModel) {
    val d = model.diagnostics
    Column(
        modifier = Modifier.fillMaxWidth().clip(RoundedCornerShape(8.dp)).background(MaterialTheme.colorScheme.surfaceVariant).padding(8.dp),
    ) {
        val status = d.dead?.let { "✖ terminated: $it" } ?: if (d.loaded) "● loaded" else "… loading"
        Text(status, fontSize = 12.sp, color = if (d.dead != null) Color.Red else if (d.loaded) Color(0xFF2E7D32) else MaterialTheme.colorScheme.onSurfaceVariant)
        Text(
            String.format(
                "JS %.0f ms in %d tasks · %.1f MB · %d ops · %d nodes · %d sync queries (%.0f ms, %d cache hits) · missed %d",
                d.stats.jsMs, d.stats.tasks, d.stats.memoryBytes / 1048576.0, d.stats.ops, d.nodes, d.stats.queries, d.stats.queryMs, d.stats.queryCacheHits, d.stats.missedMutations,
            ),
            fontSize = 10.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        for (k in d.diags.keys.sorted()) {
            val e = d.diags[k]!!
            Text("$k ×${e.count}: ${e.details.takeLast(2).joinToString(" · ")}", fontSize = 10.sp, maxLines = 2, color = if (k in redKinds) Color.Red else Color(0xFFEF6C00))
        }
        for (a in d.actions.takeLast(10)) Text("action: $a", fontSize = 10.sp)
        for (l in d.logs.takeLast(30)) {
            Text("[${l.first}] ${l.second}", fontSize = 10.sp, fontFamily = FontFamily.Monospace, maxLines = 4, color = if (l.first == "error") Color.Red else MaterialTheme.colorScheme.onSurface)
        }
    }
}
