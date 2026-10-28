package org.orcaport.mobile.prepare

import android.graphics.BitmapFactory
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import org.json.JSONArray
import org.json.JSONObject
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import org.orcaport.mobile.AppState
import org.orcaport.mobile.engine.jobj
import org.orcaport.mobile.settings.*
import org.orcaport.mobile.ui.*

/**
 * Plater::Sidebar: the Printer card (model picture, preset, variant, nozzle), the filament list and the Process
 * section with the Global / Objects switch and the process settings embedded, as on the desktop.
 */
@Composable
fun Sidebar(app: AppState, modifier: Modifier = Modifier) {
    val st = app.presets
    val c = desk()
    val process = rememberParams(app, "print")
    // the Search key (Sidebar::can_search): the process settings' search
    LaunchedEffect(app.settingsSearch) { if (app.settingsSearch > 0) process.searching = true }
    // Sidebar's scrolled panel: on a short screen the whole column scrolls and the settings keep a usable height
    BoxWithConstraints(modifier.background(c.panel)) {
    val short = maxHeight < 900.dp
    val fill = if (short) Modifier.height(maxHeight * 0.85f) else Modifier
    Column(if (short) Modifier.fillMaxWidth().verticalScroll(rememberScrollState()) else Modifier.fillMaxSize()) {
        // Printer
        SectionTitle("printer", "Printer") {
            BoxedIcon("monitor_signal_strong", "Connection") { app.openPrinterHost() }
            BoxedIcon("settings", "Click to edit preset") { app.editing = "printer" }
        }
        PrinterCard(app, st.optJSONObject("printer") ?: JSONObject())
        // Filament
        FilamentSection(app, st)
        // Process
        SectionTitle("process", "Process") {
            DeskSwitch("Global", "Objects", app.objectsMode) { app.objectsMode = it }
            Spacer(Modifier.width(4.dp))
            ModeSwitch(process)
        }
        if (app.objectsMode) {
            Column(if (short) fill else Modifier.weight(1f)) {
                Box(Modifier.weight(0.45f)) { ObjectList(app) }
                ObjectSettings(app, Modifier.weight(0.55f))
            }
        } else {
            PresetRow(process)
            val pages = visiblePages(process)
            if (!process.searching) PageTabs(process, pages)
            HorizontalDivider(color = c.border)
            ParamsContent(process, pages, (if (short) fill else Modifier.weight(1f)).fillMaxWidth())
        }
    }
    }
}

@Composable
private fun PrinterCard(app: AppState, p: JSONObject) {
    val c = desk()
    var list by remember { mutableStateOf<List<JSONObject>>(emptyList()) }
    var open by remember { mutableStateOf(false) }
    val dirty = p.optBoolean("dirty")
    Column(Modifier.padding(10.dp)) {
        Row(Modifier.height(IntrinsicSize.Min)) {
            Row(Modifier.weight(1f).fillMaxHeight().clip(RoundedCornerShape(6.dp)).border(1.dp, c.border, RoundedCornerShape(6.dp))
                .clickable { app.presetList("printer") { a -> list = (0 until a.length()).map { a.getJSONObject(it) }; open = true } }
                .padding(8.dp), verticalAlignment = Alignment.CenterVertically) {
                val cover = p.optString("cover")
                val bmp = remember(cover) { if (cover.isNotEmpty()) runCatching { BitmapFactory.decodeFile(cover)?.asImageBitmap() }.getOrNull() else null }
                if (bmp != null) Image(bmp, null, Modifier.size(56.dp).clip(RoundedCornerShape(4.dp)))
                else OrcaIcon("printer_thumbnail", 56.dp)
                Spacer(Modifier.width(8.dp))
                OrcaIcon("drop_down", 12.dp)
                Spacer(Modifier.width(8.dp))
                Text(p.optString("label") + if (dirty) "*" else "", Modifier.weight(1f), style = deskText(14, color = if (dirty) c.modified else c.text),
                    maxLines = 2, overflow = TextOverflow.Ellipsis)
            }
            Spacer(Modifier.width(8.dp))
            // Variant (nozzle diameter of the same model)
            val variants = p.optJSONArray("variants")
            var vopen by remember { mutableStateOf(false) }
            Column(Modifier.width(88.dp).fillMaxHeight().clip(RoundedCornerShape(6.dp)).border(1.dp, c.border, RoundedCornerShape(6.dp))
                .clickable(enabled = (variants?.length() ?: 0) > 1) { vopen = true }.padding(6.dp),
                horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.Center) {
                Text(tr("Variant"), style = deskText(11, color = c.textDim))
                Row(verticalAlignment = Alignment.CenterVertically) {
                    OrcaIcon("drop_down", 10.dp); Spacer(Modifier.width(6.dp))
                    Text(p.optString("variant").ifEmpty { "-" }, style = deskText(14))
                }
                DropdownMenu(vopen, { vopen = false }) {
                    if (variants != null) for (i in 0 until variants.length()) {
                        val v = variants.getJSONObject(i)
                        DropdownMenuItem(text = { Text(v.optString("variant"), style = deskText(14)) }, onClick = { vopen = false; app.selectPreset("printer", v.optString("name")) })
                    }
                }
            }
        }
        Spacer(Modifier.height(6.dp))
        Text(tr("Nozzle"), style = deskText(12, color = c.textDim))
        Row(Modifier.padding(top = 2.dp), verticalAlignment = Alignment.CenterVertically) {
            Text("%.1f".format(p.optDouble("nozzle_diameter", 0.4)), Modifier.width(36.dp), style = deskText(13))
            DeskCombo(p.optString("nozzle_volume_type", "Standard"), Modifier.weight(1f), height = 32.dp) { app.editing = "printer" }
        }
    }
    if (open) PresetPicker(list, p.optString("name"), { open = false }, listOf(
        tr("Select/Remove printers (system presets)") to { app.wizard = "models" },
        tr("Create printer") to { app.createPrinter = true })) { open = false; app.selectPreset("printer", it) }
}

