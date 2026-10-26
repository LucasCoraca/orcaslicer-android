package org.orcaport.mobile.prepare

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.foundation.clickable
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.DpOffset
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import org.json.JSONArray
import org.json.JSONObject
import org.orcaport.mobile.AppState
import org.orcaport.mobile.engine.Native
import org.orcaport.mobile.engine.jobj
import org.orcaport.mobile.settings.parseColor
import org.orcaport.mobile.ui.*
import kotlin.math.abs
import kotlin.math.roundToInt

// CustomGCode::Type
private const val COLOR_CHANGE = 0; private const val PAUSE = 1; private const val CUSTOM = 2; private const val TOOL_CHANGE = 4; private const val TEMPLATE = 3

/**
 * IMSlider (the vertical layer slider of the preview): the lower and upper handles with the height / layer number,
 * the custom G-code ticks of the plate, and its menu (long press on a handle; right click on the desktop).
 */
@Composable
fun LayerSlider(app: AppState, layers: JSONArray, lr: JSONArray, modifier: Modifier) {
    val n = layers.length()
    val plate = app.project.optInt("current_plate")
    var lo by remember(n) { mutableIntStateOf(lr.getInt(0).coerceIn(0, n - 1)) }
    var hi by remember(n) { mutableIntStateOf(lr.getInt(1).coerceIn(0, n - 1)) }
    var active by remember { mutableIntStateOf(1) }   // 0 lower, 1 higher (IMSlider selection)
    var oneLayer by remember { mutableStateOf(false) }   // IMSlider::m_is_one_layer
    var oneLayerValue by remember { mutableIntStateOf(0) }
    var ticks by remember { mutableStateOf(JSONArray()) }
    var menu by remember { mutableStateOf<JSONObject?>(null) }
    var dialog by remember { mutableStateOf<Pair<String, JSONObject>?>(null) }
    val zs = remember(layers.toString()) { JSONArray().apply { for (i in 0 until n) put(layers.getDouble(i)) } }
    fun refresh(layer: Int = -1, then: (JSONObject) -> Unit = {}) =
        app.call("ticks.get", jobj("plate" to plate, "layers" to zs, "layer" to layer)) { r -> (r as? JSONObject)?.let { ticks = it.optJSONArray("ticks") ?: JSONArray(); then(it) } }
    LaunchedEffect(plate, zs.toString(), app.project.optBoolean("dirty")) { refresh() }
    fun apply() {
        if (oneLayer) lo = hi   // correct_higher_value: one layer mode keeps both values together
        Native.previewSet(jobj("layer_range" to JSONArray().put(lo).put(hi)).toString()); app.redraw()
    }
    // IMSlider::switch_one_layer_mode
    fun switchOneLayer() {
        oneLayer = !oneLayer
        if (!oneLayer) { oneLayerValue = hi; lo = 0; hi = n - 1 }
        else {
            if (oneLayerValue <= 0 || oneLayerValue > n - 1) { oneLayerValue = (n - 1) / 2; hi = oneLayerValue }
            else if (hi == n - 1) hi = oneLayerValue
        }
        active = 1
        apply()
    }
    // the slider keys (GLCanvas3D::handle_shortcut): the active thumb, the moves slider (crossing into the next layer at its
    // ends), one layer mode and Go to layer; Shift / Ctrl step 5
    LaunchedEffect(app.sliderKey) {
        val k = app.sliderKey ?: return@LaunchedEffect
        app.sliderKey = null
        val inc = if (k.optInt("step") != 0) 5 else 1
        val vr = app.preview.optJSONArray("view_range"); val fr = app.preview.optJSONArray("full_range")
        val vHi = vr?.optInt(1) ?: 0; val fLo = fr?.optInt(0) ?: 0; val fHi = fr?.optInt(1) ?: 0
        val last = 1L shl 40   // clamped to the end of the layer's moves
        fun set(view: Long?) {
            if (oneLayer) lo = hi
            val a = jobj("layer_range" to JSONArray().put(lo).put(hi))
            view?.let { a.put("view_range", JSONArray().put(0).put(it)) }
            Native.previewSet(a.toString()); app.redraw()
        }
        when (k.optString("what")) {
            "one_layer" -> switchOneLayer()
            "go_to_layer" -> refresh(hi) { r -> dialog = "jump" to r.put("layer", hi) }
            "up", "down" -> {
                val d = if (k.optString("what") == "up") inc else -inc
                if (active == 1 || oneLayer) { hi = (hi + d).coerceIn(if (oneLayer) 0 else lo, n - 1); set(last) }
                else { lo = (lo + d).coerceIn(0, hi); set(null) }
            }
            "left" -> if (vHi == fLo && hi > 0) { hi -= 1; if (lo > hi) lo = hi; set(last) } else set((vHi - inc).coerceAtLeast(fLo).toLong())
            "right" -> if (vHi == fHi && hi < n - 1) { hi += 1; set(0) } else set((vHi + inc).toLong())
            "start" -> set(0)
            "end" -> set(last)
        }
    }
    fun change(what: String, layer: Int, gcode: String? = null) {
        val a = jobj("plate" to plate, "layers" to zs, "layer" to layer, "what" to what)
        gcode?.let { a.put("gcode", it) }
        // Plater: the ticks changed -> the plate's G-code is generated again
        app.call("ticks.change", a) { r -> if (r != null) { refresh(); app.slice(plate) } }
    }
    val c = desk()
    val d = LocalDensity.current
    var trackH by remember { mutableFloatStateOf(1f) }
    val pad = with(d) { 22.dp.toPx() }
    fun yOf(layer: Int) = pad + (trackH - 2 * pad) * (1f - layer.toFloat() / (n - 1).coerceAtLeast(1))
    fun layerAt(y: Float) = ((1f - (y - pad) / (trackH - 2 * pad).coerceAtLeast(1f)) * (n - 1)).roundToInt().coerceIn(0, n - 1)
    Box(modifier.width(150.dp)) {
        // the track, the colored band between the handles and the ticks
        Canvas(Modifier.align(Alignment.CenterEnd).fillMaxHeight().width(40.dp)
            .onSizeChanged { trackH = it.height.toFloat() }
            .pointerInput(n) {
                detectTapGestures(
                    onTap = { o -> val l = layerAt(o.y); if (oneLayer) { hi = l; active = 1 } else if (abs(l - lo) < abs(l - hi) && l < hi) { lo = l; active = 0 } else { hi = maxOf(l, lo); active = 1 }; apply() },
                    onLongPress = { o ->
                        val l = layerAt(o.y)
                        val target = if (abs(l - lo) < abs(l - hi)) lo else hi
                        active = if (target == lo && lo != hi) 0 else 1
                        refresh(target) { m -> menu = m }
                    })
            }
            .pointerInput(n) {
                detectDragGestures(onDragStart = { o -> active = if (!oneLayer && abs(o.y - yOf(lo)) < abs(o.y - yOf(hi)) && lo != hi) 0 else 1 }) { ch, _ ->
                    val l = layerAt(ch.position.y)
                    if (oneLayer) hi = l else if (active == 0) lo = l.coerceAtMost(hi) else hi = l.coerceAtLeast(lo)
                    apply()
                }
            }) {
            val x = size.width / 2
            drawLine(c.border, Offset(x, pad), Offset(x, size.height - pad), strokeWidth = 4.dp.toPx())
            drawLine(OrcaColors.accent, Offset(x, yOf(hi)), Offset(x, yOf(lo)), strokeWidth = 4.dp.toPx())
            for (i in 0 until ticks.length()) {
                val t = ticks.getJSONObject(i)
                val y = yOf(t.optInt("layer"))
                val col = when (t.optInt("type")) {
                    PAUSE -> Color(0xFFF1754E); CUSTOM, TEMPLATE -> Color(0xFF6A7FA0)
                    else -> parseColor(t.optString("color")) ?: OrcaColors.accent
                }
                drawLine(col, Offset(x - 10.dp.toPx(), y), Offset(x + 10.dp.toPx(), y), strokeWidth = 3.dp.toPx())
            }
            for ((l, sel) in if (oneLayer) listOf(hi to true) else listOf(lo to (active == 0), hi to (active == 1))) {
                drawCircle(Color.White, 9.dp.toPx(), Offset(x, yOf(l)))
                drawCircle(if (sel) OrcaColors.accent else c.textDim, 9.dp.toPx(), Offset(x, yOf(l)), style = androidx.compose.ui.graphics.drawscope.Stroke(3.dp.toPx()))
            }
        }
        // the labels of the handles (height and layer number), left of the track
        for ((l, sel) in listOf(hi to (active == 1), lo to (active == 0))) {
            if (l == lo && lo == hi && !sel) continue
            val yDp = with(d) { yOf(l).toDp() }
            Box(Modifier.offset(y = yDp - 18.dp).padding(end = 44.dp).align(Alignment.TopEnd)
                .background(if (sel) OrcaColors.accent else c.panel, RoundedCornerShape(4.dp)).padding(horizontal = 6.dp, vertical = 2.dp)) {
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    Text("%.2f".format(zs.optDouble(l)), style = deskText(12, color = if (sel) Color.White else c.text))
                    Text("${l + 1}", style = deskText(11, color = if (sel) Color.White else c.textDim))
                }
            }
        }
        // the one layer mode button below the slider (IMSlider: one_layer_on / one_layer_off)
        Box(Modifier.align(Alignment.BottomEnd).offset(y = 44.dp).size(40.dp).clickable { switchOneLayer() }, Alignment.Center) {
            OrcaIcon(if (oneLayer) "one_layer_on" else "one_layer_off", 32.dp)
        }
        // the handle menu (IMSlider::render_add_menu / render_edit_menu)
        menu?.let { m ->
            val layer = m.optInt("layer")
            val items = m.optJSONArray("menu") ?: JSONArray()
            val yDp = with(d) { yOf(layer).toDp() }
            Box(Modifier.align(Alignment.TopEnd).offset(y = yDp)) {
                DropdownMenu(true, { menu = null }, offset = DpOffset((-60).dp, 0.dp)) {
                    for (i in 0 until items.length()) {
                        val it = items.getJSONObject(i)
                        val sub = it.optJSONArray("children")
                        if (sub != null) {
                            var open by remember { mutableStateOf(false) }
                            Box {
                                DropdownMenuItem(text = { Text(it.optString("label"), style = deskText(14)) }, enabled = it.optBoolean("enabled", true),
                                    onClick = { open = true }, trailingIcon = { Text("\u203A", style = deskText(16)) })
                                org.orcaport.mobile.ui.SubMenuPopup(open, { open = false }) {
                                    for (k in 0 until sub.length()) {
                                        val s = sub.getJSONObject(k)
                                        DropdownMenuItem(text = { Text(s.optString("label"), style = deskText(14)) }, onClick = { open = false; menu = null; change(s.optString("id"), layer) },
                                            leadingIcon = { Box(Modifier.size(14.dp).background(parseColor(s.optString("color")) ?: Color.White, RoundedCornerShape(2.dp))) })
                                    }
                                }
                            }
                        } else DropdownMenuItem(text = { Text(it.optString("label"), style = deskText(14)) }, enabled = it.optBoolean("enabled", true), onClick = {
                            menu = null
                            when (val id = it.optString("id")) {
                                "custom", "edit", "jump" -> dialog = id to m
                                else -> change(id, layer)
                            }
                        })
                    }
                }
            }
        }
    }
    // IMSlider::render_input_custom_gcode / render_go_to_layer_dialog
    dialog?.let { (kind, m) ->
        val labels = m.optJSONObject("labels") ?: JSONObject()
        var text by remember { mutableStateOf(if (kind == "jump") "" else m.optString("custom")) }
        DeskDialog(if (kind == "jump") labels.optString("jump_title") else labels.optString("custom_title"), onDismiss = { dialog = null }, ok = {
            dialog = null
            if (kind == "jump") text.toIntOrNull()?.let { v -> val l = (v - 1).coerceIn(0, n - 1); if (active == 0) lo = l.coerceAtMost(hi) else hi = l.coerceAtLeast(lo); apply() }
            else if (text.isNotBlank()) change("custom", m.optInt("layer"), text)
        }) {
            Text(if (kind == "jump") labels.optString("jump_prompt") else labels.optString("custom_prompt"), style = deskText(13))
            OutlinedTextField(text, { text = it }, singleLine = kind == "jump", minLines = if (kind == "jump") 1 else 5, modifier = Modifier.fillMaxWidth())
        }
    }
}
