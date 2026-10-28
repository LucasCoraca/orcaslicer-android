package org.orcaport.mobile.settings

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.input.pointer.isCtrlPressed
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.foundation.border
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.rotate
import androidx.compose.ui.graphics.luminance
import androidx.compose.ui.text.drawText
import kotlin.math.roundToInt
import kotlinx.coroutines.launch
import org.json.JSONArray
import org.json.JSONObject
import org.orcaport.mobile.AppState
import org.orcaport.mobile.engine.jobj
import org.orcaport.mobile.prepare.PanelButton
import org.orcaport.mobile.ui.*
import kotlin.math.cos
import kotlin.math.sin

/** The desktop's custom setting widgets (create_line_with_widget in Tab.cpp), by option key. */
@Composable
fun CustomEditor(app: AppState, type: String, key: String, st: ConfigState, onSet: (String, Any) -> Unit, onDismiss: () -> Unit) {
    when (key) {
        "printable_area" -> BedShapeDialog(app, st, onSet, onDismiss)
        "compatible_printers", "compatible_prints" -> CompatibleDialog(app, key, st, onSet, onDismiss)
        "filament_ramming_parameters" -> RammingDialog(st.value(key, 0) ?: "", { onSet(key, JSONArray().put(it)) }, onDismiss)
        else -> LaunchedEffect(key) { onDismiss() }   // Tab::create_line_with_widget has no other keys
    }
}

private fun parsePoints(a: JSONArray?): List<Pair<Double, Double>> = a.strings().mapNotNull { p ->
    val xy = p.split("x"); if (xy.size == 2) (xy[0].toDoubleOrNull() ?: return@mapNotNull null) to (xy[1].toDoubleOrNull() ?: return@mapNotNull null) else null
}

/** a document the user picked, kept in the data folder (the config stores its path, like a desktop file name) */
internal fun keepFile(app: AppState, uri: android.net.Uri, dir: String): String? {
    val ctx = app.appContext ?: return null
    val name = app.displayName(ctx, uri)
    val f = java.io.File(java.io.File(app.engine.dataDir, dir).apply { mkdirs() }, name)
    return runCatching { ctx.contentResolver.openInputStream(uri)?.use { i -> f.outputStream().use { o -> i.copyTo(o) } }; f.absolutePath }.getOrNull()
}

/** BedShapeDialog / BedShapePanel: Rectangle (size + origin), Circle (diameter) or Custom (loaded from an STL), with
 *  the preview, and the custom texture (PNG / SVG) and model (STL) of the bed */
