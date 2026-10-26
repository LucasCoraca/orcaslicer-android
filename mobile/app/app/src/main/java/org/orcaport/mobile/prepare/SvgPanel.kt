package org.orcaport.mobile.prepare

import androidx.compose.foundation.Canvas
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
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.PathFillType
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import org.orcaport.mobile.AppState
import org.orcaport.mobile.engine.jobj
import org.orcaport.mobile.ui.*

/**
 * GLGizmoSVG::draw_window: the shape preview, the file name with reload and the file menu (change file, forget the path,
 * bake, save as), depth, size with the ratio lock and reset, use surface, distance from the surface, rotation with keep
 * up, mirror, face the camera, the operation and Done. A drag that starts on the shape moves it over the surface.
 */
@Composable
fun SvgPanel(app: AppState, modifier: Modifier) {
    var st by remember { mutableStateOf(JSONObject()) }
    LaunchedEffect(app.gizmoRevision, app.project) { app.call("svg.state") { if (it is JSONObject) st = it } }
    if (!st.optBoolean("active")) return
    fun apply(r: Any?) {
        if (r !is JSONObject) return
        st = r
        if (r.optBoolean("close")) app.gizmo = null
        app.redraw()
        app.call("project.state") { p -> if (p is JSONObject) app.project = p }
    }
    fun set(vararg kv: Pair<String, Any?>) = app.call("svg.set", jobj(*kv)) { apply(it) }
    fun file(what: String, vararg kv: Pair<String, Any?>) = app.call("svg.file", jobj("what" to what, *kv)) { apply(it) }
    val c = desk()
    val l = st.optJSONObject("labels") ?: JSONObject()
    val sliderColors = SliderDefaults.colors(thumbColor = OrcaColors.accent, activeTrackColor = OrcaColors.accent)
    val labelW = 110.dp
    val units = st.optString("units", "mm")
    var fileMenu by remember { mutableStateOf(false) }
    fun fmt(v: Double, d: Int = 2) = String.format(java.util.Locale.ROOT, "%.${d}f", v)
    @Composable fun labelled(label: String, modified: Boolean = false, content: @Composable RowScope.() -> Unit) = Row(verticalAlignment = Alignment.CenterVertically) {
        Text(label, Modifier.width(labelW), style = deskText(13, color = if (modified) OrcaColors.accent else c.text))
        content()
    }
    @Composable fun iconButton(icon: String, enabled: Boolean = true, on: Boolean = false, onClick: () -> Unit) =
        Box(Modifier.padding(start = 4.dp).size(32.dp).clip(RoundedCornerShape(4.dp)).background(if (on) c.selection else Color.Transparent)
            .clickable(enabled = enabled, onClick = onClick), Alignment.Center) { OrcaIcon(icon, 20.dp, alpha = if (enabled) 1f else 0.35f) }

    Column(modifier.widthIn(max = 420.dp).heightIn(max = 640.dp).shadow(4.dp, RoundedCornerShape(8.dp)).clip(RoundedCornerShape(8.dp))
        .background(c.panel).verticalScroll(rememberScrollState()).padding(horizontal = 12.dp, vertical = 10.dp),
        verticalArrangement = Arrangement.spacedBy(6.dp)) {
        // draw_preview
        val preview = st.optJSONArray("preview") ?: JSONArray()
        val ratio = st.optDouble("preview_ratio", 1.0).toFloat().coerceIn(0.05f, 20f)
        Box(Modifier.fillMaxWidth().height(120.dp), Alignment.Center) {
            val fill = c.text
            Canvas(Modifier.fillMaxHeight().aspectRatio(1f / ratio, matchHeightConstraintsFirst = ratio >= 1f)) {
                val s = maxOf(size.width, size.height)
                val path = Path().apply { fillType = PathFillType.EvenOdd }
                for (i in 0 until preview.length()) {
                    val pts = preview.getJSONArray(i)
                    if (pts.length() < 6) continue
                    path.moveTo(pts.getDouble(0).toFloat() * s, pts.getDouble(1).toFloat() * s)
                    var k = 2
                    while (k + 1 < pts.length()) { path.lineTo(pts.getDouble(k).toFloat() * s, pts.getDouble(k + 1).toFloat() * s); k += 2 }
                    path.close()
                }
                drawPath(path, fill)
            }
        }
        // draw_filename: the name, warnings, reload and the file menu
        Row(verticalAlignment = Alignment.CenterVertically) {
            if ((st.optJSONArray("warnings")?.length() ?: 0) > 0) OrcaIcon("obj_warning", 18.dp)
            Text(st.optString("name"), Modifier.weight(1f, fill = false).padding(start = 4.dp), style = deskText(14), maxLines = 1)
            Text(".svg", style = deskText(14, color = c.textDim))
            Spacer(Modifier.weight(1f))
            if (st.optBoolean("can_reload")) iconButton("refresh") { file("reload") }
            Box {
                iconButton("open") { fileMenu = true }
                DropdownMenu(fileMenu, { fileMenu = false }) {
                    DropdownMenuItem(text = { Text(l.optString("change_file")) }, leadingIcon = { OrcaIcon("open", 18.dp) }, onClick = {
                        fileMenu = false
                        val ctx = app.appContext ?: return@DropdownMenuItem
                        app.openDocument(arrayOf("image/svg+xml", "*/*")) { u -> app.scope.launchCatching { file("change", "path" to app.copyToCache(ctx, u)) } }
                    })
                    DropdownMenuItem(text = { Text(l.optString("forget")) }, leadingIcon = { OrcaIcon("burn", 18.dp) }, enabled = st.optBoolean("can_forget"),
                        onClick = { fileMenu = false; file("forget") })
                    DropdownMenuItem(text = { Text(l.optString("bake")) }, leadingIcon = { OrcaIcon("burn", 18.dp) }, onClick = { fileMenu = false; file("bake") })
                    DropdownMenuItem(text = { Text(l.optString("save_as")) }, leadingIcon = { OrcaIcon("save", 18.dp) }, onClick = {
                        fileMenu = false
                        val ctx = app.appContext ?: return@DropdownMenuItem
                        app.call("svg.file", jobj("what" to "data")) { r ->
                            val d = r as? JSONObject ?: return@call
                            app.saveDocument(d.optString("name", "shape.svg")) { uri ->
                                app.scope.launchCatching {
                                    withContext(Dispatchers.IO) { ctx.contentResolver.openOutputStream(uri, "wt")?.use { it.write(d.optString("data").toByteArray()) } }
                                }
                            }
                        }
                    })
                }
            }
        }
        val warnings = st.optJSONArray("warnings") ?: JSONArray()
        for (i in 0 until warnings.length()) Text(warnings.optString(i), style = deskText(12, color = OrcaColors.warning))
        st.optString("error").takeIf { it.isNotEmpty() }?.let { Text(it, style = deskText(12, color = OrcaColors.warning)) }
        HorizontalDivider(color = c.border)

        labelled(l.optString("depth")) {
            DeskInput(fmt(st.optDouble("depth")), true, true, Modifier.weight(1f), sidetext = units) { s ->
                s.replace(',', '.').toDoubleOrNull()?.let { set("depth" to it) }
            }
        }
        // draw_size: one slider for both with the ratio locked, else width and height
        labelled(l.optString("size"), st.optBoolean("scaled")) {
            if (st.optBoolean("keep_ratio")) {
                val r = st.optJSONArray("size_range")
                var w by remember(st) { mutableStateOf(st.optDouble("width").toFloat()) }
                val range = (r?.optDouble(0) ?: 5.0).toFloat()..(r?.optDouble(1) ?: 100.0).toFloat()
                Column(Modifier.weight(1f)) {
                    Slider(w.coerceIn(range.start, range.endInclusive), { w = it; set("width" to it.toDouble(), "final" to false) },
                        valueRange = range, colors = sliderColors, onValueChangeFinished = { set("width" to w.toDouble(), "final" to true) })
                    Text("${fmt(st.optDouble("width"))} x ${fmt(st.optDouble("height"))} $units", style = deskText(12, color = c.textDim))
                }
            } else {
                DeskInput(fmt(st.optDouble("width")), true, true, Modifier.weight(1f), sidetext = units) { s -> s.replace(',', '.').toDoubleOrNull()?.let { set("width" to it) } }
                Spacer(Modifier.width(4.dp))
                DeskInput(fmt(st.optDouble("height")), true, true, Modifier.weight(1f), sidetext = units) { s -> s.replace(',', '.').toDoubleOrNull()?.let { set("height" to it) } }
            }
            iconButton(if (st.optBoolean("keep_ratio")) "lock_closed" else "lock_open") { set("keep_ratio" to !st.optBoolean("keep_ratio")) }
            if (st.optBoolean("scaled")) iconButton("undo") { set("reset_scale" to true) }
        }
        labelled(l.optString("use_surface")) { DeskCheck(st.optBoolean("use_surface"), st.optBoolean("can_use_surface")) { set("use_surface" to it) } }
        // draw_distance
        labelled(l.optString("distance"), st.optBoolean("distance_set")) {
            val d = st.optJSONArray("distance_range")
            val range = (d?.optDouble(0) ?: -10.0).toFloat()..(d?.optDouble(1) ?: 10.0).toFloat()
            var v by remember(st) { mutableStateOf(st.optDouble("distance").toFloat()) }
            Slider(v.coerceIn(range.start, range.endInclusive), { v = it; set("distance" to it.toDouble(), "final" to false) }, Modifier.weight(1f),
                enabled = st.optBoolean("can_distance"), valueRange = range, colors = sliderColors, onValueChangeFinished = { set("distance" to v.toDouble(), "final" to true) })
            Text("${fmt(v.toDouble())} $units", Modifier.width(72.dp), style = deskText(12))
            if (st.optBoolean("distance_set")) iconButton("undo") { set("reset_distance" to true) }
        }
        // draw_rotation + keep up
        labelled(l.optString("rotation"), st.optBoolean("rotation_set")) {
            var v by remember(st) { mutableStateOf(st.optDouble("rotation").toFloat()) }
            Slider(v.coerceIn(-180f, 180f), { v = it; set("rotation" to it.toDouble(), "final" to false) }, Modifier.weight(1f),
                valueRange = -180f..180f, colors = sliderColors, onValueChangeFinished = { set("rotation" to v.toDouble(), "final" to true) })
            Text("${fmt(v.toDouble())} °", Modifier.width(72.dp), style = deskText(12))
            if (st.optBoolean("rotation_set")) iconButton("undo") { set("reset_rotation" to true) }
            if (st.optBoolean("show_keep_up")) iconButton(if (st.optBoolean("keep_up")) "lock_closed" else "lock_open") { set("keep_up" to !st.optBoolean("keep_up")) }
        }
        // draw_mirroring
        labelled(l.optString("mirror")) {
            iconButton("reflection_x") { set("mirror" to "x") }
            iconButton("reflection_y") { set("mirror" to "y") }
        }
        Row(Modifier.padding(start = labelW)) { PanelButton(l.optString("face_camera")) { set("face_camera" to true) } }
        if (st.optBoolean("show_type")) {
            HorizontalDivider(color = c.border)
            labelled(l.optString("operation")) {
                for ((key, label) in listOf("part" to "join", "negative" to "cut", "modifier" to "modifier")) {
                    val sel = st.optString("type") == key
                    Box(Modifier.padding(end = 6.dp).clip(RoundedCornerShape(4.dp)).background(if (sel) c.accent else c.field)
                        .border(1.dp, c.border, RoundedCornerShape(4.dp)).clickable { set("type" to key) }.padding(horizontal = 10.dp, vertical = 6.dp)) {
                        Text(l.optString(label), style = deskText(13, color = if (sel) Color.White else c.text))
                    }
                }
            }
        }
        Row { Spacer(Modifier.weight(1f)); PanelButton(l.optString("done"), primary = true) { app.gizmo = null } }
    }
}

private fun kotlinx.coroutines.CoroutineScope.launchCatching(block: suspend () -> Unit) = launch { runCatching { block() } }
