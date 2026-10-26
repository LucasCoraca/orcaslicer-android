package org.orcaport.mobile.ui

import android.content.Intent
import android.net.Uri
import android.webkit.WebResourceRequest
import android.webkit.WebView
import android.webkit.WebViewClient
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import org.json.JSONObject
import org.orcaport.mobile.AppState
import org.orcaport.mobile.engine.jobj
import org.orcaport.mobile.prepare.PanelButton

/** UpdateVersionDialog: the new version, "Check on GitHub", the release notes, stable only, Download / Skip / Cancel */
@Composable
fun UpdateVersionDialog(app: AppState, u: JSONObject, onClose: () -> Unit) {
    val ctx = LocalContext.current
    fun open(url: String) = runCatching { ctx.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) }
    var stable by remember { mutableStateOf(u.optBoolean("stable_only")) }
    Dialog(onDismissRequest = onClose, properties = DialogProperties(usePlatformDefaultWidth = false)) {
        Column(Modifier.width(700.dp).background(desk().panel, RoundedCornerShape(6.dp))) {
            Row(Modifier.fillMaxWidth().padding(start = 16.dp, top = 8.dp, end = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                Text(u.optString("title"), Modifier.weight(1f), style = deskText(15))
                Text("✕", Modifier.clickable(onClick = onClose).padding(8.dp), style = deskText(15, color = desk().textDim))
            }
            Box(Modifier.fillMaxWidth().height(1.dp).background(desk().border))
            Row(Modifier.padding(start = 15.dp, top = 15.dp, end = 15.dp), verticalAlignment = Alignment.CenterVertically) {
                OrcaIcon("OrcaSlicer", 64.dp, darkVariant = false)
                Spacer(Modifier.width(15.dp))
                Column {
                    Text(u.optString("info"), style = deskText(14, FontWeight.Bold))
                    Text(u.optString("link"), Modifier.clickable { open(u.optString("url")); onClose() },
                        style = deskText(14, color = OrcaColors.accent).copy(textDecoration = TextDecoration.Underline))
                }
            }
            val html = u.optString("html")
            AndroidView({ c ->
                WebView(c).apply {
                    webViewClient = object : WebViewClient() {
                        // links clicked in the notes open in the browser
                        override fun shouldOverrideUrlLoading(view: WebView, request: WebResourceRequest): Boolean { open(request.url.toString()); return true }
                    }
                    loadDataWithBaseURL(null, html, "text/html", "utf-8", null)
                }
            }, Modifier.padding(start = 15.dp, top = 15.dp, end = 15.dp).fillMaxWidth().height(420.dp).border(1.dp, desk().border))
            Row(Modifier.fillMaxWidth().padding(15.dp), verticalAlignment = Alignment.CenterVertically) {
                DeskCheck(stable) { v -> stable = v; app.call("update.set", jobj("stable_only" to v)) }
                Spacer(Modifier.width(6.dp))
                Text(u.optString("stable_only_label"), style = deskText(14))
                Spacer(Modifier.weight(1f))
                PanelButton(u.optString("download"), primary = true) { open(u.optString("url")); onClose() }
                if (u.optString("skip").isNotEmpty()) {
                    Spacer(Modifier.width(10.dp))
                    PanelButton(u.optString("skip")) { app.call("update.set", jobj("skip" to true)); onClose() }
                }
                Spacer(Modifier.width(10.dp))
                PanelButton(u.optString("cancel")) { onClose() }
            }
        }
    }
}