@Composable
private fun BedShapeDialog(app: AppState, st: ConfigState, onSet: (String, Any) -> Unit, onDismiss: () -> Unit) {
    val pts = parsePoints(st.values.optJSONArray("printable_area"))
    val minX = pts.minOfOrNull { it.first } ?: 0.0; val maxX = pts.maxOfOrNull { it.first } ?: 200.0
    val minY = pts.minOfOrNull { it.second } ?: 0.0; val maxY = pts.maxOfOrNull { it.second } ?: 200.0
    var s by remember { mutableStateOf(JSONObject()) }
    LaunchedEffect(Unit) { app.call("bedshape.strings") { if (it is JSONObject) s = it } }
    // BedShape::get_page_type: 4 axis-aligned points are a rectangle, points at one distance from the origin a circle
    val initial = remember {
        val rect = pts.size == 4 && pts.indices.all { k -> val a = pts[k]; val b = pts[(k + 1) % 4]; a.first == b.first || a.second == b.second }
        val r = pts.map { Math.hypot(it.first, it.second) }
        val circle = pts.size > 4 && r.isNotEmpty() && r.all { Math.abs(it - r[0]) < 1e-3 * r[0].coerceAtLeast(1.0) }
        if (rect) 0 else if (circle) 1 else 2
    }
    var shape by remember { mutableIntStateOf(initial) }
    var sx by remember { mutableStateOf(fmt(maxX - minX)) }
    var sy by remember { mutableStateOf(fmt(maxY - minY)) }
    var ox by remember { mutableStateOf(fmt(-minX)) }
    var oy by remember { mutableStateOf(fmt(-minY)) }
    var dia by remember { mutableStateOf(fmt(maxX - minX)) }
    var loaded by remember { mutableStateOf(if (initial == 2) pts else emptyList()) }
    var texture by remember { mutableStateOf(st.value("bed_custom_texture", 0) ?: "") }
    var model by remember { mutableStateOf(st.value("bed_custom_model", 0) ?: "") }
    fun outline(): List<Pair<Double, Double>> = when (shape) {
        0 -> {
            val w = sx.toDoubleOrNull() ?: 0.0; val h = sy.toDoubleOrNull() ?: 0.0
            val x0 = -(ox.toDoubleOrNull() ?: 0.0); val y0 = -(oy.toDoubleOrNull() ?: 0.0)
            listOf(x0 to y0, x0 + w to y0, x0 + w to y0 + h, x0 to y0 + h)
        }
        1 -> {   // BedShapePanel::update_shape: a circle as 128 points around the origin
            val r = (dia.toDoubleOrNull() ?: 0.0) / 2
            (0 until 128).map { k -> val a = 2 * Math.PI * k / 128; r * cos(a) to r * sin(a) }
        }
        else -> loaded
    }
    @Composable
    fun FileRow(title: String, value: String, mimes: Array<String>, dir: String, ext: List<String>, onChange: (String) -> Unit) {
        Text(title, style = deskText(14, FontWeight.Bold))
        Row(verticalAlignment = Alignment.CenterVertically) {
            val missing = value.isNotEmpty() && !java.io.File(value).exists()
            Text(if (value.isEmpty()) s.optString("none", "None") else (if (missing) s.optString("not_found") + " " else "") + java.io.File(value).name,
                Modifier.weight(1f), style = deskText(13, color = if (missing) OrcaColors.error else desk().text))
            PanelButton(s.optString("load", "Load...")) {
                app.openDocument(mimes) { uri ->
                    val ctx = app.appContext ?: return@openDocument
                    if (ext.none { app.displayName(ctx, uri).endsWith(it, true) }) { app.toast = s.optString("invalid"); return@openDocument }
                    keepFile(app, uri, dir)?.let(onChange)
                }
            }
            Spacer(Modifier.width(8.dp))
            PanelButton(s.optString("remove", "Remove"), enabled = value.isNotEmpty()) { onChange("") }
        }
    }
    Dialog(onDismiss, DialogProperties(usePlatformDefaultWidth = false)) {
        Surface(Modifier.fillMaxWidth(0.95f).heightIn(max = 640.dp), shape = RoundedCornerShape(12.dp)) {
            Column(Modifier.padding(16.dp).verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(s.optString("title", "Bed Shape"), style = MaterialTheme.typography.titleMedium)
                Row {
                    Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        Text(s.optString("shape", "Shape"), style = deskText(14, FontWeight.Bold))
                        DeskChoice(listOf(s.optString("rectangle"), s.optString("circle"), s.optString("custom")), shape, Modifier.width(220.dp)) { shape = it }
                        Text(s.optString("settings", "Settings"), style = deskText(14, FontWeight.Bold))
                        when (shape) {
                            0 -> {
                                LabeledNumber(tr("Size") + " X", sx, "mm") { sx = it }; LabeledNumber(tr("Size") + " Y", sy, "mm") { sy = it }
                                LabeledNumber(tr("Origin") + " X", ox, "mm") { ox = it }; LabeledNumber(tr("Origin") + " Y", oy, "mm") { oy = it }
                            }
                            1 -> LabeledNumber(tr("Diameter"), dia, "mm") { dia = it }
                            else -> PanelButton(s.optString("load_shape", "Load shape from STL...")) {
                                app.openDocument(arrayOf("*/*")) { uri ->
                                    val ctx = app.appContext ?: return@openDocument
                                    app.scope.launch {
                                        val path = app.copyToCache(ctx, uri)
                                        app.call("bedshape.load_stl", jobj("path" to path, "name" to app.displayName(ctx, uri))) { r ->
                                            (r as? JSONArray)?.let { a -> loaded = (0 until a.length()).map { k -> a.getJSONArray(k).let { p -> p.getDouble(0) to p.getDouble(1) } } }
                                        }
                                    }
                                }
                            }
                        }
                        FileRow(s.optString("texture", "Texture"), texture, arrayOf("image/png", "image/svg+xml", "*/*"), "bed_textures", listOf(".png", ".svg")) { texture = it }
                        FileRow(s.optString("model", "Model"), model, arrayOf("*/*"), "bed_models", listOf(".stl")) { model = it }
                    }
                    Spacer(Modifier.width(16.dp))
                    val o = outline()
                    val color = OrcaColors.accent
                    Canvas(Modifier.weight(1f).height(320.dp)) {   // BedShapePanel::update_preview
                        if (o.isEmpty()) return@Canvas
                        val bx0 = minOf(o.minOf { it.first }, 0.0); val bx1 = maxOf(o.maxOf { it.first }, 0.0)
                        val by0 = minOf(o.minOf { it.second }, 0.0); val by1 = maxOf(o.maxOf { it.second }, 0.0)
                        val k = minOf(size.width / (bx1 - bx0).coerceAtLeast(1.0), size.height / (by1 - by0).coerceAtLeast(1.0)).toFloat() * 0.85f
                        val cx = size.width / 2; val cy = size.height / 2
                        fun map(p: Pair<Double, Double>) = Offset(cx + ((p.first - (bx0 + bx1) / 2) * k).toFloat(), cy - ((p.second - (by0 + by1) / 2) * k).toFloat())
                        val path = Path().apply { moveTo(map(o[0]).x, map(o[0]).y); o.drop(1).forEach { lineTo(map(it).x, map(it).y) }; close() }
                        drawPath(path, color.copy(alpha = 0.15f))
                        drawPath(path, color, style = Stroke(3f))
                        val origin = map(0.0 to 0.0)
                        drawLine(androidx.compose.ui.graphics.Color.Red, origin, origin + Offset(30f, 0f), 3f)
                        drawLine(androidx.compose.ui.graphics.Color.Green, origin, origin + Offset(0f, -30f), 3f)
                    }
                }
                Row(Modifier.align(Alignment.End)) {
                    TextButton(onDismiss) { Text(tr("Cancel")) }
                    TextButton({
                        val o = outline()
                        if (o.size >= 3) onSet("printable_area", JSONArray(o.map { "${fmt(it.first)}x${fmt(it.second)}" }))
                        if (texture != (st.value("bed_custom_texture", 0) ?: "")) onSet("bed_custom_texture", texture)
                        if (model != (st.value("bed_custom_model", 0) ?: "")) onSet("bed_custom_model", model)
                        onDismiss()
                    }) { Text(tr("OK")) }
                }
            }
        }
    }
}