/**
 * Sidebar's filament area: the "Project Filaments" title (fold, count, Flushing volumes, remove last, add, Set filaments
 * to use), the physical filaments in two columns, the mixed filaments (Sidebar::update_mixed_filament_list).
 */
@Composable
private fun FilamentSection(app: AppState, st: JSONObject) {
    val c = desk()
    val area = st.optJSONObject("filament_area") ?: JSONObject()
    val fil = st.optJSONArray("filaments") ?: JSONArray()
    val physical = area.optJSONArray("physical")?.let { a -> (0 until a.length()).map { a.getInt(it) } } ?: (0 until fil.length()).toList()
    val semm = area.optBoolean("semm", true)
    val multi = semm && physical.size > 1
    var folded by remember { mutableStateOf(false) }
    var preferred by remember { mutableIntStateOf(10) }
    LaunchedEffect(Unit) { app.call("app.get", jobj("key" to "filaments_area_preferred_count")) { (it as? String)?.toIntOrNull()?.let { n -> preferred = n } } }
    fun result(r: Any?) {
        if (r !is JSONObject) return
        r.optJSONObject("presets")?.let { app.presets = it }
        r.optJSONObject("project")?.let { app.project = it }
    }
    Row(Modifier.fillMaxWidth().background(c.header).height(48.dp).clickable { folded = !folded }.padding(horizontal = 10.dp),
        verticalAlignment = Alignment.CenterVertically) {
        BoxedIcon("filament", null) { folded = !folded }
        Text(tr("Project Filaments"), Modifier.weight(1f, fill = false).padding(start = 10.dp, end = 6.dp), style = deskText(15), maxLines = 1,
            overflow = TextOverflow.Ellipsis)
        // update_filaments_counter: shown while folded or when the list is longer than the preferred count
        if (folded || physical.size > preferred) Text("(${physical.size})", style = deskText(15))
        Spacer(Modifier.weight(0.01f))
        // Sidebar::enable_purge_mode_btn (TabPrinter::update)
        var purge by remember { mutableStateOf<JSONObject?>(null) }
        if (area.optBoolean("purge_mode")) Text(tr("Purge mode"), Modifier.padding(end = 4.dp).clip(RoundedCornerShape(12.dp)).background(OrcaColors.accent)
            .clickable { app.call("purge_mode.get") { if (it is JSONObject) purge = it } }.padding(horizontal = 8.dp, vertical = 5.dp),
            style = deskText(12, color = Color.White))
        purge?.let { p -> PurgeModeDialog(app, p) { purge = null } }
        if (multi) {
            val modified = area.optBoolean("flush_modified")
            Text(tr("Flushing volumes"), Modifier.padding(end = 4.dp).clip(RoundedCornerShape(12.dp))
                .background(if (modified) c.panel else OrcaColors.accent).border(1.dp, if (modified) Color(0xFFFF6F00) else OrcaColors.accent, RoundedCornerShape(12.dp))
                .clickable { if ("flushing" !in app.webDialogs) app.webDialogs.add("flushing") }.padding(horizontal = 8.dp, vertical = 5.dp),
                style = deskText(12, color = if (modified) c.text else Color.White))
            BoxedIcon("delete_filament", tr("Remove last filament")) { app.call("filaments.delete") { result(it) } }
        }
        if (semm) BoxedIcon("add_filament", tr("Add one filament"), enabled = !area.optBoolean("at_limit")) { app.call("filaments.add") { result(it) } }
        BoxedIcon("settings", tr("Set filaments to use")) { app.wizard = "filaments" }
    }
    if (folded) return
    Column(Modifier.padding(horizontal = 10.dp, vertical = 6.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        // BBS: filament double columns
        for (row in physical.chunked(2)) Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            for (i in row) Box(Modifier.weight(1f)) { FilamentSlot(app, i, fil.optJSONObject(i) ?: JSONObject(), multi, fil, ::result) }
            if (row.size == 1 && physical.size > 1) Spacer(Modifier.weight(1f))
        }
        if (physical.size >= 2) MixedFilaments(app, area, fil, ::result)
    }
}

