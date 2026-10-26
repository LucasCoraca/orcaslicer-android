package org.orcaport.mobile.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Error
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.Warning
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import org.json.JSONArray
import org.json.JSONObject
import org.orcaport.mobile.engine.Ask
import org.orcaport.mobile.settings.colorHex
import org.orcaport.mobile.settings.strings

/** Shows the oldest question the engine is waiting on (MessageDialog, UnsavedChangesDialog ...). */
@Composable
fun AskHost(asks: List<Ask>) {
    val ask = asks.firstOrNull() ?: return
    val r = ask.request
    when (r.optString("kind")) {
        "message" -> MessageAsk(ask)
        "unsaved_changes" -> UnsavedChangesAsk(ask)
        "cloud_auth" -> CloudAuthAsk(ask)
        "3dprinteros_upload" -> UploadOptionsAsk(ask)
        "plugin_picker" -> PluginPickerAsk(ask)
        else -> ask.reply(JSONObject())
    }
}

@Composable
private fun MessageAsk(ask: Ask) {
    val r = ask.request
    val buttons = r.optJSONArray("buttons") ?: JSONArray()
    var dsa by remember(ask) { mutableStateOf(false) }
    fun answer(id: Int) = ask.reply(JSONObject().put("id", id).put("dsa", dsa))
    val cancelId = (0 until buttons.length()).map { buttons.getJSONObject(it) }.lastOrNull()?.optInt("id") ?: 5100
    AlertDialog(
        onDismissRequest = { },   // a modal question: answered by its buttons only
        icon = {
            when (r.optString("icon")) {
                "error" -> Icon(Icons.Default.Error, null, tint = OrcaColors.error)
                "warning" -> Icon(Icons.Default.Warning, null, tint = OrcaColors.warning)
                else -> Icon(Icons.Default.Info, null, tint = OrcaColors.accent)
            }
        },
        title = r.optString("caption").takeIf { it.isNotEmpty() }?.let { { Text(it) } },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                Text(r.optString("message"))
                if (r.optBoolean("dsa")) Row(verticalAlignment = Alignment.CenterVertically) {
                    Checkbox(dsa, { dsa = it }); Text(r.optString("dsa_label").ifEmpty { tr("Don't show again") })
                }
            }
        },
        confirmButton = {
            Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                for (i in 0 until buttons.length()) {
                    val b = buttons.getJSONObject(i)
                    TextButton(onClick = { answer(b.getInt("id")) }) { Text(b.optString("label")) }
                }
            }
        },
    )
    androidx.activity.compose.BackHandler { answer(cancelId) }
}

/** Tab::may_discard_current_dirty_preset / UnsavedChangesDialog: transfer, discard or save the modified options. */
@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun UnsavedChangesAsk(ask: Ask) {
    val r = ask.request
    val opts = r.optJSONArray("options") ?: JSONArray()
    val all = (0 until opts.length()).map { opts.getJSONObject(it) }
    var selected by remember(ask) { mutableStateOf(all.map { it.optString("key") }.toSet()) }
    var saving by remember(ask) { mutableStateOf(false) }
    var name by remember(ask) { mutableStateOf(r.optString("preset")) }
    fun reply(action: String) = ask.reply(JSONObject().put("action", action).put("selected", JSONArray(selected.toList())).put("name", name))
    Dialog(onDismissRequest = { reply("cancel") }, properties = DialogProperties(usePlatformDefaultWidth = false)) {
        Surface(Modifier.fillMaxWidth(0.95f).fillMaxHeight(0.85f), shape = RoundedCornerShape(12.dp)) {
            Column(Modifier.padding(16.dp)) {
                Text(tr("unsaved changes").replaceFirstChar { it.uppercase() }, style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold)
                Text(tr("Preset \"%1%\" contains the following unsaved changes:").replace("%1%", r.optString("preset")), Modifier.padding(vertical = 8.dp),
                    style = MaterialTheme.typography.bodyMedium)
                LazyColumn(Modifier.weight(1f).border(1.dp, MaterialTheme.colorScheme.outline, RoundedCornerShape(6.dp))) {
                    items(all) { o ->
                        val k = o.optString("key")
                        Row(Modifier.fillMaxWidth().clickable { selected = if (k in selected) selected - k else selected + k }.padding(8.dp),
                            verticalAlignment = Alignment.CenterVertically) {
                            Checkbox(k in selected, { selected = if (it) selected + k else selected - k })
                            Column(Modifier.weight(1f)) {
                                Text(o.optString("label"), style = MaterialTheme.typography.bodyMedium)
                                Text("${o.optString("old")}  →  ${o.optString("new")}", style = MaterialTheme.typography.bodySmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant, maxLines = 3)
                            }
                        }
                    }
                }
                if (saving) {
                    OutlinedTextField(name, { name = it }, Modifier.fillMaxWidth().padding(top = 8.dp), singleLine = true, label = { Text(tr("Name")) })
                    Row(Modifier.align(Alignment.End)) {
                        TextButton({ saving = false }) { Text(tr("Back")) }
                        TextButton({ reply("save") }, enabled = name.isNotBlank()) { Text(tr("Save")) }
                    }
                } else FlowRow(Modifier.fillMaxWidth().padding(top = 8.dp), horizontalArrangement = Arrangement.End) {
                    if (r.optBoolean("can_transfer")) TextButton({ reply("transfer") }) { Text(tr("Transfer")) }
                    TextButton({ reply("discard") }) { Text(tr("Discard")) }
                    if (r.optBoolean("can_save")) TextButton({ saving = true }) { Text(tr("Save")) }
                    TextButton({ reply("cancel") }) { Text(tr("Cancel")) }
                }
            }
        }
    }
}

