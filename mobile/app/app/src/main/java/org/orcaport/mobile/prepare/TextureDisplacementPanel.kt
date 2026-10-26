package org.orcaport.mobile.prepare

import android.graphics.BitmapFactory
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import org.json.JSONArray
import org.json.JSONObject
import org.orcaport.mobile.AppState
import org.orcaport.mobile.engine.jobj
import org.orcaport.mobile.ui.*
import java.util.Locale
import kotlin.math.roundToInt
import androidx.compose.ui.layout.positionInWindow
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.foundation.gestures.detectDragGestures

/**
 * GLGizmoTextureDisplacement::on_render_input_window: Paint / Erase and the tools (Brush, Face, Connected area, Select
 * / Erase whole model), the view, the texture layers (the active one as a card with its settings, the others as rows),
 * Add layer, Pro mode's result options and Bake. The texture library popup picks a layer's image. The layer stack lives
 * on the volume (texdisp.* commands); the brush is the Painter's ("texture" kind).
 */
@Composable
fun TextureDisplacementPanel(app: AppState, modifier: Modifier) {
    var st by remember { mutableStateOf(JSONObject()) }
    LaunchedEffect(app.project.optJSONArray("selection")?.toString(), app.project.optInt("revision"), app.gizmoRevision) {
        app.call("texdisp.state") { if (it is JSONObject) st = it }
    }
    fun apply(r: Any?) { if (r is JSONObject) { st = r; r.optJSONObject("project")?.let { p -> app.project = p }; app.redraw() } }
    val call: (String, Array<out Pair<String, Any>>) -> Unit = { cmd, kv -> app.call(cmd, jobj(*kv)) { r -> apply(r) } }
    fun cmd(name: String, vararg kv: Pair<String, Any>) = call(name, kv)
    fun paint(k: String, v: Any) = app.call("paint.set", jobj(k to v)) { app.call("texdisp.state") { r -> apply(r) } }
    val c = desk()
    val paintSt = st.optJSONObject("paint") ?: JSONObject()
    val layers = st.optJSONArray("layers") ?: JSONArray()
    val activeSlot = st.optInt("active")
    val busy = st.optBoolean("busy")
    var picker by remember { mutableIntStateOf(-1) }
    val active = (0 until layers.length()).map { layers.getJSONObject(it) }.firstOrNull { it.optInt("slot") == activeSlot }

    Row(modifier, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
    Column(Modifier.width(400.dp).shadow(4.dp, RoundedCornerShape(8.dp)).clip(RoundedCornerShape(8.dp)).background(c.panel)) {
        // the title bar: the name and Standard / Pro
        Row(Modifier.fillMaxWidth().padding(start = 12.dp, end = 8.dp, top = 8.dp), verticalAlignment = Alignment.CenterVertically) {
            Text(tr("Texture displacement"), Modifier.weight(1f), style = deskText(15, FontWeight.Bold))
            Text(tr("Pro"), style = deskText(13, color = c.textDim))
            Switch(st.optBoolean("pro"), { cmd("texdisp.options", "pro" to it) }, Modifier.padding(start = 4.dp),
                colors = SwitchDefaults.colors(checkedTrackColor = OrcaColors.accent))
        }
        if (!st.optBoolean("has_volume")) {
            Text(tr("Select an object first."), Modifier.padding(12.dp), style = deskText(13, color = c.textDim))
            return@Column
        }
        Column(Modifier.weight(1f, fill = false).verticalScroll(rememberScrollState()).padding(horizontal = 12.dp, vertical = 6.dp),
            verticalArrangement = Arrangement.spacedBy(6.dp)) {
            // Paint: into the active layer; Paint / Erase
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(tr("Paint"), style = deskText(14, FontWeight.Bold))
                Text(active?.let { String.format(tr("into %1%").replace("%1%", "%s"), it.optString("name")) } ?: tr("add a layer to paint"),
                    Modifier.weight(1f).padding(start = 8.dp), style = deskText(12, color = c.textDim), maxLines = 1, overflow = TextOverflow.Ellipsis)
                IconToggle("texture_displacement_add", !st.optBoolean("erase"), 30.dp) { cmd("texdisp.options", "erase" to false) }
                IconToggle("texture_displacement_add_negative", st.optBoolean("erase"), 30.dp) { cmd("texdisp.options", "erase" to true) }
            }
            // the tools, then the brush size and shape, the fill angle or the face hint
            val tool = paintSt.optString("tool", "circle")
            val brush = tool == "circle" || tool == "sphere"
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                IconToggle("texture_displacement_brush", brush, 38.dp) { paint("tool", "circle") }
                IconToggle("texture_displacement_face", tool == "pointer", 38.dp) { paint("tool", "pointer") }
                IconToggle("texture_displacement_connected_area", tool == "smart_fill", 38.dp) { paint("tool", "smart_fill") }
                IconToggle("texture_displacement_select_all", false, 38.dp, enabled = !busy && active != null) { cmd("texdisp.select_all", "paint" to true) }
                IconToggle("texture_displacement_erase_all", false, 38.dp, enabled = !busy && active?.optBoolean("painted") == true) {
                    cmd("texdisp.select_all", "paint" to false)
                }
            }
            when {
                brush -> Row(verticalAlignment = Alignment.CenterVertically) {
                    ValueSlider(paintSt.optDouble("radius", 2.0).toFloat(), 0.4f, 8f, "%.2f mm", Modifier.weight(1f)) { paint("radius", it) }
                    IconToggle("circle_paint", tool == "circle", 30.dp) { paint("tool", "circle") }
                    IconToggle("menu_obj_sphere", tool == "sphere", 30.dp) { paint("tool", "sphere") }
                }
                tool == "smart_fill" -> ValueSlider(paintSt.optDouble("smart_fill_angle", 30.0).toFloat(), 0f, 90f, "%.0f°") { paint("smart_fill_angle", it) }
                else -> Text(tr("Click a triangle to paint it"), style = deskText(12, color = c.textDim))
            }
            // View: the true preview; Auto rebuilds it on every change
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(tr("View"), Modifier.width(52.dp), style = deskText(13, color = c.textDim))
                val lscm = active?.optString("projection") == "lscm"
                IconToggle("texture_displacement_real_preview", st.optInt("view") == 0, 34.dp) { cmd("texdisp.uv", "view" to 0) }
                IconToggle("texture_displacement_fast_preview", st.optInt("view") == 1, 34.dp) { cmd("texdisp.uv", "view" to 1) }
                IconToggle("texture_displacement_checker", st.optInt("view") == 2, 34.dp, enabled = lscm) { cmd("texdisp.uv", "open" to true, "view" to 2) }
                IconToggle("texture_displacement_distortion", st.optInt("view") == 3, 34.dp, enabled = lscm) { cmd("texdisp.uv", "open" to true, "view" to 3) }
                Box(Modifier.padding(horizontal = 6.dp).width(1.dp).height(24.dp).background(c.border))
                IconToggle("texture_displacement_wireframe", st.optBoolean("wireframe"), 34.dp) { cmd("texdisp.options", "wireframe" to !st.optBoolean("wireframe")) }
                Spacer(Modifier.weight(1f))
                DeskCheck(st.optBoolean("auto", true)) { cmd("texdisp.options", "auto" to it) }
                Text(tr("Auto"), style = deskText(13))
            }
            HorizontalDivider(color = c.border)
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(tr("Texture layers"), Modifier.weight(1f), style = deskText(14, FontWeight.Bold))
                Text("${layers.length()} / ${st.optInt("max_layers", 8)}", style = deskText(12, color = c.textDim))
            }
            for (i in 0 until layers.length()) {
                val l = layers.getJSONObject(i)
                if (l.optInt("slot") == activeSlot) LayerCard(app, st, l, i, layers.length(), busy, { picker = it }, call)
                else LayerRow(l) { cmd("texdisp.activate", "slot" to l.optInt("slot")) }
            }
            val full = layers.length() >= st.optInt("max_layers", 8)
            Box(Modifier.fillMaxWidth().height(34.dp).clip(RoundedCornerShape(4.dp)).border(1.dp, c.border, RoundedCornerShape(4.dp))
                .clickable(enabled = !busy && !full) { cmd("texdisp.add_layer") }, Alignment.Center) {
                Text(if (full) st.optString("layers_full_text") else "+  " + tr("Add layer"),
                    style = deskText(13, color = if (full) c.textDisabled else c.text))
            }
            if (st.optBoolean("pro")) { ProPrepare(st, busy, call); ProResult(st, call) }
        }
        HorizontalDivider(color = c.border)
        Row(Modifier.fillMaxWidth().padding(10.dp), verticalAlignment = Alignment.CenterVertically) {
            if (!st.optBoolean("painted")) Text(tr("Nothing is painted yet."), Modifier.weight(1f), style = deskText(12, color = c.textDim))
            else Spacer(Modifier.weight(1f))
            PanelButton(if (busy) tr("Baking...") else trc("Bake", "Texture Displacement"), primary = true, enabled = !busy && st.optBoolean("painted")) {
                cmd("texdisp.bake")
            }
            Spacer(Modifier.width(8.dp))
            PanelButton(tr("Close")) { app.gizmo = null }
        }
    }
    // the docked UV editor (Plater::show_uv_editor), beside the panel
    if (st.optJSONObject("uv") != null)
        UVEditorPane(app, st, onState = { st = it; app.redraw() }, onPickTexture = { picker = activeSlot },
            modifier = Modifier.align(Alignment.Bottom).weight(1f, fill = false).widthIn(max = 560.dp).fillMaxHeight(0.55f))
    }
    st.optJSONObject("adjust")?.let { AdjustImageWindow(it, call) }
    if (st.optJSONObject("projector")?.optBoolean("open") == true && active?.optString("projection") == "view")
        ProjectorFrame(app, active.optString("image"), st.optJSONObject("projector")!!.optInt("opacity", 140), call)
    if (picker >= 0) TexturePicker(st, layers, picker, onDismiss = { picker = -1 },
        onPick = { path -> val slot = picker; picker = -1; cmd("texdisp.set_texture", "slot" to slot, "path" to path) },
        onImport = { val slot = picker; picker = -1; cmd("texdisp.import_texture", "slot" to slot) },
        onRemove = { path -> cmd("texdisp.remove_texture", "path" to path) },
        onExplore = { picker = -1; app.openOrcaCloud("/app/plugins/plugin-hub?types=resource-textures") })
}

