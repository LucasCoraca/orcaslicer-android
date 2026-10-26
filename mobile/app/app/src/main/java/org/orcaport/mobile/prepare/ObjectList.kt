package org.orcaport.mobile.prepare

import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import org.json.JSONArray
import org.json.JSONObject
import org.orcaport.mobile.AppState
import org.orcaport.mobile.engine.jobj
import org.orcaport.mobile.settings.parseColor
import org.orcaport.mobile.ui.*

// ObjectDataViewModel / ADD_VOLUME_MENU_ITEMS: the icon of each volume type in the list
private fun volumeIcon(v: JSONObject) = when {
    v.optBoolean("text") -> "menu_obj_text"
    v.optBoolean("svg") -> "menu_obj_svg"
    else -> when (v.optString("type")) {
        "negative" -> "menu_add_negative"
        "modifier" -> "menu_add_modifier"
        "support_blocker" -> "menu_support_blocker"
        "support_enforcer" -> "menu_support_enforcer"
        else -> "menu_add_part"
    }
}

/**
 * GUI_ObjectList: the objects of the project grouped by plate, with the desktop's columns (name, printable, filament,
 * support painting, color painting, sinking, editing) and the child items (settings, parts, height ranges, instances,
 * info items). Tap selects, long press opens the object menu (MenuFactory, built by the engine).
 */
@Composable
fun ObjectList(app: AppState) {
    val objs = app.project.optJSONArray("objects") ?: JSONArray()
    val plates = app.project.optJSONArray("plates") ?: JSONArray()
    val sel = app.project.optJSONArray("selection") ?: JSONArray()
    val selected = remember(sel.toString()) { (0 until sel.length()).map { sel.getJSONObject(it) } }
    val expanded = remember { mutableStateMapOf<String, Boolean>() }
    var menu by remember { mutableStateOf(false) }
    val c = desk()
    Column(Modifier.fillMaxSize()) {
        // header (wxDataViewCtrl columns)
        Row(Modifier.fillMaxWidth().background(c.header).padding(horizontal = 8.dp, vertical = 6.dp), verticalAlignment = Alignment.CenterVertically) {
            Text(tr("Name"), Modifier.weight(1f), style = deskText(13, color = c.textDim))
            ColumnCell { }
            ColumnCell(width = 44.dp) { Text(tr("Fila."), style = deskText(13, color = c.textDim)) }
            ColumnCell { }; ColumnCell { }; ColumnCell { }; ColumnCell { }
        }
        HorizontalDivider(color = c.border)
        Column(Modifier.weight(1f).verticalScroll(rememberScrollState())) {
            val byPlate = (0 until objs.length()).groupBy { i ->
                objs.getJSONObject(i).optJSONArray("instances")?.optJSONObject(0)?.optInt("plate", -1) ?: -1
            }
            for (p in 0 until plates.length()) {
                val list = byPlate[p] ?: continue
                val pl = plates.getJSONObject(p)
                val key = "plate$p"
                val open = expanded[key] ?: true
                // ObjectDataViewModel::AddPlate: "Plate N (name)"
                TreeRow(0, open, true, "Plate ${p + 1}" + (pl.optString("name").takeIf { it.isNotEmpty() }?.let { " ($it)" } ?: ""), null,
                    selected = false, onToggle = { expanded[key] = !open }, onClick = { app.projectCall("plate.select", jobj("plate" to p)) }) {
                    if (pl.optBoolean("locked")) OrcaIcon("lock_normal", 16.dp)
                }
                if (open) for (i in list) ObjectRows(app, i, objs.getJSONObject(i), selected, expanded) { menu = true }
            }
            byPlate[-1]?.forEach { i -> ObjectRows(app, i, objs.getJSONObject(i), selected, expanded) { menu = true } }
        }
    }
    if (menu) Box { OrcaContextMenu(app) { menu = false } }
}

