package org.orcaport.mobile.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import org.json.JSONObject
import org.orcaport.mobile.AppState
import org.orcaport.mobile.engine.jobj
import org.orcaport.mobile.prepare.PanelButton

/** A plugin's page (WebDialog / DockPanel): its HTML against the resources, window.orca's messages to the engine. */
@Composable
private fun PluginPage(app: AppState, target: String, w: JSONObject, modifier: Modifier) {
    val id = w.optInt("id")
    val handle = remember(id) { WebPageHandle() }
    // window.__orcaDispatch(...) of post()
    LaunchedEffect(id) {
        app.pageScripts.collect { r -> if (r.optString("target") == target && r.optInt("id") == id) handle.run(r.optString("script")) }
    }
    val cmd = if (target == "plugin_window") "plugins.window_message" else "plugins.dock_message"
    OrcaWebPage(w.optString("base_url"), w.optString("script"), modifier, handle, html = w.optString("html")) { msg ->
        app.post(cmd, jobj("id" to id, "message" to msg))
    }
}

/** orca.host.ui.create_window: a window (or modal dialog) with the plugin's HTML; ✕ closes it like the window's close box */
@Composable
fun PluginWindows(app: AppState) {
    for ((id, w) in app.pluginWindows.toList()) key(id) {
        val close = { app.post("plugins.window_close", jobj("id" to id)); Unit }
        Dialog(close, DialogProperties(usePlatformDefaultWidth = false, dismissOnClickOutside = false)) {
            val c = desk()
            Column(Modifier.width(w.optInt("width", 820).coerceIn(320, 1200).dp).heightIn(max = w.optInt("height", 600).coerceIn(240, 1000).dp)
                .fillMaxHeight(0.9f).clip(RoundedCornerShape(6.dp)).background(c.panel)) {
                Row(Modifier.fillMaxWidth().background(FrameColors.topbar).padding(horizontal = 14.dp, vertical = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                    Text(w.optString("title"), Modifier.weight(1f), style = deskText(15, FontWeight.Bold, Color.White), maxLines = 1)
                    Text("✕", Modifier.clickable(onClick = close).padding(6.dp), style = deskText(16, color = Color.White))
                }
                PluginPage(app, "plugin_window", w, Modifier.fillMaxWidth().weight(1f))
            }
        }
    }
}

/** orca.host.ui.create_dock_panel: the panels docked on one side of the 3D view (Prepare and Preview), with their caption */
@Composable
fun PluginDocks(app: AppState, side: String, modifier: Modifier = Modifier) {
    val docks = app.pluginDocks.values.filter { !it.optBoolean("hidden") && (it.optString("dock", "right") == side || (side == "right" && it.optString("dock") == "float")) }
    if (docks.isEmpty()) return
    val c = desk()
    val vertical = side != "bottom"
    val size = docks.maxOf { if (vertical) it.optInt("width", 320) else it.optInt("height", 240) }.coerceIn(160, 600).dp
    val m = if (vertical) modifier.width(size).fillMaxHeight() else modifier.height(size).fillMaxWidth()
    Column(m.background(c.panel).border(1.dp, c.border)) {
        for (d in docks) key(d.optInt("id")) {
            Column(Modifier.weight(1f).fillMaxWidth()) {
                Row(Modifier.fillMaxWidth().background(c.field).padding(horizontal = 10.dp, vertical = 6.dp), verticalAlignment = Alignment.CenterVertically) {
                    Text(d.optString("title"), Modifier.weight(1f), style = deskText(13, FontWeight.Bold), maxLines = 1)
                    Text("✕", Modifier.clickable { app.post("plugins.dock_close", jobj("id" to d.optInt("id"))) }.padding(4.dp), style = deskText(14, color = c.textDim))
                }
                PluginPage(app, "plugin_dock", d, Modifier.fillMaxWidth().weight(1f))
            }
        }
    }
}

/** orca.host.ui.ProgressDialog (and the Plugins dialog's waits): the gauge or the pulse, Cancel when the plugin allows it */
@Composable
fun PluginProgressDialogs(app: AppState) {
    val p = app.pluginProgress.values.maxByOrNull { it.optInt("id") } ?: return
    val c = desk()
    Dialog({}, DialogProperties(dismissOnBackPress = false, dismissOnClickOutside = false)) {
        Column(Modifier.width(420.dp).clip(RoundedCornerShape(6.dp)).background(c.panel).padding(20.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Text(p.optString("title"), style = deskText(16, FontWeight.Bold))
            Text(p.optString("message"), style = deskText(13))
            if (p.optBoolean("pulsing")) LinearProgressIndicator(Modifier.fillMaxWidth(), color = OrcaColors.accent)
            else LinearProgressIndicator({ p.optInt("value").toFloat() / p.optInt("maximum", 100).coerceAtLeast(1) }, Modifier.fillMaxWidth(), color = OrcaColors.accent)
            if (p.optBoolean("can_abort")) Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                PanelButton(tr("Cancel")) { app.post("plugins.progress_cancel", jobj("id" to p.optInt("id"))) }
            }
        }
    }
}

/** NotificationManager's custom notifications of plugins: the text, the hyperlink that calls the plugin back, ✕ */
@Composable
fun PluginNotifications(app: AppState, modifier: Modifier) {
    if (app.pluginNotifications.isEmpty()) return
    val c = desk()
    Column(modifier, verticalArrangement = Arrangement.spacedBy(6.dp)) {
        for (n in app.pluginNotifications.toList()) key(n.optInt("id")) {
            // a regular notification fades out after 10 s (NotificationManager's RegularNotificationLevel)
            if (n.optString("level") == "regular" && n.optString("hypertext").isEmpty())
                LaunchedEffect(n) { kotlinx.coroutines.delay(10_000); app.pluginNotifications.remove(n) }
            val color = when (n.optString("level")) { "error" -> Color(0xFFE14747); "warning" -> OrcaColors.warning; else -> OrcaColors.accent }
            Row(Modifier.widthIn(max = 420.dp).shadow(3.dp, RoundedCornerShape(6.dp)).background(c.panel, RoundedCornerShape(6.dp))
                .border(1.dp, color, RoundedCornerShape(6.dp)).padding(10.dp), verticalAlignment = Alignment.Top) {
                Column(Modifier.weight(1f)) {
                    Text(n.optString("text"), style = deskText(13))
                    n.optString("hypertext").takeIf { it.isNotEmpty() }?.let { h ->
                        Text(h, Modifier.clickable {
                            app.call("plugins.notification_click", jobj("id" to n.optInt("id"))) { r -> if (r == true) app.pluginNotifications.remove(n) }
                        }.padding(top = 4.dp), style = deskText(13, color = OrcaColors.accent).copy(textDecoration = TextDecoration.Underline))
                    }
                }
                Text("✕", Modifier.clickable { app.pluginNotifications.remove(n) }.padding(start = 8.dp), style = deskText(13, color = c.textDim))
            }
        }
    }
}
