package org.orcaport.mobile.prepare

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.DpOffset
import androidx.compose.ui.unit.dp
import org.json.JSONArray
import org.json.JSONObject
import org.orcaport.mobile.AppState
import org.orcaport.mobile.engine.jobj
import org.orcaport.mobile.settings.parseColor
import org.orcaport.mobile.ui.*

/**
 * The object / part / multi selection / empty space menus of the desktop (MenuFactory), built by the engine
 * ("menu.get", mobile/core/src/Menu.cpp) for the current selection and shown as a popup at the finger.
 */
@Composable
fun OrcaContextMenu(app: AppState, offset: DpOffset = DpOffset.Zero, onDismiss: () -> Unit) {
    var items by remember { mutableStateOf<JSONArray?>(null) }
    LaunchedEffect(Unit) { app.call("menu.get") { items = (it as? JSONObject)?.optJSONArray("items") } }
    val list = items ?: return
    DropdownMenu(true, onDismiss, offset = offset) { MenuItems(app, list, onDismiss) }
}

@Composable
private fun ColumnScope.MenuItems(app: AppState, items: JSONArray, close: () -> Unit) {
    for (i in 0 until items.length()) {
        val n = items.getJSONObject(i)
        if (n.optBoolean("separator")) { HorizontalDivider(color = desk().border); continue }
        val enabled = n.optBoolean("enabled", true)
        val children = n.optJSONArray("children")
        val label = n.optString("label")
        if (children != null) {
            var open by remember { mutableStateOf(false) }
            Box {
                MenuRow(n, enabled, trailing = "›") { open = true }
                SubMenuPopup(open, { open = false }) { MenuItems(app, children) { open = false; close() } }
            }
        } else MenuRow(n, enabled, trailing = n.optString("shortcut").ifEmpty { null }) { close(); runMenu(app, n.optString("id")) }
    }
}

@Composable
private fun MenuRow(n: JSONObject, enabled: Boolean, trailing: String?, onClick: () -> Unit) {
    val c = desk()
    DropdownMenuItem(
        text = { Text(n.optString("label"), style = deskText(14, color = if (enabled) c.text else c.textDisabled)) },
        onClick = onClick, enabled = enabled,
        leadingIcon = {
            Box(Modifier.size(20.dp), Alignment.Center) {
                when {
                    n.has("checked") -> if (n.optBoolean("checked")) OrcaIcon("check_on", 18.dp, darkVariant = false) else OrcaIcon("check_off", 18.dp, darkVariant = false)
                    n.has("color") -> Box(Modifier.size(16.dp).background(parseColor(n.optString("color")) ?: c.panel, RoundedCornerShape(2.dp)).border(1.dp, c.border, RoundedCornerShape(2.dp)))
                    n.optString("icon").isNotEmpty() -> OrcaIcon(n.optString("icon"), 18.dp, alpha = if (enabled) 1f else 0.4f)
                }
            }
        },
        trailingIcon = trailing?.let { t -> { Text(t, style = deskText(13, color = c.textDim)) } })
}

/** run a menu action; the ones that need the app (dialogs, pickers, panels) come back as {"ui": ...} */
fun runMenu(app: AppState, id: String, extra: JSONObject = JSONObject()) {
    val args = JSONObject(extra.toString()).put("id", id)
    app.call("menu.run", args) { r -> handleMenuResult(app, r as? JSONObject ?: return@call, id) }
}

