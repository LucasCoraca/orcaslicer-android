package org.orcaport.mobile.prepare

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.text.font.FontStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.delay
import org.json.JSONArray
import org.json.JSONObject
import org.orcaport.mobile.AppState
import org.orcaport.mobile.engine.jobj
import org.orcaport.mobile.ui.*

/**
 * GLGizmoEmboss::draw_window: the text, the style list (save / save as / rename / remove / reset), the font face with
 * bold and italic, height and depth, use surface, per glyph, alignment, the Advanced options (char / line gap, boldness,
 * skew, distance from the surface, rotation, keep up, collection, face the camera), the operation and Done. A drag that
 * starts on the text moves it over the object's surface.
 */
@Composable
fun TextPanel(app: AppState, modifier: Modifier) {
    var st by remember { mutableStateOf(JSONObject()) }
    LaunchedEffect(app.gizmoRevision, app.project) { app.call("text.state") { if (it is JSONObject) st = it } }
    if (!st.optBoolean("active")) return
    fun apply(r: Any?) { if (r is JSONObject) { st = r; app.redraw(); app.call("project.state") { p -> if (p is JSONObject) app.project = p } } }
    fun set(vararg kv: Pair<String, Any?>) = app.call("text.set", jobj(*kv)) { apply(it) }
    fun style(what: String, vararg kv: Pair<String, Any?>) = app.call("text.style", jobj("what" to what, *kv)) { apply(it) }
    val c = desk()
    val l = st.optJSONObject("labels") ?: JSONObject()
    val sliderColors = SliderDefaults.colors(thumbColor = OrcaColors.accent, activeTrackColor = OrcaColors.accent)
    val labelW = 110.dp
    var fontsOpen by remember { mutableStateOf(false) }
    var nameAsk by remember { mutableStateOf<String?>(null) }   // "save_as" / "rename"
    var advanced by remember { mutableStateOf(false) }
    val units = st.optString("units", "mm")
    fun fmt(v: Double, d: Int = 2) = String.format(java.util.Locale.ROOT, "%.${d}f", v)

    @Composable fun labelled(label: String, content: @Composable RowScope.() -> Unit) = Row(verticalAlignment = Alignment.CenterVertically) {
        Text(label, Modifier.width(labelW), style = deskText(13))
        content()
    }
    // a slider that previews while moving and commits one undo step on release, with the value as a field
    @Composable fun slider(label: String, key: String, range: ClosedFloatingPointRange<Float>, digits: Int, enabled: Boolean = true,
                           scale: Double = 1.0, unit: String? = null, send: (Double) -> Any? = { it }) {
        var v by remember(st) { mutableStateOf(st.optDouble(key, 0.0).toFloat()) }
        labelled(label) {
            Slider(v.coerceIn(range.start, range.endInclusive), { v = it; set(key to send(it.toDouble()), "final" to false) }, Modifier.weight(1f),
                enabled = enabled, valueRange = range, colors = sliderColors, onValueChangeFinished = { set(key to send(v.toDouble()), "final" to true) })
            DeskInput(fmt(v * scale, digits), enabled, true, Modifier.width(76.dp), sidetext = unit) { s ->
                s.replace(',', '.').toDoubleOrNull()?.let { set(key to send(it / scale)) }
            }
        }
    }

    Column(modifier.widthIn(max = 420.dp).heightIn(max = 640.dp).shadow(4.dp, RoundedCornerShape(8.dp)).clip(RoundedCornerShape(8.dp))
        .background(c.panel).verticalScroll(rememberScrollState()).padding(horizontal = 12.dp, vertical = 10.dp),
        verticalArrangement = Arrangement.spacedBy(6.dp)) {
        // draw_text_input: the text, in the style's look (bold / italic)
        var text by remember(st.optInt("session")) { mutableStateOf(st.optString("text")) }
        LaunchedEffect(text) {
            if (text == st.optString("text")) return@LaunchedEffect
            delay(350)
            set("text" to text)
        }
        BasicTextField(text, { text = it }, Modifier.fillMaxWidth().heightIn(min = 64.dp).clip(RoundedCornerShape(4.dp)).background(c.field)
            .border(1.dp, c.border, RoundedCornerShape(4.dp)).padding(8.dp),
            textStyle = deskText(16, if (st.optBoolean("bold")) FontWeight.Bold else FontWeight.Normal)
                .copy(fontStyle = if (st.optBoolean("italic")) FontStyle.Italic else FontStyle.Normal),
            cursorBrush = SolidColor(c.accent))
        val warnings = st.optJSONArray("warnings") ?: JSONArray()
        for (i in 0 until warnings.length()) Text(warnings.optString(i), style = deskText(12, color = OrcaColors.warning))
        st.optString("error").takeIf { it.isNotEmpty() }?.let { Text(it, style = deskText(12, color = OrcaColors.warning)) }
        HorizontalDivider(color = c.border)

        // draw_style_list + the style buttons
        val styles = st.optJSONArray("styles")?.let { a -> List(a.length()) { a.optString(it) } } ?: emptyList()
        labelled(l.optString("style")) {
            DeskChoice(styles, st.optInt("style_index", -1), Modifier.weight(1f),
                display = st.optString("style_name") + if (st.optBoolean("modified")) " (*)" else "") { style("select", "index" to it) }
        }
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(6.dp, Alignment.End)) {
            PanelButton(l.optString("save"), enabled = st.optBoolean("modified")) { style("save") }
            PanelButton(l.optString("save_as")) { nameAsk = "save_as" }
            PanelButton(l.optString("rename"), enabled = st.optInt("style_index", -1) >= 0) { nameAsk = "rename" }
            PanelButton(l.optString("remove"), enabled = styles.size > 1 && st.optInt("style_index", -1) >= 0) { style("remove") }
        }
        // draw_font_list + bold / italic
        labelled(l.optString("font")) {
            DeskCombo(st.optString("face"), Modifier.weight(1f), textColor = if (st.optBoolean("unknown_font")) OrcaColors.warning else null) { fontsOpen = true }
            Spacer(Modifier.width(4.dp))
            ToggleBox("B", st.optBoolean("bold"), FontWeight.Bold, FontStyle.Normal) { set("bold" to !st.optBoolean("bold")) }
            ToggleBox("I", st.optBoolean("italic"), FontWeight.Normal, FontStyle.Italic) { set("italic" to !st.optBoolean("italic")) }
        }
        labelled(l.optString("height")) {
            DeskInput(fmt(st.optDouble("height")), true, true, Modifier.weight(1f), sidetext = units) { s ->
                s.replace(',', '.').toDoubleOrNull()?.let { set("height" to it) }
            }
        }
        labelled(l.optString("depth")) {
            DeskInput(fmt(st.optDouble("depth")), true, true, Modifier.weight(1f), sidetext = units) { s ->
                s.replace(',', '.').toDoubleOrNull()?.let { set("depth" to it) }
            }
        }
        labelled(l.optString("use_surface")) { DeskCheck(st.optBoolean("use_surface"), st.optBoolean("can_use_surface")) { set("use_surface" to it) } }
        labelled(l.optString("per_glyph")) { DeskCheck(st.optBoolean("per_glyph"), st.optBoolean("can_per_glyph")) { set("per_glyph" to it) } }
        // draw_align: left / center / right, top / middle / bottom
        labelled(l.optString("alignment")) {
            for ((i, icon) in listOf("align_horizontal_left", "align_horizontal_center", "align_horizontal_right").withIndex())
                AlignBox(icon, st.optInt("align_h") == i) { set("align_h" to i) }
            Spacer(Modifier.width(8.dp))
            for ((i, icon) in listOf("align_vertical_top", "align_vertical_center", "align_vertical_bottom").withIndex())
                AlignBox(icon, st.optInt("align_v") == i) { set("align_v" to i) }
        }
        HorizontalDivider(color = c.border)

        // draw_advanced
        Row(Modifier.fillMaxWidth().clickable { advanced = !advanced }, verticalAlignment = Alignment.CenterVertically) {
            Text((if (advanced) "▾ " else "▸ ") + l.optString("advanced"), style = deskText(13, FontWeight.Bold))
        }
        if (advanced) {
            val gap = st.optJSONArray("gap_range")
            val gapR = (gap?.optInt(0) ?: -500).toFloat()..(gap?.optInt(1) ?: 500).toFloat()
            slider(l.optString("char_gap"), "char_gap", gapR, 0, unit = l.optString("points")) { it.toInt() }
            slider(l.optString("line_gap"), "line_gap", gapR, 0, enabled = st.optBoolean("multiline"), unit = l.optString("points")) { it.toInt() }
            val b = st.optJSONArray("boldness_range")
            slider(l.optString("boldness"), "boldness", (b?.optInt(0) ?: -200).toFloat()..(b?.optInt(1) ?: 200).toFloat(), 0, unit = l.optString("points"))
            slider(l.optString("skew"), "skew", -1f..1f, 2)
            val d = st.optJSONArray("distance_range")
            slider(l.optString("from_surface"), "distance", (d?.optDouble(0) ?: -10.0).toFloat()..(d?.optDouble(1) ?: 10.0).toFloat(), 2,
                enabled = st.optBoolean("can_distance"), unit = units)
            slider(l.optString("rotation"), "rotation", -180f..180f, 1, unit = "°")
            if (st.optBoolean("show_keep_up")) labelled(l.optString("keep_up")) { DeskCheck(st.optBoolean("keep_up")) { set("keep_up" to it) } }
            if (st.optInt("collections") > 1) labelled(l.optString("collection")) {
                DeskChoice(List(st.optInt("collections")) { "${it + 1}" }, st.optInt("collection"), Modifier.weight(1f)) { set("collection" to it) }
            }
            Row(Modifier.padding(start = labelW)) { PanelButton(l.optString("face_camera")) { set("face_camera" to true) } }
        }
        HorizontalDivider(color = c.border)
        // draw_model_type
        if (st.optBoolean("show_type")) {
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
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            PanelButton(l.optString("reset"), enabled = st.optBoolean("modified") && st.optInt("style_index", -1) >= 0) { style("reset") }
            Spacer(Modifier.weight(1f))
            PanelButton(l.optString("done"), primary = true) { app.gizmo = null }
        }
    }

    if (fontsOpen) FontPicker(app, st.optString("face"), { fontsOpen = false }) { set("face" to it) }
    nameAsk?.let { what ->
        var name by remember { mutableStateOf(st.optString("style_name")) }
        AlertDialog({ nameAsk = null }, confirmButton = {
            TextButton({ nameAsk = null; style(what, "name" to name) }) { Text(tr("OK")) }
        }, dismissButton = { TextButton({ nameAsk = null }) { Text(tr("Cancel")) } },
            title = { Text(if (what == "rename") l.optString("rename") else l.optString("save_as"), style = deskText(15, FontWeight.Bold)) },
            text = { DeskInput(name, true, false, Modifier.fillMaxWidth()) { name = it } })
    }
}