/** one physical slot: the colour swatch with its number (colour picker), the preset combo, the edit button / action menu */
@Composable
private fun FilamentSlot(app: AppState, slot: Int, f: JSONObject, multi: Boolean, all: JSONArray, result: (Any?) -> Unit) {
    val c = desk()
    var open by remember { mutableStateOf(false) }
    var list by remember { mutableStateOf<List<JSONObject>>(emptyList()) }
    var color by remember { mutableStateOf(false) }
    var menu by remember { mutableStateOf(false) }
    Row(verticalAlignment = Alignment.CenterVertically) {
        FilamentSwatch(f.optString("color"), "${slot + 1}") { color = true }
        Spacer(Modifier.width(6.dp))
        DeskCombo(f.optString("label"), Modifier.weight(1f), textColor = if (f.optBoolean("compatible", true)) null else OrcaColors.error) {
            app.presetList("filament") { a -> list = (0 until a.length()).map { a.getJSONObject(it) }; open = true }
        }
        Spacer(Modifier.width(4.dp))
        Box {
            val edit = { app.call("presets.select", jobj("type" to "filament", "name" to f.optString("name"))) { app.editing = "filament" } }
            // MenuFactory::filament_action_menu with several filaments on one extruder, the editor otherwise
            BoxedIcon(if (multi) "menu_filament" else "edit", if (multi) null else tr("Click to edit preset")) { if (multi) menu = true else edit() }
            DropdownMenu(menu, { menu = false }) {
                MenuItem(tr("Edit")) { menu = false; edit() }
                SubMenu(tr("Merge with")) { close ->
                    for (j in 0 until all.length()) if (j != slot) {
                        val o = all.optJSONObject(j) ?: continue
                        MenuItem(if (o.optBoolean("mixed")) tr("Filament %d").replace("%d", "${j + 1}") else o.optString("label")) {
                            close(); menu = false; app.call("filaments.merge", jobj("from" to slot, "to" to j)) { result(it) }
                        }
                    }
                }
                MenuItem(tr("Decompose Color")) { menu = false; app.call("filaments.decompose", jobj("index" to slot)) { result(it) } }
                HorizontalDivider()
                MenuItem(tr("Delete")) { menu = false; app.call("filaments.delete", jobj("index" to slot)) { result(it) } }
            }
        }
    }
    if (open) PresetPicker(list, f.optString("name"), { open = false }, listOf(tr("Add/Remove filament") to { app.wizard = "filaments" })) {
        open = false; app.selectPreset("filament", it, slot)
    }
    if (color) ColorPickerDialog(parseColor(f.optString("color")), onDismiss = { color = false }) { hex ->
        color = false
        app.call("presets.filament_color", jobj("slot" to slot, "color" to hex)) { if (it is JSONObject) app.presets = it }
    }
}

