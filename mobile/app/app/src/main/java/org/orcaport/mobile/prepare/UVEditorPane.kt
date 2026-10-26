package org.orcaport.mobile.prepare

import android.graphics.BitmapFactory
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.*
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.clipPath
import androidx.compose.ui.graphics.drawscope.withTransform
import androidx.compose.ui.input.pointer.PointerEventType
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.input.pointer.positionChange
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp
import org.json.JSONArray
import org.json.JSONObject
import org.orcaport.mobile.AppState
import org.orcaport.mobile.engine.jobj
import org.orcaport.mobile.ui.*
import kotlin.math.*

// UVEditorCanvas's colours
private val UV_BG = Color(0.16f, 0.16f, 0.16f, 1f)
private val UV_OUTSIDE = Color(0.16f, 0.16f, 0.16f, 0.72f)
private val UV_GRID = Color(1f, 1f, 1f, 0.08f)
private val UV_HALO = Color(0.06f, 0.06f, 0.07f, 1f)
private val UV_TILE_OUTLINE = Color(0.70f, 0.70f, 0.72f, 1f)
private val UV_WIRE = Color(1f, 1f, 1f, 0.30f)
private val UV_WIRE_SHADOW = Color(0f, 0f, 0f, 0.35f)
private val UV_BOUNDARY = Color(0.93f, 0.93f, 0.94f, 1f)
private val UV_HOVER = Color(0.72f, 0.98f, 0.93f, 1f)
private val UV_HOVER_FILL = Color(1f, 1f, 1f, 0.12f)
private val UV_SEL_FILL = Color(0.15f, 0.85f, 0.75f, 0.24f)
private val UV_SEL_WIRE = Color(0.55f, 1f, 0.92f, 0.55f)
private val UV_SEL_BOUNDARY = Color(0.16f, 0.90f, 0.78f, 1f)
private val UV_DIAL = Color(1f, 0.85f, 0.2f, 0.9f)

/**
 * UVEditorPanel: the active Unwrap layer (its image, name and tile size), the backdrop (Height / Checker / Distortion),
 * Unwrap with Seam angle and Connect islands, the selection modes, seam marking, the island tools, Snap and Frame all,
 * the canvas and its status line. The canvas logic is upstream's UVEditorCanvas in the engine (texdisp.uv): touches go
 * there as its mouse events and the view comes back to draw. Touch: one finger = the left button (Add = Shift), two
 * fingers pan and pinch-zoom (middle drag / wheel), a long press then drag = the right button (rotate the island).
 */