/** the active layer: its image and name (open the picker), move up / down, remove, Depth / Tile size / Rotation and More settings */
@Composable
private fun LayerCard(app: AppState, st: JSONObject, l: JSONObject, index: Int, count: Int, busy: Boolean, openPicker: (Int) -> Unit,
                      call: (String, Array<out Pair<String, Any>>) -> Unit) {
    val c = desk()
    val slot = l.optInt("slot")
    fun set(k: String, v: Any) = call("texdisp.layer", arrayOf("slot" to slot, k to v))
    Column(Modifier.fillMaxWidth().clip(RoundedCornerShape(6.dp)).border(1.5.dp, OrcaColors.accent, RoundedCornerShape(6.dp)).padding(8.dp),
        verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Thumb(l.optString("image"), 34.dp, Modifier.border(1.dp, OrcaColors.accent, RoundedCornerShape(3.dp)).clickable { openPicker(slot) })
            Text(l.optString("name"), Modifier.weight(1f).padding(horizontal = 8.dp).clickable { openPicker(slot) }, style = deskText(13),
                maxLines = 1, overflow = TextOverflow.Ellipsis)
            if (!l.optBoolean("painted")) Text(tr("not painted"), Modifier.padding(end = 4.dp), style = deskText(11, color = c.textDim))
            IconToggle("cog", false, 30.dp, enabled = !busy && l.optString("image").isNotEmpty()) { call("texdisp.adjust", arrayOf("action" to "open", "slot" to slot)) }
            IconToggle("texture_displacement_move_up", false, 30.dp, enabled = !busy && index > 0) { call("texdisp.move_layer", arrayOf("slot" to slot, "to" to index - 1)) }
            IconToggle("texture_displacement_move_down", false, 30.dp, enabled = !busy && index + 1 < count) { call("texdisp.move_layer", arrayOf("slot" to slot, "to" to index + 2)) }
            IconToggle("texture_displacement_cross", false, 30.dp, enabled = !busy) { call("texdisp.remove_layer", arrayOf("slot" to slot)) }
        }
        val modelUv = l.optString("projection") == "model_uv"
        Labeled(tr("Depth")) { ValueSlider(l.optDouble("depth").toFloat(), 0.01f, 10f, "%.3f mm", log = true) { set("depth", it) } }
        Labeled(tr("Tile size")) { ValueSlider(l.optDouble("tile_size").toFloat(), 0.2f, 200f, "%.2f mm", log = true, enabled = !modelUv) { set("tile_size", it) } }
        Labeled(tr("Rotation")) { ValueSlider(l.optDouble("rotation").toFloat(), 0f, 360f, "%.0f°") { set("rotation", it) } }
        val expanded = l.optBoolean("expanded")
        if (expanded) {
            HorizontalDivider(color = c.border)
            Labeled(tr("Midlevel")) { ValueSlider(l.optDouble("midlevel").toFloat(), 0f, 10f, "%.2f") { set("midlevel", it) } }
            if (l.optDouble("midlevel") > 0 && l.optDouble("depth") > 1)
                Text(tr("Deep inward displacement may self-intersect."), style = deskText(12, color = Color(0xFFFF6F00)))
            Labeled(tr("Smoothing")) { ValueSlider(l.optDouble("smoothing").toFloat(), 0f, 1f, "%.2f") { set("smoothing", it) } }
            Row(verticalAlignment = Alignment.CenterVertically) {
                DeskCheck(l.optBoolean("edge_smoothing")) { set("edge_smoothing", it) }
                Text(tr("Edge fade"), Modifier.width(80.dp), style = deskText(13))
                ValueSlider(l.optDouble("edge_amount").toFloat(), 0.02f, 1f, "%.2f", Modifier.weight(1f), enabled = l.optBoolean("edge_smoothing")) { set("edge_amount", it) }
            }
            Row(verticalAlignment = Alignment.CenterVertically) {
                DeskCheck(l.optBoolean("invert")) { set("invert", it) }
                Text(tr("Invert"), Modifier.weight(1f), style = deskText(13))
                DeskCheck(l.optBoolean("color"), enabled = l.optBoolean("has_color")) { set("color", it) }
                Text(tr("Colors"), Modifier.weight(1f), style = deskText(13, color = if (l.optBoolean("has_color")) c.text else c.textDisabled))
            }
            if (l.optBoolean("color")) {
                val o = st.optJSONObject("options") ?: JSONObject()
                Row(verticalAlignment = Alignment.CenterVertically) {
                    DeskCheck(o.optBoolean("mix")) { call("texdisp.options", arrayOf("mix" to it)) }
                    Text(tr("Mix filaments"), style = deskText(13))
                }
                if (o.optBoolean("mix")) {
                    Labeled(tr("Mixed colors")) {
                        ValueSlider(o.optInt("mix_count").toFloat(), 1f, st.optInt("max_mixes", 1).toFloat().coerceAtLeast(2f), "%.0f") {
                            call("texdisp.options", arrayOf("mix_count" to Math.round(it)))
                        }
                    }
                    Text(st.optString("palette_text"), style = deskText(12, color = c.textDim))
                }
                Labeled(tr("Denoise")) { ValueSlider(o.optInt("despeckle").toFloat(), 0f, 6f, "%.0f") { call("texdisp.options", arrayOf("despeckle" to Math.round(it))) } }
            }
            // Mapping: how the texture is laid onto the painted area
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(3.dp)) {
                Text(trc("Mapping", "Texture Displacement"), Modifier.width(64.dp), style = deskText(13, color = c.textDim))
                for ((key, icon) in listOf("triplanar" to "menu_obj_cube", "cylindrical" to "menu_obj_cylinder", "spherical" to "menu_obj_sphere",
                    "lscm" to "texture_displacement_map_unwrap", "view" to "texture_displacement_map_view", "model_uv" to "texture_displacement_map_model_uv"))
                    IconToggle(icon, l.optString("projection") == key, 32.dp, enabled = key != "model_uv" || st.optBoolean("has_uv")) { set("projection", key) }
            }
            if (l.optString("projection") == "lscm") Row(verticalAlignment = Alignment.CenterVertically) {
                PanelButton(tr("Open UV editor")) { call("texdisp.uv", arrayOf("open" to true)) }
                Text(st.optString("unwrap_text"), Modifier.padding(start = 8.dp),
                    style = deskText(12, color = desk().textDim))
            }
            if (l.optString("projection") == "view") {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    PanelButton(tr("Capture current view")) { call("texdisp.capture_view", arrayOf()) }
                }
                Row(verticalAlignment = Alignment.CenterVertically) {
                    DeskCheck(st.optBoolean("only_visible")) { call("texdisp.capture_view", arrayOf("only_visible" to it)) }
                    Text(tr("Project only on visible"), style = deskText(13))
                }
                // the projection frame: a window dragged over the model; what shows through it gets the texture
                val proj = st.optJSONObject("projector") ?: JSONObject()
                Row(verticalAlignment = Alignment.CenterVertically) {
                    DeskCheck(proj.optBoolean("open")) { call("texdisp.projector", arrayOf("open" to it)) }
                    Text(tr("Projection frame"), style = deskText(13))
                }
                if (proj.optBoolean("open")) {
                    Labeled(tr("Opacity")) { ValueSlider(proj.optInt("opacity", 140).toFloat(), 20f, 255f, "%.0f") { call("texdisp.projector", arrayOf("opacity" to Math.round(it))) } }
                    PanelButton(tr("Apply projection frame")) { call("texdisp.projector", arrayOf("action" to "apply")) }
                }
                if (l.optBoolean("projective")) Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(tr("Placed by projection frame."), Modifier.weight(1f), style = deskText(12, color = c.textDim))
                    PanelButton(tr("Clear")) { call("texdisp.projector", arrayOf("action" to "clear")) }
                }
            }
            Row(verticalAlignment = Alignment.CenterVertically) {
                DeskCheck(l.optBoolean("tile")) { set("tile", it) }
                Text(tr("Tile"), Modifier.weight(1f), style = deskText(13))
                IconToggle("texture_displacement_tile_repeat", l.optInt("tile_method") == 0, 30.dp, enabled = l.optBoolean("tile")) { set("tile_method", 0) }
                IconToggle("menu_mirror_x", l.optInt("tile_method") == 1, 30.dp, enabled = l.optBoolean("tile")) { set("tile_method", 1) }
            }
            // Blend: what a layer does over the ones before it (the base layer always adds); Adjust placement beside it
            Row(verticalAlignment = Alignment.CenterVertically) { Box(Modifier.weight(1f)) { Labeled(tr("Blend")) {
                var open by remember { mutableStateOf(false) }
                val names = listOf(trc("Add", "Texture Displacement"), trc("Subtract", "Texture Displacement"), trc("Multiply", "Texture Displacement"),
                    tr("Divide"), trc("Override", "Texture Displacement"))
                val values = listOf(0, 1, 2, 3, 4)
                Box {
                    DeskCombo(names.getOrElse(values.indexOf(l.optInt("blend"))) { names[0] }, Modifier.fillMaxWidth(), enabled = index > 0, height = 32.dp) { open = true }
                    DropdownMenu(open, { open = false }) {
                        for ((n, v) in names.zip(values)) DropdownMenuItem(text = { Text(n, style = deskText(13)) }, onClick = { open = false; set("blend", v) })
                    }
                }
            } }
                IconToggle("texture_displacement_adjust", st.optBoolean("adjust_placement"), 32.dp) {
                    call("texdisp.adjust_placement", arrayOf("on" to !st.optBoolean("adjust_placement")))
                }
            }
        }
        Text(if (expanded) tr("Fewer settings") else tr("More settings"), Modifier.align(Alignment.CenterHorizontally)
            .clickable { set("expanded", !expanded) }.padding(4.dp), style = deskText(12, color = OrcaColors.accent))
    }
}