@Composable
private fun ToggleBox(label: String, on: Boolean, weight: FontWeight, style: FontStyle, onClick: () -> Unit) {
    val c = desk()
    Box(Modifier.padding(start = 4.dp).size(32.dp).clip(RoundedCornerShape(4.dp)).background(if (on) c.accent else c.field)
        .border(1.dp, c.border, RoundedCornerShape(4.dp)).clickable(onClick = onClick), Alignment.Center) {
        Text(label, style = deskText(15, weight, if (on) Color.White else c.text).copy(fontStyle = style))
    }
}

@Composable
private fun AlignBox(icon: String, on: Boolean, onClick: () -> Unit) {
    val c = desk()
    Box(Modifier.padding(end = 2.dp).size(32.dp).clip(RoundedCornerShape(4.dp)).background(if (on) c.selection else Color.Transparent)
        .border(1.dp, if (on) c.accent else c.border, RoundedCornerShape(4.dp)).clickable(onClick = onClick), Alignment.Center) {
        OrcaIcon(icon, 20.dp)
    }
}

/** the font list (draw_font_list): every face installed on the device and shipped with Orca, filtered by a search */
@Composable
private fun FontPicker(app: AppState, current: String, onDismiss: () -> Unit, onPick: (String) -> Unit) {
    var fonts by remember { mutableStateOf(emptyList<String>()) }
    LaunchedEffect(Unit) { app.call("text.fonts") { r -> if (r is JSONArray) fonts = List(r.length()) { r.getJSONObject(it).optString("name") } } }
    var filter by remember { mutableStateOf("") }
    val c = desk()
    AlertDialog(onDismiss, confirmButton = { TextButton(onDismiss) { Text(tr("Cancel")) } }, text = {
        Column {
            BasicTextField(filter, { filter = it }, Modifier.fillMaxWidth().clip(RoundedCornerShape(4.dp)).background(c.field)
                .border(1.dp, c.border, RoundedCornerShape(4.dp)).padding(8.dp), textStyle = deskText(14), cursorBrush = SolidColor(c.accent), singleLine = true)
            Spacer(Modifier.height(6.dp))
            LazyColumn(Modifier.heightIn(max = 420.dp)) {
                items(fonts.filter { it.contains(filter, ignoreCase = true) }) { f ->
                    Text(f, Modifier.fillMaxWidth().clickable { onDismiss(); onPick(f) }.padding(vertical = 8.dp),
                        style = deskText(14, color = if (f == current) c.accent else c.text))
                }
            }
        }
    })
}
