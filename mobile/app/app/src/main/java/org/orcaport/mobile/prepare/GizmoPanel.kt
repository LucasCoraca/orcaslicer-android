package org.orcaport.mobile.prepare

import androidx.compose.foundation.isSystemInDarkTheme

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.material3.RadioButtonDefaults
import androidx.compose.material3.RadioButton
import androidx.compose.material3.HorizontalDivider
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import org.json.JSONObject
import org.orcaport.mobile.AppState
import org.orcaport.mobile.engine.jobj
import org.orcaport.mobile.ui.*
import java.util.Locale

// ColorRGBA::X / Y / Z (the axis captions are coloured like the grabbers)
private val AXIS = listOf(Color(0xFFFF3C5B), Color(0xFF64C818), Color(0xFF2F88E9))

/**
 * GizmoObjectManipulation's input windows (do_render_move_window / do_render_rotate_window / do_render_scale_window):
 * the coordinate system, one row of X / Y / Z inputs per value and the Done button. Values come from "gizmo.info".
 */
@Composable
fun GizmoPanel(app: AppState, gizmo: String, modifier: Modifier = Modifier) {
    if (gizmo == "Cut") { CutPanel(app, modifier); return }
    if (gizmo == "Texture displacement") { TextureDisplacementPanel(app, modifier); return }
    if (gizmo in PAINT_GIZMOS) { PaintPanel(app, gizmo, modifier); return }
    if (gizmo == "Simplify") { SimplifyPanel(app, modifier); return }
    if (gizmo == "Mesh boolean") { MeshBooleanPanel(app, modifier); return }
    if (gizmo == "Brim ears") { BrimEarsPanel(app, modifier); return }
    if (gizmo == "Text shape") { TextPanel(app, modifier); return }
    if (gizmo == "SVG") { SvgPanel(app, modifier); return }
    if (gizmo == "Measure" || gizmo == "Assemble") { MeasurePanel(app, modifier); return }
    if (gizmo == "Variable layer height") { LayersEditingPanel(app, modifier); return }
    if (gizmo !in listOf("Move", "Rotate", "Scale")) return
    var coords by remember(gizmo) { mutableStateOf("world") }
    var info by remember { mutableStateOf(JSONObject()) }
    var uniform by remember { mutableStateOf(true) }
    LaunchedEffect(app.project, coords, gizmo) {
        app.call("gizmo.info", jobj("coords" to coords)) { if (it is JSONObject) info = it }
    }
    if (info.optBoolean("empty", true)) return
    val single = info.optBoolean("single")
    val part = info.optBoolean("part")
    fun input(field: String, axis: Int, value: Double) =
        app.projectCall("gizmo.input", jobj("field" to field, "axis" to axis, "value" to value, "coords" to coords, "uniform" to uniform))
    val c = desk()
    Column(modifier.widthIn(max = 420.dp).shadow(4.dp, RoundedCornerShape(8.dp)).clip(RoundedCornerShape(8.dp)).background(c.panel)
        .padding(horizontal = 12.dp, vertical = 10.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        // header: coordinate system + axis captions
        Row(verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.width(96.dp)) {
                when (gizmo) {
                    "Rotate" -> Text(tr("World"), style = deskText(13))
                    else -> {
                        val modes = buildList { add("World"); if (single) add(if (part) "Part" else "Object") }
                        val keys = listOf("world", "object")
                        if (modes.size > 1) DeskChoice(modes, keys.indexOf(coords).coerceAtLeast(0), Modifier.fillMaxWidth()) { coords = keys[it] }
                        else Text(tr("World"), style = deskText(13))
                    }
                }
            }
            for (i in 0..2) Box(Modifier.weight(1f), Alignment.Center) { Text("XYZ"[i].toString(), style = deskText(13, color = AXIS[i])) }
            Spacer(Modifier.width(36.dp))
        }
        when (gizmo) {
            "Move" -> {
                val label = if (single && coords != "world") "Relative" else "Position"
                ValueRow(label, info.optJSONArray("position"), "mm") { a, v -> input("position", a, v) }
                if (!info.optBoolean("wipe_tower")) AlignSection(app)
                Row(verticalAlignment = Alignment.CenterVertically) {
                    PanelButton(tr("Drop")) { input("drop", 0, 0.0) }
                    Spacer(Modifier.weight(1f))
                    PanelButton(tr("Done"), primary = true) { app.gizmo = null }
                }
            }
            "Rotate" -> {
                ValueRow("Relative", info.optJSONArray("rotation"), "°") { a, v -> if (v != 0.0) input("rotation", a, v) }
                if (single) ValueRow("Absolute", info.optJSONArray("absolute_rotation"), "°",
                    trailing = { ResetIcon("Reset current rotation to real zeros.") { input("reset_rotation", 0, 0.0) } }) { a, v -> input("absolute_rotation", a, v) }
                Row { Spacer(Modifier.weight(1f)); PanelButton(tr("Done"), primary = true) { app.gizmo = null } }
            }
            "Scale" -> {
                ValueRow("Scale", info.optJSONArray("scale"), "%",
                    trailing = { if (single) ResetIcon("Reset scale") { input("reset_scale", 0, 0.0) } }) { a, v -> input("scale", a, v) }
                ValueRow("Size", info.optJSONArray("size"), "mm") { a, v -> input("size", a, v) }
                Row(verticalAlignment = Alignment.CenterVertically) {
                    DeskCheck(uniform) { uniform = it }
                    Text(tr("Uniform scale"), style = deskText(13))
                    Spacer(Modifier.weight(1f))
                    PanelButton(tr("Done"), primary = true) { app.gizmo = null }
                }
            }
        }
    }
}