/** a layer that is not being edited: its image, name and depth (or "not painted"); a tap makes it the active one */
@Composable
private fun LayerRow(l: JSONObject, onClick: () -> Unit) {
    val c = desk()
    Row(Modifier.fillMaxWidth().height(40.dp).clip(RoundedCornerShape(6.dp)).border(1.dp, c.border, RoundedCornerShape(6.dp)).clickable(onClick = onClick)
        .padding(horizontal = 8.dp), verticalAlignment = Alignment.CenterVertically) {
        Thumb(l.optString("image"), 28.dp)
        Text(l.optString("name"), Modifier.weight(1f).padding(horizontal = 8.dp), style = deskText(13), maxLines = 1, overflow = TextOverflow.Ellipsis)
        Text(if (l.optBoolean("painted")) String.format(Locale.ROOT, "%.3f mm", l.optDouble("depth")) else tr("not painted"), style = deskText(12, color = c.textDim))
    }
}

/** Pro mode's Result: Displace up to the border, Smooth result (Strength, Passes, Ignore outer ring), the pipeline and Budget */
@Composable
private fun ProResult(st: JSONObject, call: (String, Array<out Pair<String, Any>>) -> Unit) {
    val c = desk()
    val o = st.optJSONObject("options") ?: return
    fun set(k: String, v: Any) = call("texdisp.options", arrayOf(k to v))
    HorizontalDivider(color = c.border)
    Text(tr("Result"), style = deskText(14, FontWeight.Bold))
    Row(verticalAlignment = Alignment.CenterVertically) { DeskCheck(o.optBoolean("displace_border")) { set("displace_border", it) }; Text(tr("Displace up to the border"), style = deskText(13)) }
    Row(verticalAlignment = Alignment.CenterVertically) { DeskCheck(o.optBoolean("smooth")) { set("smooth", it) }; Text(tr("Smooth result"), style = deskText(13)) }
    if (o.optBoolean("smooth")) {
        Labeled(trc("Strength", "Texture Displacement")) {
            ValueSlider((o.optDouble("smooth_strength") * 100).toFloat(), 1f, 100f, "%.0f %%") { set("smooth_strength", (it / 100f).coerceIn(0.01f, 1f)) }
        }
        Labeled(tr("Passes")) { ValueSlider(o.optInt("smooth_iterations").toFloat(), 1f, 10f, "%.0f") { set("smooth_iterations", Math.round(it)) } }
        Row(verticalAlignment = Alignment.CenterVertically) { DeskCheck(o.optBoolean("smooth_skip_border")) { set("smooth_skip_border", it) }; Text(tr("Ignore outer ring"), style = deskText(13)) }
        PanelButton(tr("Smooth baked mesh now")) { call("texdisp.prepare", arrayOf("action" to "smooth")) }
    }
    Row(verticalAlignment = Alignment.CenterVertically) { DeskCheck(o.optBoolean("classic")) { set("classic", it) }; Text(tr("Experimental: classic bake pipeline"), style = deskText(13)) }
    if (!o.optBoolean("classic")) Labeled(tr("Budget")) {
        val k = o.optInt("budget_k", -1)
        ValueSlider(if (k < 0) 750f else k.toFloat(), 0f, 4000f, if (k < 0) "%.0f k (auto)" else "%.0f k") { set("budget_k", Math.round(it)) }
    }
}

