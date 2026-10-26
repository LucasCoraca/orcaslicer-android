package org.orcaport.mobile.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.TextMeasurer
import androidx.compose.ui.text.drawText
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import org.json.JSONArray
import org.json.JSONObject
import org.orcaport.mobile.AppState
import org.orcaport.mobile.engine.Ask
import org.orcaport.mobile.prepare.FilamentSwatch
import org.orcaport.mobile.prepare.PanelButton
import org.orcaport.mobile.settings.parseColor
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt
import kotlin.math.sqrt

private const val MAX_COMPONENTS = 3
private const val MIN_COMPONENT_RATIO = 10
private const val GRADIENT_MIN = 0.1
private const val GRADIENT_MAX = 0.9

/** GradientAnchor: x, y in [0,1], optional tangents (NaN = PCHIP default) */
private data class Anchor(val x: Double, val y: Double, val mIn: Double = Double.NaN, val mOut: Double = Double.NaN)

private fun JSONArray.ints() = (0 until length()).map { getInt(it) }
private fun JSONArray.strings() = (0 until length()).map { getString(it) }
private fun anchorsJson(a: List<Anchor>) = JSONArray().apply {
    for (p in a) put(JSONArray().put(p.x).put(p.y).put(if (p.mIn.isNaN()) JSONObject.NULL else p.mIn).put(if (p.mOut.isNaN()) JSONObject.NULL else p.mOut))
}

/**
 * MixedFilamentDialog: the components (2-3 physical filaments of one type) and their ratios (the ratio bar for two,
 * the triangle picker for three), the gradient effect over the model's height with its curve (GradientCurveEditor),
 * the mixing recommendations; the colour math is the engine's (mixed.* pure commands).
 */
