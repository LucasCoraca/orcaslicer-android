package org.orcaport.mobile.ui

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import org.json.JSONArray
import org.json.JSONObject
import org.orcaport.mobile.AppState
import org.orcaport.mobile.engine.jobj
import org.orcaport.mobile.prepare.PanelButton
import org.orcaport.mobile.settings.keepFile
import org.orcaport.mobile.settings.strings

/**
 * CreatePrinterPresetDialog (the printer combo's "Create printer"). Page 1: a new printer (vendor and model, or a custom
 * one, with its printable space, origin, bed model / texture and height) or a nozzle for an existing printer. Page 2:
 * the printer whose presets are cloned, from templates or from that printer, and which filament / process presets.
 * The checks and the cloning are the engine's (CreatePrinter.cpp).
 */
@Composable
fun CreatePrinterDialog(app: AppState, onClose: () -> Unit) {
    var st by remember { mutableStateOf<JSONObject?>(null) }
    var page by remember { mutableIntStateOf(1) }
    var busy by remember { mutableStateOf(false) }
    LaunchedEffect(Unit) { app.call("create_printer.open") { if (it is JSONObject) st = it } }
    val close = { app.call("create_printer.close"); onClose() }
    // page 1's inputs
    var newPrinter by remember { mutableStateOf(true) }
    var vendor by remember { mutableStateOf("") }
    var model by remember { mutableStateOf("") }
    var customPrinter by remember { mutableStateOf(false) }
    var customVendor by remember { mutableStateOf("") }
    var customModel by remember { mutableStateOf("") }
    var printer by remember { mutableStateOf("") }
    var nozzle by remember { mutableIntStateOf(0) }
    var customNozzle by remember { mutableStateOf(false) }
    var nozzleValue by remember { mutableStateOf("") }
    var bedX by remember { mutableStateOf("200") }
    var bedY by remember { mutableStateOf("200") }
    var originX by remember { mutableStateOf("0") }
    var originY by remember { mutableStateOf("0") }
    var height by remember { mutableStateOf("200") }
    var bedModel by remember { mutableStateOf("") }
    var bedTexture by remember { mutableStateOf("") }
    // page 2's check boxes
    val filaments = remember { mutableStateListOf<String>() }
    val processes = remember { mutableStateListOf<String>() }
    val focus = LocalFocusManager.current
    val c = desk()

    fun update(r: Any?) {
        if (r !is JSONObject) return
        st = r
        // the clones' lists start unchecked, as the dialog's check boxes
        filaments.retainAll(r.optJSONArray("filaments").names()); processes.retainAll(r.optJSONArray("processes").names())
    }
    fun send(cmd: String, args: JSONObject, then: (JSONObject) -> Unit) {
        busy = true
        app.call(cmd, args) { r -> busy = false; if (r is JSONObject) then(r) }
    }
    val s = st
    Dialog(close, DialogProperties(usePlatformDefaultWidth = false)) {
        Surface(Modifier.widthIn(max = 720.dp).fillMaxWidth(0.95f).heightIn(max = 760.dp), shape = RoundedCornerShape(6.dp), color = c.panel) {
            Column(Modifier.padding(16.dp)) {
                Text(tr("Create Printer/Nozzle"), style = deskText(16))
                Spacer(Modifier.height(8.dp))
                if (s == null) { Box(Modifier.fillMaxWidth().height(160.dp), Alignment.Center) { CircularProgressIndicator(color = OrcaColors.accent) }; return@Column }
                Column(Modifier.weight(1f, fill = false).verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                    if (page == 1) {
                        Item(tr("Create Type")) {
                            Radio(tr("Create Printer"), newPrinter) { newPrinter = true }
                            Radio(tr("Create Nozzle for Existing Printer"), !newPrinter) { newPrinter = false }
                        }
                        Item(tr("Printer")) {
                            if (!newPrinter) {
                                val printers = s.optJSONArray("printers").strings()
                                DeskChoice(printers, printers.indexOf(printer), Modifier.fillMaxWidth(), display = printer.ifEmpty { tr("Select Printer") }) { printer = printers[it] }
                            } else if (customPrinter) {
                                DeskInput(customVendor, true, false, Modifier.fillMaxWidth(), placeholder = tr("Input Custom Vendor")) { customVendor = it }
                                DeskInput(customModel, true, false, Modifier.fillMaxWidth(), placeholder = tr("Input Custom Model")) { customModel = it }
                            } else {
                                val vendors = s.optJSONArray("vendors").strings()
                                val models = s.optJSONObject("models")?.optJSONArray(vendor).strings()
                                DeskChoice(vendors, vendors.indexOf(vendor), Modifier.fillMaxWidth(), display = vendor.ifEmpty { tr("Select Vendor") }) {
                                    vendor = vendors[it]
                                    model = s.optJSONObject("models")?.optJSONArray(vendor).strings().firstOrNull() ?: ""
                                }
                                DeskChoice(models, models.indexOf(model), Modifier.fillMaxWidth(), display = model.ifEmpty { tr("Select Model") }) { model = models[it] }
                            }
                            if (newPrinter) CheckRow(tr("Can't find my printer model"), customPrinter) { customPrinter = it }
                        }
                        Item(tr("Nozzle Diameter")) {
                            if (customNozzle) DeskInput(nozzleValue, true, true, Modifier.width(200.dp), placeholder = tr("Input Custom Nozzle Diameter")) { nozzleValue = it }
                            else DeskChoice(s.optJSONArray("nozzles").strings(), nozzle, Modifier.width(200.dp)) { nozzle = it }
                            CheckRow(tr("Can't find my nozzle diameter"), customNozzle) { customNozzle = it }
                        }
                        if (newPrinter) {
                            Item(tr("Bed Shape")) { Text(tr("Rectangle"), style = deskText(14)) }
                            Item(tr("Printable Space")) {
                                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                    DeskInput(bedX, true, true, Modifier.width(110.dp), sidetext = tr("mm")) { bedX = it }
                                    DeskInput(bedY, true, true, Modifier.width(110.dp), sidetext = tr("mm")) { bedY = it }
                                }
                            }
                            Item(tr("Origin")) {
                                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                    DeskInput(originX, true, true, Modifier.width(110.dp), sidetext = tr("mm")) { originX = it }
                                    DeskInput(originY, true, true, Modifier.width(110.dp), sidetext = tr("mm")) { originY = it }
                                }
                            }
                            Item(tr("Hot Bed STL")) { BedFile(app, bedModel, arrayOf("*/*"), "bed_models", listOf(".stl")) { bedModel = it } }
                            Item(tr("Hot Bed SVG")) { BedFile(app, bedTexture, arrayOf("image/png", "image/svg+xml", "*/*"), "bed_textures", listOf(".png", ".svg")) { bedTexture = it } }
                            Item(tr("Max Print Height")) { DeskInput(height, true, true, Modifier.width(110.dp), sidetext = tr("mm")) { height = it } }
                        }
                    } else {
                        Item(tr("Printer Preset")) {
                            Text(tr("Create Based on Current Printer"), style = deskText(13, color = c.textDim))
                            val vendors = s.optJSONArray("preset_vendors").strings()
                            val models = s.optJSONArray("preset_models").strings()
                            val pv = s.optString("preset_vendor"); val pm = s.optString("preset_model")
                            DeskChoice(vendors, vendors.indexOf(pv), Modifier.fillMaxWidth(), enabled = !busy, display = pv.ifEmpty { tr("Select Vendor") }) {
                                send("create_printer.page2", jobj("vendor" to vendors[it])) { r -> update(r) }
                            }
                            DeskChoice(models, models.indexOf(pm), Modifier.fillMaxWidth(), enabled = !busy && pv.isNotEmpty(), display = pm.ifEmpty { tr("Select Model") }) {
                                send("create_printer.page2", jobj("model" to models[it])) { r -> update(r) }
                            }
                        }
                        Item(tr("Presets")) {
                            Radio(tr("Create from Template"), s.optBoolean("from_template"), s.optBoolean("template_enabled") && !busy) {
                                send("create_printer.page2", jobj("from_template" to true)) { r -> update(r) }
                            }
                            Radio(tr("Create Based on Current Printer"), !s.optBoolean("from_template"), !busy) {
                                send("create_printer.page2", jobj("from_template" to false)) { r -> update(r) }
                            }
                        }
                        if (busy) LinearProgressIndicator(Modifier.fillMaxWidth(), color = OrcaColors.accent)
                        PresetChecks(tr("Filament Preset Template"), s.optJSONArray("filaments").names(), filaments)
                        PresetChecks(tr("Process Preset Template"), s.optJSONArray("processes").names(), processes)
                    }
                }
                Spacer(Modifier.height(12.dp))
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                    if (page == 2) PanelButton(tr("Return"), enabled = !busy) { page = 1 }
                    Spacer(Modifier.weight(1f))
                    if (page == 1) PanelButton(tr("OK"), primary = true, enabled = !busy) {
                        focus.clearFocus()   // a field being edited commits first
                        val nozzles = s.optJSONArray("nozzles").strings()
                        send("create_printer.page1", jobj("type" to if (newPrinter) "printer" else "nozzle", "vendor" to vendor, "model" to model,
                            "custom_printer" to customPrinter, "custom_vendor" to customVendor, "custom_model" to customModel, "printer" to printer,
                            "nozzle" to nozzles.getOrElse(nozzle) { "" }, "custom_nozzle" to customNozzle, "nozzle_value" to nozzleValue,
                            "bed_x" to bedX, "bed_y" to bedY, "origin_x" to originX, "origin_y" to originY, "height" to height,
                            "bed_model" to bedModel, "bed_texture" to bedTexture)) { r ->
                            when {
                                r.optBoolean("closed") -> onClose()   // switched to the existing system preset
                                r.optBoolean("ok") -> { update(r); page = 2 }
                            }
                        }
                    } else PanelButton(tr("Create"), primary = true, enabled = !busy) {
                        send("create_printer.create", jobj("filaments" to JSONArray(filaments.toList()), "processes" to JSONArray(processes.toList()))) { r ->
                            when {
                                r.optBoolean("ok") -> { onClose(); if (r.optBoolean("edit")) app.editing = "printer" }
                                r.optBoolean("page1") -> page = 1
                            }
                        }
                    }
                    PanelButton(tr("Cancel"), enabled = !busy, onClick = close)
                }
            }
        }
    }
}