@Composable
private fun LabeledNumber(label: String, value: String, unit: String, onChange: (String) -> Unit) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(label, Modifier.width(100.dp))
        CommitField(value, true, true, Modifier.weight(1f), unit) { onChange(it) }
    }
}

private fun fmt(d: Double): String = if (d == Math.floor(d)) d.toLong().toString() else String.format(java.util.Locale.ROOT, "%.3f", d).trimEnd('0').trimEnd('.')

/** Tab::compatible_widget_create: "All" or a selection of printers / process presets. */
@Composable
private fun CompatibleDialog(app: AppState, key: String, st: ConfigState, onSet: (String, Any) -> Unit, onDismiss: () -> Unit) {
    val current = st.values.optJSONArray(key).strings().toMutableSet()
    var sel by remember { mutableStateOf(current.toSet()) }
    var all by remember { mutableStateOf(current.isEmpty()) }
    var names by remember { mutableStateOf<List<String>>(emptyList()) }
    LaunchedEffect(key) {
        app.presetList(if (key == "compatible_printers") "printer" else "print", all = true) { a ->
            names = (0 until a.length()).map { a.getJSONObject(it) }.filter { !it.optBoolean("default") }.map { it.optString("name") }
        }
    }
    Dialog(onDismiss, DialogProperties(usePlatformDefaultWidth = false)) {
        Surface(Modifier.fillMaxWidth(0.95f).fillMaxHeight(0.85f), shape = RoundedCornerShape(12.dp)) {
            Column(Modifier.padding(16.dp)) {
                Text(if (key == "compatible_printers") "Compatible printers" else "Compatible process profiles", style = MaterialTheme.typography.titleMedium)
                Row(verticalAlignment = Alignment.CenterVertically) { Checkbox(all, { all = it }); Text(tr("All")) }
                LazyColumn(Modifier.weight(1f)) {
                    items(names) { n ->
                        Row(Modifier.fillMaxWidth().clickable(enabled = !all) { sel = if (n in sel) sel - n else sel + n }, verticalAlignment = Alignment.CenterVertically) {
                            Checkbox(n in sel, { sel = if (it) sel + n else sel - n }, enabled = !all); Text(n)
                        }
                    }
                }
                Row(Modifier.align(Alignment.End)) {
                    TextButton(onDismiss) { Text(tr("Cancel")) }
                    TextButton({ onSet(key, JSONArray(if (all) emptyList() else sel.toList())); onDismiss() }) { Text(tr("OK")) }
                }
            }
        }
    }
}