/** Pro mode's mesh preparation: Subdivision (uniform steps, or adaptive over the painted area) with its wireframe preview, Remeshing */
@Composable
private fun ProPrepare(st: JSONObject, busy: Boolean, call: (String, Array<out Pair<String, Any>>) -> Unit) {
    val c = desk()
    val p = st.optJSONObject("prep") ?: return
    fun set(k: String, v: Any) = call("texdisp.prepare", arrayOf(k to v))
    HorizontalDivider(color = c.border)
    Text(tr("Subdivision"), style = deskText(14, FontWeight.Bold))
    Row(verticalAlignment = Alignment.CenterVertically) { DeskCheck(p.optBoolean("adaptive")) { set("adaptive", it) }; Text(tr("Only painted area (adaptive)"), style = deskText(13)) }
    val adaptive = p.optBoolean("adaptive")
    if (adaptive) {
        val feature = p.optBoolean("feature")
        Row(verticalAlignment = Alignment.CenterVertically) { DeskCheck(feature) { set("feature", it) }; Text(tr("Follow texture detail"), style = deskText(13)) }
        Labeled(if (feature) tr("Max edge") else tr("Target edge")) { ValueSlider(p.optDouble("target").toFloat(), 0.001f, 20f, "%.3f mm", log = true) { set("target", it) } }
        if (feature) {
            Labeled(tr("Detail")) { ValueSlider(p.optDouble("detail").toFloat(), 0.001f, 1f, "%.3f mm", log = true) { set("detail", it) } }
            Labeled(tr("Min edge")) { ValueSlider(p.optDouble("min_edge").toFloat(), 0.001f, 20f, "%.3f mm", log = true) { set("min_edge", it) } }
        }
        Labeled(tr("Edge detail")) { ValueSlider(p.optDouble("border").toFloat(), 0f, 5f, "%.3f mm") { set("border", it) } }
        if (p.optBoolean("any_colors")) Labeled(tr("Color detail")) { ValueSlider(p.optDouble("color").toFloat(), 0f, 5f, "%.3f mm") { set("color", it) } }
        Text(tr("Triangle budget: set with Triangles, below."), style = deskText(12, color = c.textDim))
    } else Labeled(tr("Steps")) { ValueSlider(p.optInt("steps").toFloat(), 0f, 5f, "%.0f") { set("steps", Math.round(it)) } }
    if (p.optBoolean("editing") && p.optInt("preview_tris") > 0)
        Text(st.optString("preview_tris_text"), style = deskText(12))
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        PanelButton(if (p.optBoolean("editing")) tr("Hide preview") else tr("Preview subdivision"), enabled = !busy) { set("action", "preview") }
        PanelButton(tr("Subdivide"), enabled = !busy && (if (adaptive) p.optDouble("target") > 0 else p.optInt("steps") >= 1)) { set("action", "subdivide") }
    }
    HorizontalDivider(color = c.border)
    Text(tr("Remeshing"), style = deskText(14, FontWeight.Bold))
    Labeled(tr("Target edge")) { ValueSlider(p.optDouble("remesh_edge").toFloat(), 0.1f, 20f, "%.2f mm", log = true) { set("remesh_edge", it) } }
    Row(verticalAlignment = Alignment.CenterVertically) {
        DeskCheck(p.optBoolean("keep_sharp")) { set("keep_sharp", it) }
        Text(tr("Keep sharp edges"), Modifier.width(120.dp), style = deskText(13))
        ValueSlider(p.optDouble("sharp_angle").toFloat(), 5f, 90f, "%.0f°", Modifier.weight(1f), enabled = p.optBoolean("keep_sharp")) { set("sharp_angle", it) }
    }
    PanelButton(tr("Remesh"), enabled = !busy) { set("action", "remesh") }
}