// Orca's colour picker palette (ColorPickerDialog / FilamentColor): the common filament colours
private val PALETTE = listOf(
    "#FFFFFF", "#FFF144", "#DCF478", "#0ACC38", "#057748", "#0D6284", "#0EE2A0", "#76D9F4", "#46A8F9", "#2850E0",
    "#443089", "#A03CF7", "#F330F9", "#D4B1DD", "#F95D73", "#F72323", "#7C4B00", "#F98C36", "#FCECD6", "#D3C5A3",
    "#AF7933", "#898989", "#BCBCBC", "#161616",
)

@OptIn(ExperimentalLayoutApi::class)
@Composable
fun ColorPickerDialog(initial: Color?, onDismiss: () -> Unit, onPick: (String) -> Unit) {
    val hsv = remember { FloatArray(3).also { android.graphics.Color.colorToHSV((initial ?: Color.White).let { c -> android.graphics.Color.rgb((c.red * 255).toInt(), (c.green * 255).toInt(), (c.blue * 255).toInt()) }, it) } }
    var h by remember { mutableFloatStateOf(hsv[0]) }
    var s by remember { mutableFloatStateOf(hsv[1]) }
    var v by remember { mutableFloatStateOf(hsv[2]) }
    val current = Color(android.graphics.Color.HSVToColor(floatArrayOf(h, s, v)))
    var hex by remember(current) { mutableStateOf(colorHex(current)) }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(tr("Color")) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    for (p in PALETTE) {
                        val c = Color(android.graphics.Color.parseColor(p))
                        Box(Modifier.size(30.dp).background(c, CircleShape).border(1.dp, MaterialTheme.colorScheme.outline, CircleShape)
                            .clickable { val a = FloatArray(3); android.graphics.Color.colorToHSV(android.graphics.Color.parseColor(p), a); h = a[0]; s = a[1]; v = a[2] })
                    }
                }
                // saturation / value square
                Box(Modifier.fillMaxWidth().height(150.dp)
                    .background(Brush.horizontalGradient(listOf(Color.White, Color(android.graphics.Color.HSVToColor(floatArrayOf(h, 1f, 1f))))))
                    .background(Brush.verticalGradient(listOf(Color.Transparent, Color.Black)))
                    .pointerInput(Unit) {
                        fun at(o: Offset) { s = (o.x / size.width).coerceIn(0f, 1f); v = 1f - (o.y / size.height).coerceIn(0f, 1f) }
                        detectTapGestures { at(it) }
                    }
                    .pointerInput(Unit) {
                        detectDragGestures { ch, _ -> val o = ch.position; s = (o.x / size.width).coerceIn(0f, 1f); v = 1f - (o.y / size.height).coerceIn(0f, 1f) }
                    })
                // hue
                Box(Modifier.fillMaxWidth().height(28.dp)
                    .background(Brush.horizontalGradient((0..6).map { Color(android.graphics.Color.HSVToColor(floatArrayOf(it * 60f, 1f, 1f))) }), RoundedCornerShape(4.dp))
                    .pointerInput(Unit) { detectTapGestures { h = (it.x / size.width * 360f).coerceIn(0f, 359.9f) } }
                    .pointerInput(Unit) { detectDragGestures { ch, _ -> h = (ch.position.x / size.width * 360f).coerceIn(0f, 359.9f) } })
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Box(Modifier.size(36.dp).background(current, RoundedCornerShape(6.dp)).border(1.dp, MaterialTheme.colorScheme.outline, RoundedCornerShape(6.dp)))
                    Spacer(Modifier.width(10.dp))
                    OutlinedTextField(hex, { t ->
                        hex = t
                        runCatching { android.graphics.Color.parseColor(if (t.startsWith("#")) t else "#$t") }.getOrNull()?.let { c ->
                            val a = FloatArray(3); android.graphics.Color.colorToHSV(c, a); h = a[0]; s = a[1]; v = a[2]
                        }
                    }, singleLine = true, modifier = Modifier.width(140.dp))
                }
            }
        },
        confirmButton = { TextButton({ onPick(colorHex(current)) }) { Text(tr("OK")) } },
        dismissButton = { TextButton(onDismiss) { Text(tr("Cancel")) } })
}