@Composable
fun MixedFilamentAsk(app: AppState, ask: Ask) {
    val r = ask.request
    val c = desk()
    val colors = remember(ask) { r.optJSONArray("colors")?.strings() ?: emptyList() }
    val names = remember(ask) { r.optJSONArray("names")?.strings() ?: emptyList() }
    val types = remember(ask) { r.optJSONArray("types")?.strings() ?: emptyList() }
    val comps = remember(ask) { mutableStateListOf<Int>().apply { addAll(r.optJSONArray("components")?.ints() ?: listOf(1, 2)) } }
    val ratios = remember(ask) { mutableStateListOf<Int>().apply { addAll(r.optJSONArray("ratios")?.ints() ?: listOf(50, 50)) } }
    var gradient by remember(ask) { mutableStateOf(r.optBoolean("gradient")) }
    var direction by remember(ask) { mutableIntStateOf(r.optInt("direction")) }
    var perPart by remember(ask) { mutableStateOf(r.optBoolean("per_part")) }
    // the curve the result carries (empty until edited: a default curve is not stored) and the editor's own points
    var resultCurve by remember(ask) {
        mutableStateOf(r.optJSONArray("curve")?.let { a -> (0 until a.length()).map { val p = a.getJSONArray(it); Anchor(p.getDouble(0), p.getDouble(1), p.optDouble(2), p.optDouble(3)) } } ?: emptyList())
    }
    var editorCurve by remember(ask) {
        mutableStateOf(resultCurve.ifEmpty { if (direction == 0) listOf(Anchor(0.0, 0.9), Anchor(1.0, 0.1)) else listOf(Anchor(0.0, 0.1), Anchor(1.0, 0.9)) })
    }
    var ratioEdit by remember(ask) { mutableStateOf<Int?>(null) }
    fun compColor(i: Int): String = comps.getOrNull(i)?.let { k -> colors.getOrNull(k - 1) } ?: "#D9D9D9"
    fun reply(ok: Boolean) {
        if (!ok) { ask.reply(JSONObject().put("ok", false)); return }
        ask.reply(JSONObject().put("ok", true).put("components", JSONArray(comps.toList())).put("ratios", JSONArray(ratios.toList()))
            .put("gradient", gradient).put("direction", direction).put("per_part", perPart).put("curve", anchorsJson(resultCurve)))
    }
    val n = comps.size
    val isTwo = n == 2
    val isThree = n >= 3

    // ---- MixedFilamentDialog::apply_manual_ratio: a typed ratio, the others rescaled to the rest
    fun applyManualRatio(idx: Int, v: Int) {
        if (idx >= n) return
        if (ratios.size != n) { ratios.clear(); repeat(n) { ratios.add(100 / n) } }
        val maxValue = 100 - (n - 1) * MIN_COMPONENT_RATIO
        val value = v.coerceIn(MIN_COMPONENT_RATIO, max(MIN_COMPONENT_RATIO, maxValue))
        if (n == 2) {
            if (idx == 0) { ratios[0] = value; ratios[1] = 100 - value } else { ratios[1] = value; ratios[0] = 100 - value }
            ratios[0] = ratios[0].coerceIn(MIN_COMPONENT_RATIO, 100 - MIN_COMPONENT_RATIO)
            ratios[1] = 100 - ratios[0]
        } else {
            ratios[idx] = value
            val remaining = 100 - value
            val others = (0 until n).filter { it != idx }
            val othersSum = others.sumOf { ratios[it] }
            if (othersSum > 0) {
                var assigned = 0
                for (k in others) { val nv = max(((remaining.toDouble() * ratios[k] / othersSum) + 0.5).toInt(), MIN_COMPONENT_RATIO); ratios[k] = nv; assigned += nv }
                while (assigned != remaining) {
                    if (assigned > remaining) {
                        val pick = others.filter { ratios[it] > MIN_COMPONENT_RATIO }.maxByOrNull { ratios[it] } ?: break
                        ratios[pick]--; assigned--
                    } else {
                        val pick = others.maxBy { ratios[it] }
                        ratios[pick]++; assigned++
                    }
                }
            } else {
                val base = remaining / others.size
                for (k in others) ratios[k] = base
                ratios[others.last()] += remaining - base * others.size
            }
        }
    }
    // apply_dragged_triangle_ratio: clamped to the minimum, the sum brought back to 100
    fun applyTriangle(r0: Int, r1: Int, r2: Int) {
        val rs = intArrayOf(r0.coerceIn(MIN_COMPONENT_RATIO, 100), r1.coerceIn(MIN_COMPONENT_RATIO, 100), r2.coerceIn(MIN_COMPONENT_RATIO, 100))
        var sum = rs.sum()
        while (sum > 100) { val i = rs.indices.maxBy { rs[it] }; if (rs[i] <= MIN_COMPONENT_RATIO) break; rs[i]--; sum-- }
        while (sum < 100) { val i = rs.indices.minBy { rs[it] }; rs[i]++; sum++ }
        for (i in 0..2) ratios[i] = rs[i]
    }
    fun setGradient(on: Boolean) { gradient = on; if (!on) perPart = false }

    // ---- the colours (engine)
    var preview by remember { mutableStateOf("#808080") }
    var curveYs by remember { mutableStateOf<List<Double>>(emptyList()) }
    var previewRamp by remember { mutableStateOf<List<String>>(emptyList()) }
    LaunchedEffect(comps.toList(), ratios.toList(), gradient, editorCurve) {
        runCatching {
            if (gradient && isTwo) {
                val res = app.engine.pure("mixed.curve", JSONObject().put("curve", anchorsJson(editorCurve)).put("steps", 80)
                    .put("first", compColor(0)).put("second", compColor(1))) as JSONObject
                previewRamp = res.getJSONArray("ramp").strings()
            } else {
                preview = app.engine.pure("mixed.blend", JSONObject().put("colors", JSONArray((0 until n).map { compColor(it) }))
                    .put("weights", JSONArray((0 until n).map { ratios.getOrElse(it) { 0 } / 100.0 }))) as String
            }
            val ys = app.engine.pure("mixed.curve", JSONObject().put("curve", anchorsJson(editorCurve)).put("steps", 128)) as JSONObject
            curveYs = (ys.getJSONArray("y")).let { a -> (0 until a.length()).map { a.getDouble(it) } }
        }
    }

    // ---- the OK state: no unselected component, one filament type (update_ok_button_state)
    val groups = comps.filter { it >= 1 && it <= types.size }.groupBy { types[it - 1] }
    val mismatch = types.isNotEmpty() && comps.size >= 2 && groups.size > 1
    val unselected = comps.any { it == 0 }
    val mismatchText = if (mismatch) tr("Different filament types cannot be mixed: %s. Please select the same filament type.")
        .replace("%s", groups.entries.joinToString("; ") { (t, ids) -> tr("Slot %s (%s)").replaceFirst("%s", ids.joinToString(", ")).replaceFirst("%s", t) }) else ""

    Dialog({ reply(false) }, DialogProperties(usePlatformDefaultWidth = false, decorFitsSystemWindows = false)) {
        val width = if (isTwo && gradient) 470.dp else 439.dp
        Column(Modifier.width(width).heightIn(max = 760.dp).fillMaxHeight(0.94f).clip(RoundedCornerShape(6.dp)).background(c.panel).imePadding()) {
            Row(Modifier.fillMaxWidth().padding(start = 20.dp, end = 8.dp, top = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                Text(r.optString("title"), Modifier.weight(1f), style = deskText(15))
                Text("✕", Modifier.clickable { reply(false) }.padding(8.dp), style = deskText(15, color = c.textDim))
            }
            Column(Modifier.weight(1f).verticalScroll(rememberScrollState())) {
                Row(Modifier.fillMaxWidth()) {
                    // Effect Preview
                    Column(Modifier.padding(20.dp), horizontalAlignment = Alignment.CenterHorizontally) {
                        Box(Modifier.size(129.dp), Alignment.Center) {
                            if (gradient && isTwo && previewRamp.isNotEmpty())
                                Box(Modifier.size(80.dp).clip(RoundedCornerShape(6.dp))
                                    .background(androidx.compose.ui.graphics.Brush.verticalGradient(previewRamp.reversed().map { parseColor(it) ?: Color.Gray })))
                            else Box(Modifier.size(80.dp).clip(RoundedCornerShape(6.dp)).background(parseColor(preview) ?: Color.Gray))
                        }
                        Text(tr("Effect Preview"), style = deskText(13, color = Color(0xFF6B6A6A)))
                    }
                    Column(Modifier.weight(1f).padding(top = 20.dp, end = 20.dp, bottom = 20.dp)) {
                        // the summary: swatch + ratio, "+" between (A → B for a gradient)
                        Row(Modifier.fillMaxWidth().height(40.dp).background(c.field).padding(horizontal = 13.dp), verticalAlignment = Alignment.CenterVertically) {
                            if (gradient && isTwo) {
                                val a = if (direction == 0) 0 else 1
                                FilamentSwatch(compColor(a), "${comps[a]}", 20.dp); Text(" → ", style = deskText(13)); FilamentSwatch(compColor(1 - a), "${comps[1 - a]}", 20.dp)
                            } else for (i in 0 until n) {
                                if (i > 0) Text(" + ", style = deskText(13))
                                FilamentSwatch(compColor(i), "${comps[i]}", 20.dp)
                                Text(" ${ratios.getOrElse(i) { 0 }}%", style = deskText(13))
                            }
                        }
                        Text(tr("Select Mixed Materials"), Modifier.padding(top = 6.dp), style = deskText(12, color = Color(0xFF6B6A6A)))
                        for (i in 0 until n) MaterialRow(i, comps, names, colors, types) { sel -> comps[i] = sel }
                        Row(Modifier.padding(top = 9.dp), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                            val canAdd = n < MAX_COMPONENTS && colors.size > n
                            Box(Modifier.weight(1f)) {
                                PanelButton(tr("+ Add Material"), enabled = canAdd) {
                                    // on_add_material: the first unused filament, even ratios
                                    val next = (1..names.size).firstOrNull { it !in comps } ?: return@PanelButton
                                    comps.add(next)
                                    val each = 100 / comps.size
                                    ratios.clear(); repeat(comps.size - 1) { ratios.add(each) }; ratios.add(100 - each * (comps.size - 1))
                                    if (comps.size >= 3) setGradient(false)
                                }
                            }
                            if (isThree) Box(Modifier.weight(1f)) {
                                PanelButton(tr("- Delete Material")) {
                                    while (comps.size > 2) comps.removeAt(comps.size - 1)
                                    ratios.clear(); ratios.addAll(listOf(50, 50))
                                }
                            }
                        }
                        // Gradient Effect (two components only)
                        if (isTwo) {
                            Row(Modifier.padding(top = 7.dp), verticalAlignment = Alignment.CenterVertically) {
                                DeskCheck(gradient) { setGradient(it) }
                                Text(tr("Gradient Effect"), Modifier.padding(start = 4.dp), style = deskText(13))
                                if (gradient) {
                                    Spacer(Modifier.width(8.dp))
                                    DirectionChoice(direction, comps, colors) { d ->
                                        if (d != direction) {
                                            direction = d
                                            // on_gradient_direction_changed: the curve mirrored around 0.5
                                            editorCurve = editorCurve.map { Anchor(it.x, 1 - it.y, if (it.mIn.isNaN()) it.mIn else -it.mIn, if (it.mOut.isNaN()) it.mOut else -it.mOut) }
                                            resultCurve = editorCurve
                                        }
                                    }
                                }
                            }
                            if (gradient) {
                                GradientCurveEditor(editorCurve, curveYs, parseColor(compColor(0)) ?: Color.Gray, parseColor(compColor(1)) ?: Color.Gray) { pts ->
                                    editorCurve = pts; resultCurve = pts
                                }
                                Row(Modifier.padding(top = 2.dp), verticalAlignment = Alignment.CenterVertically) {
                                    DeskCheck(perPart) { perPart = it }
                                    Text(tr("Enable per-part gradient effect"), Modifier.padding(start = 4.dp), style = deskText(13))
                                }
                            }
                        }
                        // Ratio: the blend bar of two components
                        if (isTwo && !gradient) RatioBar(app, compColor(0), compColor(1), ratios.getOrElse(1) { 50 }, ratios.getOrElse(0) { 50 },
                            onRatio = { a -> ratios[0] = a; ratios[1] = 100 - a }, onEdit = { ratioEdit = it })
                        if (isThree) TrianglePicker(app, listOf(compColor(0), compColor(1), compColor(2)), ratios.toList(),
                            onRatios = { a, b, d -> applyTriangle(a, b, d) }, onEdit = { ratioEdit = it })
                    }
                }
                Recommendations(app, colors, names, types, isThree) { picked ->
                    comps.clear(); comps.addAll(picked)
                    ratios.clear(); ratios.addAll(if (picked.size == 3) listOf(25, 25, 50) else listOf(50, 50))
                    if (picked.size == 3) setGradient(false)
                }
                if (mismatch) Row(Modifier.padding(start = 25.dp, end = 25.dp, top = 12.dp).fillMaxWidth().heightIn(min = 48.dp)
                    .border(1.dp, Color(0xFFD01B1B), RoundedCornerShape(4.dp)).background(c.field, RoundedCornerShape(4.dp)).padding(10.dp),
                    verticalAlignment = Alignment.CenterVertically) {
                    Box(Modifier.size(14.dp).clip(CircleShape).background(Color(0xFFD01B1B)), Alignment.Center) { Text("!", style = deskText(10, color = Color.White)) }
                    Text(mismatchText, Modifier.padding(start = 6.dp), style = deskText(12, color = Color(0xFFD01B1B)))
                }
            }
            Row(Modifier.fillMaxWidth().padding(20.dp), horizontalArrangement = Arrangement.End) {
                PanelButton(tr("Cancel")) { reply(false) }
                Spacer(Modifier.width(12.dp))
                PanelButton(tr("OK"), primary = true, enabled = !mismatch && !unselected) { reply(true) }
            }
        }
    }
    ratioEdit?.let { idx ->
        // RatioLabelPanel's editor: the typed percentage
        var text by remember(idx) { mutableStateOf("${ratios.getOrElse(idx) { 0 }}") }
        org.orcaport.mobile.prepare.DeskDialog(tr("Ratio"), onDismiss = { ratioEdit = null }, ok = {
            text.trim().removeSuffix("%").toIntOrNull()?.let { applyManualRatio(idx, it) }
            ratioEdit = null
        }) {
            DeskInput(text, enabled = true, numeric = true, modifier = Modifier.width(120.dp), sidetext = "%") { text = it }
        }
    }
}

/** "Filament N" and its combo: the physical filaments not used by the other rows, other types dimmed */
@Composable
private fun MaterialRow(i: Int, comps: List<Int>, names: List<String>, colors: List<String>, types: List<String>, onSelect: (Int) -> Unit) {
    val c = desk()
    var open by remember { mutableStateOf(false) }
    val others = comps.filterIndexed { k, _ -> k != i }.toSet()
    val otherTypes = others.mapNotNull { types.getOrNull(it - 1) }.toSet()
    val cur = comps.getOrElse(i) { 0 }
    Row(Modifier.padding(top = 9.dp), verticalAlignment = Alignment.CenterVertically) {
        Text(tr("Filament %d").replace("%d", "${i + 1}"), Modifier.width(72.dp), style = deskText(12))
        Box(Modifier.weight(1f)) {
            DeskCombo(if (cur == 0) tr("-- Select --") else names.getOrElse(cur - 1) { "" }, Modifier.fillMaxWidth(), height = 32.dp,
                leading = if (cur >= 1) ({ FilamentSwatch(colors.getOrElse(cur - 1) { "#D9D9D9" }, "$cur", 20.dp) }) else null) { open = true }
            DropdownMenu(open, { open = false }) {
                for (j in names.indices) {
                    val id = j + 1
                    if (id in others) continue
                    val dim = otherTypes.isNotEmpty() && types.isNotEmpty() && types.getOrElse(j) { "PLA" } !in otherTypes
                    DropdownMenuItem(text = { Text(names[j], style = deskText(14, color = if (dim) c.textDim else c.text)) },
                        leadingIcon = { FilamentSwatch(colors.getOrElse(j) { "#D9D9D9" }, "$id", 20.dp) },
                        onClick = { open = false; onSelect(id) })
                }
            }
        }
    }
}

/** the gradient direction: A → B or B → A */
@Composable
private fun DirectionChoice(direction: Int, comps: List<Int>, colors: List<String>, onSelect: (Int) -> Unit) {
    var open by remember { mutableStateOf(false) }
    val a = comps.getOrElse(0) { 1 }; val b = comps.getOrElse(1) { 2 }
    @Composable fun item(from: Int, to: Int) = Row(verticalAlignment = Alignment.CenterVertically) {
        FilamentSwatch(colors.getOrElse(from - 1) { "#D9D9D9" }, "$from", 20.dp); Text(" → ", style = deskText(13)); FilamentSwatch(colors.getOrElse(to - 1) { "#D9D9D9" }, "$to", 20.dp)
    }
    Box {
        Row(Modifier.width(152.dp).height(32.dp).clip(RoundedCornerShape(4.dp)).background(desk().field).border(1.dp, desk().border, RoundedCornerShape(4.dp))
            .clickable { open = true }.padding(horizontal = 8.dp), verticalAlignment = Alignment.CenterVertically) {
            OrcaIcon("drop_down", 12.dp); Spacer(Modifier.width(8.dp))
            if (direction == 0) item(a, b) else item(b, a)
        }
        DropdownMenu(open, { open = false }) {
            DropdownMenuItem(text = { item(a, b) }, onClick = { open = false; onSelect(0) })
            DropdownMenuItem(text = { item(b, a) }, onClick = { open = false; onSelect(1) })
        }
    }
}

/** draw_mixed_ratio_blend_bar: the lerp from A to B, the divider at B's share; drag to set, the labels to type */
@Composable
private fun RatioBar(app: AppState, first: String, second: String, secondPct: Int, firstPct: Int, onRatio: (Int) -> Unit, onEdit: (Int) -> Unit) {
    var ramp by remember { mutableStateOf<List<Color>>(emptyList()) }
    LaunchedEffect(first, second) {
        runCatching {
            ramp = (app.engine.pure("mixed.lerp_ramp", JSONObject().put("from", first).put("to", second).put("steps", 96)) as JSONArray).strings().map { parseColor(it) ?: Color.Gray }
        }
    }
    Column(Modifier.padding(top = 7.dp)) {
        Text(tr("Ratio"), Modifier.padding(bottom = 4.dp), style = deskText(12, color = Color(0xFF6B6A6A)))
        Canvas(Modifier.fillMaxWidth().height(27.dp).pointerInput(Unit) {
            fun set(x: Float) = onRatio((100 - (x * 100f / size.width + 0.5f).toInt()).coerceIn(MIN_COMPONENT_RATIO, 100 - MIN_COMPONENT_RATIO))
            detectTapGestures { set(it.x) }
        }.pointerInput(Unit) {
            detectDragGestures { ch, _ -> onRatio((100 - (ch.position.x * 100f / size.width + 0.5f).toInt()).coerceIn(MIN_COMPONENT_RATIO, 100 - MIN_COMPONENT_RATIO)) }
        }) {
            if (ramp.isNotEmpty()) {
                val w = size.width / ramp.size
                ramp.forEachIndexed { i, col -> drawRect(col, Offset(i * w, 0f), Size(w + 1f, size.height)) }
            }
            val x = secondPct / 100f * size.width
            drawLine(Color(0xFF505050), Offset(x, 0f), Offset(x, size.height), 4.dp.toPx())
            drawLine(Color.White, Offset(x, 0f), Offset(x, size.height), 2.dp.toPx())
        }
        Row(Modifier.fillMaxWidth().padding(top = 2.dp)) {
            Text("$firstPct%", Modifier.clickable { onEdit(0) }.padding(4.dp), style = deskText(12))
            Spacer(Modifier.weight(1f))
            Text("$secondPct%", Modifier.clickable { onEdit(1) }.padding(4.dp), style = deskText(12))
        }
    }
}

/** the triangle picker: barycentric weights of three components, the fill blended by the engine */
@Composable
private fun TrianglePicker(app: AppState, cols: List<String>, ratios: List<Int>, onRatios: (Int, Int, Int) -> Unit, onEdit: (Int) -> Unit) {
    val density = LocalDensity.current
    val side = 120.dp
    val triH = side * (sqrt(3.0) / 2).toFloat()
    var bitmap by remember { mutableStateOf<androidx.compose.ui.graphics.ImageBitmap?>(null) }
    val wPx = with(density) { side.roundToPx() }; val hPx = with(density) { triH.roundToPx() }
    LaunchedEffect(cols) {
        runCatching {
            val res = app.engine.pure("mixed.triangle", JSONObject().put("colors", JSONArray(cols)).put("width", min(wPx, 360)).put("height", min(hPx, 312))) as JSONObject
            val w = res.getInt("width"); val h = res.getInt("height"); val px = res.getJSONArray("pixels")
            val arr = IntArray(w * h) { px.getInt(it) }
            bitmap = android.graphics.Bitmap.createBitmap(arr, w, h, android.graphics.Bitmap.Config.ARGB_8888).asImageBitmap()
        }
    }
    val sum = ratios.take(3).sum().coerceAtLeast(1)
    val wx = ratios.getOrElse(0) { 33 }.toDouble() / sum; val wy = ratios.getOrElse(1) { 33 }.toDouble() / sum
    Column(Modifier.padding(top = 7.dp).fillMaxWidth(), horizontalAlignment = Alignment.CenterHorizontally) {
        Text("${ratios.getOrElse(0) { 0 }}%", Modifier.clickable { onEdit(0) }.padding(3.dp), style = deskText(11))
        Box(Modifier.size(side, triH)) {
            bitmap?.let { Image(it, null, Modifier.fillMaxSize()) }
            Canvas(Modifier.fillMaxSize().pointerInput(Unit) {
                fun pick(p: Offset, start: Boolean) {
                    val v0 = Offset(size.width / 2f, 0f); val v1 = Offset(0f, size.height.toFloat()); val v2 = Offset(size.width.toFloat(), size.height.toFloat())
                    val det = (v1.y - v2.y) * (v0.x - v2.x) + (v2.x - v1.x) * (v0.y - v2.y)
                    var l0 = ((v1.y - v2.y) * (p.x - v2.x) + (v2.x - v1.x) * (p.y - v2.y)) / det
                    var l1 = ((v2.y - v0.y) * (p.x - v2.x) + (v0.x - v2.x) * (p.y - v2.y)) / det
                    var l2 = 1 - l0 - l1
                    if (start && (l0 < 0 || l1 < 0 || l2 < 0)) return
                    // tri_clamp: back onto the triangle
                    l0 = l0.coerceAtLeast(0f); l1 = l1.coerceAtLeast(0f); l2 = l2.coerceAtLeast(0f)
                    val s = l0 + l1 + l2
                    val r0 = (l0 / s * 100 + 0.5f).toInt(); val r1 = (l1 / s * 100 + 0.5f).toInt()
                    onRatios(r0.coerceIn(0, 100), r1.coerceIn(0, 100), (100 - r0 - r1).coerceIn(0, 100))
                }
                detectTapGestures { pick(it, true) }
            }.pointerInput(Unit) {
                var inside = false
                detectDragGestures(onDragStart = { inside = true }, onDragEnd = { inside = false }) { ch, _ ->
                    if (!inside) return@detectDragGestures
                    val v0 = Offset(size.width / 2f, 0f); val v1 = Offset(0f, size.height.toFloat()); val v2 = Offset(size.width.toFloat(), size.height.toFloat())
                    val p = ch.position
                    val det = (v1.y - v2.y) * (v0.x - v2.x) + (v2.x - v1.x) * (v0.y - v2.y)
                    val l0 = (((v1.y - v2.y) * (p.x - v2.x) + (v2.x - v1.x) * (p.y - v2.y)) / det).coerceAtLeast(0f)
                    val l1 = (((v2.y - v0.y) * (p.x - v2.x) + (v0.x - v2.x) * (p.y - v2.y)) / det).coerceAtLeast(0f)
                    val l2 = (1 - (((v1.y - v2.y) * (p.x - v2.x) + (v2.x - v1.x) * (p.y - v2.y)) / det) - (((v2.y - v0.y) * (p.x - v2.x) + (v0.x - v2.x) * (p.y - v2.y)) / det)).coerceAtLeast(0f)
                    val s = (l0 + l1 + l2).coerceAtLeast(1e-6f)
                    val r0 = (l0 / s * 100 + 0.5f).toInt(); val r1 = (l1 / s * 100 + 0.5f).toInt()
                    onRatios(r0, r1, 100 - r0 - r1)
                }
            }) {
                val v0 = Offset(size.width / 2f, 0f); val v1 = Offset(0f, size.height); val v2 = Offset(size.width, size.height)
                val path = Path().apply { moveTo(v0.x, v0.y); lineTo(v1.x, v1.y); lineTo(v2.x, v2.y); close() }
                drawPath(path, Color(0xFFCECECE), style = Stroke(1.dp.toPx()))
                // the drag handle at the weights
                val wz = 1 - wx - wy
                val p = Offset((v0.x * wx + v1.x * wy + v2.x * wz).toFloat(), (v0.y * wx + v1.y * wy + v2.y * wz).toFloat())
                drawCircle(Color.White, 6.dp.toPx(), p); drawCircle(Color(0xFF262E30), 6.dp.toPx(), p, style = Stroke(2.dp.toPx()))
            }
        }
        Row(Modifier.width(side + 40.dp)) {
            Text("${ratios.getOrElse(1) { 0 }}%", Modifier.clickable { onEdit(1) }.padding(3.dp), style = deskText(11))
            Spacer(Modifier.weight(1f))
            Text("${ratios.getOrElse(2) { 0 }}%", Modifier.clickable { onEdit(2) }.padding(3.dp), style = deskText(11))
        }
    }
}

/**
 * GradientCurveEditor: "Model Height" → "Material Ratio" of the first component; both components' curves, the selected
 * one thick with its anchors. Drag an anchor to move it; drag from the selected curve to add one there; tap the other
 * curve to select it; long press an interior anchor to delete it (the desktop's right click).
 */
@Composable
private fun GradientCurveEditor(points: List<Anchor>, ys: List<Double>, low: Color, high: Color, onChange: (List<Anchor>) -> Unit) {
    val c = desk()
    val measurer = rememberTextMeasurer()
    var selected by remember { mutableIntStateOf(0) }
    var pts by remember(points) { mutableStateOf(points) }
    var dragIdx by remember { mutableIntStateOf(-1) }
    // the gesture handlers stay alive across edits (a restart would end the drag that inserted an anchor)
    val samples by rememberUpdatedState(ys)
    val change by rememberUpdatedState(onChange)
    val labelStyle = deskText(10, color = c.textDim)
    val strongStyle = deskText(10, FontWeight.SemiBold, color = c.text)
    val yRatio = tr("Material Ratio"); val xRatio = tr("Model Height")
    BoxWithConstraints(Modifier.fillMaxWidth().padding(top = 4.dp).height(200.dp)) {
        val wPx = constraints.maxWidth.toFloat(); val hPx = constraints.maxHeight.toFloat()
        // mixed_gradient_plot_rect: a square plot
        val left = wPx * 0.0316f; val top = hPx * 0.1529f
        val sidePx = min(wPx * 0.6766f - left, hPx * 0.8474f - top).coerceAtLeast(1f)
        fun toPx(x: Double, y: Double) = Offset(left + (x * sidePx).toFloat(), top + ((1 - y) * sidePx).toFloat())
        fun toData(p: Offset) = Pair(((p.x - left) / sidePx).toDouble().coerceIn(0.0, 1.0), (1 - (p.y - top) / sidePx).toDouble().coerceIn(0.0, 1.0))
        fun visual(curve: Int, y: Double) = if (curve == 0) y else 1 - y
        fun sample(x: Double): Double = samples.let { y -> if (y.size > 1) y[(x * (y.size - 1)).roundToInt().coerceIn(0, y.size - 1)] else 0.5 }
        val hitR = with(LocalDensity.current) { 14.dp.toPx() }
        fun hitAnchor(p: Offset): Int = pts.indices.minByOrNull { (toPx(pts[it].x, visual(selected, pts[it].y)) - p).getDistance() }
            ?.takeIf { (toPx(pts[it].x, visual(selected, pts[it].y)) - p).getDistance() <= hitR } ?: -1
        fun hitCurve(p: Offset): Int {
            val (x, _) = toData(p)
            for (curve in listOf(selected, 1 - selected)) if ((toPx(x, visual(curve, sample(x))) - p).getDistance() <= hitR) return curve
            return -1
        }
        Canvas(Modifier.fillMaxSize().pointerInput(Unit) {
            detectTapGestures(onLongPress = { p ->
                val i = hitAnchor(p)
                if (i > 0 && i < pts.size - 1) { pts = pts.toMutableList().also { it.removeAt(i) }; change(pts) }
            }, onTap = { p ->
                val curve = hitCurve(p)
                if (curve >= 0 && curve != selected && hitAnchor(p) < 0) selected = curve
            })
        }.pointerInput(Unit) {
            detectDragGestures(onDragStart = { p ->
                dragIdx = hitAnchor(p)
                if (dragIdx >= 0) {
                    // an anchor dragged returns to the PCHIP shape around it
                    pts = pts.toMutableList().also { it[dragIdx] = it[dragIdx].copy(mIn = Double.NaN, mOut = Double.NaN) }
                } else if (hitCurve(p) == selected) {
                    val (x, _) = toData(p)
                    if (x > 0 && x < 1) {
                        val seg = (1 until pts.size).firstOrNull { x <= pts[it].x }?.minus(1) ?: (pts.size - 2)
                        pts = pts.toMutableList().also { it.add(seg + 1, Anchor(x, sample(x))) }
                        dragIdx = seg + 1
                    }
                }
            }, onDragEnd = { if (dragIdx >= 0) change(pts); dragIdx = -1 }, onDragCancel = { dragIdx = -1 }) { ch, _ ->
                val i = dragIdx
                if (i < 0 || i >= pts.size) return@detectDragGestures
                val (nx0, vy) = toData(ch.position)
                var nx = pts[i].x
                if (i != 0 && i != pts.size - 1) nx = nx0.coerceIn(pts[i - 1].x + 1e-4, pts[i + 1].x - 1e-4)
                val stored = (if (selected == 0) vy else 1 - vy).coerceIn(GRADIENT_MIN, GRADIENT_MAX)
                pts = pts.toMutableList().also { it[i] = it[i].copy(x = nx, y = stored) }
            }
        }) {
            val grid = c.border; val axis = Color(0xFF6B6B6B)
            for (k in 0..9) {
                val x = left + sidePx * k / 9; val y = top + sidePx * k / 9
                drawLine(grid, Offset(x, top), Offset(x, top + sidePx)); drawLine(grid, Offset(left, y), Offset(left + sidePx, y))
            }
            val arrow = 10.dp.toPx(); val half = 5.dp.toPx()
            val yTitle = measurer.measure(yRatio, labelStyle); val pct = measurer.measure("100%", strongStyle); val xTitle = measurer.measure(xRatio, labelStyle)
            val yTitleY = max(0f, top - yTitle.size.height - pct.size.height - 3.dp.toPx())
            drawLine(axis, Offset(left, yTitleY + arrow), Offset(left, top + sidePx), 2f)
            drawPath(Path().apply { moveTo(left, yTitleY); lineTo(left - half, yTitleY + arrow); lineTo(left + half, yTitleY + arrow); close() }, axis)
            val xAxisY = top + sidePx
            val xArrowTx = max(left + sidePx + arrow, min(left + sidePx + 10.dp.toPx(), wPx - xTitle.size.width - 10.dp.toPx() - arrow))
            drawLine(axis, Offset(left, xAxisY), Offset(xArrowTx, xAxisY), 2f)
            drawPath(Path().apply { moveTo(xArrowTx + arrow, xAxisY); lineTo(xArrowTx, xAxisY - half); lineTo(xArrowTx, xAxisY + half); close() }, axis)
            drawText(yTitle, topLeft = Offset(left + 10.dp.toPx(), yTitleY))
            drawText(pct, topLeft = Offset(left + 10.dp.toPx(), yTitleY + yTitle.size.height))
            drawText(pct, topLeft = Offset(left + sidePx - pct.size.width, xAxisY))
            drawText(xTitle, topLeft = Offset(xArrowTx + arrow + 4.dp.toPx(), xAxisY - xTitle.size.height / 2))
            // both curves (unselected first), from the engine's samples of the current points
            if (ys.size > 1) for (curve in listOf(1 - selected, selected)) {
                val path = Path()
                ys.forEachIndexed { i, y -> val p = toPx(i.toDouble() / (ys.size - 1), visual(curve, y)); if (i == 0) path.moveTo(p.x, p.y) else path.lineTo(p.x, p.y) }
                val col = if (curve == 0) low else high
                val stroke = (if (curve == selected) 4 else 2).dp.toPx()
                if (abs(col.red - c.panel.red) + abs(col.green - c.panel.green) + abs(col.blue - c.panel.blue) < 0.18f)
                    drawPath(path, Color(0xFFACACAC), style = Stroke(stroke + 2.dp.toPx(), cap = StrokeCap.Round))
                drawPath(path, col, style = Stroke(stroke, cap = StrokeCap.Round))
            }
            for (a in pts) {
                val p = toPx(a.x, visual(selected, a.y))
                drawCircle(c.panel, 4.dp.toPx(), p); drawCircle(axis, 4.dp.toPx(), p, style = Stroke(1f))
            }
        }
    }
}

/** Mixing Recommendations: same-type pairs at 50 / 50 (three: each takes the 50 % role in turn), at most 100 */
@Composable
private fun Recommendations(app: AppState, colors: List<String>, names: List<String>, types: List<String>, three: Boolean, onPick: (List<Int>) -> Unit) {
    val c = desk()
    val items = remember(colors, types, three) {
        val groups = linkedMapOf<String, MutableList<Int>>()
        for (i in colors.indices) {
            val t = types.getOrElse(i) { "PLA" }
            if (t.endsWith("-S")) continue
            groups.getOrPut(t) { mutableListOf() }.add(i)
        }
        val out = mutableListOf<List<Int>>()
        for ((_, idx) in groups.toSortedMap()) {
            if (three) {
                for (a in idx.indices) for (b in a + 1 until idx.size) for (d in b + 1 until idx.size) {
                    val t = listOf(idx[a], idx[b], idx[d])
                    for (dom in 0..2) if (out.size < 100) out += listOf(t[(dom + 1) % 3], t[(dom + 2) % 3], t[dom])
                }
            } else for (a in idx.indices) for (b in a + 1 until idx.size) if (out.size < 100) out += listOf(idx[a], idx[b])
        }
        out
    }
    var swatches by remember { mutableStateOf<List<String>>(emptyList()) }
    LaunchedEffect(items) {
        runCatching {
            swatches = if (three) items.map { t ->
                app.engine.pure("mixed.blend", JSONObject().put("colors", JSONArray(t.map { colors[it] })).put("weights", JSONArray(listOf(0.25, 0.25, 0.5)))) as String
            } else (app.engine.pure("mixed.lerp", JSONObject().put("pairs", JSONArray(items.map { JSONArray().put(colors[it[0]]).put(colors[it[1]]).put(0.5) }))) as JSONArray).strings()
        }
    }
    Column(Modifier.padding(horizontal = 25.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(tr("Mixing Recommendations"), Modifier.padding(end = 4.dp), style = deskText(10, color = c.textDim))
            Box(Modifier.weight(1f).height(1.dp).background(c.border))
        }
        @OptIn(androidx.compose.foundation.layout.ExperimentalLayoutApi::class)
        FlowRow(Modifier.padding(top = 4.dp).fillMaxWidth().heightIn(min = 60.dp).background(c.field).padding(8.dp),
            horizontalArrangement = Arrangement.spacedBy(6.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            items.forEachIndexed { k, t ->
                val col = swatches.getOrNull(k)?.let { parseColor(it) } ?: Color.LightGray
                Box(Modifier.size(28.dp).background(col).clickable { onPick(t.map { it + 1 }) })
            }
        }
        if (names.isEmpty()) Unit
    }
}

/**
 * PublishSettingsDialog::add_mixed_visual: a mixed slot's page shows what would be published, read-only -- the ratio
 * bar (two components), the triangle picker (three), the gradient plot, with the dialog's painters.
 */
@Composable
fun MixedVisual(app: AppState, v: JSONObject) {
    val cols = v.optJSONArray("colors")?.strings() ?: return
    val ratios = v.optJSONArray("ratios")?.let { a -> (0 until a.length()).map { (a.getDouble(it) * 100 + 0.5).toInt() } } ?: emptyList()
    Box(Modifier.padding(vertical = 8.dp)) {
        when {
            v.optBoolean("gradient") && cols.size >= 2 -> {
                val samples = v.optJSONArray("samples")
                val ys = samples?.let { a -> (0 until a.length()).map { a.getJSONArray(it).getDouble(1) } } ?: emptyList()
                val anchors = v.optJSONArray("anchors")?.let { a -> (0 until a.length()).map { Anchor(a.getJSONArray(it).getDouble(0), a.getJSONArray(it).getDouble(1)) } } ?: emptyList()
                Box(Modifier.width(260.dp)) {
                    GradientCurveEditor(anchors, ys, parseColor(cols[0]) ?: Color.Gray, parseColor(cols[1]) ?: Color.Gray) {}
                }
            }
            cols.size == 3 && ratios.size == 3 -> TrianglePicker(app, cols, ratios, { _, _, _ -> }, {})
            cols.size == 2 && ratios.size == 2 -> Box(Modifier.width(260.dp)) { RatioBar(app, cols[0], cols[1], ratios[1], ratios[0], {}, {}) }
            else -> Row(Modifier.width(260.dp).height(27.dp)) {   // draw_mixed_ratio_segments
                cols.forEachIndexed { i, c ->
                    Box(Modifier.weight(ratios.getOrElse(i) { 1 }.coerceAtLeast(1).toFloat()).fillMaxHeight().background(parseColor(c) ?: Color.Gray), Alignment.Center) {
                        Text("${ratios.getOrElse(i) { 0 }}%", style = deskText(12, color = Color.White))
                    }
                }
            }
        }
        // read-only: the painters' gestures are covered
        Box(Modifier.matchParentSize().pointerInput(Unit) { awaitPointerEventScope { while (true) awaitPointerEvent().changes.forEach { it.consume() } } })
    }
}