@Composable
private fun ValueRow(label: String, values: org.json.JSONArray?, unit: String, trailing: (@Composable () -> Unit)? = null, onCommit: (Int, Double) -> Unit) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(label, Modifier.width(96.dp), style = deskText(13))
        for (i in 0..2) {
            val v = values?.optDouble(i, 0.0) ?: 0.0
            DeskInput(String.format(Locale.ROOT, "%.2f", v), true, true, Modifier.weight(1f).padding(horizontal = 2.dp)) { s ->
                s.replace(',', '.').toDoubleOrNull()?.let { if (Math.abs(it - v) > 1e-6) onCommit(i, it) }
            }
        }
        Box(Modifier.width(36.dp), Alignment.Center) { if (trailing != null) trailing() else Text(unit, style = deskText(12, color = desk().textDim)) }
    }
}

@Composable
private fun ResetIcon(tooltip: String, onClick: () -> Unit) {
    Box(Modifier.size(32.dp).clip(RoundedCornerShape(4.dp)).clickable(onClick = onClick), Alignment.Center) { OrcaIcon("undo", 18.dp) }
}

@Composable
fun PanelButton(text: String, primary: Boolean = false, enabled: Boolean = true, onClick: () -> Unit) {
    val c = desk()
    Box(Modifier.height(32.dp).clip(RoundedCornerShape(4.dp)).background(if (primary && enabled) OrcaColors.accent else c.panel)
        .border(1.dp, if (primary && enabled) OrcaColors.accent else c.border, RoundedCornerShape(4.dp)).clickable(enabled = enabled, onClick = onClick).padding(horizontal = 16.dp),
        Alignment.Center) {
        Text(text, style = deskText(13, color = if (!enabled) c.textDisabled else if (primary) Color.White else c.text))
    }
}