private fun JSONArray?.names(): List<String> = this?.let { a -> List(a.length()) { a.optJSONObject(it)?.optString("name") ?: "" } } ?: emptyList()

/** a row of the dialog: its label on the left (above on a phone), the controls on the right */
@Composable
private fun Item(label: String, content: @Composable ColumnScope.() -> Unit) {
    if (isCompact()) Column(verticalArrangement = Arrangement.spacedBy(6.dp)) { Text(label, style = deskText(14, FontWeight.Bold)); content() }
    else Row {
        Text(label, Modifier.width(160.dp).padding(top = 8.dp), style = deskText(14))
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(6.dp), content = content)
    }
}

@Composable
private fun Radio(label: String, selected: Boolean, enabled: Boolean = true, onClick: () -> Unit) {
    Row(Modifier.clickable(enabled = enabled, onClick = onClick), verticalAlignment = Alignment.CenterVertically) {
        RadioButton(selected, onClick, enabled = enabled, colors = RadioButtonDefaults.colors(selectedColor = OrcaColors.accent))
        Text(label, style = deskText(14, color = if (enabled) desk().text else desk().textDisabled))
    }
}

@Composable
private fun CheckRow(label: String, checked: Boolean, onChange: (Boolean) -> Unit) {
    Row(Modifier.clickable { onChange(!checked) }, verticalAlignment = Alignment.CenterVertically) {
        DeskCheck(checked) { onChange(it) }
        Text(label, style = deskText(13))
    }
}

