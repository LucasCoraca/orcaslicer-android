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
import kotlinx.coroutines.delay
import org.json.JSONObject
import org.orcaport.mobile.AppState
import org.orcaport.mobile.engine.jobj
import org.orcaport.mobile.ui.*

/**
 * GLGizmoSimplify::on_render_input_window: mesh name and triangles, detail level or decimate ratio, the resulting
 * triangle count, wireframe, the progress of the preview and Apply / Cancel. The engine simplifies on a worker thread;
 * the panel polls it while it runs.
 */
@Composable
fun SimplifyPanel(app: AppState, modifier: Modifier) {
    var st by remember { mutableStateOf(JSONObject()) }
    LaunchedEffect(app.project.optJSONArray("selection")?.toString()) {
        app.call("simplify.state") { if (it is JSONObject) st = it }
    }
    // poll while the worker runs (and once more after it ends, to pick up the result)
    LaunchedEffect(st.optBoolean("running"), st.optBoolean("cancelling"), st.optBoolean("ready")) {
        while (st.optBoolean("running") || st.optBoolean("cancelling") || (!st.optBoolean("ready") && !st.has("error"))) {
            delay(250)
            app.call("simplify.state") { if (it is JSONObject) { st = it; app.redraw() } }
        }
    }
    st.optString("error").takeIf { it.isNotEmpty() }?.let { msg ->
        // the desktop closes the gizmo and shows a message box
        LaunchedEffect(msg) { app.toast = msg; app.gizmo = null }
        return
    }
    if (!st.has("name")) return
    fun set(k: String, v: Any) = app.call("simplify.set", jobj(k to v)) { if (it is JSONObject) { st = it; app.redraw() } }
    val c = desk()
    val running = st.optBoolean("running")
    Column(modifier.widthIn(max = 380.dp).shadow(4.dp, RoundedCornerShape(8.dp)).clip(RoundedCornerShape(8.dp)).background(c.panel)
        .padding(horizontal = 12.dp, vertical = 10.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Text(tr("Simplify"), style = deskText(14))
        Row { Text(tr("Mesh name") + ":", Modifier.width(110.dp), style = deskText(13)); Text(st.optString("name"), style = deskText(13, color = c.textDim), maxLines = 1) }
        Row { Text(tr("Triangles") + ":", Modifier.width(110.dp), style = deskText(13)); Text("${st.optInt("triangles")}", style = deskText(13, color = c.textDim)) }
        HorizontalDivider(color = c.border)
        val useCount = st.optBoolean("use_count")
        val radio = RadioButtonDefaults.colors(selectedColor = OrcaColors.accent)
        Row(verticalAlignment = Alignment.CenterVertically) {
            RadioButton(!useCount, { set("use_count", false) }, colors = radio)
            Text(tr("Detail level"), Modifier.width(100.dp), style = deskText(13, color = if (useCount) c.textDim else c.text))
            val levels = st.optJSONArray("levels")
            val r = st.optInt("reduction", 2)
            Column(Modifier.weight(1f)) {
                Slider(r.toFloat(), { set("reduction", it.toInt()) }, valueRange = 0f..4f, steps = 3, enabled = !useCount,
                    colors = SliderDefaults.colors(thumbColor = OrcaColors.accent, activeTrackColor = OrcaColors.accent))
                Text(levels?.optString(r) ?: "", style = deskText(12, color = OrcaColors.accent))
            }
        }
        Row(verticalAlignment = Alignment.CenterVertically) {
            RadioButton(useCount, { set("use_count", true) }, colors = radio)
            Text(tr("Decimate ratio"), Modifier.width(100.dp), style = deskText(13, color = if (useCount) c.text else c.textDim))
            val ratio = st.optDouble("decimate_ratio").toFloat()
            Slider(ratio.coerceIn(0f, 100f), { set("decimate_ratio", it) }, Modifier.weight(1f), valueRange = 0f..100f, enabled = useCount,
                colors = SliderDefaults.colors(thumbColor = OrcaColors.accent, activeTrackColor = OrcaColors.accent))
            val fmt = if (ratio > 10) "%.0f %%" else if (ratio > 1) "%.1f %%" else "%.2f %%"
            Text(String.format(java.util.Locale.ROOT, fmt, ratio), Modifier.width(56.dp), style = deskText(12))
        }
        Text(st.optString("count_text"), Modifier.padding(start = 148.dp), style = deskText(13, color = if (useCount) c.text else c.textDim))
        Row(verticalAlignment = Alignment.CenterVertically) {
            DeskCheck(st.optBoolean("wireframe")) { set("wireframe", it) }
            Text(tr("Show wireframe"), style = deskText(13))
        }
        Row(verticalAlignment = Alignment.CenterVertically) {
            if (running) {
                LinearProgressIndicator({ st.optInt("progress") / 100f }, Modifier.weight(1f).height(8.dp).clip(RoundedCornerShape(4.dp)), color = OrcaColors.accent)
                Text("${st.optInt("progress")}%", Modifier.padding(horizontal = 8.dp), style = deskText(12, color = c.textDim))
            } else Spacer(Modifier.weight(1f))
            PanelButton(tr("Apply"), primary = true, enabled = !running && st.optBoolean("ready")) {
                app.call("simplify.apply") { r ->
                    if (r is JSONObject && r.has("objects")) { app.project = r; r.optString("notice").takeIf { it.isNotEmpty() }?.let { app.toast = it } }
                    app.gizmo = null
                    app.redraw()
                }
            }
            Spacer(Modifier.width(8.dp))
            PanelButton(tr("Cancel"), enabled = !st.optBoolean("cancelling")) { app.gizmo = null }
        }
    }
}
