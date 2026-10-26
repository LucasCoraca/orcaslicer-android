package org.orcaport.mobile.prepare

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
import androidx.compose.ui.unit.dp
import org.json.JSONObject
import org.orcaport.mobile.AppState
import org.orcaport.mobile.engine.jobj
import org.orcaport.mobile.settings.parseColor
import org.orcaport.mobile.ui.*

/**
 * The painting gizmos' windows (GLGizmoFdmSupports / GLGizmoSeam / GLGizmoMmuSegmentation / GLGizmoFuzzySkin
 * on_render_input_window): tool type, brush size or fill angle, the filaments (colour painting), overhang
 * highlighting (supports), vertical / horizontal strokes and Erase all. On a touch screen the mouse buttons and Shift
 * become a choice of what a stroke does.
 */
@Composable
fun PaintPanel(app: AppState, gizmo: String, modifier: Modifier) {
    val kind = when (gizmo) { "Support painting" -> "support"; "Seam painting" -> "seam"; "Color painting" -> "mmu"; else -> "fuzzy" }
    var st by remember(kind) { mutableStateOf(JSONObject()) }
    LaunchedEffect(kind, app.project.optJSONArray("selection")?.toString()) {
        app.call("paint.state") { if (it is JSONObject) st = it }
    }
    fun set(k: String, v: Any) = app.call("paint.set", jobj(k to v)) { if (it is JSONObject) { st = it; app.redraw() } }
    val c = desk()
    // the tools each gizmo offers (tool_ids), with Orca's icons
    val tools = when (kind) {
        "support" -> listOf("circle" to "circle_paint", "sphere" to "toolbar_modifier_sphere", "fill" to "fill_paint")
        "seam" -> listOf("circle" to "circle_paint", "sphere" to "toolbar_modifier_sphere")
        "mmu" -> listOf("circle" to "circle_paint", "sphere" to "toolbar_modifier_sphere", "pointer" to "triangle_paint", "height_range" to "height_range", "fill" to "fill_paint", "gap_fill" to "gap_fill")
        else -> listOf("circle" to "circle_paint", "sphere" to "toolbar_modifier_sphere", "pointer" to "triangle_paint", "smart_fill" to "fill_paint")
    }
    val tool = st.optString("tool", "circle")
    Column(modifier.widthIn(max = 380.dp).shadow(4.dp, RoundedCornerShape(8.dp)).clip(RoundedCornerShape(8.dp)).background(c.panel)
        .verticalScroll(rememberScrollState()).padding(horizontal = 12.dp, vertical = 10.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        if (st.optInt("object", -1) < 0) { Text(tr("Select an object first."), style = deskText(13, color = c.textDim)); return@Column }
        // what a stroke does: the left button / the right button / Shift on the desktop
        val modes = when (kind) {
            "support" -> listOf("Enforce supports", "Block supports", "Erase")
            "seam" -> listOf("Enforce seam", "Block seam", "Erase")
            "fuzzy" -> listOf("Add fuzzy skin", "Remove fuzzy skin")
            else -> listOf("Paint", "Erase")
        }
        Row(verticalAlignment = Alignment.CenterVertically) {
            for ((i, l) in modes.withIndex()) {
                val m = if (kind == "mmu" && i == 1) 2 else i
                val sel = st.optInt("mode") == m
                Box(Modifier.weight(1f).height(34.dp).clip(RoundedCornerShape(4.dp)).background(if (sel) c.selection else Color.Transparent)
                    .border(1.dp, if (sel) OrcaColors.accent else c.border, RoundedCornerShape(4.dp)).clickable { set("mode", m) }, Alignment.Center) {
                    Text(l, style = deskText(12, color = if (sel) OrcaColors.accent else c.text), maxLines = 1)
                }
                if (i < modes.size - 1) Spacer(Modifier.width(4.dp))
            }
        }
        if (kind == "mmu") {
            // the filament buttons (render_extruders_list / "Filaments")
            Text(tr("Filaments"), style = deskText(13))
            val colors = st.optJSONArray("colors")
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                for (i in 0 until (colors?.length() ?: 0)) {
                    val col = parseColor(colors!!.getString(i)) ?: Color.White
                    val sel = st.optInt("extruder") == i
                    val lum = 0.299 * col.red + 0.587 * col.green + 0.114 * col.blue
                    Box(Modifier.size(34.dp).clip(RoundedCornerShape(4.dp)).background(col)
                        .border(if (sel) 3.dp else 1.dp, if (sel) OrcaColors.accent else c.border, RoundedCornerShape(4.dp)).clickable { set("extruder", i) }, Alignment.Center) {
                        Text("${i + 1}", style = deskText(13, color = if (lum > 0.6) Color.Black else Color.White))
                    }
                }
            }
        }
        Text(if (kind == "mmu") "Tool type" else "Tool type", style = deskText(13))
        Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            for ((t, icon) in tools) {
                val sel = tool == t
                Box(Modifier.size(40.dp).clip(RoundedCornerShape(4.dp)).background(if (sel) c.selection else Color.Transparent)
                    .border(1.dp, if (sel) OrcaColors.accent else c.border, RoundedCornerShape(4.dp)).clickable { set("tool", t) }, Alignment.Center) {
                    OrcaIcon(icon, 24.dp)
                }
            }
        }
        @Composable fun slider(label: String, key: String, min: Float, max: Float, unit: String) = Column {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(label, Modifier.weight(1f), style = deskText(13))
                Text(String.format(java.util.Locale.ROOT, "%.1f", st.optDouble(key)) + unit, style = deskText(13, color = c.textDim))
            }
            Slider(st.optDouble(key).toFloat().coerceIn(min, max), { set(key, it) }, valueRange = min..max,
                colors = SliderDefaults.colors(thumbColor = OrcaColors.accent, activeTrackColor = OrcaColors.accent))
        }
        when (tool) {
            "circle", "sphere" -> slider("Brush size", "radius", 0.4f, 8f, "")
            "fill", "smart_fill" -> slider("Smart fill angle", "smart_fill_angle", 0f, 90f, "°")
            "height_range" -> slider("Height range", "height", 0.1f, 8f, "mm")
            "gap_fill" -> {   // GLGizmoMmuSegmentation: patches smaller than the area take their neighbour's colour on Apply
                slider("Gap area", "gap_area", 0f, 5f, "")
                PanelButton(tr("Apply")) { app.call("paint.gap_fill") { if (it is JSONObject) { st = it; app.redraw() } } }
            }
        }
        if (tool == "circle" || tool == "sphere") Row(verticalAlignment = Alignment.CenterVertically) {
            DeskCheck(st.optBoolean("vertical_only")) { set("vertical_only", it) }; Text(tr("Vertical"), style = deskText(13))
            Spacer(Modifier.width(12.dp))
            DeskCheck(st.optBoolean("horizontal_only")) { set("horizontal_only", it) }; Text(tr("Horizontal"), style = deskText(13))
        }
        if (kind == "support") {
            slider("Highlight overhangs", "highlight_angle", 0f, 90f, "°")
            Row(verticalAlignment = Alignment.CenterVertically) {
                DeskCheck(st.optBoolean("overhangs_only"), enabled = st.optDouble("highlight_angle") > 0) { set("overhangs_only", it) }
                Text(tr("On highlighted overhangs only"), style = deskText(13))
            }
        }
        HorizontalDivider(color = c.border)
        Row(verticalAlignment = Alignment.CenterVertically) {
            PanelButton(tr("Erase all")) { app.call("paint.erase_all") { if (it is JSONObject) { st = it; app.redraw() } } }
            Spacer(Modifier.weight(1f))
            PanelButton(tr("Done"), primary = true) { app.gizmo = null }
        }
    }
}
