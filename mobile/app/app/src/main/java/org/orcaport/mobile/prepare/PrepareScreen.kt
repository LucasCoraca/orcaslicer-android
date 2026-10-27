package org.orcaport.mobile.prepare

import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.ui.layout.positionInWindow

import androidx.compose.ui.layout.onSizeChanged

import android.app.Activity
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.DpOffset
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.compose.ui.window.Popup
import kotlinx.coroutines.launch
import org.json.JSONArray
import org.json.JSONObject
import org.orcaport.mobile.AppState
import org.orcaport.mobile.engine.Native
import org.orcaport.mobile.engine.jobj
import org.orcaport.mobile.ui.*

/** Plater: the sidebar and the 3D canvas (Prepare) or the G-code preview (Preview), as on the desktop. */
@Composable
fun PrepareScreen(app: AppState, activity: Activity, preview: Boolean) {
    val compact = isCompact()
    // the "<>" button / Shift+Tab (MainFrame collapse); the compact layout starts on the 3D view
    val collapsed = app.sidebarCollapsed ?: compact
    Row(Modifier.fillMaxSize()) {
        if (!preview && !collapsed)
            Sidebar(app, Modifier.then(if (compact) Modifier.fillMaxSize() else Modifier.width(392.dp)).fillMaxHeight())
        if (!(compact && !collapsed && !preview)) {
            // the plugins' docked panels share the 3D view's dock area (Plater::add_dock_pane)
            org.orcaport.mobile.ui.PluginDocks(app, "left")
            Column(Modifier.weight(1f).fillMaxHeight()) {
                Canvas(app, Modifier.weight(1f).fillMaxWidth(), preview, collapsed) { app.sidebarCollapsed = !collapsed }
                org.orcaport.mobile.ui.PluginDocks(app, "bottom")
            }
            org.orcaport.mobile.ui.PluginDocks(app, "right")
        }
    }
    if (compact && !collapsed && !preview)   // the compact layout's "3D view" button (Sidebar::show_compact_header)
        Box(Modifier.fillMaxSize()) {
            SmallFloatingActionButton(onClick = { app.sidebarCollapsed = true }, Modifier.align(Alignment.BottomEnd).padding(16.dp), containerColor = OrcaColors.accent) {
                // the canvas's collapse chevrons (its "^" opens this panel), not the Prepare tab's icon
                OrcaIcon("collapse_btn", 26.dp, darkVariant = false)
            }
        }
}