@Composable
fun UVEditorPane(app: AppState, st: JSONObject, onState: (JSONObject) -> Unit, onPickTexture: () -> Unit, modifier: Modifier) {
    val pane = st.optJSONObject("uv") ?: return
    val c = desk()
    var view by remember { mutableStateOf<JSONObject?>(null) }
    var size by remember { mutableStateOf(IntSize(1, 1)) }
    var add by remember { mutableStateOf(false) }
    fun send(vararg kv: Pair<String, Any>) = app.call("texdisp.uv", jobj("w" to size.width, "h" to size.height, *kv)) {
        if (it is JSONObject) { it.optJSONObject("uv_view")?.let { v -> view = v }; onState(it) }
    }
    LaunchedEffect(size, st.optInt("active"), pane.optBoolean("unwrapped"), pane.optInt("island_count"), app.gizmoRevision) { send() }
    val hasLayer = pane.optBoolean("has_layer")
    Column(modifier.shadow(4.dp, RoundedCornerShape(8.dp)).clip(RoundedCornerShape(8.dp)).background(c.panel).padding(8.dp),
        verticalArrangement = Arrangement.spacedBy(6.dp)) {
        // the layer: its image (opens the texture library), name and tile size; close
        Row(verticalAlignment = Alignment.CenterVertically) {
            val img = pane.optString("image")
            val bmp = remember(img) { if (img.isEmpty()) null else runCatching { BitmapFactory.decodeFile(img, BitmapFactory.Options().apply { inSampleSize = 4 })?.asImageBitmap() }.getOrNull() }
            Box(Modifier.size(30.dp).clip(RoundedCornerShape(3.dp)).background(c.field).clickable(enabled = hasLayer) { onPickTexture() }) {
                bmp?.let { androidx.compose.foundation.Image(it, null, Modifier.fillMaxSize(), contentScale = androidx.compose.ui.layout.ContentScale.Crop) }
            }
            Text(if (hasLayer) pane.optString("layer_name") else tr("No layer mapped with Unwrap"), Modifier.weight(1f).padding(horizontal = 8.dp),
                style = deskText(13, FontWeight.Bold), maxLines = 1, overflow = TextOverflow.Ellipsis)
            if (hasLayer) Text(String.format(java.util.Locale.ROOT, tr("%.1f mm tile"), pane.optDouble("tile_mm")), style = deskText(12, color = c.textDim))
            Text("✕", Modifier.clickable { send("cmd" to 17) }.padding(6.dp), style = deskText(15, color = c.textDim))
        }
        // backdrop, Unwrap, Seam angle, Connect islands
        Row(Modifier.horizontalScroll(rememberScrollState()), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
            for ((i, icon) in listOf("texture_displacement_uv_bg_height", "texture_displacement_checker", "texture_displacement_distortion").withIndex())
                PaneTool(icon, pane.optInt("background") == i, hasLayer) { send("cmd" to 15, "value" to i) }
            Spacer(Modifier.width(6.dp))
            Box {
                PanelButton(tr("Unwrap"), primary = pane.optBoolean("stale") || !pane.optBoolean("unwrapped"), enabled = hasLayer) { send("cmd" to 7) }
                if (pane.optBoolean("stale")) Box(Modifier.align(Alignment.TopEnd).size(8.dp).clip(RoundedCornerShape(4.dp)).background(Color(0xFFFF6F00)))
            }
            Spacer(Modifier.width(6.dp))
            Text(tr("Seam angle"), style = deskText(12, color = c.textDim))
            var angle by remember(pane.optDouble("seam_angle")) { mutableStateOf(pane.optDouble("seam_angle").roundToInt()) }
            PaneTool("", false, hasLayer && angle > 5, label = "−") { angle = max(5, angle - 5); send("cmd" to 8, "value" to angle) }
            Text("$angle°", Modifier.width(36.dp), style = deskText(12))
            PaneTool("", false, hasLayer && angle < 90, label = "+") { angle = min(90, angle + 5); send("cmd" to 8, "value" to angle) }
            DeskCheck(pane.optBoolean("connect"), enabled = hasLayer) { send("cmd" to 9, "value" to if (it) 1 else 0) }
            Text(tr("Connect islands"), style = deskText(12))
        }
        // selection modes, seams, island tools, Snap, Frame; Add (= Shift), Rotate / Scale (R / S)
        val islands = pane.optBoolean("has_islands")
        val selected = pane.optInt("selected", -1) >= 0
        Row(Modifier.horizontalScroll(rememberScrollState()), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(3.dp)) {
            for ((i, icon) in listOf("texture_displacement_uv_select_island", "texture_displacement_uv_select_vertex", "texture_displacement_uv_select_edge").withIndex())
                PaneTool(icon, pane.optInt("select_mode") == i, islands) { send("cmd" to 10, "value" to i) }
            PaneSep()
            PaneTool("texture_displacement_uv_seam", pane.optBoolean("mark_seams"), hasLayer) { send("cmd" to 11, "value" to if (pane.optBoolean("mark_seams")) 0 else 1) }
            PaneTool("texture_displacement_uv_path", pane.optBoolean("seam_path"), hasLayer && pane.optBoolean("mark_seams")) {
                send("cmd" to 12, "value" to if (pane.optBoolean("seam_path")) 0 else 1)
            }
            PaneTool("texture_displacement_cross", false, hasLayer && pane.optBoolean("has_seams")) { send("cmd" to 13) }
            PaneSep()
            PaneTool("texture_displacement_uv_avg_scale", false, islands) { send("cmd" to 2) }
            PaneTool("texture_displacement_uv_cut", false, islands && selected) { send("cmd" to 3) }
            PaneTool("texture_displacement_uv_join", false, islands && selected) { send("cmd" to 5) }
            PaneTool("texture_displacement_uv_unjoin", false, islands && selected) { send("cmd" to 6) }
            PaneTool("texture_displacement_uv_clear_edits", false, hasLayer && pane.optBoolean("has_uv_edits")) { send("cmd" to 14) }
            PaneSep()
            PaneTool("texture_displacement_uv_snap", pane.optBoolean("snap"), islands) { send("cmd" to 1) }
            PaneTool("texture_displacement_uv_frame", false, islands) { send("cmd" to 0) }
            PaneSep()
            PaneTool("", add, islands, label = tr("Add")) { add = !add }
            PaneTool("", false, islands && selected && pane.optInt("select_mode") == 0, label = "R") { send("key" to 'R'.code) }
            PaneTool("", false, islands && selected && pane.optInt("select_mode") == 0, label = "S") { send("key" to 'S'.code) }
        }
        UVCanvas(view, Modifier.fillMaxWidth().weight(1f).clip(RoundedCornerShape(4.dp)).background(UV_BG).onSizeChanged { size = it },
            onEvent = { kind, x, y, extra -> send("event" to kind, "x" to x, "y" to y, "shift" to add, *extra) })
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(view?.optString("status") ?: "", Modifier.weight(1f), style = deskText(11, color = c.textDim), maxLines = 2, overflow = TextOverflow.Ellipsis)
            Text(pane.optString("stats"), style = deskText(11, color = c.textDim))
        }
    }
}