/**
 * TextureProjectorFrame: a frame over the 3D view showing the layer's image at the chosen opacity; drag it to move, drag
 * its corner to resize. Its rectangle (the view's pixels) goes to the engine after each move, for Apply projection frame.
 */
@Composable
private fun ProjectorFrame(app: AppState, image: String, opacity: Int, call: (String, Array<out Pair<String, Any>>) -> Unit) {
    val c = desk()
    var parent by remember { mutableStateOf(androidx.compose.ui.geometry.Offset.Zero) }
    // centred on the view at half its size, like show_projector
    var rect by remember {
        mutableStateOf(androidx.compose.ui.geometry.Rect(app.glOrigin.x + app.glSize.width / 4f, app.glOrigin.y + app.glSize.height / 4f,
            app.glOrigin.x + app.glSize.width * 0.75f, app.glOrigin.y + app.glSize.height * 0.75f))
    }
    fun send() = call("texdisp.projector", arrayOf("rect" to org.json.JSONArray(listOf((rect.left - app.glOrigin.x).toDouble(),
        (rect.top - app.glOrigin.y).toDouble(), rect.width.toDouble(), rect.height.toDouble()))))
    LaunchedEffect(Unit) { send() }
    val bmp = remember(image) { if (image.isEmpty()) null else runCatching { BitmapFactory.decodeFile(image)?.asImageBitmap() }.getOrNull() }
    Box(Modifier.size(0.dp).onGloballyPositioned { parent = it.positionInWindow() })
    val d = androidx.compose.ui.platform.LocalDensity.current
    androidx.compose.ui.window.Popup(offset = androidx.compose.ui.unit.IntOffset((rect.left - parent.x).roundToInt(), (rect.top - parent.y).roundToInt())) {
        Box(Modifier.size(with(d) { rect.width.toDp() }, with(d) { rect.height.toDp() }).border(2.dp, OrcaColors.accent)
            .pointerInput(Unit) { detectDragGestures(onDragEnd = { send() }) { ch, dd -> ch.consume(); rect = rect.translate(dd) } }) {
            if (bmp != null) Image(bmp, null, Modifier.fillMaxSize(), contentScale = ContentScale.FillBounds, alpha = opacity / 255f)
            Box(Modifier.align(Alignment.BottomEnd).size(28.dp).background(OrcaColors.accent.copy(alpha = 0.8f))
                .pointerInput(Unit) {
                    detectDragGestures(onDragEnd = { send() }) { ch, dd ->
                        ch.consume()
                        rect = androidx.compose.ui.geometry.Rect(rect.left, rect.top, maxOf(rect.left + 80f, rect.right + dd.x), maxOf(rect.top + 80f, rect.bottom + dd.y))
                    }
                })
        }
    }
}