/** get_extruder_color_icon: a filament colour with its number; a bottom-to-top ramp for a gradient mix */
@Composable
fun FilamentSwatch(color: String, label: String, size: androidx.compose.ui.unit.Dp = 32.dp, ramp: List<String>? = null, onClick: (() -> Unit)? = null) {
    val c = desk()
    val col = parseColor(color) ?: Color.White
    val mid = ramp?.let { parseColor(it[it.size / 2]) } ?: col
    val lum = 0.299 * mid.red + 0.587 * mid.green + 0.114 * mid.blue
    val bg = if (ramp != null && ramp.size > 1) Modifier.background(androidx.compose.ui.graphics.Brush.verticalGradient(ramp.reversed().map { parseColor(it) ?: Color.Gray }))
             else Modifier.background(col)
    Box(Modifier.size(size).clip(RoundedCornerShape(3.dp)).then(bg).border(1.dp, c.border, RoundedCornerShape(3.dp))
        .then(if (onClick != null) Modifier.clickable(onClick = onClick) else Modifier), Alignment.Center) {
        Text(label, style = deskText(if (size < 28.dp) 12 else 14, color = if (lum > 0.6) Color.Black else Color.White))
    }
}

/** the mixed filament block: "+ Add Mixed Filament", or the title with remove last / remove all / add and the slots */
@Composable
private fun MixedFilaments(app: AppState, area: JSONObject, all: JSONArray, result: (Any?) -> Unit) {
    val c = desk()
    val mixed = area.optJSONArray("mixed") ?: JSONArray()
    val atLimit = area.optBoolean("at_limit")
    if (mixed.length() == 0) {
        if (!atLimit) Row(Modifier.fillMaxWidth().padding(vertical = 4.dp).height(32.dp).clip(RoundedCornerShape(4.dp)).background(c.field)
            .clickable { app.call("mixed.add") { result(it) } }, horizontalArrangement = Arrangement.Center, verticalAlignment = Alignment.CenterVertically) {
            OrcaIcon("add_filament", 16.dp); Spacer(Modifier.width(4.dp)); Text(tr("Add Mixed Filament"), style = deskText(13))
        }
        return
    }
    Row(Modifier.fillMaxWidth().padding(top = 6.dp), verticalAlignment = Alignment.CenterVertically) {
        Text(tr("Mixed Filament"), Modifier.weight(1f), style = deskText(14, FontWeight.Bold))
        BoxedIcon("delete_filament", tr("Remove last mixed filament")) { app.call("mixed.delete_last") { result(it) } }
        BoxedIcon("delete_all_filaments", tr("Remove all mixed filaments")) { app.call("mixed.remove_all") { result(it) } }
        BoxedIcon("add_filament", tr("Add mixed filament"), enabled = !atLimit) { app.call("mixed.add") { result(it) } }
    }
    for (row in (0 until mixed.length()).chunked(2)) Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        for (k in row) Box(Modifier.weight(1f)) { MixedSlot(app, k, mixed.getJSONObject(k), all, result) }
        if (row.size == 1) Spacer(Modifier.weight(1f))
    }
}