/** create_hot_bed_stl_item / create_hot_bed_svg_item: Load... and the file's name (load_model_stl / load_texture) */
@Composable
private fun BedFile(app: AppState, value: String, mimes: Array<String>, dir: String, ext: List<String>, onChange: (String) -> Unit) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        PanelButton(tr("Load...")) {
            app.openDocument(mimes) { uri ->
                val ctx = app.appContext ?: return@openDocument
                if (ext.none { app.displayName(ctx, uri).endsWith(it, true) }) { app.toast = tr("Invalid file format."); return@openDocument }
                val path = keepFile(app, uri, dir) ?: return@openDocument
                // STL_SVG_MAX_FILE_SIZE_MB
                if (java.io.File(path).length() > 3 * 1024 * 1024) { app.toast = tr("The file exceeds %d MB, please import again.").replace("%d", "3"); return@openDocument }
                onChange(path)
            }
        }
        Spacer(Modifier.width(8.dp))
        Text(if (value.isEmpty()) tr("Empty") else java.io.File(value).name, Modifier.weight(1f), style = deskText(13, color = desk().textDim), maxLines = 1)
    }
}

/** the filament / process preset templates (create_presets_template_item): check boxes with Select All / Deselect All */
@Composable
private fun PresetChecks(title: String, names: List<String>, chosen: MutableList<String>) {
    Column {
        Text(title, style = deskText(14, FontWeight.Bold))
        Row {
            Text(tr("Select All"), Modifier.clickable { chosen.clear(); chosen.addAll(names) }.padding(6.dp), style = deskText(13, color = OrcaColors.accent))
            Text(tr("Deselect All"), Modifier.clickable { chosen.clear() }.padding(6.dp), style = deskText(13, color = OrcaColors.accent))
        }
        for (name in names) Row(Modifier.fillMaxWidth().clickable { if (name in chosen) chosen.remove(name) else chosen.add(name) }, verticalAlignment = Alignment.CenterVertically) {
            DeskCheck(name in chosen) { on -> if (on) chosen.add(name) else chosen.remove(name) }
            Text(name, style = deskText(13))
        }
    }
}