/** GLGizmoCut3D::render_cut_plane_input_window (planar mode) */
@Composable
private fun CutPanel(app: AppState, modifier: Modifier) {
    var info by remember { mutableStateOf(JSONObject()) }
    LaunchedEffect(app.project, app.gizmoRevision) { app.call("gizmo.cut_info") { if (it is JSONObject) info = it } }
    if (!info.optBoolean("valid")) return
    LaunchedEffect(info.optBoolean("editing")) { app.cutEditing = info.optBoolean("editing") }
    DisposableEffect(Unit) { onDispose { app.cutEditing = false } }
    if (info.optBoolean("editing")) { ConnectorsPanel(app, info, modifier) { info = it }; return }
    val groove = info.optJSONObject("groove") ?: JSONObject()
    val isGroove = groove.optInt("mode") == 1
    var keepUpper by remember { mutableStateOf(true) }; var keepLower by remember { mutableStateOf(true) }
    var placeUpper by remember { mutableStateOf(true) }; var placeLower by remember { mutableStateOf(false) }
    var flipUpper by remember { mutableStateOf(false) }; var flipLower by remember { mutableStateOf(false) }
    var asParts by remember { mutableStateOf(false) }
    val c = desk()
    fun set(a: JSONObject) = app.call("gizmo.cut_set", a) { if (it is JSONObject) { info = it; app.redraw() } }
    Column(modifier.widthIn(max = 380.dp).shadow(4.dp, RoundedCornerShape(8.dp)).clip(RoundedCornerShape(8.dp)).background(c.panel).verticalScroll(rememberScrollState())
        .padding(horizontal = 12.dp, vertical = 10.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(tr("Mode"), Modifier.width(110.dp), style = deskText(13))
            val hasConnectors = info.optInt("connectors") > 0
            DeskChoice(listOf("Planar", "Dovetail"), groove.optInt("mode"), Modifier.weight(1f), enabled = !hasConnectors) {
                app.call("gizmo.cut_groove", jobj("mode" to it)) { r -> if (r is JSONObject) { info = r; app.redraw() } }
            }
        }
        val size = info.optJSONArray("size")
        if (size != null) Text(tr("Size:") + " %.2f x %.2f x %.2f mm".format(size.getDouble(0), size.getDouble(1), size.getDouble(2)), style = deskText(12, color = c.textDim))
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(tr("Cut position"), Modifier.width(110.dp), style = deskText(13))
            DeskInput(String.format(java.util.Locale.ROOT, "%.2f", info.optDouble("z")), true, true, Modifier.weight(1f), sidetext = "mm") { s ->
                s.replace(',', '.').toDoubleOrNull()?.let { set(JSONObject().put("z", it)) }
            }
            Box(Modifier.size(32.dp).clip(RoundedCornerShape(4.dp)).clickable(enabled = !info.optBoolean("initial")) { set(JSONObject().put("reset", true)) },
                Alignment.Center) { OrcaIcon("undo", 18.dp, alpha = if (info.optBoolean("initial")) 0.35f else 1f) }
        }
        // Shift + drag on the desktop: here a switch, then a one-finger drag draws the line
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(info.optString("cut_line_label", "Cut by line"), Modifier.width(110.dp), style = deskText(13))
            DeskCheck(info.optBoolean("cut_line"), enabled = !info.optBoolean("editing")) { set(JSONObject().put("cut_line", it)) }
            Text(info.optString("cut_line_tip"), style = deskText(12, color = c.textDim))
        }
        if (isGroove) GrooveInputs(app, groove) { info = it }
        if (!isGroove) CutShortcuts(listOf(tr("Drag") to tr("Move cut line"), tr("Right mouse button") to tr("Part selection")))
        // connectors need both parts kept (render_cut_plane_input_window)
        val parts = info.optJSONObject("parts") ?: JSONObject()
        val canConnect = keepUpper && keepLower && !asParts && !(parts.optBoolean("valid") && parts.optBoolean("one_object"))
        if (!isGroove) PanelButton(if (info.optInt("connectors") > 0) "Edit connectors" else "Add connectors", enabled = canConnect) {
            // GLGizmoCut3D::set_connectors_editing: the camera looks at the cut plane
            app.call("gizmo.cut_connectors_edit", jobj("edit" to true)) {
                if (it is JSONObject) { info = it; org.orcaport.mobile.engine.Native.vpView("top"); org.orcaport.mobile.engine.Native.vpZoomTo("selection"); app.redraw() }
            }
        }
        HorizontalDivider(color = c.border)
        Text(tr("After cut") + ":", style = deskText(13))
        @Composable fun partRow(label: String, marker: Color, keep: Boolean, onKeep: (Boolean) -> Unit, place: Boolean, onPlace: (Boolean) -> Unit, flip: Boolean, onFlip: (Boolean) -> Unit) =
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(Modifier.size(12.dp).background(marker, RoundedCornerShape(2.dp))); Spacer(Modifier.width(6.dp))
                Text(label, Modifier.width(90.dp), style = deskText(13))
                DeskCheck(keep, enabled = !asParts, onChange = onKeep); Text(tr("Keep"), style = deskText(13))
                DeskCheck(place, enabled = keep && !asParts) { onPlace(it); if (it) onFlip(false) }; Text(tr("Place on cut"), style = deskText(13))
                DeskCheck(flip, enabled = keep && !asParts) { onFlip(it); if (it) onPlace(false) }; Text(tr("Flip"), style = deskText(13))
            }
        partRow("Upper part", Color.Cyan, keepUpper, { keepUpper = it }, placeUpper, { placeUpper = it }, flipUpper, { flipUpper = it })
        partRow("Lower part", Color.Magenta, keepLower, { keepLower = it }, placeLower, { placeLower = it }, flipLower, { flipLower = it })
        Row(verticalAlignment = Alignment.CenterVertically) {
            if (parts.optBoolean("valid") && asParts) asParts = false
            DeskCheck(asParts, enabled = !isGroove && info.optInt("connectors") == 0 && !parts.optBoolean("valid")) {
                asParts = it
                if (it) { keepUpper = true; keepLower = true; placeUpper = false; placeLower = false; flipUpper = false; flipLower = false }
            }
            Text(tr("Cut to parts"), style = deskText(13))
        }
        HorizontalDivider(color = c.border)
        Row(verticalAlignment = Alignment.CenterVertically) {
            PanelButton(tr("Reset")) { set(JSONObject().put("reset", true)) }
            Spacer(Modifier.weight(1f))
            PanelButton(tr("Perform cut"), primary = true) {
                app.call("gizmo.cut_perform", jobj("keep_upper" to keepUpper, "keep_lower" to keepLower, "place_upper" to placeUpper, "place_lower" to placeLower,
                    "flip_upper" to flipUpper, "flip_lower" to flipLower, "keep_as_parts" to asParts)) { r ->
                    if (r is JSONObject && r.has("objects")) { app.project = r; app.gizmo = null; app.redraw() }
                }
            }
        }
    }
}