@Composable
private fun MixedSlot(app: AppState, k: Int, m: JSONObject, all: JSONArray, result: (Any?) -> Unit) {
    val c = desk()
    val cfg = m.optInt("index")
    val comps = m.optJSONArray("components") ?: JSONArray()
    val ratios = m.optJSONArray("ratios") ?: JSONArray()
    val gradient = m.optBoolean("gradient")
    val ramp = m.optJSONArray("ramp")?.let { a -> (0 until a.length()).map { a.getString(it) } }
    var menu by remember { mutableStateOf(false) }
    val edit = { app.call("mixed.edit", jobj("index" to k)) { result(it) }; Unit }
    Row(verticalAlignment = Alignment.CenterVertically) {
        FilamentSwatch(m.optString("color"), "${cfg + 1}", ramp = ramp, onClick = edit)
        Spacer(Modifier.width(6.dp))
        // the components: swatch + ratio, "+" between them ("→" for a gradient, without ratios)
        Row(Modifier.weight(1f).height(36.dp).clip(RoundedCornerShape(4.dp)).background(c.field).border(1.dp, c.border, RoundedCornerShape(4.dp))
            .clickable(onClick = edit).padding(horizontal = 4.dp), verticalAlignment = Alignment.CenterVertically) {
            for (i in 0 until comps.length()) {
                if (i > 0) Text(if (gradient) "→" else "+", Modifier.padding(horizontal = 2.dp), style = deskText(12))
                val id = comps.getInt(i)
                val o = all.optJSONObject(id - 1)
                if (o != null && id >= 1) FilamentSwatch(o.optString("color"), "$id", 20.dp) else
                    Box(Modifier.size(20.dp).border(1.dp, c.textDim), Alignment.Center) { Text("—", style = deskText(12, color = c.textDim)) }
                if (!gradient) Text("${(ratios.optDouble(i, 0.0) * 100 + 0.5).toInt()}%", Modifier.padding(start = 1.dp), style = deskText(12), maxLines = 1, softWrap = false)
            }
        }
        Spacer(Modifier.width(4.dp))
        Box {
            BoxedIcon(if (m.optBoolean("broken")) "error" else "menu_filament",
                if (m.optBoolean("broken")) tr("Mixed filament has broken component references") else tr("Edit / Delete / Merge")) { menu = true }
            DropdownMenu(menu, { menu = false }) {
                MenuItem(tr("Edit")) { menu = false; edit() }
                if (all.length() > 1) SubMenu(tr("Merge with")) { close ->
                    for (j in 0 until all.length()) if (j != cfg) {
                        val o = all.optJSONObject(j) ?: continue
                        MenuItem(if (o.optBoolean("mixed")) tr("Filament %d").replace("%d", "${j + 1}") else o.optString("label")) {
                            close(); menu = false; app.call("filaments.merge", jobj("from" to cfg, "to" to j)) { result(it) }
                        }
                    }
                }
                HorizontalDivider()
                MenuItem(tr("Delete")) { menu = false; app.call("mixed.delete", jobj("index" to k)) { result(it) } }
            }
        }
    }
}

/**
 * Objects mode below the list: the settings of the selected object or part (ParamsPanel with TabPrintObject /
 * TabPrintPart): the process values with the item's overrides, an unlocked lock on each override (reset removes it).
 */
@Composable
private fun ObjectSettings(app: AppState, modifier: Modifier) {
    val sel = app.project.optJSONArray("selection")
    val item = if (sel != null && sel.length() == 1) sel.getJSONObject(0) else null
    val c = desk()
    if (item == null) {
        return   // upstream shows no object settings without a selection
    }
    val li = app.layerItem?.takeIf { it.optInt("obj") == item.optInt("obj") && item.optInt("vol", -1) < 0 }
    if (li != null && li.optBoolean("root")) { ObjectLayersPanel(app, li.optInt("obj"), modifier); return }
    val m = rememberParams(app, "print")
    val range = li?.optJSONArray("range")
    val key = "${item.optInt("obj")}/${item.optInt("vol", -1)}/${range ?: ""}"
    LaunchedEffect(key, app.project) {
        m.target = JSONObject().put("obj", item.optInt("obj")).put("vol", item.optInt("vol", -1)).apply { if (range != null) put("range", range) }
        runCatching { m.reload() }
    }
    if (m.target == null) return
    Column(modifier.fillMaxWidth()) {
        HorizontalDivider(color = c.border)
        val pages = visiblePages(m)
        PageTabs(m, pages)
        HorizontalDivider(color = c.border)
        ParamsContent(m, pages, Modifier.weight(1f).fillMaxWidth())
    }
}


