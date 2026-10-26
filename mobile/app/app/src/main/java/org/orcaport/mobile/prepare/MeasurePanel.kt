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
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import org.json.JSONObject
import org.orcaport.mobile.AppState
import org.orcaport.mobile.engine.jobj
import org.orcaport.mobile.ui.*

/**
 * GLGizmoMeasure / GLGizmoAssembly::on_render_input_window: the two selections, the results table with copy buttons,
 * the X / Y / Z distance boxes, the assembly mode and its face actions, the warnings. A tap on a part picks the feature
 * under the finger; "Select point" stands for holding Shift.
 */
@Composable
fun MeasurePanel(app: AppState, modifier: Modifier) {
    var st by remember { mutableStateOf(JSONObject()) }
    LaunchedEffect(app.gizmoRevision, app.project) { app.call("measure.state") { if (it is JSONObject) st = it } }
    if (!st.optBoolean("active")) return
    fun cmd(what: String, vararg kv: Pair<String, Any>) =
        app.call("measure.command", jobj("what" to what, *kv)) { if (it is JSONObject) { st = it; app.redraw() } }
    val c = desk()
    val labels = st.optJSONObject("labels") ?: JSONObject()
    val clipboard = LocalClipboardManager.current
    val assembly = st.optBoolean("assembly")
    Column(modifier.widthIn(max = 420.dp).shadow(4.dp, RoundedCornerShape(8.dp)).clip(RoundedCornerShape(8.dp)).background(c.panel)
        .padding(horizontal = 12.dp, vertical = 10.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        if (assembly) Row(verticalAlignment = Alignment.CenterVertically) {   // render_assembly_mode_combo
            Text(labels.optString("mode"), Modifier.width(80.dp), style = deskText(13))
            val modes = labels.optJSONArray("modes")
            DeskChoice((0 until (modes?.length() ?: 0)).map { modes!!.getString(it) }, st.optInt("assembly_mode"), Modifier.weight(1f)) {
                cmd("assembly_mode", "mode" to it)
            }
        }
        labels.optString("tip").takeIf { it.isNotEmpty() }?.let { Text(it.replace(" \n ", " ").replace("\n", " "), style = deskText(13)) }
        @Composable fun selection(caption: String, text: String, color: Color, reset: (() -> Unit)?) = Row(verticalAlignment = Alignment.CenterVertically) {
            Text(caption, Modifier.width(130.dp), style = deskText(13, color = color))
            Text(text, Modifier.weight(1f), style = deskText(13, color = color))
            if (reset != null) Box(Modifier.size(30.dp).clickable(onClick = reset), Alignment.Center) { OrcaIcon("revert_btn", 18.dp) }
        }
        selection(labels.optString("sel1"), st.optString("sel1"), Color(0xFF40BFBF), if (st.optBoolean("has1")) ({ cmd("reset1") }) else null)
        selection(labels.optString("sel2"), st.optString("sel2"), Color(0xFFBF40BF), if (st.optBoolean("has1") && st.optBoolean("has2")) ({ cmd("reset2") }) else null)
        st.optString("reset_first_tip").takeIf { it.isNotEmpty() }?.let { Text(it.replace("\n", " "), style = deskText(12, color = c.textDim)) }
        // show_face_face_assembly_common
        st.optJSONObject("actions")?.let { a ->
            Row {
                PanelButton(labels.optString("center_coincidence"), primary = true, enabled = a.optBoolean("center_coincidence")) { cmd("center_coincidence") }
                Spacer(Modifier.width(8.dp))
                PanelButton(labels.optString("parallel"), enabled = a.optBoolean("parallel")) { cmd("parallel") }
            }
        }
        HorizontalDivider(color = c.border)
        // show_face_face_assembly_senior
        st.optJSONObject("actions")?.let { a ->
            Row(verticalAlignment = Alignment.CenterVertically) {
                DeskCheck(a.optBoolean("flip")) { cmd("flip") }
                Text(labels.optString("flip"), style = deskText(13))
            }
            if (a.has("parallel_distance")) Row(verticalAlignment = Alignment.CenterVertically) {
                Text(labels.optString("parallel_distance"), Modifier.width(170.dp), style = deskText(13))
                NumberBox(a.optDouble("parallel_distance"), Modifier.width(110.dp)) { cmd("parallel_distance", "value" to it) }
            }
            if (a.optBoolean("rotate_around_center")) Row(verticalAlignment = Alignment.CenterVertically) {
                Text(labels.optString("rotate_around_center"), Modifier.width(170.dp), style = deskText(13))
                NumberBox(0.0, Modifier.width(110.dp)) { if (it != 0.0) cmd("rotate_around_center", "value" to it) }
                Text(tr("°"), style = deskText(13))
            }
        }
        // show_distance_xyz_ui
        if (!assembly) Text(labels.optString("measure"), style = deskText(13))
        val rows = st.optJSONArray("rows")
        for (i in 0 until (rows?.length() ?: 0)) {
            val r = rows!!.getJSONObject(i)
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(r.optString("label"), Modifier.width(170.dp), style = deskText(13, color = OrcaColors.accent))
                Text(r.optString("value"), Modifier.weight(1f), style = deskText(13))
                Box(Modifier.size(30.dp).clickable { clipboard.setText(AnnotatedString(r.optString("label") + ": " + r.optString("value"))) }, Alignment.Center) {
                    OrcaIcon("copy_menu", 18.dp)
                }
            }
        }
        st.optJSONObject("xyz")?.let { x ->
            val values = x.getJSONArray("values")
            val enabled = x.getJSONArray("enabled")
            for (axis in 0..2) Row(verticalAlignment = Alignment.CenterVertically) {
                Text(listOf("X:", "Y:", "Z:")[axis], Modifier.width(40.dp), style = deskText(13, color = listOf(Color(0xFFE53935), Color(0xFF43A047), Color(0xFF1E88E5))[axis]))
                NumberBox(values.getDouble(axis), Modifier.width(120.dp), enabled = enabled.getBoolean(axis)) { cmd("distance", "axis" to axis, "value" to it) }
            }
        }
        if (!assembly) Row(verticalAlignment = Alignment.CenterVertically) {   // Shift + left click on the desktop
            DeskCheck(st.optBoolean("point_selection")) { cmd("point_selection", "on" to it) }
            Text(labels.optString("point_selection"), style = deskText(13))
        }
        val warnings = st.optJSONArray("warnings")
        if ((warnings?.length() ?: 0) > 0) HorizontalDivider(color = c.border)
        for (i in 0 until (warnings?.length() ?: 0)) Text(warnings!!.getString(i).replace("\n", " "), style = deskText(13, color = OrcaColors.warning))
        HorizontalDivider(color = c.border)
        Row { Spacer(Modifier.weight(1f)); PanelButton(labels.optString("done"), primary = true) { app.gizmo = null } }
    }
}