/** GLGizmoCut3D::render_connectors_input_window */
@Composable
private fun ConnectorsPanel(app: AppState, info: JSONObject, modifier: Modifier, onInfo: (JSONObject) -> Unit) {
    val p = info.optJSONObject("params") ?: JSONObject()
    val c = desk()
    fun set(k: String, v: Any) = app.call("gizmo.cut_connector_params", jobj(k to v)) { if (it is JSONObject) { onInfo(it); app.redraw() } }
    fun num(v: Double) = String.format(java.util.Locale.ROOT, "%.2f", v)
    Column(modifier.widthIn(max = 380.dp).shadow(4.dp, RoundedCornerShape(8.dp)).clip(RoundedCornerShape(8.dp)).background(c.panel).verticalScroll(rememberScrollState())
        .padding(horizontal = 12.dp, vertical = 10.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(tr("Connectors"), Modifier.weight(1f), style = deskText(13, color = Color(0xFFF1754E)))
            val has = info.optInt("connectors") > 0
            Box(Modifier.size(32.dp).clip(RoundedCornerShape(4.dp)).clickable(enabled = has) {
                app.call("gizmo.cut_connectors_clear") { if (it is JSONObject) { onInfo(it); app.redraw() } }
            }, Alignment.Center) { OrcaIcon("undo", 18.dp, alpha = if (has) 1f else 0.35f) }
        }
        Text(tr("Type"), style = deskText(13))
        Row(verticalAlignment = Alignment.CenterVertically) {
            for ((i, l) in listOf("Plug", "Dowel", "Snap").withIndex()) {
                RadioButton(p.optInt("type") == i, { set("type", i) }, colors = RadioButtonDefaults.colors(selectedColor = OrcaColors.accent))
                Text(l, style = deskText(13)); Spacer(Modifier.width(8.dp))
            }
        }
        @Composable fun combo(label: String, items: List<String>, sel: Int, enabled: Boolean, key: String) = Row(verticalAlignment = Alignment.CenterVertically) {
            Text(label, Modifier.width(90.dp), style = deskText(13, color = if (enabled) c.text else c.textDisabled))
            DeskChoice(items, sel, Modifier.weight(1f), enabled = enabled) { set(key, it) }
        }
        combo("Style", listOf("Prism", "Frustum"), p.optInt("style"), p.optInt("type") == 0, "style")
        combo("Shape", listOf("Triangle", "Square", "Hexagon", "Circle"), p.optInt("shape"), p.optInt("type") != 2, "shape")
        @Composable fun two(label: String, k: String, tk: String?) = Row(verticalAlignment = Alignment.CenterVertically) {
            Text(label, Modifier.width(90.dp), style = deskText(13))
            DeskInput(num(p.optDouble(k)), true, true, Modifier.weight(1f), sidetext = if (k == "angle") "°" else "mm") { s -> s.replace(',', '.').toDoubleOrNull()?.let { set(k, it) } }
            if (tk != null) {
                Spacer(Modifier.width(6.dp)); Text("±", style = deskText(13)); Spacer(Modifier.width(4.dp))
                DeskInput(num(p.optDouble(tk)), true, true, Modifier.weight(0.7f), sidetext = "mm") { s -> s.replace(',', '.').toDoubleOrNull()?.let { set(tk, it) } }
            }
        }
        two("Depth", "depth", "depth_tol")
        two("Size", "size", "size_tol")
        two("Rotation", "angle", null)
        if (p.optInt("type") == 2) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(tr("Bulge"), Modifier.width(90.dp), style = deskText(13))
                DeskInput(num(p.optDouble("bulge") * 100), true, true, Modifier.weight(1f), sidetext = "%") { s -> s.toDoubleOrNull()?.let { set("bulge", it / 100) } }
            }
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(tr("Space"), Modifier.width(90.dp), style = deskText(13))
                DeskInput(num(p.optDouble("space") * 100), true, true, Modifier.weight(1f), sidetext = "%") { s -> s.toDoubleOrNull()?.let { set("space", it / 100) } }
            }
        }
        CutShortcuts(listOf(tr("Left mouse button") to tr("Add connector"), tr("Right mouse button") to tr("Remove connector"),
            tr("Drag") to tr("Move connector")))
        HorizontalDivider(color = c.border)
        Row(verticalAlignment = Alignment.CenterVertically) {
            Spacer(Modifier.weight(1f))
            PanelButton(tr("Confirm connectors"), primary = true) {
                app.call("gizmo.cut_connectors_edit", jobj("edit" to false)) { if (it is JSONObject) { onInfo(it); app.redraw() } }
            }
            Spacer(Modifier.width(8.dp))
            PanelButton(tr("Cancel")) {
                app.call("gizmo.cut_connectors_edit", jobj("edit" to false, "cancel" to true)) { if (it is JSONObject) { onInfo(it); app.redraw() } }
            }
        }
    }
}

