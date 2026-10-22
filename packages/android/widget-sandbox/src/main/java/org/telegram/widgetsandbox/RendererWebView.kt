// The renderer: an android.webkit.WebView that executes only our renderer
// bundle (nonce CSP), served from a per-widget https origin by
// shouldInterceptRequest, with every other request answered by an empty 403,
// every navigation cancelled, network loads blocked at the WebView level, and
// a document-start script that removes the network APIs the renderer never
// uses (defense in depth — invariant 6).
//
// Transport: renderer → host through the `tw` @JavascriptInterface object
// (JSON), host → renderer through `__twReceive(json)` evaluated on the page.
// Both callbacks (`onMessage`, `onDiag`) are delivered on the main thread.

package org.telegram.widgetsandbox

import android.annotation.SuppressLint
import android.content.Context
import android.content.res.Configuration
import android.graphics.Color
import android.net.http.SslError
import android.os.Handler
import android.os.Looper
import android.os.Message
import android.util.Base64
import android.view.ContextThemeWrapper
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.webkit.ClientCertRequest
import android.webkit.ConsoleMessage
import android.webkit.CookieManager
import android.webkit.GeolocationPermissions
import android.webkit.HttpAuthHandler
import android.webkit.JavascriptInterface
import android.webkit.JsPromptResult
import android.webkit.JsResult
import android.webkit.PermissionRequest
import android.webkit.RenderProcessGoneDetail
import android.webkit.SslErrorHandler
import android.webkit.ValueCallback
import android.webkit.WebChromeClient
import android.webkit.WebResourceError
import android.webkit.WebResourceRequest
import android.webkit.WebResourceResponse
import android.webkit.WebSettings
import android.webkit.WebView
import android.webkit.WebViewClient
import androidx.webkit.WebSettingsCompat
import androidx.webkit.WebViewCompat
import androidx.webkit.WebViewFeature
import java.io.ByteArrayInputStream
import java.security.SecureRandom
import java.util.UUID

@SuppressLint("SetJavaScriptEnabled")
class RendererWebView(context: Context, rendererBundle: String, theme: String) : Renderer {
    companion object {
        /** Every widget gets its own origin: https://w-<uuid>.tgwidget.invalid/ (.invalid never resolves; nothing is fetched anyway). */
        const val ORIGIN_SUFFIX = ".tgwidget.invalid"

        fun csp(nonce: String): String = listOf(
            "default-src 'none'", "script-src 'nonce-$nonce'", "style-src 'unsafe-inline'", "img-src data: blob:", "font-src data:",
            "media-src data: blob:", "connect-src 'none'", "frame-src 'none'", "child-src 'none'", "worker-src 'none'", "form-action 'none'", "base-uri 'none'",
        ).joinToString("; ")

        fun document(rendererJs: String, nonce: String, csp: String): String {
            val js = rendererJs.replace(Regex("</script", RegexOption.IGNORE_CASE), "<\\/script").replace("<!--", "<\\!--")
            return "<!DOCTYPE html><html><head><meta charset=\"utf-8\">" +
                "<meta http-equiv=\"Content-Security-Policy\" content=\"$csp\">" +
                "<meta http-equiv=\"x-dns-prefetch-control\" content=\"off\">" +
                "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">" +
                "<script nonce=\"$nonce\">$js</script></head><body></body></html>"
        }

        fun makeNonce(): String {
            val bytes = ByteArray(18)
            SecureRandom().nextBytes(bytes)
            return Base64.encodeToString(bytes, Base64.NO_WRAP).replace(Regex("[+/=]"), "x")
        }

        /** Removes network-capable APIs from the renderer window. Our renderer code uses none of them. */
        const val HARDENING_SCRIPT = """
            (function () {
              for (const k of ['fetch', 'XMLHttpRequest', 'WebSocket', 'EventSource', 'RTCPeerConnection', 'webkitRTCPeerConnection', 'RTCDataChannel', 'WebTransport', 'Worker', 'SharedWorker', 'importScripts', 'open']) {
                try { Object.defineProperty(window, k, { value: undefined, writable: false, configurable: false }); } catch (e) {}
              }
              try { Object.defineProperty(navigator, 'sendBeacon', { value: undefined, writable: false, configurable: false }); } catch (e) {}
              try { Object.defineProperty(navigator, 'serviceWorker', { value: undefined, writable: false, configurable: false }); } catch (e) {}
            })();
        """

        /** JSON string → JavaScript string literal (also escapes U+2028/2029 and `<`). */
        fun jsStringLiteral(s: String): String {
            val out = StringBuilder(s.length + 16)
            out.append('"')
            for (ch in s) {
                when (ch) {
                    '"' -> out.append("\\\"")
                    '\\' -> out.append("\\\\")
                    '\n' -> out.append("\\n")
                    '\r' -> out.append("\\r")
                    ' ' -> out.append("\\u2028")
                    ' ' -> out.append("\\u2029")
                    '<' -> out.append("\\x3c")
                    else -> if (ch < ' ') out.append(String.format("\\u%04x", ch.code)) else out.append(ch)
                }
            }
            return out.append('"').toString()
        }

        /**
         * A context whose uiMode matches the widget theme, so the WebView's own
         * `prefers-color-scheme` agrees with the chat. It wraps the caller's context
         * instead of replacing it (`createConfigurationContext` returns a bare
         * ContextImpl): Chromium looks for the Activity behind the WebView's context
         * to show its own dialogs — the colour and date pickers and `<select>`
         * popups silently do nothing without one. The theme resource matters too:
         * with targetSdk ≥ 33 Chromium reads `prefers-color-scheme` from the
         * context theme's `isLightTheme` attribute (uiMode is only the fallback),
         * and a wrapper with theme 0 resolves to the always-light
         * DeviceDefault.Light.DarkActionBar.
         */
        fun themedContext(context: Context, theme: String): Context {
            val dark = theme == "dark"
            val config = Configuration()
            config.uiMode = if (dark) Configuration.UI_MODE_NIGHT_YES else Configuration.UI_MODE_NIGHT_NO
            val themeRes = if (dark) android.R.style.Theme_DeviceDefault else android.R.style.Theme_DeviceDefault_Light
            return ContextThemeWrapper(context, themeRes).apply { applyOverrideConfiguration(config) }
        }
    }