@Composable
private fun Canvas(app: AppState, modifier: Modifier, preview: Boolean, collapsed: Boolean, onCollapse: () -> Unit) {
    var menu by remember { mutableStateOf<JSONObject?>(null) }
    var plateSettings by remember { mutableStateOf<Int?>(null) }
    var ghost by remember { mutableStateOf<JSONObject?>(null) }   // the tapped IDEX/IQEX ghost's tooltip / picker
    val plate = app.project.optInt("current_plate")
    LaunchedEffect(preview, plate) { Native.vpPreview(preview, plate); app.redraw() }
    // GLGizmosManager: the active gizmo is drawn natively; it closes when nothing it can work on is selected
    val hasSel = (app.project.optJSONArray("selection")?.length() ?: 0) > 0
    LaunchedEffect(hasSel) { if (!hasSel && app.gizmo != null && app.gizmo != "Measure") app.gizmo = null }
    LaunchedEffect(app.gizmo, preview, app.gizmoRequest) {
        val args = (app.gizmoArgs ?: JSONObject()).put("name", if (preview) "" else (app.gizmo ?: ""))
        app.gizmoArgs = null
        app.call("gizmo.set", args) { r ->
            if (r is JSONObject && r.has("objects")) { app.project = r; app.gizmoRevision++ }
            if (r is JSONObject) {
                r.optString("error").takeIf { it.isNotEmpty() }?.let { app.toast = it }
                if (r.has("svg_active") && !r.optBoolean("svg_active")) app.gizmo = null
            }
            app.redraw()
        }
    }
    // the GL view and its listener outlive a switch between Prepare and Preview: they read the current tab
    val previewNow by androidx.compose.runtime.rememberUpdatedState(preview)
    Box(modifier.background(desk().panel)) {
        ghost?.let { g -> ImexGhostPopup(app, g) { ghost = null } }
        AndroidView(factory = { c ->
            GLView(c, object : GLView.Listener {
                override fun onPick(hit: JSONObject, add: Boolean) {
                    if (previewNow) return
                    // a painting gizmo: a tap on the object paints (a dab of the brush, or a fill)
                    if (app.gizmo in PAINT_GIZMOS) {
                        if (Native.vpPaintTap(hit.optDouble("x"), hit.optDouble("y"))) { app.redraw(); return }
                    }
                    // GLGizmoMeasure::on_mouse: a tap picks the feature under the finger
                    if (app.gizmo == "Measure" || app.gizmo == "Assemble") {
                        app.call("measure.tap", jobj("x" to hit.optDouble("x"), "y" to hit.optDouble("y"))) { app.gizmoRevision++; app.redraw() }
                        return
                    }
                    // GLGizmoBrimEars::gizmo_event: a tap adds an ear on the object or selects one
                    if (app.gizmo == "Brim ears") {
                        app.call("brim_ears.tap", jobj("x" to hit.optDouble("x"), "y" to hit.optDouble("y"))) { app.gizmoRevision++; app.redraw() }
                        return
                    }
                    // GLGizmoMeshBoolean::gizmo_event: a tap picks the source / tool part
                    if (app.gizmo == "Mesh boolean") {
                        app.call("boolean.tap", jobj("x" to hit.optDouble("x"), "y" to hit.optDouble("y"))) { app.gizmoRevision++; app.redraw() }
                        return
                    }
                    // GLGizmoCut3D connectors editing: a tap adds a connector on the cut section or selects one
                    if (app.gizmo == "Cut" && app.cutEditing) {
                        app.call("gizmo.cut_connector_tap", jobj("x" to hit.optDouble("x"), "y" to hit.optDouble("y"))) { app.gizmoRevision++; app.redraw() }
                        return
                    }
                    // Lay on face: a tap on a highlighted face (GLGizmoFlatten::on_mouse)
                    hit.optJSONObject("flatten")?.let { f ->
                        app.projectCall("object.lay_on_face", jobj("obj" to f.getInt("obj"), "inst" to f.getInt("inst"), "normal" to f.getJSONArray("normal")))
                        return
                    }
                    // an IDEX/IQEX ghost: its head and filament (the hover tooltip), the lanes of a multi-lane head
                    if (hit.has("imex_ghost")) {
                        val x = hit.optDouble("x"); val y = hit.optDouble("y")
                        app.call("imex.ghost", jobj("head" to hit.getInt("imex_ghost"))) { r -> if (r is JSONObject) ghost = r.put("x", x).put("y", y) }
                        return
                    }
                    val icon = hit.optString("plate_icon")
                    val p = hit.optInt("plate", -1)
                    if (icon.isNotEmpty() && p >= 0) { plateIcon(app, p, icon) { plateSettings = it }; return }
                    val items = JSONArray()
                    // the Assembly view in Part mode selects the part (Selection::Volume)
                    if (hit.optBoolean("hit") && app.assembleView && app.assemblePartMode)
                        items.put(jobj("obj" to hit.getInt("obj"), "inst" to hit.optInt("inst", 0), "vol" to hit.getInt("vol")))
                    else if (hit.optBoolean("hit")) items.put(jobj("obj" to hit.getInt("obj"), "inst" to -1, "vol" to -1))
                    else if (p >= 0 && p != app.project.optInt("current_plate")) app.projectCall("plate.select", jobj("plate" to p))
                    app.projectCall("selection.set", jobj("items" to items, "add" to add))
                }
                override fun onDoubleTap(hit: JSONObject) {
                    Native.vpZoomTo(if (hit.optBoolean("hit")) "selection" else "plate"); app.redraw()
                }
                override fun onLongPress(hit: JSONObject, x: Float, y: Float) {
                    if (previewNow) return
                    // the IDEX/IQEX mode icon: the menu of the modes (right click)
                    if (hit.optString("plate_icon") == "imex_mode") {
                        val plate = hit.optInt("plate")
                        app.call("plate.imex_mode", jobj("plate" to plate, "menu" to true)) { r ->
                            (r as? JSONObject)?.let { app.menuDialog = JSONObject().put("ui", "imex_mode").put("plate", plate).put("items", it.optJSONArray("items")).put("warning", it.optString("warning")) }
                        }
                        return
                    }
                    if (app.gizmo == "Brim ears") {   // right click on an ear: remove it
                        app.call("brim_ears.tap", jobj("x" to x.toDouble(), "y" to y.toDouble(), "remove" to true)) { app.gizmoRevision++; app.redraw() }
                        return
                    }
                    if (app.gizmo == "Cut" && !app.cutEditing) {   // right click on a part: it changes sides (PartSelection)
                        app.call("gizmo.cut_part_toggle", jobj("x" to x.toDouble(), "y" to y.toDouble())) { app.gizmoRevision++; app.redraw() }
                        return
                    }
                    if (app.gizmo == "Cut" && app.cutEditing) {   // right click on a connector: delete it
                        app.call("gizmo.cut_connector_tap", jobj("x" to x.toDouble(), "y" to y.toDouble(), "delete" to true)) { app.gizmoRevision++; app.redraw() }
                        return
                    }
                    if (hit.optBoolean("hit")) {
                        val sel = app.project.optJSONArray("selection")
                        val already = sel != null && (0 until sel.length()).any { sel.getJSONObject(it).optInt("obj") == hit.getInt("obj") }
                        if (!already) app.projectCall("selection.set", jobj("items" to JSONArray().put(jobj("obj" to hit.getInt("obj"), "inst" to -1, "vol" to -1))))
                    } else app.projectCall("selection.set", jobj("items" to JSONArray()))
                    // GLCanvas3D right click: the menu for the (new) selection, at the finger
                    app.scope.launch { kotlinx.coroutines.delay(150); menu = JSONObject(hit.toString()).put("x", x).put("y", y) }
                }
                override fun onMoved(result: String) {
                    if (result.trim().startsWith("[")) app.projectCall("selection.translate", jobj("delta" to JSONArray(result)))
                    else JSONObject(result).let { g ->
                        if (g.has("connector")) app.call("gizmo.cut_connector_move", jobj("index" to g.getInt("connector"), "pos" to g.getJSONArray("pos"))) { app.gizmoRevision++; app.redraw() }
                        else if (g.optString("gizmo") in listOf("cut", "brim_ears", "layers", "text", "svg")) app.gizmoRevision++
                        else if (g.has("wipe_tower")) app.projectCall("wipe_tower.move", jobj("plate" to g.getInt("wipe_tower"), "delta" to g.getJSONArray("delta")))
                        else app.projectCall("selection.transform_world", jobj("matrix" to g.getJSONArray("matrix"), "name" to g.optString("name")))
                    }
                }
                override fun onFrame() { runCatching { app.overlay = JSONObject(Native.vpOverlay()) } }
            }).also { v -> app.redraw = { v.requestRender() }; v.post { Native.vpZoomTo("plate"); v.requestRender() } }
        }, modifier = Modifier.fillMaxSize().onGloballyPositioned { app.glOrigin = it.positionInWindow(); app.glSize = it.size })

        PlateLabels(app)
        if (!preview) ObjectLabels(app)
        if (!preview && (app.gizmo == "Measure" || app.gizmo == "Assemble")) MeasureLabels(app)
        // the "<>" button that hides the sidebar (MainFrame collapse)
        if (!preview) Box(Modifier.padding(6.dp).size(32.dp).clip(RoundedCornerShape(4.dp)).clickable(onClick = onCollapse), Alignment.Center) {
            OrcaIcon(if (collapsed) "expand_btn" else "collapse_btn", 20.dp)
        }
        if (!preview && app.assembleView) AssembleToolbar(app, Modifier.fillMaxWidth().padding(top = 4.dp))
        else if (!preview) MainToolbar(app, Modifier.align(Alignment.TopCenter).padding(start = 44.dp, top = 4.dp))
        else PreviewOverlay(app)
        ViewControls(app, preview, Modifier.align(Alignment.BottomStart).padding(12.dp))
        if (!preview && !app.assembleView) ObjectInfo(app, Modifier.align(Alignment.BottomEnd).padding(start = 12.dp, end = 12.dp, top = 12.dp,
            bottom = if (isCompact()) 196.dp else 12.dp))   // a phone: above the navigator and the canvas toolbar   // the Assembly view shows its Assembly Info
        SliceNotification(app, Modifier.align(Alignment.BottomEnd).padding(end = 12.dp, bottom = if (preview) 60.dp else 120.dp))
        app.progress?.let { p ->
            Surface(Modifier.align(Alignment.Center), shape = RoundedCornerShape(6.dp), color = desk().panel, shadowElevation = 6.dp) {
                Row(Modifier.padding(16.dp), verticalAlignment = Alignment.CenterVertically) {
                    CircularProgressIndicator(Modifier.size(22.dp), color = OrcaColors.accent); Spacer(Modifier.width(12.dp)); Text(p, style = deskText(14))
                    if (app.progressPercent >= 0) Text("  ${app.progressPercent}%", style = deskText(14, color = desk().textDim))
                    app.progressCancel?.let { cancel ->
                        Spacer(Modifier.width(16.dp))
                        PanelButton(tr("Cancel")) { app.scope.launch { runCatching { app.engine.pure(cancel) } } }
                    }
                }
            }
        }
        if (!preview) app.gizmo?.let { GizmoPanel(app, it, Modifier.align(Alignment.TopStart).padding(top = 60.dp, start = 8.dp, bottom = 8.dp)) }
        menu?.let { hit ->
            val d = LocalDensity.current
            val off = with(d) { DpOffset(hit.optDouble("x").toFloat().toDp(), (hit.optDouble("y").toFloat() - 40f).toDp()) }
            Box(Modifier.align(Alignment.TopStart)) { OrcaContextMenu(app, off) { menu = null } }
        }
        plateSettings?.let { p -> PlateSettingsDialog(app, p) { plateSettings = null } }
    }
}

val PAINT_GIZMOS = setOf("Support painting", "Seam painting", "Color painting", "Fuzzy skin painting", "Texture displacement")

