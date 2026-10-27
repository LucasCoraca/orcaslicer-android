package org.orcaport.mobile.ui

import android.annotation.SuppressLint
import android.net.Uri
import android.webkit.JavascriptInterface
import android.webkit.WebResourceRequest
import android.webkit.WebResourceResponse
import android.webkit.WebView
import android.webkit.WebViewClient
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.viewinterop.AndroidView
import java.io.ByteArrayInputStream
import java.io.File

/** Runs JavaScript in a page created by [OrcaWebPage]. */
class WebPageHandle { var view: WebView? = null; fun run(js: String) { view?.post { view?.evaluateJavascript(js, null) } } }

/**
 * One of Orca's HTML pages (resources/web) with the desktop's host contract: window.wx.postMessage() reaches
 * [onMessage], the user agent carries "BBL-Slicer" (the pages only talk to a slicer that does), and [startScript]
 * runs before the page's own scripts (wxWebView::AddUserScript on the desktop).
 */
@SuppressLint("SetJavaScriptEnabled", "JavascriptInterface")
@Composable
fun OrcaWebPage(url: String, startScript: String, modifier: Modifier, handle: WebPageHandle, remote: Boolean = false,
                keepOrigin: Boolean = false, html: String? = null, onMessage: (String) -> Unit) {
    val bridge = remember { object { @JavascriptInterface fun postMessage(s: String) = onMessage(s) } }
    // WebView::RecreateAll's user agent: Orca's pages take their dark style from "(dark)" (globalapi.js ExecuteDarkMode)
    val dark = androidx.compose.foundation.isSystemInDarkTheme()
    fun agent(stock: String) = stock + " BBL-Slicer/v01.09.00 (" + (if (dark) "dark" else "light") + ") BBL-Language/" +
        java.util.Locale.getDefault().language + " OrcaSlicer"
    AndroidView(modifier = modifier, update = { v ->
        // the system theme changed: the new agent, and a reload so navigator.userAgent has it
        val stock = v.tag as? String ?: return@AndroidView
        if (v.settings.userAgentString != agent(stock)) { v.settings.userAgentString = agent(stock); v.reload() }
    }, factory = { ctx ->
        WebView.setWebContentsDebuggingEnabled(org.orcaport.mobile.BuildConfig.DEBUG)
        WebView(ctx).apply {
            // WRAP_CONTENT (AndroidView's default) makes Chromium lay pages out with a zero-height viewport
            layoutParams = android.view.ViewGroup.LayoutParams(android.view.ViewGroup.LayoutParams.MATCH_PARENT, android.view.ViewGroup.LayoutParams.MATCH_PARENT)
            settings.javaScriptEnabled = true
            settings.domStorageEnabled = true
            settings.allowFileAccess = true
            @Suppress("DEPRECATION") settings.allowFileAccessFromFileURLs = true
            @Suppress("DEPRECATION") settings.allowUniversalAccessFromFileURLs = true
            tag = settings.userAgentString
            settings.userAgentString = agent(settings.userAgentString)
            addJavascriptInterface(bridge, "OrcaHost")
            webChromeClient = object : android.webkit.WebChromeClient() {
                override fun onConsoleMessage(m: android.webkit.ConsoleMessage): Boolean {
                    android.util.Log.i("orca-web", "${m.message()} (${m.sourceId()}:${m.lineNumber()})"); return true
                }
            }
            val bridgeJs = "window.wx={postMessage:function(s){OrcaHost.postMessage(typeof s==='string'?s:JSON.stringify(s));}};"
            val inject = "<script>" + bridgeJs + startScript.replace("</", "<\\/") + "</script>"
            // a web page of a service (the OrcaCloud login): the bridge goes in before the page's scripts on every load
            val documentStart = remote && androidx.webkit.WebViewFeature.isFeatureSupported(androidx.webkit.WebViewFeature.DOCUMENT_START_SCRIPT)
            if (documentStart) androidx.webkit.WebViewCompat.addDocumentStartJavaScript(this, bridgeJs + startScript, setOf("*"))
            webViewClient = object : WebViewClient() {
                // the top-level HTML gets the bridge and the start script in front of its own scripts
                override fun shouldInterceptRequest(view: WebView, req: WebResourceRequest): WebResourceResponse? {
                    val u = req.url
                    if (!req.isForMainFrame || u.scheme != "file") return null
                    val f = File(u.path ?: return null)
                    if (!f.exists() || !f.name.endsWith(".html")) return null
                    var html = f.readText()
                    val i = html.indexOf("<head>", ignoreCase = true)
                    html = if (i >= 0) html.substring(0, i + 6) + inject + html.substring(i + 6) else inject + html
                    return WebResourceResponse("text/html", "utf-8", ByteArrayInputStream(html.toByteArray()))
                }
                override fun onPageStarted(view: WebView, u: String?, favicon: android.graphics.Bitmap?) {
                    if (remote && !documentStart) view.evaluateJavascript(bridgeJs + startScript, null)
                }
                override fun shouldOverrideUrlLoading(view: WebView, req: WebResourceRequest): Boolean {
                    // the OrcaCloud pane (WebViewPanel::OnCloudWebViewEvent): its own site stays, other sites open in the browser
                    if (remote && keepOrigin) {
                        val here = view.url?.let { Uri.parse(it) }
                        val sameSite = here != null && here.scheme == req.url.scheme && here.host == req.url.host && here.port == req.url.port
                        if (sameSite || req.url.scheme !in setOf("http", "https") || !req.isForMainFrame) return false
                        runCatching { ctx.startActivity(android.content.Intent(android.content.Intent.ACTION_VIEW, req.url)) }
                        return true
                    }
                    if (remote) return false   // the service's own pages (sign in, providers) stay in the dialog
                    val s = req.url.scheme
                    if (s == "http" || s == "https") {   // external links open in the browser
                        runCatching { ctx.startActivity(android.content.Intent(android.content.Intent.ACTION_VIEW, req.url)) }
                        return true
                    }
                    return false
                }
            }
            handle.view = this
            // a plugin's page (wxWebView::SetPage): its HTML against the resources base URL, the scripts in front
            if (html != null) loadDataWithBaseURL(url, inject + html, "text/html", "utf-8", null)
            else loadUrl(url)
        }
    })
}

fun fileUrl(f: File): String = Uri.fromFile(f).toString()
