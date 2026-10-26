package org.orcaport.mobile.prepare

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.unit.dp
import org.json.JSONObject
import org.orcaport.mobile.AppState
import org.orcaport.mobile.engine.jobj
import org.orcaport.mobile.ui.*

/**
 * LayersEditing::render_variable_layer_height_dialog: Adaptive with the quality slider, Smooth with radius and keep min,
 * Reset, Done. On touch the mouse buttons / Shift become "what a touch on the bar does", the wheel the edit area slider.
 */
@Composable
fun LayersEditingPanel(app: AppState, modifier: Modifier) {
    var st by remember { mutableStateOf(JSONObject()) }
    LaunchedEffect(app.gizmoRevision, app.project) { app.call("vlh.state") { if (it is JSONObject) st = it } }
    if (!st.optBoolean("enabled") || !st.has("labels")) return
    fun cmd(what: String, vararg kv: Pair<String, Any>) =
        app.call("vlh.command", jobj("what" to what, *kv)) { if (it is JSONObject) { st = it; app.redraw() } }
    val c = desk()
    val labels = st.getJSONObject("labels")
    val sliderColors = SliderDefaults.colors(thumbColor = OrcaColors.accent, activeTrackColor = OrcaColors.accent)
    Column(modifier.widthIn(max = 420.dp).shadow(4.dp, RoundedCornerShape(8.dp)).clip(RoundedCornerShape(8.dp)).background(c.panel)
        .padding(horizontal = 12.dp, vertical = 10.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            PanelButton(labels.optString("adaptive")) { cmd("adaptive", "quality" to st.optDouble("adaptive_quality")) }
            Spacer(Modifier.width(8.dp))
            Text(labels.optString("quality"), Modifier.width(110.dp), style = deskText(13))
            Slider(st.optDouble("adaptive_quality").toFloat(), { cmd("set", "quality" to it) }, Modifier.weight(1f), valueRange = 0f..1f, colors = sliderColors)
            Text(String.format(java.util.Locale.ROOT, "%.2f", st.optDouble("adaptive_quality")), Modifier.width(40.dp), style = deskText(13))
        }
        Row(verticalAlignment = Alignment.CenterVertically) {
            PanelButton(labels.optString("smooth")) { cmd("smooth") }
            Spacer(Modifier.width(8.dp))
            Text(labels.optString("radius"), Modifier.width(110.dp), style = deskText(13))
            Slider(st.optInt("radius").toFloat(), { cmd("set", "radius" to it.toInt()) }, Modifier.weight(1f), valueRange = 1f..10f, steps = 8, colors = sliderColors)
            Text("${st.optInt("radius")}", Modifier.width(40.dp), style = deskText(13))
        }
        Row(verticalAlignment = Alignment.CenterVertically) {
            DeskCheck(st.optBoolean("keep_min")) { cmd("set", "keep_min" to it) }
            Text(labels.optString("keep_min"), style = deskText(13))
        }
        HorizontalDivider(color = c.border)
        // the shortcuts of the desktop's tooltip as a choice: left / right button, Shift + left / right
        val actions = labels.optJSONArray("actions")
        DeskChoice((0 until (actions?.length() ?: 0)).map { actions!!.getString(it) }, st.optInt("touch_action"), Modifier.fillMaxWidth()) { cmd("set", "touch_action" to it) }
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(labels.optString("edit_area"), Modifier.width(150.dp), style = deskText(13))
            Slider(st.optDouble("band_width").toFloat(), { cmd("set", "band_width" to it) }, Modifier.weight(1f), valueRange = 1.5f..10f, colors = sliderColors)
        }
        HorizontalDivider(color = c.border)
        Row {
            PanelButton(labels.optString("reset"), enabled = st.optBoolean("reset_enabled")) { cmd("reset") }
            Spacer(Modifier.weight(1f))
            PanelButton(labels.optString("done"), primary = true) { app.gizmo = null }
        }
    }
}