    val webView: WebView
    override val view: View get() = webView
    override var onMessage: ((String) -> Unit)? = null
    override var onDiag: ((String, String) -> Unit)? = null

    val documentUrl: String = "https://w-${UUID.randomUUID().toString().lowercase()}$ORIGIN_SUFFIX/"
    private val main = Handler(Looper.getMainLooper())
    private val html: ByteArray
    private val csp: String
    @Volatile private var destroyed = false
    @Volatile private var documentLoaded = false

    init {
        val nonce = makeNonce()
        csp = csp(nonce)
        html = document(rendererBundle, nonce, csp).toByteArray(Charsets.UTF_8)

        webView = GestureWebView(themedContext(context, theme))
        val s = webView.settings
        s.javaScriptEnabled = true
        // Belt: no network resource may be loaded whatever the request path does
        // (legal without the INTERNET permission; only setting false needs it).
        s.blockNetworkLoads = true
        s.allowFileAccess = false
        s.allowContentAccess = false
        s.domStorageEnabled = false
        s.databaseEnabled = false
        s.setGeolocationEnabled(false)
        s.javaScriptCanOpenWindowsAutomatically = false
        s.setSupportMultipleWindows(false)
        s.mixedContentMode = WebSettings.MIXED_CONTENT_NEVER_ALLOW
        s.cacheMode = WebSettings.LOAD_NO_CACHE
        s.mediaPlaybackRequiresUserGesture = false
        s.setSupportZoom(false)
        s.builtInZoomControls = false
        s.displayZoomControls = false
        s.useWideViewPort = false
        s.loadWithOverviewMode = false
        s.setNeedInitialFocus(false)
        if (WebViewFeature.isFeatureSupported(WebViewFeature.SAFE_BROWSING_ENABLE)) WebSettingsCompat.setSafeBrowsingEnabled(s, false)
        if (WebViewFeature.isFeatureSupported(WebViewFeature.ALGORITHMIC_DARKENING)) WebSettingsCompat.setAlgorithmicDarkeningAllowed(s, false)
        CookieManager.getInstance().setAcceptCookie(false)
        CookieManager.getInstance().setAcceptThirdPartyCookies(webView, false)

        webView.setBackgroundColor(Color.TRANSPARENT)
        webView.isVerticalScrollBarEnabled = false
        webView.isHorizontalScrollBarEnabled = false
        webView.overScrollMode = View.OVER_SCROLL_NEVER
        webView.isLongClickable = false
        webView.setOnLongClickListener { true }

        // Register the bridge, the clients and the document-start script BEFORE loadUrl.
        webView.webViewClient = Client()
        webView.webChromeClient = Chrome()
        webView.addJavascriptInterface(Bridge(), "tw")
        if (WebViewFeature.isFeatureSupported(WebViewFeature.DOCUMENT_START_SCRIPT)) {
            WebViewCompat.addDocumentStartJavaScript(webView, HARDENING_SCRIPT, setOf("*"))
        } else {
            diag("renderer", "document-start script unsupported by this WebView; relying on CSP + request interception")
        }
        webView.setDownloadListener { url, _, _, _, _ -> diag("navigation-blocked", "download → ${url.take(120)}") }
    }