/**
 * RammingDialog / RammingChart (slic3r/GUI/WipeTowerDialog.cpp, RammingChart.cpp): control points (time s, speed
 * mm³/s) joined by a cubic spline with zero end slopes, sampled every 0.25 s. Value: "<width%> <step%> v1 v2 ...| x1 y1 x2 y2 ...".
 * Drag a point to move it, double-tap the chart to add one, long-press a point to remove it.
 */
@Composable
private fun RammingDialog(value: String, onSet: (String) -> Unit, onDismiss: () -> Unit) {
    // RammingPanel: "<width%> <spacing%> <speeds...>| <x y>..." (the chart's handles after the bar)
    val parts = value.split("|")
    val head = parts.getOrNull(0)?.trim()?.split(Regex("\\s+"))?.mapNotNull { it.toFloatOrNull() } ?: emptyList()
    var width by remember { mutableIntStateOf((head.getOrNull(0) ?: 100f).roundToInt().coerceIn(10, 300)) }
    var step by remember { mutableIntStateOf((head.getOrNull(1) ?: 100f).roundToInt().coerceIn(10, 300)) }
    val nSpeeds = (head.size - 2).coerceAtLeast(0)
    // Chart::visible_area: sampling * ramming_speed_size wide, 60 mm³/s high
    var time by remember { mutableFloatStateOf(nSpeeds * 0.25f) }
    val maxY = 60f
    val buttons = remember {
        mutableStateListOf<Pair<Float, Float>>().apply {
            val b = (parts.getOrNull(1) ?: "").trim().split(Regex("\\s+")).mapNotNull { it.toFloatOrNull() }
            for (i in 0 until b.size / 2) add(b[2 * i] to b[2 * i + 1])
        }
    }
    // Chart::recalculate_line: a natural cubic spline with zero end slopes through the handles inside the visible time
    fun spline(x: Float): Float {
        val pts = ArrayList<Pair<Float, Float>>()
        for (b in buttons) {
            if (pts.isNotEmpty() && pts.last().first == b.first) continue
            if (pts.isNotEmpty() && b.first > time) break
            pts.add(b)
        }
        pts.sortBy { it.first }
        if (pts.size < 2) return 0f
        val n = pts.size - 1
        val h = FloatArray(n + 1); val diag = FloatArray(n + 1) { 2f }; val mu = FloatArray(n + 1); val lambda = FloatArray(n + 1); val rhs = FloatArray(n + 1)
        for (i in 1..n) h[i] = pts[i].first - pts[i - 1].first
        for (i in 1 until n) {
            mu[i] = h[i] / (h[i] + h[i + 1]); lambda[i] = 1f - mu[i]
            rhs[i] = 6 * ((pts[i + 1].second - pts[i].second) / (h[i + 1] * (pts[i + 1].first - pts[i - 1].first)) -
                (pts[i].second - pts[i - 1].second) / (h[i] * (pts[i + 1].first - pts[i - 1].first)))
        }
        lambda[0] = 1f; mu[n] = 1f
        rhs[0] = (6f / h[1]) * ((pts[0].second - pts[1].second) / (pts[0].first - pts[1].first))
        rhs[n] = (6f / h[n]) * (0f - (pts[n - 1].second - pts[n].second) / (pts[n - 1].first - pts[n].first))
        for (i in 1..n) { val m = mu[i] / diag[i - 1]; diag[i] -= m * lambda[i - 1]; rhs[i] -= m * rhs[i - 1] }
        rhs[n] = rhs[n] / diag[n]
        for (i in n - 1 downTo 0) rhs[i] = (rhs[i] - lambda[i] * rhs[i + 1]) / diag[i]
        val y = when {
            x <= pts[0].first -> pts[0].second
            x >= pts[n].first -> pts[n].second
            else -> {
                var i = 1; while (i < n && pts[i].first < x) i++
                val a = pts[i].first - x; val b = x - pts[i - 1].first
                (rhs[i - 1] * a * a * a + rhs[i] * b * b * b) / (6 * h[i]) +
                    (pts[i - 1].second - rhs[i - 1] * h[i] * h[i] / 6f) * a / h[i] + (pts[i].second - rhs[i] * h[i] * h[i] / 6f) * b / h[i]
            }
        }
        return y.coerceIn(0f, maxY)
    }
    // Chart::get_ramming_speed(0.25): the mean of the line at both ends of each step; the volume integrates the line
    fun speeds(): List<Float> = (0 until Math.round(time / 0.25f)).map { j -> (spline(j * 0.25f) + spline((j + 1) * 0.25f)) / 2f }
    val volume = if (time <= 0f) 0f else { val k = 400; (0 until k).sumOf { spline((it + 0.5f) * time / k).toDouble() }.toFloat() * time / k }
    val c = org.orcaport.mobile.ui.desk()
    Dialog(onDismiss, DialogProperties(usePlatformDefaultWidth = false)) {
        Surface(Modifier.widthIn(max = 900.dp).fillMaxWidth(0.95f), shape = RoundedCornerShape(12.dp), color = c.panel) {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(tr("Ramming customization"), style = deskText(16))
                Row(horizontalArrangement = Arrangement.spacedBy(20.dp)) {
                    Column(Modifier.weight(1f)) {
                        RammingChart(buttons, time, maxY, ::spline)
                        Text(tr("For constant flow rate, hold %1% while dragging.").replace("%1%", "Ctrl"), Modifier.padding(top = 6.dp), style = deskText(14))
                    }
                    Column(Modifier.width(260.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                        // the spins: Time (step 250 ms, no typing), Volume (read only), the ramming line's Width / Spacing
                        Text(tr("Total ramming"), style = deskText(14, FontWeight.Bold))
                        RammingSpin(tr("Time"), (time * 1000).roundToInt(), tr("ms"), 0, 5000, 250, typing = false) {
                            // Chart::set_xy_range: whole quarter seconds
                            time = (it / 1000f / 0.25f).toInt() * 0.25f
                        }
                        RammingSpin(tr("Volume"), volume.roundToInt(), tr("mm³"), 0, 10000, 1, enabled = false) {}
                        Spacer(Modifier.height(10.dp))
                        Text(tr("Ramming line"), style = deskText(14, FontWeight.Bold))
                        RammingSpin(tr("Width"), width, "%", 10, 300, 1) { width = it }
                        RammingSpin(tr("Spacing"), step, "%", 10, 300, 1) { step = it }
                    }
                }
                Row(Modifier.align(Alignment.End)) {
                    TextButton({
                        // RammingPanel::get_parameters
                        val sb = StringBuilder("$width $step")
                        speeds().forEach { sb.append(" ").append(fmt(it.toDouble())) }
                        sb.append("|")
                        buttons.forEach { sb.append(" ").append(fmt(it.first.toDouble())).append(" ").append(fmt(it.second.toDouble())) }
                        onSet(sb.toString()); onDismiss()
                    }) { Text(tr("OK")) }
                    TextButton(onDismiss) { Text(tr("Cancel")) }
                }
            }
        }
    }
}