fun handleMenuResult(app: AppState, r: JSONObject, id: String) {
    if (r.has("objects")) { app.project = r; app.redraw() }
    r.optString("notice").takeIf { it.isNotEmpty() }?.let { app.toast = it }
    if (r.optBoolean("switch_to_objects")) app.objectsMode = true
    r.optJSONObject("tip")?.let { app.menuDialog = JSONObject().put("ui", "tip").put("tip", it) }
    when (r.optString("ui")) {
        "" -> {}
        "rename", "number", "clone", "confirm" -> app.menuDialog = r
        "import" -> app.openImporter()
        "objects_mode" -> app.objectsMode = true
        "object_table" -> app.objectTable = true
        "reload" -> app.appContext?.let { app.reloadFromDisk(it) }
        "load_part" -> app.appContext?.let { app.loadPart(it, r.optString("type")) }
        "replace" -> app.appContext?.let { app.replaceVolume(it) }
        "replace_all" -> app.appContext?.let { app.toast = r.optString("title"); app.replaceAll(it) }
        "layers" -> { app.objectsMode = true; app.layerItem = jobj("obj" to r.optInt("obj"), "root" to true) }
        "gizmo" -> if (r.optString("gizmo") == "SVG" && r.optString("type").isNotEmpty()) {
            // GLGizmoSVG::create_volume: choose_svg_file first
            val ctx = app.appContext
            app.openDocument(arrayOf("image/svg+xml", "*/*")) { u -> if (ctx != null) app.openSvg(ctx, u, r.optString("type")) }
        } else { app.gizmoArgs = r.optString("type").takeIf { it.isNotEmpty() }?.let { jobj("type" to it) }; app.gizmoRequest++; app.gizmo = r.optString("gizmo") }
        "labels" -> app.redraw()
        "export" -> {
            val base = app.project.optString("project").ifEmpty { "Untitled" }
            when (r.optString("what")) {
                "stl" -> app.export("export.stl", jobj("selection_only" to true), "$base.stl", "STL exported")
                "drc" -> app.export("export.stl", jobj("selection_only" to true, "format" to "drc"), "$base.drc", "DRC exported")
                "stls" -> app.exportToFolder("export.stl", jobj("selection_only" to true), "STLs exported")
                "drcs" -> app.exportToFolder("export.stl", jobj("selection_only" to true, "format" to "drc"), "DRCs exported")
                else -> app.toast = tr("Not available")
            }
        }
        else -> app.toast = tr("Not available")
    }
}

