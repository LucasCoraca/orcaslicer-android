package org.orcaport.mobile.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import org.json.JSONArray
import org.json.JSONObject
import org.orcaport.mobile.AppState
import org.orcaport.mobile.engine.jobj
import org.orcaport.mobile.prepare.PanelButton

/**
 * PreferencesDialog: the tabs on the left (General, Control, Graphics, Online, Developer), the rows of the selected tab
 * — titles, check boxes, combo boxes, inputs, spin controls, buttons — as the engine reads them from upstream's
 * create_items(); every change is stored in the app config right away, like the desktop's.
 */
@Composable
fun PreferencesDialog(app: AppState, onClose: () -> Unit) {
    var st by remember { mutableStateOf(JSONObject()) }
    var tab by remember { mutableIntStateOf(0) }
    LaunchedEffect(Unit) { app.call("prefs.get") { if (it is JSONObject) st = it } }
    fun set(vararg kv: Pair<String, Any?>) = app.call("prefs.set", jobj(*kv)) { if (it is JSONObject) st = it }
    val c = desk()
    val tabs = st.optJSONArray("tabs")?.let { a -> List(a.length()) { a.getJSONObject(it) } } ?: emptyList()
    Dialog(onClose, DialogProperties(usePlatformDefaultWidth = false)) {
        Column(Modifier.fillMaxWidth(0.9f).fillMaxHeight(0.9f).background(c.panel)) {
            Row(Modifier.fillMaxWidth().background(FrameColors.topbar).padding(horizontal = 16.dp, vertical = 10.dp), verticalAlignment = Alignment.CenterVertically) {
                Text(st.optString("title"), Modifier.weight(1f), style = deskText(16, FontWeight.Bold, Color.White))
                Text("✕", Modifier.clickable(onClick = onClose).padding(6.dp), style = deskText(18, color = Color.White))
            }
            Row(Modifier.weight(1f)) {
                Column(Modifier.width(170.dp).fillMaxHeight().background(c.field).padding(8.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    tabs.forEachIndexed { i, t ->
                        val sel = i == tab
                        Text(t.optString("tab"), Modifier.fillMaxWidth().background(if (sel) c.selection else Color.Transparent).clickable { tab = i }
                            .padding(horizontal = 10.dp, vertical = 10.dp), style = deskText(14, if (sel) FontWeight.Bold else FontWeight.Normal))
                    }
                }
                val items = tabs.getOrNull(tab)?.optJSONArray("items") ?: JSONArray()
                Column(Modifier.weight(1f).verticalScroll(rememberScrollState()).padding(horizontal = 20.dp, vertical = 10.dp),
                    verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    for (k in 0 until items.length()) PrefRow(items.getJSONObject(k), ::set) { v ->
                        // the buttons that open another dialog: Keyboard shortcuts, 3D scene benchmark (closes Preferences), Network test
                        when (v) {
                            "item_shortcuts" -> app.shortcutsDialog = "global"
                            "item_benchmark" -> { onClose(); app.startBenchmark() }
                            "item_network_test" -> app.networkTest = true
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun PrefRow(it: JSONObject, set: (Array<out Pair<String, Any?>>) -> Unit, open: (String) -> Unit) {
    fun send(vararg kv: Pair<String, Any?>) = set(kv)
    val c = desk()
    val kind = it.optString("kind")
    val key = it.optString("key")
    if (kind == "title") {
        Row(Modifier.padding(top = 10.dp), verticalAlignment = Alignment.CenterVertically) {
            Text(it.optString("label"), style = deskText(15, FontWeight.Bold))
            Spacer(Modifier.width(8.dp))
            HorizontalDivider(Modifier.weight(1f), color = c.border)
        }
        return
    }
    Row(Modifier.fillMaxWidth().heightIn(min = 40.dp), verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f).padding(end = 12.dp)) {
            Text(it.optString("label"), style = deskText(14))
            val tip = it.optString("tooltip")
            if (tip.isNotEmpty() && tip != it.optString("label")) Text(tip, style = deskText(11, color = c.textDim), maxLines = 3)
        }
        Box(Modifier.widthIn(min = 220.dp), Alignment.CenterEnd) {
            when (kind) {
                "checkbox" -> Row(verticalAlignment = Alignment.CenterVertically) {
                    DeskCheck(it.optString("value") == "true" || it.optString("value") == "1") { v -> send("key" to key, "value" to v) }
                    it.optString("secondary").takeIf { s -> s.isNotEmpty() }?.let { s -> Text(s, style = deskText(12, color = c.textDim)) }
                }
                "combobox" -> {
                    val opts = it.optJSONArray("options")?.let { a -> List(a.length()) { i -> a.optString(i) } } ?: emptyList()
                    val values = it.optJSONArray("values")?.let { a -> List(a.length()) { i -> a.optString(i) } }
                    val cur = it.optString("value")
                    val idx = values?.indexOf(cur)?.takeIf { i -> i >= 0 } ?: cur.toIntOrNull() ?: 0
                    DeskChoice(opts, idx, Modifier.width(220.dp)) { i -> send("key" to key, "value" to (values?.getOrNull(i) ?: i.toString())) }
                }
                "language" -> {
                    val opts = it.optJSONArray("options")?.let { a -> List(a.length()) { i -> a.getJSONObject(i) } } ?: emptyList()
                    val idx = opts.indexOfFirst { o -> o.optString("value") == it.optString("value") }.coerceAtLeast(0)
                    DeskChoice(opts.map { o -> o.optString("label") }, idx, Modifier.width(220.dp)) { i -> send("kind" to "language", "value" to opts[i].optString("value")) }
                }
                "loglevel" -> {
                    val opts = it.optJSONArray("options")?.let { a -> List(a.length()) { i -> a.optString(i) } } ?: emptyList()
                    DeskChoice(opts, it.optString("value").toIntOrNull() ?: 1, Modifier.width(220.dp)) { i -> send("kind" to "loglevel", "value" to i.toString()) }
                }
                "darkmode" -> DeskCheck(it.optString("value") == "1") { v -> send("kind" to "darkmode", "value" to if (v) "1" else "0") }
                "backup", "auto_reslice" -> Row(verticalAlignment = Alignment.CenterVertically) {
                    DeskCheck(it.optBoolean("value")) { v -> send("kind" to kind, "value" to v) }
                    DeskInput(it.optString("interval"), it.optBoolean("value"), true, Modifier.width(120.dp), sidetext = it.optString("side")) { s ->
                        send("kind" to kind, "interval" to s.filter(Char::isDigit))
                    }
                }
                "input", "spinctrl", "decimal" -> DeskInput(it.optString("value"), true, true, Modifier.width(160.dp), sidetext = it.optString("side")) { s ->
                    val v = s.replace(',', '.')
                    val ok = when (kind) {
                        "spinctrl" -> v.toIntOrNull()?.let { n -> n >= it.optInt("min", Int.MIN_VALUE) && n <= it.optInt("max", Int.MAX_VALUE) } ?: false
                        "decimal" -> v.toDoubleOrNull()?.let { n -> n >= it.optDouble("min", -1e30) && n <= it.optDouble("max", 1e30) } ?: false
                        else -> true
                    }
                    if (ok) send("key" to key, "value" to v)
                }
                "button" -> PanelButton(it.optString("button")) {
                    if (it.optString("var") in setOf("item_shortcuts", "item_benchmark", "item_network_test")) { open(it.optString("var")); return@PanelButton }
                    send("kind" to "button", "clear" to it.optString("clear").takeIf { s -> s.isNotEmpty() },
                        "erase" to it.optJSONArray("erase"))
                }
            }
        }
    }
}
