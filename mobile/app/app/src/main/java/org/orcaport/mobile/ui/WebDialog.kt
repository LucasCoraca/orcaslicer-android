package org.orcaport.mobile.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import org.json.JSONObject
import org.orcaport.mobile.AppState
import org.orcaport.mobile.engine.jobj

/**
 * WebViewHostDialog: one of upstream's web dialogs (resources/web/dialog/...). The engine gives the page and its
 * document-start scripts; each window.wx.postMessage goes to the dialog's on_script_message and the scripts it answers
 * with run in the page. A file the dialog wrote ("save") goes to the user's save picker.
 */
@Composable
fun WebDialog(app: AppState, name: String, args: JSONObject = JSONObject(), onResult: (JSONObject) -> Unit = {}, onClose: () -> Unit) {
    var page by remember(name) { mutableStateOf<JSONObject?>(null) }
    LaunchedEffect(name) { app.call("webdialog.open", JSONObject(args.toString()).put("name", name)) { if (it is JSONObject) page = it } }
    val p = page ?: return
    val handle = remember(name) { WebPageHandle() }
    fun close() { app.call("webdialog.close", jobj("name" to name)) { r -> if (r is JSONObject) onResult(r) }; onClose() }
    // what the dialog runs on its page after the message that started it (a worker's result: WebViewHostDialog::call_web_handler)
    LaunchedEffect(name) {
        app.pageScripts.collect { r ->
            if (r.optString("target") != "webdialog" || r.optString("name") != name) return@collect
            r.optString("script").takeIf { it.isNotEmpty() }?.let { handle.run(it) }
            r.optString("url").takeIf { it.isNotEmpty() }?.let { u -> handle.view?.post { handle.view?.loadUrl(u) } }
            if (r.optBoolean("close")) close()
        }
    }
    // SpeedDialWebDialog: a borderless popup of fixed width whose height follows its page (resize_to_content)
    val speedDial = name == "speed_dial"
    var height by remember(name) { mutableIntStateOf(282) }
    val maxHeight = (LocalConfiguration.current.screenHeightDp * 85 / 100)
    // the dialog stays above the on-screen keyboard (its input rows: the terminal's command, the search fields)
    Dialog(onDismissRequest = ::close, properties = DialogProperties(usePlatformDefaultWidth = false, decorFitsSystemWindows = false)) {
        Column((if (speedDial) Modifier.width(540.dp).height(height.coerceIn(60, maxHeight).dp).clip(RoundedCornerShape(10.dp)).background(desk().panel)
               else if (name == "plugins") Modifier.width(900.dp).heightIn(max = 820.dp).fillMaxHeight(0.92f).background(desk().panel, RoundedCornerShape(6.dp))
               else Modifier.width(820.dp).heightIn(max = 660.dp).fillMaxHeight(0.92f).background(desk().panel, RoundedCornerShape(6.dp))).imePadding()) {
            if (!speedDial) Row(Modifier.fillMaxWidth().padding(start = 16.dp, top = 8.dp, end = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                Text(p.optString("title"), Modifier.weight(1f), style = deskText(15))
                Text("✕", Modifier.clickable { close() }.padding(8.dp), style = deskText(15, color = desk().textDim))
            }
            OrcaWebPage(p.optString("url"), p.optString("script"), Modifier.fillMaxWidth().weight(1f), handle, remote = p.optBoolean("remote")) { msg ->
                app.call("webdialog.message", jobj("name" to name, "message" to msg)) { r ->
                    if (r !is JSONObject) return@call
                    r.optJSONArray("scripts")?.let { s -> for (i in 0 until s.length()) handle.run(s.getString(i)) }
                    r.optJSONObject("save")?.let { s ->
                        val ctx = app.appContext ?: return@let
                        app.saveDocument(s.optString("name")) { uri ->
                            runCatching {
                                ctx.contentResolver.openOutputStream(uri)?.use { o -> java.io.File(s.optString("path")).inputStream().use { it.copyTo(o) } }
                                app.toast = "Exported " + s.optString("name")
                            }.onFailure { app.toast = it.message }
                        }
                    }
                    r.optString("open").takeIf { it.isNotEmpty() }?.let { app.webDialogs.add(it) }
                    if (speedDial && r.optInt("height") > 0) height = r.optInt("height")
                    if (r.optBoolean("closed")) { onResult(r); onClose() }
                }
            }
        }
    }
}