@Composable
private fun PaneSep() = Box(Modifier.padding(horizontal = 3.dp).width(1.dp).height(22.dp).background(desk().border))

@Composable
private fun PaneTool(icon: String, selected: Boolean, enabled: Boolean, label: String = "", onClick: () -> Unit) {
    val c = desk()
    Box(Modifier.height(32.dp).widthIn(min = 32.dp).clip(RoundedCornerShape(4.dp)).background(if (selected) c.selection else Color.Transparent)
        .border(1.dp, if (selected) OrcaColors.accent else Color.Transparent, RoundedCornerShape(4.dp)).clickable(enabled = enabled, onClick = onClick)
        .padding(horizontal = if (label.isNotEmpty()) 6.dp else 0.dp), Alignment.Center) {
        if (icon.isNotEmpty()) OrcaIcon(icon, 20.dp, alpha = if (enabled) 1f else 0.35f)
        else Text(label, style = deskText(13, color = if (enabled) c.text else c.textDisabled))
    }
}

/** UVEditorCanvas::render from the engine's view, and the touches as its mouse events (pane pixels) */
@Composable
private fun UVCanvas(view: JSONObject?, modifier: Modifier, onEvent: (String, Float, Float, Array<Pair<String, Any>>) -> Unit) {
    val bgPath = view?.optString("background") ?: ""
    val bgRev = view?.optInt("bg_revision") ?: 0
    val bg = remember(bgPath, bgRev) { if (bgPath.isEmpty()) null else runCatching { BitmapFactory.decodeFile(bgPath)?.asImageBitmap() }.getOrNull() }
    Canvas(modifier.pointerInput(Unit) {
        awaitEachGesture {
            val down = awaitFirstDown(requireUnconsumed = false)
            var mode = 0          // 0 left, 1 two-finger, 2 right (long press)
            var last = down.position
            var lastSpan = 0f
            var moved = false
            val start = down.uptimeMillis
            onEvent("down", down.position.x, down.position.y, arrayOf())
            while (true) {
                val ev = awaitPointerEvent()
                val pressed = ev.changes.filter { it.pressed }
                if (pressed.isEmpty()) {
                    onEvent(if (mode == 2) "rup" else if (mode == 1) "mup" else "up", last.x, last.y, arrayOf())
                    break
                }
                if (pressed.size >= 2) {
                    val ctr = Offset(pressed.map { it.position.x }.average().toFloat(), pressed.map { it.position.y }.average().toFloat())
                    val span = (pressed[0].position - pressed[1].position).getDistance()
                    if (mode != 1) {
                        // the first finger's press is taken back, the pair pans (middle button) and pinches (wheel)
                        onEvent("up", last.x, last.y, arrayOf())
                        onEvent("mdown", ctr.x, ctr.y, arrayOf())
                        mode = 1
                        lastSpan = span
                    } else {
                        onEvent("move", ctr.x, ctr.y, arrayOf("held" to false))
                        if (lastSpan > 0f && span > 0f && abs(span / lastSpan - 1f) > 0.01f) {
                            // m_zoom *= 0.9 ^ (rotation / 120): spreading the fingers zooms in
                            val rotation = 120.0 * ln((span / lastSpan).toDouble()) / -ln(0.9)
                            onEvent("wheel", ctr.x, ctr.y, arrayOf("wheel" to rotation))
                            lastSpan = span
                        }
                    }
                    last = ctr
                    ev.changes.forEach { it.consume() }
                    continue
                }
                val p = pressed[0]
                if (mode == 0 && !moved && p.uptimeMillis - start > 450 && (p.position - down.position).getDistance() < 12f) {
                    // a long press: the right button (rotate the island)
                    onEvent("up", last.x, last.y, arrayOf())
                    onEvent("rdown", p.position.x, p.position.y, arrayOf())
                    mode = 2
                }
                if (p.positionChange() != Offset.Zero) {
                    moved = moved || (p.position - down.position).getDistance() > 8f
                    if (mode != 1) onEvent("move", p.position.x, p.position.y, arrayOf("held" to (mode == 0)))
                    last = p.position
                }
                p.consume()
            }
        }
    }) {
        val v = view ?: return@Canvas
        drawUV(v, bg)
    }
}