/** render_image_adjust_window: the adjusted image, Brightness / Contrast / Sharpness / Blur / Hue, Reset, Cancel, Save as copy */
@Composable
private fun AdjustImageWindow(adj: JSONObject, call: (String, Array<out Pair<String, Any>>) -> Unit) {
    val c = desk()
    fun set(k: String, v: Any) = call("texdisp.adjust", arrayOf(k to v))
    Dialog({ call("texdisp.adjust", arrayOf("action" to "cancel")) }, DialogProperties(usePlatformDefaultWidth = false, dismissOnClickOutside = false)) {
        Column(Modifier.width(380.dp).clip(RoundedCornerShape(8.dp)).background(c.panel).padding(14.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(tr("Adjust image"), Modifier.weight(1f), style = deskText(15, FontWeight.Bold))
                Text("✕", Modifier.clickable { call("texdisp.adjust", arrayOf("action" to "cancel")) }.padding(6.dp), style = deskText(15, color = c.textDim))
            }
            val path = adj.optString("image")
            val bmp = remember(path) { if (path.isEmpty()) null else runCatching { BitmapFactory.decodeFile(path)?.asImageBitmap() }.getOrNull() }
            val aspect = adj.optDouble("aspect", 1.0).toFloat().coerceAtLeast(0.05f)
            if (bmp != null) Image(bmp, null, Modifier.fillMaxWidth().aspectRatio(aspect), contentScale = ContentScale.Fit)
            else Spacer(Modifier.fillMaxWidth().aspectRatio(aspect))
            Labeled(tr("Brightness")) { ValueSlider(adj.optDouble("brightness").toFloat(), -1f, 1f, "%.2f") { set("brightness", it) } }
            Labeled(tr("Contrast")) { ValueSlider(adj.optDouble("contrast").toFloat(), -1f, 1f, "%.2f") { set("contrast", it) } }
            Labeled(tr("Sharpness")) { ValueSlider(adj.optDouble("sharpness").toFloat(), 0f, 1f, "%.2f") { set("sharpness", it) } }
            Labeled(tr("Blur")) { ValueSlider(adj.optDouble("blur").toFloat(), 0f, 1f, "%.2f") { set("blur", it) } }
            Labeled(tr("Hue")) { ValueSlider(adj.optDouble("hue").toFloat(), -180f, 180f, "%.0f°", enabled = adj.optBoolean("has_color")) { set("hue", it) } }
            if (!adj.optBoolean("has_color")) Text(tr("This texture is grayscale, so it has no hue to shift."), style = deskText(12, color = c.textDim))
            PanelButton(tr("Reset")) { set("action", "reset") }
            HorizontalDivider(color = c.border)
            Text(tr("The model updates when you release a slider. Save as copy writes the adjusted image to your texture library and points this layer at it."),
                style = deskText(12, color = c.textDim))
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                PanelButton(tr("Cancel")) { call("texdisp.adjust", arrayOf("action" to "cancel")) }
                Spacer(Modifier.width(8.dp))
                PanelButton(tr("Save as copy"), primary = true, enabled = !adj.optBoolean("identity")) { call("texdisp.adjust", arrayOf("action" to "save")) }
            }
        }
    }
}