/** the dovetail inputs (render_groove_two_float_input, render_groove_angle_input, render_groove_int_input) */
@Composable
private fun GrooveInputs(app: AppState, g: JSONObject, onInfo: (JSONObject) -> Unit) {
    val c = desk()
    fun set(k: String, v: Any) = app.call("gizmo.cut_groove", jobj(k to v)) { if (it is JSONObject) { onInfo(it); app.redraw() } }
    fun num(v: Double) = String.format(java.util.Locale.ROOT, "%.2f", v)
    HorizontalDivider(color = c.border)
    Text(tr("Groove") + ":", style = deskText(13, color = Color(0xFFF1754E)))
    @Composable fun row(label: String, k: String, unit: String, tk: String? = null) = Row(verticalAlignment = Alignment.CenterVertically) {
        Text(label, Modifier.width(100.dp), style = deskText(13))
        DeskInput(num(g.optDouble(k)), true, true, Modifier.weight(1f), sidetext = unit) { s -> s.replace(',', '.').toDoubleOrNull()?.let { set(k, it) } }
        if (tk != null) {
            Spacer(Modifier.width(6.dp)); Text("±", style = deskText(13)); Spacer(Modifier.width(4.dp))
            DeskInput(num(g.optDouble(tk)), true, true, Modifier.weight(0.7f), sidetext = unit) { s -> s.replace(',', '.').toDoubleOrNull()?.let { set(tk, it) } }
        }
    }
    row("Depth", "depth", "mm", "depth_tol")
    row("Width", "width", "mm", "width_tol")
    row("Flap Angle", "flaps_angle", "°")
    row("Groove Angle", "angle", "°")
    Text(tr("Multiple") + ":", style = deskText(13, color = Color(0xFFF1754E)))
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(tr("Count"), Modifier.width(100.dp), style = deskText(13))
        DeskInput(g.optInt("count").toString(), true, true, Modifier.weight(1f)) { s -> s.toIntOrNull()?.let { set("count", it) } }
    }
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(tr("Gap"), Modifier.width(100.dp), style = deskText(13, color = if (g.optInt("count") > 1) c.text else c.textDisabled))
        DeskInput(num(g.optDouble("gap")), g.optInt("count") > 1, true, Modifier.weight(1f), sidetext = "mm") { s -> s.replace(',', '.').toDoubleOrNull()?.let { set("gap", it) } }
    }
    Row { Text(tr("Spacing"), Modifier.width(100.dp), style = deskText(13, color = c.textDisabled)); Text(num(g.optDouble("spacing")) + "mm", style = deskText(13, color = c.textDisabled)) }
    if (!g.optBoolean("valid", true)) Text(tr("Cut plane with groove is invalid"), style = deskText(13, color = OrcaColors.error))
}

/**
 * GizmoObjectManipulation's align section (GLGizmoAlignment): the reference (Selection / Object or Plate), three align
 * buttons under each axis and the Distribute row; a disabled button says why when pressed (the desktop's tooltip).
 */