/** PartPlate icon clicks (Plater::priv::on_plate_selected / plate toolbar) */
private fun plateIcon(app: AppState, plate: Int, icon: String, openSettings: (Int) -> Unit) {
    if (plate != app.project.optInt("current_plate")) app.projectCall("plate.select", jobj("plate" to plate))
    when (icon) {
        "close" -> app.projectCall("plate.delete", jobj("plate" to plate))
        "orient" -> app.projectCall("orient", jobj("plate" to plate))
        "arrange" -> app.projectCall("arrange", jobj("plate" to plate))
        "lock" -> {
            val locked = app.project.optJSONArray("plates")?.optJSONObject(plate)?.optBoolean("locked") ?: false
            app.projectCall("plate.set", jobj("plate" to plate, "locked" to !locked))
        }
        "settings" -> openSettings(plate)
        "move_front" -> app.projectCall("plate.move_front", jobj("plate" to plate))
        "imex_mode" -> app.projectCall("plate.imex_mode", jobj("plate" to plate))   // a tap cycles the IDEX/IQEX mode
    }
}

/** plate name above the top-left corner (orange for the current plate) and the plate number "01" at the bottom-right */
@Composable
private fun PlateLabels(app: AppState) {
    val plates = app.overlay.optJSONArray("plates") ?: return
    val cur = app.overlay.optInt("current")
    val d = LocalDensity.current
    for (i in 0 until plates.length()) {
        val p = plates.getJSONObject(i)
        val px = p.optDouble("icon_px", 0.0).toFloat()
        if (px < 6f) continue
        val fs = with(d) { (px * 0.7f).toSp() }
        val col = if (i == cur) Color(0xFFF1754E) else Color(0xFF9DA6A8)
        p.optJSONArray("number")?.let { a ->
            Text("%02d".format(i + 1), Modifier.offset { IntOffset(a.getDouble(0).toInt(), (a.getDouble(1) - px).toInt()) },
                color = col, fontSize = fs, fontWeight = FontWeight.Bold, fontFamily = OrcaAssets.font)
        }
        p.optJSONArray("name")?.let { a ->
            Text(p.optString("name_text"), Modifier.offset { IntOffset(a.getDouble(0).toInt(), (a.getDouble(1) - px).toInt()) },
                color = col, fontSize = fs * 0.8f, fontFamily = OrcaAssets.font)
        }
    }
}

/** GLCanvas3D's main toolbar and the gizmo toolbar (GLToolbar / GLGizmosManager), with Orca's icons */
@Composable
private fun MainToolbar(app: AppState, modifier: Modifier) {
    val p = app.project
    val hasSel = (p.optJSONArray("selection")?.length() ?: 0) > 0
    val c = desk()
    // GLCanvas3D::_check_and_update_toolbar_icon_scale: the icons shrink so the whole toolbar fits the canvas width
    BoxWithConstraints(modifier) {
    val icons = 24
    val fit = (maxWidth - 8.dp - 8.dp * 3) / icons
    toolSize = fit.coerceIn(28.dp, 44.dp)
    Row(Modifier.clip(RoundedCornerShape(6.dp)).background(c.panel.copy(alpha = 0.95f)).horizontalScroll(rememberScrollState()).padding(horizontal = 4.dp, vertical = 2.dp),
        verticalAlignment = Alignment.CenterVertically) {
        ToolIcon("toolbar_open", "Add") { app.openImporter() }
        ToolIcon("toolbar_add_plate", "Add plate") { app.projectCall("plate.add") }
        ToolIcon("toolbar_orient", "Auto orient") { app.projectCall("orient") }
        ToolIcon("toolbar_arrange", "Arrange all objects") { app.projectCall("arrange") }
        Separator()
        ToolIcon("instance_add", "Add instance", hasSel) { app.projectCall("objects.instances", jobj("delta" to 1)) }
        ToolIcon("instance_remove", "Remove instance", hasSel) { app.projectCall("objects.instances", jobj("delta" to -1)) }
        ToolIcon("split_objects", "Split to objects", hasSel) {
            p.optJSONArray("selection")?.optJSONObject(0)?.let { app.projectCall("object.split", jobj("obj" to it.getInt("obj"))) }
        }
        ToolIcon("split_parts", "Split to parts", hasSel) {
            p.optJSONArray("selection")?.optJSONObject(0)?.let { app.projectCall("object.split", jobj("obj" to it.getInt("obj"), "to_parts" to true)) }
        }
        ToolIcon("toolbar_variable_layer_height", "Variable layer height", hasSel, selected = app.gizmo == "Variable layer height") {
            app.gizmo = if (app.gizmo == "Variable layer height") null else "Variable layer height"
        }
        Separator()
        for ((icon, name) in listOf("toolbar_move" to "Move", "toolbar_rotate" to "Rotate", "toolbar_scale" to "Scale", "toolbar_flatten" to "Lay on face",
            "toolbar_cut" to "Cut", "toolbar_meshboolean" to "Mesh boolean", "toolbar_support" to "Support painting", "toolbar_seam" to "Seam painting",
            "toolbar_fuzzy_skin_paint" to "Fuzzy skin painting", "mmu_segmentation" to "Color painting",
            "toolbar_texture_displacement" to "Texture displacement", "toolbar_text" to "Text shape",
            "toolbar_measure" to "Measure", "toolbar_assembly" to "Assemble", "toolbar_brimears" to "Brim ears"))
            ToolIcon(icon, name, hasSel || icon == "toolbar_measure" || icon == "toolbar_text", selected = app.gizmo == name) { app.gizmo = if (app.gizmo == name) null else name }
        Separator()
        // GLCanvas3D's assemble view toolbar: enabled by Plater::has_assmeble_view
        var canAssemble by remember { mutableStateOf(false) }
        LaunchedEffect(app.project) { app.call("assemble.available") { canAssemble = it == true } }
        ToolIcon("toolbar_assemble", "Assembly View", canAssemble) { app.setAssembleView(true) }
    }
    }
}

private var toolSize = 44.dp

/**
 * The Assembly view's toolbars: the Return button (GLCanvas3D::_render_return_toolbar) at the left and the gizmos it
 * offers in the middle (GLGizmosManager::get_selectable_idxs: Move, Rotate, Measure, Assembly, Color painting).
 */
@Composable
private fun AssembleToolbar(app: AppState, modifier: Modifier) {
    val hasSel = (app.project.optJSONArray("selection")?.length() ?: 0) > 0
    val c = desk()
    Box(modifier) {
        Row(Modifier.align(Alignment.TopStart).padding(start = 44.dp).height(36.dp).clip(RoundedCornerShape(18.dp))
            .background(c.panel.copy(alpha = 0.95f)).clickable { app.setAssembleView(false) }.padding(horizontal = 14.dp),
            verticalAlignment = Alignment.CenterVertically) {
            OrcaIcon("assemble_return", 9.dp, height = 18.dp, darkVariant = false, tint = c.text); Spacer(Modifier.width(6.dp)); Text(tr("Return"), style = deskText(14))
        }
        Row(Modifier.align(Alignment.TopCenter).clip(RoundedCornerShape(6.dp)).background(c.panel.copy(alpha = 0.95f)).padding(horizontal = 4.dp, vertical = 2.dp),
            verticalAlignment = Alignment.CenterVertically) {
            for ((icon, name) in listOf("toolbar_move" to "Move", "toolbar_rotate" to "Rotate", "toolbar_measure" to "Measure",
                "toolbar_assembly" to "Assemble", "mmu_segmentation" to "Color painting"))
                ToolIcon(icon, name, hasSel || icon == "toolbar_measure", selected = app.gizmo == name) { app.gizmo = if (app.gizmo == name) null else name }
        }
    }
}