/** render_texture_library_popup: Built-in, the texture libraries of plugins, My textures with the import tile */
@Composable
private fun TexturePicker(st: JSONObject, layers: JSONArray, slot: Int, onDismiss: () -> Unit, onPick: (String) -> Unit, onImport: () -> Unit,
                          onRemove: (String) -> Unit, onExplore: () -> Unit) {
    val c = desk()
    val lib = st.optJSONArray("library") ?: JSONArray()
    val entries = (0 until lib.length()).map { lib.getJSONObject(it) }
    val current = (0 until layers.length()).map { layers.getJSONObject(it) }.firstOrNull { it.optInt("slot") == slot }?.optString("image")
    Dialog(onDismiss, DialogProperties(usePlatformDefaultWidth = false)) {
        Column(Modifier.width(560.dp).fillMaxHeight(0.85f).clip(RoundedCornerShape(8.dp)).background(c.panel).padding(14.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(tr("Choose a texture"), Modifier.weight(1f), style = deskText(15, FontWeight.Bold))
                Text("✕", Modifier.clickable(onClick = onDismiss).padding(6.dp), style = deskText(15, color = c.textDim))
            }
            Column(Modifier.weight(1f).verticalScroll(rememberScrollState())) {
                @Composable fun section(title: String, items: List<JSONObject>, user: Boolean) {
                    Text(title, Modifier.padding(top = 10.dp, bottom = 6.dp), style = deskText(13, FontWeight.Bold))
                    if (items.isEmpty() && !user) Text(tr("No textures found."), style = deskText(12, color = c.textDim))
                    val tiles = items.map { it as JSONObject? } + if (user) listOf(null) else emptyList()
                    for (row in tiles.chunked(6)) Row(horizontalArrangement = Arrangement.spacedBy(6.dp), modifier = Modifier.padding(bottom = 6.dp)) {
                        for (e in row) {
                            if (e == null) Box(Modifier.size(80.dp).clip(RoundedCornerShape(4.dp)).border(1.dp, c.border, RoundedCornerShape(4.dp))
                                .clickable(onClick = onImport), Alignment.Center) { Text("+", style = deskText(24, color = c.textDim)) }
                            else {
                                var menu by remember { mutableStateOf(false) }
                                Box {
                                    Column(Modifier.width(80.dp).clickable { onPick(e.optString("path")) }, horizontalAlignment = Alignment.CenterHorizontally) {
                                        Thumb(e.optString("path"), 80.dp, Modifier.border(if (e.optString("path") == current) 2.dp else 1.dp,
                                            if (e.optString("path") == current) OrcaColors.accent else c.border, RoundedCornerShape(3.dp)))
                                        Text(e.optString("name"), style = deskText(10), maxLines = 1, overflow = TextOverflow.Ellipsis)
                                    }
                                    if (user) Text("✕", Modifier.align(Alignment.TopEnd).clip(RoundedCornerShape(9.dp)).background(c.panel.copy(alpha = 0.8f))
                                        .clickable { onRemove(e.optString("path")) }.padding(horizontal = 5.dp), style = deskText(12, color = c.textDim))
                                }
                            }
                        }
                    }
                }
                section(tr("Built-in"), entries.filter { it.optInt("source") == 0 }, false)
                val packs = entries.filter { it.optInt("source") == 2 }.groupBy { it.optString("pack") }
                Text(tr("Texture libraries"), Modifier.padding(top = 10.dp), style = deskText(13, FontWeight.Bold))
                for ((pack, items) in packs) section(pack, items, false)
                PanelButton(tr("Explore more texture libraries")) { onExplore() }
                section(tr("My textures"), entries.filter { it.optInt("source") == 1 }, true)
                Text(tr("PNG, JPG or BMP - converted to a height map on import. Hover one of yours to remove it."), style = deskText(11, color = c.textDim))
            }
        }
    }
}