    /** Loads the renderer document. Main thread. */
    override fun load() {
        webView.loadUrl(documentUrl)
    }

    /** Host → renderer. Must be called on the main thread. */
    override fun send(json: String) {
        if (destroyed) return
        webView.evaluateJavascript("__twReceive(${jsStringLiteral(json)});undefined", null)
    }

    /** Evaluates trusted JavaScript in the renderer (tests / diagnostics only). Main thread. */
    override fun evaluate(script: String, callback: ((String?) -> Unit)?) {
        if (destroyed) return
        webView.evaluateJavascript(script, callback?.let { cb -> ValueCallback<String> { cb(it) } })
    }

    override fun destroy() {
        if (destroyed) return
        destroyed = true
        webView.removeJavascriptInterface("tw")
        webView.stopLoading()
        (webView.parent as? ViewGroup)?.removeView(webView)
        webView.destroy()
    }

    /**
     * Renderer's answer to the last primary pointerdown (`gesture` message): the
     * widget wants the touch gesture (touch-action forbids vertical panning, or
     * it prevents pointerdown there). Until the answer arrives the parent is
     * not allowed to intercept; a `false` (or 150 ms of silence) hands the
     * gesture back to the scrolling container. Main thread.
     */
    override fun gestureDecision(wants: Boolean) = gestureDecision(WidgetGestureClaim(x = false, y = wants))

    /**
     * The whole claim: the parent stays out of the press when the widget claims
     * vertical drags (the list must not scroll) or holds (a parent's long-press
     * recognizer that intercepts must stand back too).
     * Horizontal drags are the client's own recognizers' (WidgetHost.onGesture).
     */
    override fun gestureDecision(claim: WidgetGestureClaim) {
        (webView as GestureWebView).decide(claim.y || claim.hold)
    }

    /**
     * A WebView inside a scrolling container (chat list) loses the touch to the
     * container as soon as the finger moves past the slop: Android cannot see
     * `touch-action` inside the page. Deny interception from ACTION_DOWN and
     * release it only when the renderer says the widget does not want the gesture.
     */
    private class GestureWebView(context: Context) : WebView(context) {
        private var decided = false
        private val timeout = Runnable { if (!decided) release() }

        private fun release() {
            decided = true
            parent?.requestDisallowInterceptTouchEvent(false)
        }

        fun decide(wants: Boolean) {
            removeCallbacks(timeout)
            if (decided) return
            if (wants) decided = true else release()
        }

        override fun onTouchEvent(event: MotionEvent): Boolean {
            when (event.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    decided = false
                    parent?.requestDisallowInterceptTouchEvent(true)
                    removeCallbacks(timeout)
                    postDelayed(timeout, GESTURE_DECISION_TIMEOUT_MS)
                }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> removeCallbacks(timeout)
            }
            return super.onTouchEvent(event)
        }

