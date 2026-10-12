// The renderer as WidgetHost sees it: a View plus the JSON message contract of
// docs/protocol.md (host → renderer: ops / query / listen / preventDefault /
// requestFrame / frozen; renderer → host: ready / event / gesture / result /
// size / resize / frame / media / violation). RendererWebView is the product
// renderer; other implementations (the Blitz prototype) plug in through
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
    /** Answer to the last primary pointerdown's `gesture` message (see RendererWebView). */
    fun gestureDecision(wants: Boolean)
    fun destroy()
}

/** Builds a renderer for one widget: width in CSS px (= dp) and the chat theme. */
fun interface RendererFactory {
    fun create(context: Context, resources: SandboxResources, theme: String, widthDp: Double): Renderer
}