fun JSONObject.strs(key: String) = optJSONArray(key).strings()

/** PrinterCloudAuthDialog: the host's login page; its window.wx "login_token" message is the API key */
@Composable
private fun CloudAuthAsk(ask: Ask) {
    val r = ask.request
    val handle = remember(ask) { WebPageHandle() }
    androidx.compose.ui.window.Dialog(onDismissRequest = { ask.reply(JSONObject()) }, properties = DialogProperties(usePlatformDefaultWidth = false)) {
        Column(Modifier.width(820.dp).fillMaxHeight(0.92f).background(desk().panel, androidx.compose.foundation.shape.RoundedCornerShape(6.dp))) {
            Row(Modifier.fillMaxWidth().padding(start = 16.dp, top = 8.dp, end = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                Text(tr(r.optString("title", "Login")), Modifier.weight(1f), style = deskText(15))
                TextButton(onClick = { ask.reply(JSONObject()) }) { Text("✕") }
            }
            OrcaWebPage(r.optString("url"), "", Modifier.fillMaxWidth().weight(1f), handle, remote = true) { msg ->
                // OnScriptMessage: any message closes the dialog; "login_token" carries the key
                val j = runCatching { JSONObject(msg) }.getOrNull()
                val token = if (j?.optString("command") == "login_token") j.optJSONObject("data")?.optString("token") ?: "" else ""
                ask.reply(JSONObject().put("token", token))
            }
        }
    }
}

/** 3DPrinterOS's UploadOptionsDialog: single file or a cloud project, and the printer type (OK needs one) */
@Composable
private fun UploadOptionsAsk(ask: Ask) {
    val r = ask.request
    val types = r.optJSONArray("printer_types") ?: JSONArray()
    val projects = r.optJSONArray("projects") ?: JSONArray()
    var projectFile by remember(ask) { mutableStateOf(false) }
    var project by remember(ask) { mutableStateOf("") }
    var type by remember(ask) { mutableStateOf(r.optInt("selected", -1)) }
    var projectsOpen by remember(ask) { mutableStateOf(false) }
    var typesOpen by remember(ask) { mutableStateOf(false) }
    AlertDialog(onDismissRequest = { },
        title = { Text(r.optString("title")) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    RadioButton(!projectFile, { projectFile = false }); Text(r.optString("single"))
                    Spacer(Modifier.width(12.dp))
                    RadioButton(projectFile, { projectFile = true }); Text(r.optString("project_file"))
                }
                if (projectFile) {
                    Text(r.optString("project_label"))
                    Box {
                        OutlinedTextField(project, { project = it }, Modifier.fillMaxWidth(), singleLine = true,
                            trailingIcon = { TextButton(onClick = { projectsOpen = true }) { Text("▾") } })
                        DropdownMenu(projectsOpen, { projectsOpen = false }) {
                            for (i in 0 until projects.length()) DropdownMenuItem({ Text(projects.getString(i)) }, { project = projects.getString(i); projectsOpen = false })
                        }
                    }
                }
                Text(r.optString("printer_label"))
                Box {
                    OutlinedButton(onClick = { typesOpen = true }, Modifier.fillMaxWidth()) { Text(if (type >= 0) types.optString(type) else "") }
                    DropdownMenu(typesOpen, { typesOpen = false }) {
                        for (i in 0 until types.length()) DropdownMenuItem({ Text(types.getString(i)) }, { type = i; typesOpen = false })
                    }
                }
                if (type < 0 && r.optString("warning").isNotEmpty()) Text(r.optString("warning"), color = OrcaColors.error)
            }
        },
        confirmButton = {
            TextButton(enabled = type >= 0, onClick = {
                ask.reply(JSONObject().put("ok", true).put("project", if (projectFile) project else "").put("printer_type", types.optString(type)))
            }) { Text(tr("OK")) }
        },
        dismissButton = { TextButton(onClick = { ask.reply(JSONObject().put("ok", false)) }) { Text(tr("Cancel")) } })
}

/** PluginPickerDialog: one of the enabled capabilities of the option's type, with its package as the description */
@Composable
private fun PluginPickerAsk(ask: Ask) {
    val r = ask.request
    val items = r.optJSONArray("items") ?: JSONArray()
    var sel by remember(ask) { mutableIntStateOf(0) }
    val c = desk()
    org.orcaport.mobile.prepare.DeskDialog(r.optString("title"), onDismiss = { ask.reply(JSONObject().put("ok", false)) },
        ok = { ask.reply(JSONObject().put("ok", true).put("index", sel)) }, okLabel = r.optString("ok", "OK"), cancelLabel = r.optString("cancel", "Cancel")) {
        Text(r.optString("type"), style = deskText(13, color = c.textDim))
        DeskChoice((0 until items.length()).map { items.getJSONObject(it).optString("label") }, sel, Modifier.width(420.dp)) { sel = it }
        Text(items.optJSONObject(sel)?.optString("description") ?: "", style = deskText(13))
    }
}