@Composable
private fun ToolIcon(icon: String, tooltip: String, enabled: Boolean = true, selected: Boolean = false, onClick: () -> Unit) {
    Box(Modifier.size(toolSize).clip(RoundedCornerShape(4.dp)).background(if (selected) desk().selection else Color.Transparent)
        .clickable(enabled = enabled, onClick = onClick), Alignment.Center) {
        OrcaIcon(icon, toolSize * 34f / 44f, alpha = if (enabled) 1f else 0.35f)
    }
}

@Composable
private fun Separator() = OrcaIcon("seperator", 8.dp, height = toolSize * 36f / 44f)

/** the 3D navigator and the canvas toolbar at the bottom left (GLCanvas3D::_render_canvas_toolbar: menu, zoom) */
@Composable
private fun ViewControls(app: AppState, preview: Boolean, modifier: Modifier) {
    var menu by remember { mutableStateOf(false) }
    val o = app.viewOptions
    Row(modifier, verticalAlignment = Alignment.Bottom) {
        // the navigator cube is drawn by the engine in this corner (NAVIGATOR_SIZE); the toolbar sits right of it
        if (o.optBoolean("show_3d_navigator", true)) Spacer(Modifier.size(128.dp))
        Spacer(Modifier.width(10.dp))
        Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
            SectionViewButton(app)
            Box {
                RoundButton("canvas_menu") { menu = true }
                CanvasMenu(app, preview, menu) { menu = false }
            }
            // "Fit camera to scene or selected object."
            if (o.optBoolean("show_canvas_zoom_button", true))
                RoundButton("canvas_zoom") {
                    val sel = (app.project.optJSONArray("selection")?.length() ?: 0) > 0
                    Native.vpZoomTo(if (sel && !preview) "selection" else "plate"); app.redraw()
                }
        }
    }
}

/**
 * GLCanvas3D::_render_imex_ghost_tooltip at the tap (the swatch and "T<head> -> filament <n>") and, on a head with
 * several lanes, IMEXFilamentPickerPopover's choice of the lane it prints with.
 */
@Composable
private fun ImexGhostPopup(app: AppState, g: JSONObject, onDismiss: () -> Unit) {
    val c = desk()
    val px = with(LocalDensity.current) { 16.dp.roundToPx() }   // ~16px off the finger, as off the cursor
    Popup(offset = IntOffset(g.optDouble("x").toInt() + px, g.optDouble("y").toInt() + px), onDismissRequest = onDismiss) {
        Column(Modifier.shadow(4.dp, RoundedCornerShape(6.dp)).clip(RoundedCornerShape(6.dp)).background(c.panel).padding(10.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                g.optString("swatch").takeIf { it.isNotEmpty() }?.let {
                    Box(Modifier.size(18.dp).background(runCatching { Color(android.graphics.Color.parseColor(it)) }.getOrDefault(Color.Gray)))
                    Spacer(Modifier.width(8.dp))
                }
                Text(g.optString("label"), style = deskText(14))
            }
            val lanes = g.optJSONArray("lanes") ?: return@Column
            Spacer(Modifier.height(6.dp))
            for (i in 0 until lanes.length()) {
                val l = lanes.getJSONObject(i)
                Row(Modifier.clickable {
                    onDismiss()
                    app.projectCall("imex.ghost_set", jobj("head" to g.getInt("head"), "slot" to l.getInt("slot")))
                }.padding(vertical = 6.dp, horizontal = 4.dp), verticalAlignment = Alignment.CenterVertically) {
                    RadioButton(l.optBoolean("checked"), null, Modifier.size(18.dp))
                    Spacer(Modifier.width(8.dp))
                    Box(Modifier.size(16.dp).background(runCatching { Color(android.graphics.Color.parseColor(l.optString("color"))) }.getOrDefault(Color.Gray)))
                    Spacer(Modifier.width(8.dp))
                    Text(l.optString("label"), style = deskText(14))
                }
            }
        }
    }
}

/**
 * The canvas toolbar's section view (GLCanvas3D::_render_section_view_panel): a tap opens the panel (slider, value,
 * Set viewing angle, Reset), a long press toggles the cut like the icon's right click.
 */