        companion object {
            const val GESTURE_DECISION_TIMEOUT_MS = 150L
        }
    }

    private fun diag(kind: String, detail: String) {
        if (Looper.myLooper() === Looper.getMainLooper()) onDiag?.invoke(kind, detail)
        else main.post { if (!destroyed) onDiag?.invoke(kind, detail) }
    }

    // MARK: renderer → host (called on the WebView's JavaBridge thread, delivered in order on main)

    inner class Bridge {
        @JavascriptInterface
        fun postMessage(json: String) {
            main.post { if (!destroyed) onMessage?.invoke(json) }
        }
    }

    // MARK: serving the document, refusing everything else

    private inner class Client : WebViewClient() {
        /** Runs on a WebView IO thread. */
        override fun shouldInterceptRequest(view: WebView, request: WebResourceRequest): WebResourceResponse {
            val url = request.url.toString()
            if (request.isForMainFrame && url == documentUrl) {
                val headers = mapOf(
                    "Content-Security-Policy" to csp,
                    "X-DNS-Prefetch-Control" to "off",
                    "Cache-Control" to "no-store",
                    "Referrer-Policy" to "no-referrer",
                )
                return WebResourceResponse("text/html", "utf-8", 200, "OK", headers, ByteArrayInputStream(html))
            }
            diag("url-blocked", "renderer subresource request refused: ${url.take(120)}")
            return WebResourceResponse("text/plain", "utf-8", 403, "Blocked", mapOf("Content-Length" to "0"), ByteArrayInputStream(ByteArray(0)))
        }

        override fun shouldOverrideUrlLoading(view: WebView, request: WebResourceRequest): Boolean {
            diag("navigation-blocked", "${if (request.hasGesture()) "gesture" else "script"} → ${request.url.toString().take(120)}")
            return true
        }

        override fun onPageFinished(view: WebView, url: String) {
            if (url == documentUrl) documentLoaded = true
        }

        override fun onReceivedError(view: WebView, request: WebResourceRequest, error: WebResourceError) {
            if (request.isForMainFrame) diag("renderer", "document load failed: ${error.errorCode} ${error.description}")
        }

        override fun onReceivedHttpError(view: WebView, request: WebResourceRequest, errorResponse: WebResourceResponse) {
            if (request.isForMainFrame) diag("renderer", "document http error: ${errorResponse.statusCode}")
        }

        override fun onReceivedSslError(view: WebView, handler: SslErrorHandler, error: SslError) = handler.cancel()
        override fun onReceivedHttpAuthRequest(view: WebView, handler: HttpAuthHandler, host: String, realm: String) = handler.cancel()
        override fun onReceivedClientCertRequest(view: WebView, request: ClientCertRequest) = request.cancel()
        override fun onFormResubmission(view: WebView, dontResend: Message, resend: Message) = dontResend.sendToTarget()

        override fun onRenderProcessGone(view: WebView, detail: RenderProcessGoneDetail): Boolean {
            diag("renderer", "web content process terminated (crash=${detail.didCrash()})")
            (view.parent as? ViewGroup)?.removeView(view)
            return true // handled: do not kill the app
        }
    }

    // MARK: UI delegate — no popups, no dialogs, no permissions

    private inner class Chrome : WebChromeClient() {
        override fun onCreateWindow(view: WebView, isDialog: Boolean, isUserGesture: Boolean, resultMsg: Message): Boolean {
            diag("navigation-blocked", "window.open")
            return false
        }
        override fun onJsAlert(view: WebView, url: String, message: String, result: JsResult): Boolean { result.cancel(); return true }
        override fun onJsConfirm(view: WebView, url: String, message: String, result: JsResult): Boolean { result.cancel(); return true }
        override fun onJsPrompt(view: WebView, url: String, message: String, defaultValue: String, result: JsPromptResult): Boolean { result.cancel(); return true }
        override fun onJsBeforeUnload(view: WebView, url: String, message: String, result: JsResult): Boolean { result.cancel(); return true }
        override fun onGeolocationPermissionsShowPrompt(origin: String, callback: GeolocationPermissions.Callback) = callback.invoke(origin, false, false)
        override fun onPermissionRequest(request: PermissionRequest) = request.deny()
        override fun onShowFileChooser(webView: WebView, filePathCallback: ValueCallback<Array<android.net.Uri>>, fileChooserParams: FileChooserParams): Boolean = false
        override fun onConsoleMessage(consoleMessage: ConsoleMessage): Boolean {
            diag("renderer-console", "${consoleMessage.messageLevel()} ${consoleMessage.message().take(300)} (${consoleMessage.sourceId().takeLast(40)}:${consoleMessage.lineNumber()})")
            return true
        }
    }
}