/** ObjectLayers: a row per height range (start, end, layer height), remove and add-after buttons */
@Composable
private fun ObjectLayersPanel(app: AppState, obj: Int, modifier: Modifier) {
    var st by remember { mutableStateOf(JSONObject()) }
    LaunchedEffect(obj, app.project) { app.call("layers.state", jobj("obj" to obj)) { r -> (r as? JSONObject)?.optJSONObject("layers")?.let { st = it } } }
    fun call(cmd: String, vararg kv: Pair<String, Any?>) = app.call(cmd, jobj("obj" to obj, *kv)) { r ->
        if (r is JSONObject) {
            r.optString("notice").takeIf { it.isNotEmpty() }?.let { app.toast = it }
            r.optJSONObject("layers")?.let { st = it }
            if (r.has("objects")) app.project = r
        }
    }
    val c = desk()
    val l = st.optJSONObject("labels") ?: JSONObject()
    val ranges = st.optJSONArray("ranges") ?: JSONArray()
    fun fmt(v: Double) = String.format(java.util.Locale.ROOT, "%.2f", v)
    Column(modifier.fillMaxWidth().verticalScroll(rememberScrollState()).padding(horizontal = 10.dp, vertical = 8.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        HorizontalDivider(color = c.border)
        Row(verticalAlignment = Alignment.CenterVertically) {
            OrcaIcon("height_range_layer", 18.dp); Spacer(Modifier.width(6.dp))
            Text(l.optString("title"), style = deskText(14, FontWeight.Bold))
        }
        Row {
            Text(l.optString("start"), Modifier.weight(1f), style = deskText(12, color = c.textDim))
            Text(l.optString("end"), Modifier.weight(1f), style = deskText(12, color = c.textDim))
            Text(l.optString("layer_height"), Modifier.weight(1f), style = deskText(12, color = c.textDim))
            Spacer(Modifier.width(72.dp))
        }
        for (k in 0 until ranges.length()) {
            val r = ranges.getJSONObject(k)
            val range = JSONArray().put(r.getDouble("min")).put(r.getDouble("max"))
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                DeskInput(fmt(r.getDouble("min")), true, true, Modifier.weight(1f), sidetext = "mm") { s -> s.replace(',', '.').toDoubleOrNull()?.let { call("layers.edit", "range" to range, "min" to it) } }
                DeskInput(fmt(r.getDouble("max")), true, true, Modifier.weight(1f), sidetext = "mm") { s -> s.replace(',', '.').toDoubleOrNull()?.let { call("layers.edit", "range" to range, "max" to it) } }
                DeskInput(fmt(r.getDouble("layer_height")), true, true, Modifier.weight(1f), sidetext = "mm") { s -> s.replace(',', '.').toDoubleOrNull()?.let { call("layers.edit", "range" to range, "layer_height" to it) } }
                BoxedIcon("delete", l.optString("delete"), size = 32.dp) { call("layers.delete", "range" to range) }
                BoxedIcon("add", l.optString("add"), size = 32.dp) { call("layers.add_after", "range" to range) }
            }
        }
    }
}

/** PurgeModeDialog: two cards (Standard, Fast / Prime Saving), Confirm stores the project's prime_volume_mode */
@Composable
private fun PurgeModeDialog(app: AppState, p: JSONObject, onClose: () -> Unit) {
    val c = desk()
    var mode by remember { mutableIntStateOf(p.optInt("mode")) }
    val options = p.optJSONArray("options") ?: JSONArray()
    DeskDialog(p.optString("title"), onDismiss = onClose, okLabel = p.optString("confirm"), cancelLabel = p.optString("cancel"), ok = {
        app.call("purge_mode.set", jobj("mode" to mode)) { if (it is JSONObject) app.presets = it }
        onClose()
    }) {
        Row(Modifier.width(480.dp), horizontalArrangement = Arrangement.spacedBy(24.dp)) {
            for (i in 0 until options.length()) {
                val o = options.getJSONObject(i)
                val sel = o.optInt("mode") == mode
                Column(Modifier.weight(1f).heightIn(min = 150.dp).clip(RoundedCornerShape(8.dp))
                    .background(if (sel) Color(0xFFE5F0EE) else c.panel).border(if (sel) 2.dp else 1.dp, if (sel) OrcaColors.accent else Color(0xFFCECECE), RoundedCornerShape(8.dp))
                    .clickable { mode = o.optInt("mode") }.padding(14.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        OrcaIcon(o.optString("icon"), 20.dp); Spacer(Modifier.width(8.dp))
                        Text(o.optString("title"), style = deskText(14, FontWeight.Bold, color = if (sel) Color(0xFF262E30) else c.text))
                    }
                    Text(o.optString("text"), style = deskText(12, color = if (sel) Color(0xFF6B6B6B) else c.textDim))
                }
            }
        }
    }
}