/**
 * Chart: the handles at fixed times dragged up and down (all of them with Ctrl held, or with two fingers), the line under
 * them filled red to green by height, the axes, the value of each handle.
 */
@Composable
private fun RammingChart(buttons: MutableList<Pair<Float, Float>>, time: Float, maxY: Float, spline: (Float) -> Float) {
    val c = org.orcaport.mobile.ui.desk()
    val measurer = androidx.compose.ui.text.rememberTextMeasurer()
    val labelStyle = deskText(12)
    val legend = 50.dp
    Box(Modifier.fillMaxWidth().height(360.dp)) {
        Canvas(Modifier.fillMaxSize().pointerInput(time) {
            awaitEachGesture {
                val down = awaitFirstDown()
                val l = legend.toPx(); val w = size.width - l; val h = size.height - l
                fun sx(x: Float) = l + x / time * w
                fun sy(y: Float) = (1 - y / maxY) * h
                val side = 16.dp.toPx()
                val idx = if (time < 0.499f) -1 else buttons.indexOfFirst { (bx, by) ->
                    bx <= time && kotlin.math.abs(sx(bx) - down.position.x) < side && kotlin.math.abs(sy(by) - down.position.y) < side
                }
                if (idx < 0) return@awaitEachGesture
                var last = down.position.y
                while (true) {
                    val ev = awaitPointerEvent()
                    val p = ev.changes.firstOrNull { it.pressed } ?: break
                    val uniform = ev.keyboardModifiers.isCtrlPressed || ev.changes.count { it.pressed } >= 2
                    val dy = -(p.position.y - last) / h * maxY
                    last = p.position.y
                    if (uniform) for (i in buttons.indices) buttons[i] = buttons[i].first to buttons[i].second + dy
                    else buttons[idx] = buttons[idx].first to buttons[idx].second + dy
                    ev.changes.forEach { it.consume() }
                }
            }
        }) {
            val l = legend.toPx(); val w = size.width - l; val h = size.height - l
            drawRect(Color(0xFFF1F1F1).let { if (c.panel.luminance() < 0.5f) Color(0xFF3B3B41) else it }, Offset(l, 0f), Size(w, h))
            drawRect(c.border, Offset(l, 0f), Size(w, h), style = Stroke(1f))
            if (time < 0.499f) {
                val t = measurer.measure(tr("NO RAMMING AT ALL"), labelStyle.copy(color = Color(0xFFFF6F00)))
                drawText(t, topLeft = Offset(l + w / 2 - t.size.width / 2, h / 2))
                return@Canvas
            }
            fun sx(x: Float) = l + x / time * w
            fun sy(y: Float) = (1 - y / maxY) * h
            // the fill: one column per pixel, red to green by height
            var px = 0f
            val line = Path()
            while (px <= w) {
                val y = sy(spline(px / w * time))
                val col = (444 * ((h - y) / h)).toInt()
                drawLine(Color(minOf(222, col), 222 - maxOf(col - 222, 0), 60), Offset(l + px, y), Offset(l + px, h), 1.5f)
                if (px == 0f) line.moveTo(l + px, y) else line.lineTo(l + px, y)
                px += 1f
            }
            drawPath(line, Color(0xFF363636), style = Stroke(1.5f))
            // the axes' ticks and labels
            val tick = 4.dp.toPx()
            var lastMark = -10000f
            var mx = 0f
            while (mx < time) {
                val x = sx(mx)
                if (x - lastMark >= l) {
                    drawLine(c.text, Offset(x, h - tick), Offset(x, h + tick), 1f)
                    val t = measurer.measure(if (mx == 0f) "0" else String.format(java.util.Locale.ROOT, "%.1f", mx), labelStyle)
                    drawText(t, topLeft = Offset(x - t.size.width / 2, h + tick))
                    lastMark = x
                }
                mx += 0.1f
            }
            lastMark = 10000f
            for (my in 0 until maxY.toInt()) {
                val y = sy(my.toFloat())
                if (lastMark - y < l) continue
                drawLine(c.text, Offset(l - tick, y), Offset(l + tick, y), 1f)
                val t = measurer.measure("$my", labelStyle)
                drawText(t, topLeft = Offset(l - tick * 2 - t.size.width, y - t.size.height / 2))
                lastMark = y
            }
            val xl = measurer.measure(tr("Time") + " (" + tr("s") + ")", labelStyle)
            drawText(xl, topLeft = Offset(l + w / 2 - xl.size.width / 2, h + l * 0.55f))
            val yl = measurer.measure(tr("Volumetric speed") + " (mm³/s)", labelStyle)
            rotate(-90f, Offset(0f, h / 2)) { drawText(yl, topLeft = Offset(-yl.size.width / 2f, h / 2 - yl.size.height)) }
            // the handles and their values
            val side = 10.dp.toPx()
            for ((bx, by) in buttons) {
                if (bx > time) continue
                val p = Offset(sx(bx), sy(by.coerceIn(0f, maxY)))
                drawCircle(Color(0xFF009688), side / 2, p)
                drawCircle(Color(0xFF363636), side / 2, p, style = Stroke(1f))
                val t = measurer.measure(String.format(java.util.Locale.ROOT, "%.1f", by), labelStyle)
                val ly = if (p.y - side - t.size.height - 8 < 0) p.y + side else p.y - side - t.size.height
                val lx = (p.x - t.size.width / 2).coerceIn(l + 8, l + w - t.size.width - 8)
                drawRoundRect(Color(0xCCF1F1F1), Offset(lx - 4, ly - 4), Size(t.size.width + 8f, t.size.height + 8f),
                    androidx.compose.ui.geometry.CornerRadius(2f))
                drawText(t, topLeft = Offset(lx, ly))
            }
        }
    }
}

/** SpinInput: the value with − / + by its step, typing allowed or not */
@Composable
private fun RammingSpin(label: String, value: Int, unit: String, min: Int, max: Int, step: Int, typing: Boolean = true, enabled: Boolean = true,
                        onChange: (Int) -> Unit) {
    val c = org.orcaport.mobile.ui.desk()
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(label, Modifier.width(80.dp), style = deskText(14))
        Row(Modifier.weight(1f).height(36.dp).border(1.dp, c.border, RoundedCornerShape(4.dp)), verticalAlignment = Alignment.CenterVertically) {
            if (typing && enabled) CommitField("$value", true, true, Modifier.weight(1f), unit) { s -> s.toIntOrNull()?.let { onChange(it.coerceIn(min, max)) } }
            else Text("$value $unit", Modifier.weight(1f).padding(start = 8.dp), style = deskText(14, color = if (enabled) c.text else c.textDisabled))
            if (enabled) {
                Text("−", Modifier.clickable { onChange((value - step).coerceIn(min, max)) }.padding(horizontal = 10.dp), style = deskText(16))
                Text("+", Modifier.clickable { onChange((value + step).coerceIn(min, max)) }.padding(horizontal = 10.dp), style = deskText(16))
            }
        }
    }
}