/** the dialogs the menu actions open (ObjectList::rename_item, GetNumberFromUser, CloneDialog, confirmations, TipsDialog) */
@Composable
fun MenuDialogs(app: AppState) {
    val d = app.menuDialog ?: return
    val close = { app.menuDialog = null }
    when (d.optString("ui")) {
        // NotificationManager::OrcaSyncConflictNotification: Pull (Delete for a preset deleted in the cloud) / Force push
        "cloud_conflict" -> DeskDialog(tr("Resolve cloud sync conflict"), onDismiss = close,
            okLabel = tr("Force push"), cancelLabel = if (d.optInt("code") == -3) tr("Delete") else tr("Pull"),
            ok = { close(); app.call("cloud.conflict", jobj("id" to d.optInt("id"), "action" to 1)) },
            cancel = { close(); app.call("cloud.conflict", jobj("id" to d.optInt("id"), "action" to 0)) }) {
            Text(d.optString("text"), style = deskText(13))
        }
        // the IDEX/IQEX mode menu of a plate (radio items)
        "imex_mode" -> {
            val items = d.optJSONArray("items") ?: JSONArray()
            DeskDialog(tr("IDEX/IQEX Print Mode"), onDismiss = close, ok = close) {
                d.optString("warning").takeIf { it.isNotEmpty() }?.let { Text(it, style = deskText(13, color = OrcaColors.warning)) }
                for (k in 0 until items.length()) {
                    val it = items.getJSONObject(k)
                    Row(Modifier.fillMaxWidth().clickable {
                        close(); app.projectCall("plate.imex_mode", jobj("plate" to d.optInt("plate"), "mode" to it.optString("mode")))
                    }, verticalAlignment = Alignment.CenterVertically) {
                        RadioButton(it.optBoolean("checked"), null)
                        Text(it.optString("label"), style = deskText(14))
                    }
                }
            }
        }
        // PlateNameEditDialog (Speed Dial's "Rename Plate")
        "plate_rename" -> {
            val plate = d.optInt("plate")
            var text by remember { mutableStateOf(app.project.optJSONArray("plates")?.optJSONObject(plate)?.optString("name") ?: "") }
            DeskDialog(tr("Edit Plate Name"), onDismiss = close, ok = {
                close()
                app.projectCall("plate.set", jobj("plate" to plate, "name" to text))
            }) {
                OutlinedTextField(text, { text = it }, singleLine = true, modifier = Modifier.fillMaxWidth())
            }
        }
        "rename" -> {
            val sel = app.project.optJSONArray("selection")?.optJSONObject(0) ?: run { close(); return }
            val o = app.project.optJSONArray("objects")?.optJSONObject(sel.optInt("obj")) ?: run { close(); return }
            val vol = sel.optInt("vol", -1)
            val current = if (vol >= 0) o.optJSONArray("volumes")?.optJSONObject(vol)?.optString("name") ?: "" else o.optString("name")
            var text by remember { mutableStateOf(current) }
            DeskDialog(tr("Renaming"), onDismiss = close, ok = {
                close()
                if (text.isNotBlank()) app.projectCall("object.rename", jobj("obj" to sel.optInt("obj"), "vol" to vol, "name" to text))
            }) {
                Text(tr("Enter new name") + ":", style = deskText(13))
                OutlinedTextField(text, { text = it }, singleLine = true, modifier = Modifier.fillMaxWidth())
            }
        }
        "number", "clone" -> {
            var text by remember { mutableStateOf(d.optInt("value").toString()) }
            var arrange by remember { mutableStateOf(true) }
            DeskDialog(d.optString("title"), onDismiss = close, ok = {
                close()
                text.toIntOrNull()?.let { v -> runMenu(app, d.optString("id"), JSONObject().put("value", v).put("arrange", arrange)) }
            }) {
                Text(d.optString("label"), style = deskText(13))
                OutlinedTextField(text, { text = it.filter(Char::isDigit) }, singleLine = true, modifier = Modifier.fillMaxWidth(),
                    keyboardOptions = androidx.compose.foundation.text.KeyboardOptions(keyboardType = androidx.compose.ui.text.input.KeyboardType.Number))
                if (d.optString("ui") == "clone") Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(d.optString("arrange_label"), style = deskText(13)); DeskCheck(arrange) { arrange = it }
                }
            }
        }
        "confirm" -> DeskDialog("", onDismiss = close, okLabel = "Yes", cancelLabel = "No", ok = {
            close(); runMenu(app, d.optString("id"), JSONObject().put("confirmed", true))
        }) { Text(d.optString("text"), style = deskText(13)) }
        "restore" -> DeskDialog(d.optString("title"), onDismiss = { close(); app.call("project.restore", jobj("restore" to false)) },
            okLabel = "Yes", cancelLabel = "No", ok = {
                close(); app.call("project.restore", jobj("restore" to true)) { r -> if (r is JSONObject && r.has("objects")) { app.project = r; app.redraw() } }
            }) { Text(d.optString("text"), style = deskText(13)) }
        "zip" -> {   // FileArchiveDialog: the archive's model files, all checked
            val entries = d.optJSONArray("entries") ?: org.json.JSONArray()
            val checked = remember { mutableStateListOf<Boolean>().apply { repeat(entries.length()) { add(true) } } }
            DeskDialog(d.optString("title"), onDismiss = close, okLabel = d.optString("open"), cancelLabel = d.optString("cancel"), ok = {
                close()
                val sel = org.json.JSONArray()
                for (i in 0 until entries.length()) if (checked.getOrElse(i) { false }) sel.put(entries.getJSONObject(i))
                app.call("zip.extract", jobj("path" to d.optString("path"), "entries" to sel)) { r -> if (r is JSONObject && r.has("objects")) { app.project = r; app.redraw() } }
            }) {
                Column(Modifier.heightIn(max = 400.dp).verticalScroll(rememberScrollState())) {
                    for (i in 0 until entries.length()) Row(verticalAlignment = Alignment.CenterVertically) {
                        DeskCheck(checked[i]) { checked[i] = it }
                        Text(entries.getJSONObject(i).optString("path"), style = deskText(13))
                    }
                }
            }
        }
        "message" -> DeskDialog(d.optString("title"), onDismiss = close, ok = close, cancelLabel = null) { Text(d.optString("text"), style = deskText(13)) }
        "tip" -> {
            val t = d.optJSONObject("tip") ?: JSONObject()
            var dontShow by remember { mutableStateOf(false) }
            DeskDialog(t.optString("title"), onDismiss = close, ok = {
                close(); if (dontShow) app.call("app.set", jobj("key" to t.optString("key"), "value" to "1"))
            }, cancelLabel = null) {
                Text(t.optString("text"), style = deskText(13))
                Row(verticalAlignment = Alignment.CenterVertically) { DeskCheck(dontShow) { dontShow = it }; Text(tr("Don't show again"), style = deskText(13)) }
            }
        }
        else -> close()
    }
}

/** a desktop-style dialog: title, content, OK (teal) / Cancel */
@Composable
fun DeskDialog(title: String, onDismiss: () -> Unit, ok: () -> Unit, okLabel: String = "OK", cancelLabel: String? = "Cancel",
               cancel: (() -> Unit)? = null, content: @Composable ColumnScope.() -> Unit) {
    AlertDialog(onDismissRequest = onDismiss, containerColor = desk().panel, shape = RoundedCornerShape(6.dp),
        title = if (title.isNotEmpty()) ({ Text(title, style = deskText(16)) }) else null,
        text = { Column(verticalArrangement = Arrangement.spacedBy(8.dp), content = content) },
        confirmButton = { PanelButton(okLabel, primary = true, onClick = ok) },
        dismissButton = cancelLabel?.let { l -> { PanelButton(l, onClick = cancel ?: onDismiss) } })
}
