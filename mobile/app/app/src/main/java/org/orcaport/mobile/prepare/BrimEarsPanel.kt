package org.orcaport.mobile.prepare

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.unit.dp
import org.json.JSONObject
import org.orcaport.mobile.AppState
import org.orcaport.mobile.engine.jobj
import org.orcaport.mobile.ui.*

/**
 * GLGizmoBrimEars::on_render_input_window: ear radius (also of the selected ears), max angle and detection radius for
 * Auto-generate, Remove selected / all, Done, and the warnings (brim type not "painted", invalid ears). A tap on the
 * object adds an ear, a tap on an ear selects it, a long press removes it, a drag moves it.
 */
@Composable
fun BrimEarsPanel(app: AppState, modifier: Modifier) {
    var st by remember { mutableStateOf(JSONObject()) }
    LaunchedEffect(app.gizmoRevision, app.project) { app.call("brim_ears.state") { if (it is JSONObject) st = it } }
    if (!st.optBoolean("active")) return
    fun set(vararg kv: Pair<String, Any>) = app.call("brim_ears.set", jobj(*kv)) { if (it is JSONObject) { st = it; app.redraw() } }
    fun action(what: String) = app.call("brim_ears.action", jobj("what" to what)) { if (it is JSONObject) { st = it; app.redraw() } }
    val c = desk()
    val labels = st.optJSONObject("labels") ?: JSONObject()
    val sliderColors = SliderDefaults.colors(thumbColor = OrcaColors.accent, activeTrackColor = OrcaColors.accent)
    @Composable fun row(label: String, key: String, max: Float, onEnd: (() -> Unit)? = null) = Row(verticalAlignment = Alignment.CenterVertically) {
        Text(label, Modifier.width(130.dp), style = deskText(13))
        Slider(st.optDouble(key).toFloat().coerceIn(0f, max), { set(key to it) }, Modifier.weight(1f), valueRange = 0f..max, colors = sliderColors,
            onValueChangeFinished = onEnd)
        Text(String.format(java.util.Locale.ROOT, "%.1f", st.optDouble(key)), Modifier.width(44.dp), style = deskText(13))
    }
    Column(modifier.widthIn(max = 400.dp).shadow(4.dp, RoundedCornerShape(8.dp)).clip(RoundedCornerShape(8.dp)).background(c.panel)
        .padding(horizontal = 12.dp, vertical = 10.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        // the radius: one undo step when the slider is released (apply_radius_change)
        row(labels.optString("brim_ear_radius"), "radius", 100f) { set("radius" to st.optDouble("radius"), "final" to true) }
        HorizontalDivider(color = c.border)
        row(labels.optString("max_angle"), "max_angle", 180f)
        row(labels.optString("detection_radius"), "detection_radius", st.optDouble("detection_radius_max", 100.0).toFloat().coerceAtLeast(0.1f))
        HorizontalDivider(color = c.border)
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(labels.optString("create"), Modifier.width(130.dp), style = deskText(13))
            PanelButton(labels.optString("auto_generate")) { action("auto_generate") }
        }
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(labels.optString("remove"), Modifier.width(130.dp), style = deskText(13))
            PanelButton(labels.optString("remove_selected"), enabled = st.optBoolean("any_selected")) { action("remove_selected") }
            Spacer(Modifier.width(8.dp))
            PanelButton(labels.optString("remove_all"), enabled = st.optInt("count") > 0) { action("remove_all") }
        }
        HorizontalDivider(color = c.border)
        Row { Spacer(Modifier.weight(1f)); PanelButton(labels.optString("done"), primary = true) { app.gizmo = null } }
        if (st.optBoolean("brim_not_painted") || st.optInt("invalid") > 0) {
            HorizontalDivider(color = c.border)
            if (st.optBoolean("brim_not_painted")) {
                Text(labels.optString("warning_painted"), style = deskText(13, color = OrcaColors.warning))
                Text(labels.optString("set_painted"), Modifier.padding(start = 24.dp).clickable { set("painted" to true) },
                    style = deskText(13, color = OrcaColors.accent).copy(textDecoration = TextDecoration.Underline))
            }
            if (st.optInt("invalid") > 0) Text(labels.optString("invalid"), style = deskText(13, color = OrcaColors.warning))
        }
    }
}