/** render_dimensioning's labels (ImGui windows on the desktop): the distance with "Edit to scale", the angle */
@Composable
fun MeasureLabels(app: AppState) {
    val labels = app.overlay.optJSONArray("measure") ?: return
    var editing by remember { mutableStateOf<JSONObject?>(null) }
    val c = desk()
    for (i in 0 until labels.length()) {
        val l = labels.getJSONObject(i)
        Row(Modifier.offset { IntOffset(l.getDouble("x").toInt(), l.getDouble("y").toInt() - 40) }.background(Color.White.copy(alpha = 0.5f))
            .padding(horizontal = 4.dp), verticalAlignment = Alignment.CenterVertically) {
            Text(l.optString("text"), style = deskText(13, color = Color.Black))
            if (l.optBoolean("editable")) Box(Modifier.size(28.dp).clickable { editing = l }, Alignment.Center) { OrcaIcon("edit", 16.dp) }
        }
    }
    editing?.let { l ->
        var value by remember(l) { mutableStateOf(l.optDouble("value")) }
        AlertDialog(onDismissRequest = { editing = null }, containerColor = c.panel,
            text = { NumberBox(value, Modifier.width(160.dp)) { value = it } },
            confirmButton = {
                PanelButton(tr("Scale all"), primary = true) {
                    app.call("measure.command", jobj("what" to "scale", "value" to value, "old" to l.optDouble("value"))) { app.gizmoRevision++; app.redraw() }
                    editing = null
                }
            },
            dismissButton = { PanelButton(tr("Cancel")) { editing = null } })
    }
}

/** BBLInputDouble ("%.2f"): commits a number on Done / focus loss */
@Composable
private fun NumberBox(value: Double, modifier: Modifier, enabled: Boolean = true, onCommit: (Double) -> Unit) =
    DeskInput(String.format(java.util.Locale.ROOT, "%.2f", value), enabled, true, modifier) { t -> t.replace(',', '.').toDoubleOrNull()?.let(onCommit) }