private fun DrawScope.drawUV(v: JSONObject, bg: ImageBitmap?) {
    val pan = v.optJSONArray("pan") ?: return
    val half = v.optJSONArray("half") ?: return
    val px0 = pan.getDouble(0).toFloat(); val py0 = pan.getDouble(1).toFloat()
    val hw = half.getDouble(0).toFloat(); val hh = half.getDouble(1).toFloat()
    val w = size.width; val h = size.height
    fun X(u: Float) = ((u - px0) / hw + 1f) * 0.5f * w
    fun Y(t: Float) = ((t - py0) / hh + 1f) * 0.5f * h
    val uvs = v.optJSONArray("uvs") ?: JSONArray()
    val nv = uvs.length() / 2
    val xs = FloatArray(nv) { X(uvs.getDouble(it * 2).toFloat()) }
    val ys = FloatArray(nv) { Y(uvs.getDouble(it * 2 + 1).toFloat()) }
    val tris = v.optJSONArray("tris") ?: JSONArray()
    val vIsland = v.optJSONArray("vertex_island") ?: JSONArray()
    val islandCount = v.optInt("islands")
    val sel = v.optJSONArray("selection")?.let { a -> (0 until a.length()).map { a.getInt(it) }.toSet() } ?: emptySet()
    val hover = v.optInt("hover_island", -1)
    // the backdrop: the layer's image over its tiles (mirrored every other one with Mirrored repeat)
    if (bg != null) {
        val lo = v.optJSONArray("bg_lo"); val hi = v.optJSONArray("bg_hi")
        val x0 = floor(lo.getDouble(0)).toInt(); val x1 = ceil(hi.getDouble(0)).toInt()
        val y0 = floor(lo.getDouble(1)).toInt(); val y1 = ceil(hi.getDouble(1)).toInt()
        val mirrored = v.optBoolean("mirrored")
        if ((x1 - x0) * (y1 - y0) <= 400)
            for (i in x0 until x1) for (j in y0 until y1) {
                val l = X(i.toFloat()); val r = X(i + 1f); val t = Y(j.toFloat()); val b = Y(j + 1f)
                val flipX = mirrored && ((i % 2) + 2) % 2 == 1; val flipY = mirrored && ((j % 2) + 2) % 2 == 1
                withTransform({
                    translate(if (flipX) r else l, if (flipY) b else t)
                    scale(if (flipX) -1f else 1f, if (flipY) -1f else 1f, Offset.Zero)
                }) {
                    drawImage(bg, IntOffset.Zero, IntSize(bg.width, bg.height), IntOffset.Zero, IntSize(ceil(r - l).toInt().coerceAtLeast(1), ceil(b - t).toInt().coerceAtLeast(1)))
                }
            }
    }
    // the islands' shapes (one path each)
    val islandPaths = Array(islandCount) { Path() }
    val all = Path()
    for (k in 0 until tris.length() / 3) {
        val a = tris.getInt(k * 3); val b = tris.getInt(k * 3 + 1); val cc = tris.getInt(k * 3 + 2)
        if (a !in 0 until nv || b !in 0 until nv || cc !in 0 until nv) continue
        val isl = vIsland.optInt(a, -1)
        val p = Path().apply { moveTo(xs[a], ys[a]); lineTo(xs[b], ys[b]); lineTo(xs[cc], ys[cc]); close() }
        if (isl in 0 until islandCount) islandPaths[isl].addPath(p)
        all.addPath(p)
    }
    // dim the texture outside the islands
    if (islandCount > 0) clipPath(all, ClipOp.Difference) { drawRect(UV_OUTSIDE) }
    // the grid
    val zoom = hh.coerceAtMost(hw)
    val span = 2f * zoom
    var step = 1f
    while (step > 0.01f && span / step > 40f) step *= 2f
    while (span / step < 4f) step *= 0.5f
    run {
        val lox = px0 - hw - step; val hix = px0 + hw + step; val loy = py0 - hh - step; val hiy = py0 + hh + step
        val fx = floor(lox / step).toInt(); val lx = ceil(hix / step).toInt(); val fy = floor(loy / step).toInt(); val ly = ceil(hiy / step).toInt()
        if (lx - fx < 4000 && ly - fy < 4000) {
            for (i in fx..lx) drawLine(UV_GRID, Offset(X(i * step), 0f), Offset(X(i * step), h), 1f)
            for (i in fy..ly) drawLine(UV_GRID, Offset(0f, Y(i * step)), Offset(w, Y(i * step)), 1f)
        }
    }
    val dp = density
    fun stroke(path: Path, width: Float, color: Color, haloPx: Float) {
        drawPath(path, UV_HALO, style = Stroke(width = (width + 2 * haloPx) * dp, cap = StrokeCap.Square))
        drawPath(path, color, style = Stroke(width = width * dp, cap = StrokeCap.Square))
    }
    // the first tile's outline
    stroke(Path().apply { moveTo(X(0f), Y(0f)); lineTo(X(1f), Y(0f)); lineTo(X(1f), Y(1f)); lineTo(X(0f), Y(1f)); close() }, 1.5f, UV_TILE_OUTLINE, 1f)
    // fills: the distortion heat, the selection, the hover
    val fills = v.optJSONArray("fills")
    for (i in 0 until islandCount) {
        var fill = fills?.optJSONArray(i)?.let { Color(it.getDouble(0).toFloat(), it.getDouble(1).toFloat(), it.getDouble(2).toFloat(), it.getDouble(3).toFloat()) } ?: Color.Transparent
        if (i in sel) fill = UV_SEL_FILL else if (i == hover && (fills == null || fills.length() == 0)) fill = UV_HOVER_FILL
        if (fill.alpha > 0f) drawPath(islandPaths[i], fill)
    }
    // interior edges (with their 1 px shadow), then the outlines: plain, hovered, selected
    for (i in 0 until islandCount) {
        withTransform({ translate(dp, dp) }) { drawPath(islandPaths[i], UV_WIRE_SHADOW, style = Stroke(1f)) }
        drawPath(islandPaths[i], if (i in sel) UV_SEL_WIRE else UV_WIRE, style = Stroke(1f))
    }
    val boundary = v.optJSONArray("boundary") ?: JSONArray()
    val plain = Path(); val hovered = Path(); val selected = Path()
    fun edgePath(p: Path, a: Int, b: Int) { if (a in 0 until nv && b in 0 until nv) { p.moveTo(xs[a], ys[a]); p.lineTo(xs[b], ys[b]) } }
    for (k in 0 until boundary.length() / 2) {
        val a = boundary.getInt(k * 2); val b = boundary.getInt(k * 2 + 1)
        val isl = vIsland.optInt(a, -1)
        edgePath(if (isl in sel) selected else if (isl == hover) hovered else plain, a, b)
    }
    stroke(plain, 1.5f, UV_BOUNDARY, 1.25f)
    stroke(hovered, 2.5f, UV_HOVER, 1.5f)
    stroke(selected, 3f, UV_SEL_BOUNDARY, 1.75f)
    // the rotation protractor
    v.optJSONObject("dial")?.let { d ->
        val cx = X(d.getDouble("x").toFloat()); val cy = Y(d.getDouble("y").toFloat()); val r = 90f * dp
        val dial = Path()
        dial.addOval(androidx.compose.ui.geometry.Rect(cx - r, cy - r, cx + r, cy + r))
        for (deg in 0 until 360 step 15) {
            val card = deg % 90 == 0
            val a = Math.toRadians(deg.toDouble())
            val r0 = r * (if (card) 0.80f else 0.90f); val r1 = r * (if (card) 1.08f else 1.0f)
            dial.moveTo(cx + r0 * cos(a).toFloat(), cy + r0 * sin(a).toFloat()); dial.lineTo(cx + r1 * cos(a).toFloat(), cy + r1 * sin(a).toFloat())
        }
        val na = Math.toRadians(d.getDouble("deg"))
        dial.moveTo(cx, cy); dial.lineTo(cx + 1.12f * r * cos(na).toFloat(), cy + 1.12f * r * sin(na).toFloat())
        drawPath(dial, UV_DIAL, style = Stroke(2f * dp))
    }
    // Vertex / Edge mode: the handles
    fun handle(vtx: Int, color: Color, px: Float) {
        if (vtx !in 0 until nv) return
        val s1 = (px + 4f) * dp; val s2 = px * dp
        drawRect(UV_HALO, Offset(xs[vtx] - s1 / 2, ys[vtx] - s1 / 2), Size(s1, s1))
        drawRect(color, Offset(xs[vtx] - s2 / 2, ys[vtx] - s2 / 2), Size(s2, s2))
    }
    when (v.optInt("mode")) {
        1 -> {
            val sv = v.optJSONArray("sel_vertices")?.let { a -> (0 until a.length()).map { a.getInt(it) } } ?: emptyList()
            val hv = v.optInt("hover_vertex", -1)
            if (hv >= 0 && hv !in sv) handle(hv, UV_HOVER, 7f)
            if (sv.isEmpty()) handle(v.optInt("active_vertex", -1), UV_SEL_BOUNDARY, 8f) else sv.forEach { handle(it, UV_SEL_BOUNDARY, 8f) }
        }
        2 -> {
            val he = v.optJSONArray("hover_edge")
            val se = v.optJSONArray("sel_edges")?.let { a -> (0 until a.length() / 2).map { a.getInt(it * 2) to a.getInt(it * 2 + 1) } } ?: emptyList()
            if (he != null && he.getInt(0) >= 0 && (he.getInt(0) to he.getInt(1)) !in se) stroke(Path().also { edgePath(it, he.getInt(0), he.getInt(1)) }, 3f, UV_HOVER, 2f)
            val edges = se.ifEmpty { v.optJSONArray("active_edge")?.let { if (it.getInt(0) >= 0) listOf(it.getInt(0) to it.getInt(1)) else emptyList() } ?: emptyList() }
            stroke(Path().also { p -> edges.forEach { (a, b) -> edgePath(p, a, b) } }, 4f, UV_SEL_BOUNDARY, 2f)
            edges.forEach { (a, b) -> handle(a, UV_SEL_BOUNDARY, 6f); handle(b, UV_SEL_BOUNDARY, 6f) }
        }
    }
}