@Composable
private fun AlignSection(app: AppState) {
    val c = desk()
    var parent by remember { mutableStateOf(false) }
    var st by remember { mutableStateOf(JSONObject()) }
    LaunchedEffect(app.project, parent) { app.call("align.state", jobj("parent" to parent)) { if (it is JSONObject) st = it } }
    val modes = st.optJSONArray("modes") ?: return
    val toParent = st.optBoolean("parent")
    val buttons = st.optJSONObject("buttons") ?: JSONObject()
    val dark = isSystemInDarkTheme()
    @Composable fun AlignButton(type: Int, icon: String, tip: String) {
        val b = buttons.optJSONObject(type.toString()) ?: JSONObject()
        val enabled = b.optBoolean("enabled")
        Box(Modifier.size(32.dp).clip(RoundedCornerShape(4.dp)).clickable {
            if (enabled) app.projectCall("align.run", jobj("type" to type, "parent" to toParent))
            else app.toast = tr(tip) + b.optString("why").takeIf { it.isNotEmpty() }?.let { "\n$it" }.orEmpty()
        }, Alignment.Center) { OrcaIcon(if (dark) "${icon}_dark" else icon, 18.dp, darkVariant = false, alpha = if (enabled) 1f else 0.3f) }
    }
    HorizontalDivider(color = c.border)
    Row(verticalAlignment = Alignment.CenterVertically) {
        Box(Modifier.width(96.dp)) {
            val list = (0 until modes.length()).map { modes.getString(it) }
            if (list.size > 1) DeskChoice(list, if (toParent) list.size - 1 else 0, Modifier.fillMaxWidth()) { parent = it == list.size - 1 }
            else Text(list.firstOrNull() ?: "", style = deskText(13))
        }
        // AlignType: CENTER_X 0, CENTER_Y 1, CENTER_Z 2, Y_MAX 3, Y_MIN 4, X_MAX 5, X_MIN 6, Z_MAX 7, Z_MIN 8
        Row(Modifier.weight(1f), horizontalArrangement = Arrangement.Center) {
            AlignButton(6, "align_x_min", "Align left"); AlignButton(0, "align_x_center", "Align left-right center"); AlignButton(5, "align_x_max", "Align right")
        }
        Row(Modifier.weight(1f), horizontalArrangement = Arrangement.Center) {
            AlignButton(4, "align_y_min", "Align front"); AlignButton(1, "align_y_center", "Align front-back center"); AlignButton(3, "align_y_max", "Align back")
        }
        Row(Modifier.weight(1f), horizontalArrangement = Arrangement.Center) {
            AlignButton(8, "align_z_min", "Align bottom"); AlignButton(2, "align_z_center", "Align top-bottom center"); AlignButton(7, "align_z_max", "Align top")
        }
        Spacer(Modifier.width(36.dp))
    }
    if (!toParent) Row(verticalAlignment = Alignment.CenterVertically) {
        Text(st.optString("distribute"), Modifier.width(96.dp), style = deskText(13))
        Box(Modifier.weight(1f), Alignment.Center) { AlignButton(9, "distribute_x", "Distribute left-right") }
        Box(Modifier.weight(1f), Alignment.Center) { AlignButton(10, "distribute_y", "Distribute front-back") }
        Box(Modifier.weight(1f), Alignment.Center) { AlignButton(11, "distribute_z", "Distribute top-bottom") }
        Spacer(Modifier.width(36.dp))
    }
    HorizontalDivider(color = c.border)
}


/**
 * GLGizmoUtils::render_tooltip_button: the "?" that shows the gizmo's shortcuts (m_shortcuts_cut / m_shortcuts_connector).
 * On touch the left button is a tap and the right one a long press.
 */
@Composable
private fun CutShortcuts(rows: List<Pair<String, String>>) {
    val c = desk()
    var open by remember { mutableStateOf(false) }
    Row(verticalAlignment = Alignment.CenterVertically) {
        Box(Modifier.size(28.dp).clip(RoundedCornerShape(14.dp)).border(1.dp, c.border, RoundedCornerShape(14.dp)).clickable { open = !open },
            Alignment.Center) { Text("?", style = deskText(13, color = c.textDim)) }
    }
    if (open) Column(Modifier.padding(start = 4.dp), verticalArrangement = Arrangement.spacedBy(2.dp)) {
        for ((k, v) in rows) Row {
            Text(k, Modifier.width(150.dp), style = deskText(12, FontWeight.Bold, color = c.textDim))
            Text(v, style = deskText(12, color = c.textDim))
        }
    }
}