@OptIn(androidx.compose.foundation.ExperimentalFoundationApi::class)
@Composable
private fun SectionViewButton(app: AppState) {
    var st by remember { mutableStateOf(JSONObject()) }
    var open by remember { mutableStateOf(false) }
    LaunchedEffect(Unit) { app.call("section.state") { if (it is JSONObject) st = it } }
    fun send(cmd: String, args: JSONObject = JSONObject()) = app.call(cmd, args) { if (it is JSONObject) { st = it; app.redraw() } }
    val active = st.optBoolean("active")
    Box {
        Box(Modifier.size(40.dp).shadow(2.dp, CircleShape).clip(CircleShape).background(desk().panel)
            .combinedClickable(onClick = { open = !open }, onLongClick = { send("section.toggle") }), Alignment.Center) {
            OrcaIcon(if (active) "canvas_section_active" else "canvas_section", 24.dp)
        }
        if (open) Popup(alignment = Alignment.BottomStart, offset = IntOffset(0, with(LocalDensity.current) { -50.dp.roundToPx() }),
            onDismissRequest = { open = false }) {
            val c = desk()
            Row(Modifier.shadow(4.dp, RoundedCornerShape(8.dp)).clip(RoundedCornerShape(8.dp)).background(c.panel)
                .padding(horizontal = 12.dp, vertical = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                Text(tr("Section view"), style = deskText(14))
                Spacer(Modifier.width(10.dp))
                var ratio by remember(st) { mutableStateOf(st.optDouble("ratio", 0.0).toFloat()) }
                Slider(ratio, { ratio = it; send("section.set", jobj("ratio" to it.toDouble())) }, Modifier.width(220.dp), valueRange = 0f..1f)
                Spacer(Modifier.width(8.dp))
                var text by remember(st) { mutableStateOf("%.2f".format(st.optDouble("ratio", 0.0))) }
                OutlinedTextField(text, { text = it; it.toDoubleOrNull()?.let { v -> send("section.set", jobj("ratio" to v)) } },
                    Modifier.width(84.dp), singleLine = true, textStyle = deskText(13))
                Spacer(Modifier.width(8.dp))
                OutlinedButton(onClick = { send("section.align") }) { Text(tr("Set viewing angle"), style = deskText(13)) }
                Spacer(Modifier.width(4.dp))
                // RevertBtn: Reset
                Box(Modifier.size(36.dp).clickable(enabled = active) { send("section.set", jobj("ratio" to 0.0)) }, Alignment.Center) {
                    OrcaIcon("revert_btn", 20.dp, alpha = if (active) 1f else 0.35f)
                }
            }
        }
    }
}

/** GLCanvas3D's "CanvasToolbarMenu": the view toggles with an eye showing their state */
@Composable
private fun CanvasMenu(app: AppState, preview: Boolean, open: Boolean, onDismiss: () -> Unit) {
    val o = app.viewOptions
    @Composable
    fun item(name: String, enabled: Boolean, key: String, close: Boolean = false) {
        val on = o.optBoolean(key)
        DropdownMenuItem(enabled = enabled, onClick = { app.setViewOption(key, !on); if (close) onDismiss() },
            leadingIcon = { OrcaIcon(if (on) "im_visible" else "im_hidden", 18.dp, alpha = if (enabled) 1f else 0.4f) },
            text = { Text(name, style = deskText(14, color = if (enabled) desk().text else desk().textDisabled)) })
    }
    DropdownMenu(open, onDismiss) {
        item(tr("3D Navigator"), true, "show_3d_navigator", close = true)
        item(tr("Zoom button"), true, "show_canvas_zoom_button", close = true)
        HorizontalDivider()
        item(tr("Overhangs"), !preview, "overhang")
        item(tr("Outline"), !preview, "show_outline")
        item(tr("Wireframe"), !preview, "wireframe")
        item(tr("X-Ray"), !preview, "xray")
        item(tr("Realistic View"), true, "opengl_realistic_mode")
        HorizontalDivider()
        item(tr("Perspective"), true, "use_perspective_camera")
        HorizontalDivider()
        item(tr("Axes"), true, "show_axes")
        item(tr("Gridlines"), true, "show_plate_gridlines")
        item(tr("Focus mode"), !preview, "focus_mode")
        HorizontalDivider()
        item(tr("Labels"), !preview, "labels")
        item(tr("Center of mass"), true, "show_center_of_mass")
    }
}

/** GLCanvas3D::Labels: the name of each object on the current plate (orange border when selected) */
@Composable
private fun ObjectLabels(app: AppState) {
    val labels = app.overlay.optJSONArray("labels") ?: return
    val d = LocalDensity.current
    for (i in 0 until labels.length()) {
        val l = labels.getJSONObject(i)
        val at = l.optJSONArray("at") ?: continue
        val sel = l.optBoolean("selected")
        var size by remember { mutableStateOf(IntOffset(0, 0)) }
        Text(l.optString("text"), Modifier.offset { IntOffset(at.getDouble(0).toInt() - size.x / 2, at.getDouble(1).toInt() - size.y / 2) }
            .onSizeChanged { size = IntOffset(it.width, it.height) }
            .border(if (sel) 3.dp else 1.5.dp, if (sel) Color(0xFFC16737) else Color(0xFFBFBFBF))
            .background(desk().panel.copy(alpha = 0.9f)).padding(horizontal = 6.dp, vertical = 3.dp),
            style = deskText(13))
    }
}

@Composable
private fun RoundButton(icon: String, onClick: () -> Unit) {
    Box(Modifier.size(40.dp).shadow(2.dp, CircleShape).clip(CircleShape).background(desk().panel).clickable(onClick = onClick), Alignment.Center) {
        OrcaIcon(icon, 24.dp)
    }
}

/** the object information box (GLCanvas3D: name, size, volume, triangles of the selected object) */
@Composable
private fun ObjectInfo(app: AppState, modifier: Modifier) {
    val info = app.project.optJSONObject("object_info") ?: return
    var shown by remember(info.optString("text")) { mutableStateOf(true) }
    if (!shown) return
    Row(modifier.widthIn(max = 460.dp).border(2.dp, OrcaColors.accent, RoundedCornerShape(4.dp)).background(desk().panel, RoundedCornerShape(4.dp))
        .padding(start = 14.dp, top = 8.dp, bottom = 8.dp, end = 4.dp)) {
        Column(Modifier.weight(1f)) {
            // bbl_show_objectsinfo_notification: the text, the <Error> part in the error colour
            Text(info.optString("text").trimEnd(), style = deskText(15))
            info.optString("errors").takeIf { it.isNotEmpty() }?.let { Text(it, style = deskText(15, color = OrcaColors.warning)) }
        }
        Box(Modifier.size(32.dp).clickable { shown = false }, Alignment.Center) { OrcaIcon("topbar_close", 14.dp, darkVariant = false, alpha = 0.6f) }
    }
}

/** NotificationManager: the slicing progress / result notification */
@Composable
private fun SliceNotification(app: AppState, modifier: Modifier) {
    val plate = app.project.optInt("current_plate")
    val result = app.sliceResults[plate]
    Column(modifier.widthIn(max = 440.dp), horizontalAlignment = Alignment.End, verticalArrangement = Arrangement.spacedBy(8.dp)) {
        // Plater::priv::on_slicing / update_slicing_notif_dailytips: a new tip for each slice
        var tip by remember { mutableStateOf<JSONObject?>(null) }
        var tipsOpen by remember { mutableStateOf(false) }
        LaunchedEffect(app.slicing) { if (app.slicing) app.call("tips.get", jobj("nav" to if (tip == null) "random" else "next")) { if (it is JSONObject) tip = it } }
        if (app.slicing) {
            val ev = app.sliceEvent
            Column(Modifier.shadow(3.dp, RoundedCornerShape(4.dp)).background(desk().panel, RoundedCornerShape(4.dp))) {
                NotificationBox {
                    Column(Modifier.weight(1f)) {
                        Text(ev?.optString("text").takeIf { !it.isNullOrEmpty() } ?: "Slicing...", style = deskText(14), maxLines = 2)
                        LinearProgressIndicator(progress = { (ev?.optInt("percent", 0) ?: 0).coerceIn(0, 100) / 100f },
                            Modifier.fillMaxWidth().padding(top = 6.dp), color = OrcaColors.accent)
                    }
                    Text(tr("Cancel"), Modifier.clickable { app.call("slice.cancel") }.padding(start = 12.dp), style = deskText(14, color = OrcaColors.accent))
                }
                // the DailyTipsPanel of the slicing notification: "Daily Tips" to expand, "Collapse" to fold
                tip?.let { t ->
                    val str = t.optJSONObject("strings") ?: JSONObject()
                    if (tipsOpen) Column(Modifier.padding(start = 16.dp, end = 12.dp)) { org.orcaport.mobile.ui.DailyTipContent(app, t, showImage = false) }
                    Row(Modifier.fillMaxWidth().padding(start = 16.dp, end = 4.dp), verticalAlignment = Alignment.CenterVertically) {
                        Text((if (tipsOpen) str.optString("collapse") + " ˅" else str.optString("daily_tips") + " ›"),
                            Modifier.weight(1f).clickable { tipsOpen = !tipsOpen }, style = deskText(13, FontWeight.Bold))
                        if (tipsOpen) org.orcaport.mobile.ui.DailyTipNav(t) { nav -> app.call("tips.get", jobj("nav" to nav)) { if (it is JSONObject) tip = it } }
                    }
                }
            }
        } else if (result != null && result.optString("state") == "error")
            NotificationBox(OrcaColors.error) { Text(result.optString("text"), style = deskText(14)) }
        app.mixedBroken?.let { NotificationBox(OrcaColors.error) { Text(it, style = deskText(13)) } }
        if (!app.slicing) for (w in app.sliceWarnings) NotificationBox(OrcaColors.warning) { Text(w, style = deskText(13)) }
    }
}

@Composable
private fun NotificationBox(bar: Color = OrcaColors.accent, content: @Composable RowScope.() -> Unit) {
    Row(Modifier.shadow(3.dp, RoundedCornerShape(4.dp)).background(desk().panel, RoundedCornerShape(4.dp)).height(IntrinsicSize.Min)) {
        Box(Modifier.width(4.dp).fillMaxHeight().background(bar))
        Row(Modifier.padding(12.dp), verticalAlignment = Alignment.CenterVertically, content = content)
    }
}

/** the object context menu (GUI_Factories for an object, or the plate menu on empty space) */
/**
 * PlateSettingsDialog: plate name, bed type (the printer model's types), print sequence, spiral vase, first layer
 * filament sequence (Auto / Customize with the DragCanvas), other layers filament sequence (layer ranges, each with its
 * order; + / − add and remove the last), the mixed-filament warning. The engine fills it (plate.dialog) and applies it
 * as Plater::on_plate_settings does (plate.dialog_apply).
 */
@Composable
private fun PlateSettingsDialog(app: AppState, plate: Int, onDismiss: () -> Unit) {
    var d by remember { mutableStateOf<JSONObject?>(null) }
    var name by remember { mutableStateOf("") }
    var bed by remember { mutableIntStateOf(0) }
    var seq by remember { mutableIntStateOf(0) }
    var spiral by remember { mutableIntStateOf(0) }
    var firstCustom by remember { mutableStateOf(false) }
    val first = remember { mutableStateListOf<Int>() }
    var otherCustom by remember { mutableStateOf(false) }
    // a range: begin, end (-1 = End; -2 = Customize with nothing typed yet), order
    val other = remember { mutableStateListOf<Triple<Int, Int, List<Int>>>() }
    LaunchedEffect(plate) {
        app.call("plate.dialog", jobj("plate" to plate)) { r ->
            val j = r as? JSONObject ?: return@call
            d = j; name = j.optString("name"); bed = j.optInt("bed_index"); seq = j.optInt("print_seq"); spiral = j.optInt("spiral")
            val n = j.optJSONArray("colors")?.length() ?: 0
            firstCustom = j.optBoolean("first_custom")
            first.clear(); j.optJSONArray("first")?.let { a -> for (k in 0 until a.length()) first += a.getInt(k) }
            if (first.isEmpty()) for (k in 1..n) first += k
            otherCustom = j.optBoolean("other_custom")
            other.clear()
            j.optJSONArray("other")?.let { a ->
                for (k in 0 until a.length()) {
                    val o = a.getJSONObject(k); val ord = o.optJSONArray("order")
                    other += Triple(o.optInt("begin"), o.optInt("end", -1), (0 until (ord?.length() ?: 0)).map { ord!!.getInt(it) })
                }
            }
            if (other.isEmpty()) other += Triple(2, -1, (1..n).toList())
        }
    }
    val dj = d ?: return
    val str = dj.optJSONObject("strings") ?: JSONObject()
    val colors = dj.optJSONArray("colors") ?: JSONArray()
    val mixed = dj.optBoolean("mixed")
    fun labels(key: String) = dj.optJSONArray(key)?.let { a -> (0 until a.length()).map { a.getString(it) } } ?: emptyList()
    AlertDialog(onDismissRequest = onDismiss, title = { Text(str.optString("title"), style = deskText(16, FontWeight.Bold)) }, text = {
        Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            @Composable fun row(label: String, content: @Composable () -> Unit) = Row(verticalAlignment = Alignment.CenterVertically) {
                Text(label, Modifier.width(230.dp), style = deskText(14)); Box(Modifier.weight(1f)) { content() }
            }
            row(str.optString("name")) { DeskInput(name, true, false, Modifier.fillMaxWidth()) { name = it } }
            row(str.optString("bed")) { DeskChoice(labels("bed_labels"), bed, Modifier.fillMaxWidth(), enabled = dj.optBoolean("bed_enabled")) { bed = it } }
            row(str.optString("seq")) { DeskChoice(labels("print_seq_labels"), seq, Modifier.fillMaxWidth()) { seq = it } }
            row(str.optString("spiral")) { DeskChoice(labels("spiral_labels"), spiral, Modifier.fillMaxWidth()) { spiral = it } }
            val autoCustom = listOf(str.optString("auto"), str.optString("custom"))
            row(str.optString("first")) { DeskChoice(autoCustom, if (firstCustom) 1 else 0, Modifier.fillMaxWidth(), enabled = !mixed) { firstCustom = it == 1 } }
            if (firstCustom) Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) { DragCanvas(colors, first) }
            row(str.optString("other")) { DeskChoice(autoCustom, if (otherCustom) 1 else 0, Modifier.fillMaxWidth(), enabled = !mixed) { otherCustom = it == 1 } }
            if (otherCustom) {
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.Center) {
                    Box(Modifier.size(32.dp).clickable { other += Triple(-2, -1, (1..colors.length()).toList()) }, Alignment.Center) { OrcaIcon("add_filament", 20.dp) }
                    Box(Modifier.size(32.dp).clickable { if (other.size > 1) other.removeAt(other.size - 1) }, Alignment.Center) { OrcaIcon("delete_filament", 20.dp) }
                }
                for (k in other.indices) {
                    val (b, e, ord) = other[k]
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                        Text(str.optString("layer"), style = deskText(14))
                        LayerNumberInput(b, null, Modifier.width(100.dp)) { v -> other[k] = Triple(v, if (e >= 2 && v > e) v else e, ord) }
                        Text(str.optString("to"), style = deskText(14))
                        LayerNumberInput(e, str.optString("end"), Modifier.width(110.dp)) { v -> other[k] = Triple(b, if (v >= 2 && v < b) b else v, ord) }
                        Spacer(Modifier.weight(1f))
                        val list = remember(k) { mutableStateListOf<Int>().also { it.addAll(ord) } }
                        LaunchedEffect(list.toList()) { if (list.toList() != other.getOrNull(k)?.third) other.getOrNull(k)?.let { other[k] = Triple(it.first, it.second, list.toList()) } }
                        DragCanvas(colors, list)
                    }
                }
            }
            if (mixed) Row(verticalAlignment = Alignment.CenterVertically) {
                OrcaIcon("warning", 16.dp); Spacer(Modifier.width(5.dp))
                Text(dj.optString("mixed_text"), style = deskText(12, color = Color(0xFFFF6F00)))
            }
        }
    }, confirmButton = {
        TextButton({
            val bedValues = dj.optJSONArray("bed_values")
            val otherJson = JSONArray().apply { for ((b, e, ord) in other) put(jobj("begin" to b, "end" to e, "order" to JSONArray(ord))) }
            app.call("plate.dialog_apply", jobj("plate" to plate, "name" to name, "bed" to (bedValues?.optInt(bed) ?: 0), "print_seq" to seq, "spiral" to spiral,
                "first_custom" to firstCustom, "first" to JSONArray(first.toList()), "other_custom" to otherCustom, "other" to otherJson)) { r ->
                val j = r as? JSONObject ?: return@call
                if (j.optBoolean("invalid")) return@call   // the layer value message was shown; the dialog stays
                app.project = j; app.redraw(); onDismiss()
            }
        }) { Text(tr("OK")) }
    }, dismissButton = { TextButton(onDismiss) { Text(tr("Cancel")) } })
}

