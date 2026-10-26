package org.orcaport.mobile.ui

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import org.json.JSONArray
import org.json.JSONObject
import org.orcaport.mobile.AppState
import org.orcaport.mobile.engine.jobj
import org.orcaport.mobile.prepare.DeskDialog

/**
 * ExportConfigsDialog: the export type (printer config bundle, filament bundle, printer / filament / process presets),
 * the printers or filaments to include, "Include custom bed texture and model"; OK asks for the folder.
 */
@Composable
fun ExportBundleDialog(app: AppState, onClose: () -> Unit) {
    var st by remember { mutableStateOf<JSONObject?>(null) }
    LaunchedEffect(Unit) { app.call("bundle.state") { if (it is JSONObject) st = it } }
    val s = st ?: return
    val types = s.optJSONArray("types") ?: JSONArray()
    var type by remember { mutableIntStateOf(0) }
    var includeBed by remember { mutableStateOf(true) }
    val chosen = remember(type) { mutableStateListOf<String>() }
    val t = types.optJSONObject(type) ?: JSONObject()
    val items = t.optJSONArray("items")?.let { a -> List(a.length()) { a.optString(it) } } ?: emptyList()
    val c = desk()
    DeskDialog(s.optString("title"), onDismiss = onClose, ok = {
        if (chosen.isEmpty()) { app.toast = s.optString("none_selected"); return@DeskDialog }
        onClose()
        app.exportToFolder("bundle.export", jobj("type" to t.optString("key"), "items" to JSONArray(chosen.toList()), "include_bed" to includeBed),
            "Preset bundle exported")
    }) {
        Row {
            Text(s.optString("presets_label"), Modifier.width(90.dp), style = deskText(14))
            Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                for (i in 0 until types.length()) {
                    val ti = types.getJSONObject(i)
                    Row(Modifier.clickable { type = i }, verticalAlignment = Alignment.CenterVertically) {
                        RadioButton(type == i, { type = i }, colors = RadioButtonDefaults.colors(selectedColor = OrcaColors.accent))
                        Text(ti.optString("label"), style = deskText(14))
                    }
                    ti.optString("description").takeIf { it.isNotEmpty() }?.let { Text(it, Modifier.padding(start = 48.dp), style = deskText(12, color = c.textDim)) }
                    if (ti.optString("key") == "preset_bundle") Row(Modifier.padding(start = 40.dp), verticalAlignment = Alignment.CenterVertically) {
                        DeskCheck(includeBed, enabled = type == i) { includeBed = it }
                        Text(s.optString("include_bed"), style = deskText(12, color = c.textDim))
                    }
                }
            }
        }
        HorizontalDivider(color = c.border)
        val hint = when (t.optString("key")) { "preset_bundle", "printer_preset" -> "hint_bundle"; "process_preset" -> "hint_process"; else -> "hint_filament" }
        Text(s.optString(hint), style = deskText(12, color = c.textDim))
        Column(Modifier.heightIn(max = 260.dp).verticalScroll(rememberScrollState())) {
            for (name in items) Row(verticalAlignment = Alignment.CenterVertically) {
                DeskCheck(name in chosen) { on -> if (on) chosen.add(name) else chosen.remove(name) }
                Text(name, style = deskText(13))
            }
        }
    }
}