private val thumbCache = HashMap<String, ImageBitmap?>()

/** a texture's image, decoded at a thumbnail size (THUMBNAIL_MAX_PX) */
@Composable
private fun Thumb(path: String, size: Dp, modifier: Modifier = Modifier) {
    val bmp = remember(path) {
        if (path.isEmpty()) null else thumbCache.getOrPut(path) {
            runCatching {
                val o = BitmapFactory.Options().apply { inJustDecodeBounds = true }
                BitmapFactory.decodeFile(path, o)
                var s = 1
                while (o.outWidth / (s * 2) >= 128 && o.outHeight / (s * 2) >= 128) s *= 2
                BitmapFactory.decodeFile(path, BitmapFactory.Options().apply { inSampleSize = s })?.asImageBitmap()
            }.getOrNull()
        }
    }
    if (bmp != null) Image(bmp, null, modifier.size(size).clip(RoundedCornerShape(3.dp)), contentScale = ContentScale.Crop)
    else Box(modifier.size(size).clip(RoundedCornerShape(3.dp)).background(desk().field), Alignment.Center) { Text("?", style = deskText(13)) }
}

@Composable
private fun IconToggle(icon: String, selected: Boolean, size: Dp, enabled: Boolean = true, onClick: () -> Unit) {
    val c = desk()
    Box(Modifier.size(size).clip(RoundedCornerShape(4.dp)).background(if (selected) c.selection else Color.Transparent)
        .border(1.dp, if (selected) OrcaColors.accent else Color.Transparent, RoundedCornerShape(4.dp)).clickable(enabled = enabled, onClick = onClick),
        Alignment.Center) { OrcaIcon(icon, size * 0.62f, alpha = if (enabled) 1f else 0.35f) }
}

@Composable
private fun Labeled(label: String, content: @Composable () -> Unit) = Row(verticalAlignment = Alignment.CenterVertically) {
    Text(label, Modifier.width(80.dp), style = deskText(13, color = desk().textDim))
    Box(Modifier.weight(1f)) { content() }
}

/** a slider sending its value when the finger lifts; `log` for the ranges ImGui shows logarithmically (ImGuiLogSlider) */
@Composable
private fun ValueSlider(value: Float, min: Float, max: Float, format: String, modifier: Modifier = Modifier, log: Boolean = false, enabled: Boolean = true,
                        onChange: (Float) -> Unit) {
    fun toPos(v: Float) = if (log) ((Math.log(v.toDouble()) - Math.log(min.toDouble())) / (Math.log(max.toDouble()) - Math.log(min.toDouble()))).toFloat() else (v - min) / (max - min)
    fun fromPos(p: Float) = if (log) Math.exp(Math.log(min.toDouble()) + p * (Math.log(max.toDouble()) - Math.log(min.toDouble()))).toFloat() else min + p * (max - min)
    var pos by remember(value) { mutableFloatStateOf(toPos(value.coerceIn(min, max))) }
    Row(modifier, verticalAlignment = Alignment.CenterVertically) {
        Slider(pos, { pos = it }, Modifier.weight(1f).height(30.dp), enabled = enabled, onValueChangeFinished = { onChange(fromPos(pos)) },
            colors = SliderDefaults.colors(thumbColor = OrcaColors.accent, activeTrackColor = OrcaColors.accent))
        Text(String.format(Locale.ROOT, format, fromPos(pos)), Modifier.width(78.dp).padding(start = 6.dp), style = deskText(12))
    }
}