/** LayerNumberTextInput: a layer number (>= 2) typed in, or "End" for the last layer of the range (endLabel given) */
@Composable
private fun LayerNumberInput(value: Int, endLabel: String?, modifier: Modifier, onChange: (Int) -> Unit) {
    var open by remember { mutableStateOf(false) }
    Box(modifier) {
        if (endLabel != null && value == -1) DeskCombo(endLabel, Modifier.fillMaxWidth(), height = 34.dp) { open = true }
        else Row(verticalAlignment = Alignment.CenterVertically) {
            DeskInput(if (value < 0) "" else "$value", true, true, Modifier.weight(1f)) { s -> onChange(s.toIntOrNull()?.coerceIn(2, Int.MAX_VALUE - 1) ?: -2) }
            if (endLabel != null) Text("˅", Modifier.clickable { open = true }.padding(4.dp), style = deskText(13))
        }
        if (endLabel != null) DropdownMenu(open, { open = false }) {
            DropdownMenuItem(text = { Text(endLabel, style = deskText(13)) }, onClick = { open = false; onChange(-1) })
            DropdownMenuItem(text = { Text(tr("Customize"), style = deskText(13)) }, onClick = { open = false; onChange(-2) })
        }
    }
}

/** DragCanvas: the filaments as numbered colour chips with arrows between; drag a chip onto another to swap them */
@Composable
private fun DragCanvas(colors: JSONArray, order: MutableList<Int>) {
    val c = desk()
    var dragging by remember { mutableIntStateOf(-1) }
    var dragX by remember { mutableFloatStateOf(0f) }
    val chip = 30.dp; val gap = 18.dp
    val d = LocalDensity.current
    val step = with(d) { (chip + gap).toPx() }
    Row(Modifier.border(1.dp, c.border, RoundedCornerShape(4.dp)).padding(6.dp).pointerInput(order.size) {
        detectDragGestures(onDragStart = { o -> dragging = (o.x / step).toInt().coerceIn(0, order.size - 1); dragX = o.x },
            onDragEnd = {
                val target = (dragX / step).toInt().coerceIn(0, order.size - 1)
                if (dragging >= 0 && target != dragging) { val t = order[dragging]; order[dragging] = order[target]; order[target] = t }
                dragging = -1
            }, onDragCancel = { dragging = -1 }) { ch, delta -> ch.consume(); dragX += delta.x }
    }, verticalAlignment = Alignment.CenterVertically) {
        order.forEachIndexed { i, f ->
            if (i > 0) Box(Modifier.width(gap), Alignment.Center) { Text("→", style = deskText(12, color = c.textDim)) }
            val col = org.orcaport.mobile.settings.parseColor(colors.optString(f - 1)) ?: Color.Gray
            val lum = 0.299 * col.red + 0.587 * col.green + 0.114 * col.blue
            Box(Modifier.size(chip).clip(RoundedCornerShape(4.dp)).background(col)
                .border(if (i == dragging) 2.dp else 1.dp, if (i == dragging) OrcaColors.accent else c.border, RoundedCornerShape(4.dp)), Alignment.Center) {
                Text("$f", style = deskText(13, color = if (lum > 0.6) Color.Black else Color.White))
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------


@Composable
private fun BoxScope.PreviewOverlay(app: AppState) {
    val pv = app.preview
    val c = desk()
    if (!pv.optBoolean("loaded")) return   // GCodeViewer: nothing over the plate until it is sliced
    val layers = pv.optJSONArray("layers") ?: JSONArray()
    val n = layers.length()
    val lr = pv.optJSONArray("layer_range")
    val compactLegend = isCompact()
    var legendOpen by remember { mutableStateOf(!compactLegend) }   // GCodeViewer::m_fold
    val gcodeWindow = app.viewOptions.optBoolean("show_gcode_window")   // GUI_App::show_gcode_window
    val legend = pv.optJSONObject("legend") ?: JSONObject()
    // GCodeViewer::render_legend, at the top right left of the layer slider
    Column(Modifier.align(Alignment.TopEnd).padding(top = 8.dp, end = 96.dp, bottom = 64.dp).fillMaxHeight().widthIn(max = 460.dp), horizontalAlignment = Alignment.End) {
        Column(Modifier.weight(1f, fill = false).shadow(2.dp, RoundedCornerShape(8.dp)).background(c.panel.copy(alpha = 0.92f), RoundedCornerShape(8.dp)).padding(8.dp)
            .verticalScroll(rememberScrollState())) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(Modifier.size(32.dp).clickable { legendOpen = !legendOpen }, Alignment.Center) { OrcaIcon(if (legendOpen) "im_fold" else "im_unfold", 16.dp, tint = c.text) }
                Box(Modifier.size(32.dp).clickable {   // toggle_show_gcode_window
                    app.setViewOption("show_gcode_window", !gcodeWindow)
                }, Alignment.Center) { OrcaIcon("im_code", 16.dp, tint = c.text) }
                var typeMenu by remember { mutableStateOf(false) }
                val types = legend.optJSONArray("view_types") ?: JSONArray()
                val cur = (0 until types.length()).map { types.getJSONObject(it) }.firstOrNull { it.optInt("id") == pv.optInt("view_type") }
                Box(Modifier.width(220.dp)) {
                    DeskCombo(cur?.optString("name") ?: "", Modifier.fillMaxWidth()) { typeMenu = true }
                    DropdownMenu(typeMenu, { typeMenu = false }) {
                        for (i in 0 until types.length()) {
                            val t = types.getJSONObject(i)
                            DropdownMenuItem(text = { Text(t.optString("name"), style = deskText(14)) }, onClick = {
                                typeMenu = false; legendOpen = true
                                Native.previewSet(jobj("view_type" to t.optInt("id")).toString()); app.redraw()
                            })
                        }
                    }
                }
            }
            if (legendOpen) LegendSections(app, legend.optJSONArray("sections") ?: JSONArray())
        }
        if (gcodeWindow) BoxWithConstraints(Modifier.weight(1f)) {
            // GCodeWindow: as many lines as fit between the legend and the bottom
            GCodeWindow(app, pv.optLong("current_line"), ((maxHeight - 24.dp) / 16.dp).toInt().coerceIn(0, 40))
        }
    }
    pv.optJSONObject("position")?.let { ToolPositionWindow(it, Modifier.align(Alignment.BottomCenter).padding(bottom = 56.dp)) }
    if (n > 0 && lr != null) {
        LayerSlider(app, layers, lr, Modifier.align(Alignment.CenterEnd).padding(end = 6.dp).fillMaxHeight(0.8f))
        val vr = pv.optJSONArray("view_range"); val fr = pv.optJSONArray("full_range")
        if (vr != null && fr != null && fr.getInt(1) > fr.getInt(0)) {
            var pos by remember(lr.getInt(1), fr.getInt(1), vr.getInt(1)) { mutableFloatStateOf(vr.getInt(1).toFloat()) }
            Slider(value = pos.coerceIn(fr.getInt(0).toFloat(), fr.getInt(1).toFloat()), onValueChange = {
                pos = it
                Native.previewSet(jobj("view_range" to JSONArray().put(fr.getInt(0)).put(it.toInt())).toString()); app.redraw()
            }, valueRange = fr.getInt(0).toFloat()..fr.getInt(1).toFloat(),
                colors = SliderDefaults.colors(thumbColor = OrcaColors.accent, activeTrackColor = OrcaColors.accent),
                modifier = Modifier.align(Alignment.BottomCenter).padding(start = 140.dp, end = 80.dp, bottom = 8.dp).fillMaxWidth())
        }
    }
}

/** the legend's sections (render_legend: headers, colour rows with the eye toggle, titles, label: value pairs, buttons) */
@Composable
private fun LegendSections(app: AppState, sections: JSONArray) {
    val c = desk()
    val colW = 64.dp
    for (i in 0 until sections.length()) {
        val s = sections.getJSONObject(i)
        when {
            s.has("headers") -> Row(Modifier.padding(top = 8.dp, bottom = 2.dp)) {
                val h = s.getJSONArray("headers")
                Spacer(Modifier.width(20.dp))
                for (k in 0 until h.length())
                    Text(h.getString(k), if (k == 0) Modifier.width(130.dp) else Modifier.width(colW), style = deskText(13, FontWeight.Bold), maxLines = 1)
            }
            s.has("title") -> Text(s.getString("title"), Modifier.padding(top = 8.dp, bottom = 2.dp), style = deskText(13, FontWeight.Bold))
            s.has("rows") -> {
                val rows = s.getJSONArray("rows")
                for (k in 0 until rows.length()) {
                    val r = rows.getJSONObject(k)
                    val toggle = r.optJSONObject("toggle")
                    val visible = if (r.isNull("visible")) true else r.optBoolean("visible")
                    Row(Modifier.then(if (toggle != null) Modifier.clickable {
                        if (toggle.has("role")) Native.previewSet(jobj("toggle_role" to toggle.getInt("role")).toString())
                        else Native.previewSet(jobj("toggle_option" to toggle.getInt("option")).toString())
                        app.redraw()
                    } else Modifier).padding(vertical = 3.dp), verticalAlignment = Alignment.CenterVertically) {
                        val col = r.optJSONArray("color")
                        if (col != null) Box(Modifier.size(12.dp).background(Color(col.getInt(0), col.getInt(1), col.getInt(2)), RoundedCornerShape(2.dp)))
                        else Spacer(Modifier.width(12.dp))
                        Spacer(Modifier.width(8.dp))
                        val cols = r.getJSONArray("cols")
                        val tc = if (visible) c.text else c.textDisabled
                        for (j in 0 until cols.length())
                            Text(cols.getString(j), if (j == 0) Modifier.width(130.dp) else Modifier.width(colW), style = deskText(12, color = tc))
                        if (toggle != null) OrcaIcon(if (visible) "im_visible" else "im_hidden", 16.dp)
                    }
                }
            }
            s.has("pairs") -> {
                val pairs = s.getJSONArray("pairs")
                for (k in 0 until pairs.length()) {
                    val pr = pairs.getJSONArray(k)
                    Row(Modifier.padding(vertical = 2.dp)) {
                        Text(pr.getString(0), Modifier.width(170.dp), style = deskText(12))
                        Text(pr.getString(1), style = deskText(12))
                    }
                }
            }
            s.has("button") -> PanelButton(s.getString("button")) { Native.previewSet(jobj("time_mode" to s.optInt("time_mode")).toString()); app.redraw() }
        }
    }
}

/** SequentialView::Marker::render_position_window: X / Y / Z, speed and the view's detail; the fold button shows the properties */
@Composable
private fun ToolPositionWindow(p: JSONObject, modifier: Modifier) {
    var shown by remember { mutableStateOf(false) }   // properties_shown
    val c = desk()
    Column(modifier.shadow(2.dp, RoundedCornerShape(8.dp)).background(c.panel.copy(alpha = 0.92f), RoundedCornerShape(8.dp)).padding(10.dp)) {
        if (shown) {
            val rows = p.optJSONArray("properties") ?: JSONArray()
            for (i in 0 until rows.length()) {
                val r = rows.getJSONArray(i)
                Row { Text(r.getString(0), Modifier.width(140.dp), style = deskText(12, color = OrcaColors.accent)); Text(r.getString(1), style = deskText(12)) }
            }
            HorizontalDivider(Modifier.padding(vertical = 6.dp), color = c.border)
        }
        Row(verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.size(28.dp).clickable { shown = !shown }, Alignment.Center) { OrcaIcon(if (shown) "im_unfold" else "im_fold", 16.dp, tint = c.text) }
            Spacer(Modifier.width(8.dp))
            Column {
                Row {
                    for ((axis, col) in listOf("x" to Color(0xFFE53935), "y" to Color(0xFF43A047), "z" to Color(0xFF1E88E5))) {
                        Text(axis.uppercase() + " ", style = deskText(13, color = col))
                        Text(p.optString(axis), Modifier.width(80.dp), style = deskText(13))
                    }
                }
                Row {
                    Text(p.optString("speed"), Modifier.width(110.dp), style = deskText(13))
                    Text(p.optString("detail"), style = deskText(13))
                }
            }
        }
    }
}

/** GCodeViewer::SequentialView::GCodeWindow: the G-code around the current move */
@Composable
private fun GCodeWindow(app: AppState, currentLine: Long, count: Int) {
    var lines by remember { mutableStateOf(JSONArray()) }
    LaunchedEffect(currentLine, count) { if (currentLine > 0 && count > 0) app.call("preview.gcode_lines", jobj("count" to count)) { if (it is JSONArray) lines = it } }
    if (currentLine <= 0 || count <= 0 || lines.length() == 0) return
    Column(Modifier.padding(top = 6.dp).shadow(2.dp, RoundedCornerShape(8.dp)).background(Color(0xCC2B2B2B), RoundedCornerShape(8.dp)).padding(8.dp)) {
        for (i in 0 until lines.length()) {
            val l = lines.getJSONObject(i)
            Row(Modifier.height(16.dp).then(if (l.optBoolean("current")) Modifier.border(1.dp, Color(0xFFE88A0E)) else Modifier).padding(horizontal = 2.dp)) {
                Text("${l.optLong("id")} ", style = monoStyle.copy(color = Color(0xFFF4A460), fontSize = 11.sp))
                Text(l.optString("command"), style = monoStyle.copy(color = Color(0xFFCCCC00), fontSize = 11.sp))
                Text(l.optString("parameters"), style = monoStyle.copy(color = Color.White, fontSize = 11.sp))
                Text(" " + l.optString("comment"), style = monoStyle.copy(color = Color(0xFFB3B3B3), fontSize = 11.sp))
            }
        }
    }
}

fun fmtTime(s: Double): String {
    if (s.isNaN() || s <= 0) return "0s"
    val t = s.toLong(); val d = t / 86400; val h = t % 86400 / 3600; val m = t % 3600 / 60; val sec = t % 60
    return buildString {
        if (d > 0) append("${d}d ")
        if (h > 0 || d > 0) append("${h}h ")
        if (m > 0 || h > 0 || d > 0) append("${m}m ")
        if (d == 0L && h == 0L) append("${sec}s")
    }.trim()
}
