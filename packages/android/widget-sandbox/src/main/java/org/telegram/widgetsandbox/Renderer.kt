// The renderer as WidgetHost sees it: a View plus the JSON message contract of
// docs/protocol.md (host → renderer: ops / query / listen / preventDefault /
// requestFrame / theme / frozen; renderer → host: ready / event / gesture /
// result / size / resize / frame / media / violation). `theme` carries the
// WidgetTheme (docs/sdk-v1.md §3.4) at load and on every change; a renderer
// that has no use for it ignores it, as any unknown type. `gesture` is
// `{wants, x, y}` (`wants` = `y`). RendererWebView is the product renderer;
// other implementations (the Blitz and litehtml prototypes) plug in through
// WidgetHost's `rendererFactory`. All members are main-thread.

package org.telegram.widgetsandbox

import android.content.Context
import android.view.View

interface Renderer {
    val view: View
    /** Renderer → host messages (JSON), delivered on the main thread. */
    var onMessage: ((String) -> Unit)?
    var onDiag: ((String, String) -> Unit)?

    /** Starts the renderer; it announces itself with `{type:'ready'}`. */
    fun load()
    /** Host → renderer message (JSON). */
    fun send(json: String)
    /** Trusted script evaluated in the renderer (tests / diagnostics only); renderers without script answer `null`. */
    fun evaluate(script: String, callback: ((String?) -> Unit)?)
    /** Answer to the last primary pointerdown's `gesture` message, its `wants` (= `y`, see RendererWebView); WidgetHost also reports `x`/`y` to `onGestureClaim`. */
    fun gestureDecision(wants: Boolean)
    fun destroy()
}

/** Builds a renderer for one widget: width in CSS px (= dp) and the chat's colour scheme ("light" | "dark"; the palette follows as a `theme` message). */
fun interface RendererFactory {
    fun create(context: Context, resources: SandboxResources, theme: String, widthDp: Double): Renderer
}