@Composable
private fun ObjectRows(app: AppState, i: Int, o: JSONObject, selected: List<JSONObject>, expanded: MutableMap<String, Boolean>, openMenu: () -> Unit) {
    val vols = o.optJSONArray("volumes") ?: JSONArray()
    val insts = o.optJSONArray("instances") ?: JSONArray()
    val ranges = o.optJSONArray("layer_ranges") ?: JSONArray()
    val cats = o.optJSONArray("categories") ?: JSONArray()
    // children: settings, parts (when more than one or not a plain part), layers, instances, info items
    val showParts = vols.length() > 1 || (vols.length() == 1 && vols.getJSONObject(0).optString("type") != "part")
    val infos = buildList {
        if (o.optBoolean("supports_painted")) add("toolbar_support" to "Support Painting")
        if (o.optBoolean("mm_painted")) add("mmu_segmentation" to "Color Painting")
        if (o.optBoolean("fuzzy_painted")) add("objlist_fuzzy_skin_paint" to "Paint-on fuzzy skin")
        if (o.optBoolean("variable_layer_height")) add("layers" to "Variable layer height")
    }
    val hasChildren = showParts || cats.length() > 0 || ranges.length() > 0 || insts.length() > 1 || infos.isNotEmpty()
    val key = "obj$i"
    val open = expanded[key] ?: true
    val isSel = selected.any { it.optInt("obj") == i && it.optInt("vol") < 0 }
    fun select(inst: Int = -1, vol: Int = -1, layer: JSONObject? = null) {
        app.layerItem = layer
        app.projectCall("selection.set", jobj("items" to JSONArray().put(jobj("obj" to i, "inst" to inst, "vol" to vol))))
    }
    val warning = o.optInt("open_edges") > 0 || o.optInt("repaired_errors") > 0
    TreeRow(1, open, hasChildren, o.optString("name"), if (warning) "obj_warning" else if (o.optBoolean("cut")) "cut_" else null, isSel,
        onToggle = { expanded[key] = !open }, onClick = { select() }, onLongClick = { if (!isSel) select(); openMenu() }) {
        PrintableCell(o.optBoolean("printable", true) && (0 until insts.length()).all { insts.getJSONObject(it).optBoolean("printable", true) }) {
            select(); runMenu(app, "printable")
        }
        FilamentCell(app, o.optInt("extruder", 0).coerceAtLeast(1)) { e -> select(); runMenu(app, "filament:$e") }
        IconCell(if (o.optBoolean("supports_painted")) "objlist_support_painting" else null)
        IconCell(if (o.optBoolean("mm_painted")) "objlist_color_painting" else null)
        IconCell(if (o.optBoolean("sinking")) "objlist_sinking" else null)
        ColumnCell { Box(Modifier.size(28.dp).clickable { select(); app.objectsMode = true; app.editingObject = true }, Alignment.Center) { OrcaIcon("settings", 18.dp) } }
    }
    if (!open) return
    if (cats.length() > 0)
        TreeRow(2, false, false, (0 until cats.length()).joinToString(", ") { cats.getString(it) }, "settings", false,
            onClick = { select(); app.editingObject = true })
    if (showParts) for (v in 0 until vols.length()) {
        val vo = vols.getJSONObject(v)
        val vSel = selected.any { it.optInt("obj") == i && it.optInt("vol") == v }
        TreeRow(2, false, false, vo.optString("name"), volumeIcon(vo), vSel, onClick = { select(vol = v) }, onLongClick = { if (!vSel) select(vol = v); openMenu() }) {
            ColumnCell { }
            if (vo.optString("type") == "part" || vo.optString("type") == "modifier")
                FilamentCell(app, vo.optInt("extruder", 0), allowDefault = vo.optString("type") == "modifier") { e -> select(vol = v); runMenu(app, "filament:$e") }
            else ColumnCell(width = 44.dp) { }
            ColumnCell { }; ColumnCell { }; ColumnCell { }
            ColumnCell { Box(Modifier.size(28.dp).clickable { select(vol = v); app.editingObject = true }, Alignment.Center) { OrcaIcon("settings", 18.dp) } }
        }
    }
    if (ranges.length() > 0) {
        // the Layers item (the ObjectLayers panel) and one item per range (its settings, TabPrintLayer)
        val li = app.layerItem
        val rootSel = li != null && li.optInt("obj") == i && li.optBoolean("root")
        TreeRow(2, true, true, "Layers", "height_range_layer", rootSel, onClick = { app.objectsMode = true; select(layer = jobj("obj" to i, "root" to true)) })
        for (r in 0 until ranges.length()) {
            val rg = ranges.getJSONArray(r)
            val range = JSONArray().put(rg.getDouble(0)).put(rg.getDouble(1))
            val rSel = li != null && li.optInt("obj") == i && li.optJSONArray("range")?.let { it.optDouble(0) == rg.getDouble(0) && it.optDouble(1) == rg.getDouble(1) } == true
            TreeRow(3, false, false, String.format(java.util.Locale.ROOT, "%.2f-%.2f mm", rg.getDouble(0), rg.getDouble(1)), "height_range_modifier", rSel,
                onClick = { app.objectsMode = true; select(layer = jobj("obj" to i, "range" to range)) }) {
                FilamentCell(app, rg.optInt(2, 0), allowDefault = true) { e -> app.projectCall("layers.edit", jobj("obj" to i, "range" to range, "extruder" to e)) }
                ColumnCell { }; ColumnCell { }; ColumnCell { }
                ColumnCell { Box(Modifier.size(28.dp).clickable { app.objectsMode = true; select(layer = jobj("obj" to i, "range" to range)) }, Alignment.Center) { OrcaIcon("settings", 18.dp) } }
            }
        }
    }
    if (insts.length() > 1) {
        TreeRow(2, true, true, "Instances", null, false, onClick = { select() })
        for (k in 0 until insts.length()) {
            val ins = insts.getJSONObject(k)
            val iSel = selected.any { it.optInt("obj") == i && it.optInt("inst") == k && it.optInt("vol") < 0 }
            TreeRow(3, false, false, "Instance ${k + 1}", null, iSel, onClick = { select(inst = k) }, onLongClick = { if (!iSel) select(inst = k); openMenu() }) {
                PrintableCell(ins.optBoolean("printable", true)) { select(inst = k); runMenu(app, "printable") }
            }
        }
    }
    for ((icon, label) in infos) TreeRow(2, false, false, label, icon, false, onClick = { select() })
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun TreeRow(level: Int, open: Boolean, expandable: Boolean, label: String, icon: String?, selected: Boolean,
                    onToggle: () -> Unit = {}, onClick: () -> Unit, onLongClick: () -> Unit = {}, cells: @Composable RowScope.() -> Unit = {}) {
    val c = desk()
    Row(Modifier.fillMaxWidth().height(40.dp).background(if (selected) c.selection else Color.Transparent)
        .combinedClickable(onClick = onClick, onLongClick = onLongClick).padding(start = (4 + 16 * level).dp, end = 8.dp),
        verticalAlignment = Alignment.CenterVertically) {
        Box(Modifier.size(24.dp).clickable(enabled = expandable, onClick = onToggle), Alignment.Center) {
            if (expandable) Text(if (open) "▾" else "▸", style = deskText(13, color = c.textDim))
        }
        if (icon != null) { OrcaIcon(icon, 18.dp); Spacer(Modifier.width(6.dp)) }
        Text(label, Modifier.weight(1f), style = deskText(14), maxLines = 1, overflow = TextOverflow.Ellipsis)
        cells()
    }
}

@Composable
private fun ColumnCell(width: Dp = 30.dp, content: @Composable BoxScope.() -> Unit) = Box(Modifier.width(width), Alignment.Center, content = content)

@Composable
private fun IconCell(icon: String?) = ColumnCell { if (icon != null) OrcaIcon(icon, 18.dp) }

@Composable
private fun PrintableCell(printable: Boolean, onClick: () -> Unit) = ColumnCell {
    Box(Modifier.size(28.dp).clickable(onClick = onClick), Alignment.Center) { OrcaIcon(if (printable) "check_on" else "check_off_focused", 18.dp, darkVariant = false) }
}

/** the "Fila." column: the filament number on its colour (BitmapChoiceRenderer), a popup to change it */
@Composable
private fun FilamentCell(app: AppState, extruder: Int, allowDefault: Boolean = false, onPick: (Int) -> Unit) {
    val fil = app.presets.optJSONArray("filaments")
    var open by remember { mutableStateOf(false) }
    ColumnCell(width = 44.dp) {
        val colour = if (extruder >= 1) parseColor(fil?.optJSONObject(extruder - 1)?.optString("color") ?: "") else null
        Box(Modifier.size(width = 30.dp, height = 22.dp).background(colour ?: desk().panel, RoundedCornerShape(3.dp))
            .border(1.dp, desk().border, RoundedCornerShape(3.dp)).clickable(enabled = (fil?.length() ?: 0) > 1 || allowDefault) { open = true }, Alignment.Center) {
            val lum = colour?.let { 0.299 * it.red + 0.587 * it.green + 0.114 * it.blue } ?: 1.0
            Text(if (extruder >= 1) "$extruder" else "-", style = deskText(12, color = if (lum > 0.6) Color.Black else Color.White))
        }
        DropdownMenu(open, { open = false }) {
            if (allowDefault) DropdownMenuItem(text = { Text(tr("Default"), style = deskText(14)) }, onClick = { open = false; onPick(0) })
            for (k in 0 until (fil?.length() ?: 0)) {
                val f = fil!!.getJSONObject(k)
                DropdownMenuItem(text = { Text("${k + 1}  ${f.optString("label")}", style = deskText(14)) }, onClick = { open = false; onPick(k + 1) },
                    leadingIcon = { Box(Modifier.size(16.dp).background(parseColor(f.optString("color")) ?: Color.White, RoundedCornerShape(2.dp))) })
            }
        }
    }
}
