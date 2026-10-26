package org.orcaport.mobile.prepare

import androidx.compose.foundation.background
import androidx.compose.foundation.border
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
import androidx.compose.ui.unit.dp
import org.json.JSONObject
import org.orcaport.mobile.AppState
import org.orcaport.mobile.engine.jobj
import org.orcaport.mobile.ui.*

/**
 * GLGizmoMeshBoolean::on_render_input_window: Union / Difference / Intersection tabs, the two parts (tap one in the 3D
 * view while its "Select" is active), "Delete input" and the operation button.
 */
@Composable
fun MeshBooleanPanel(app: AppState, modifier: Modifier) {
    var st by remember { mutableStateOf(JSONObject()) }
    LaunchedEffect(app.gizmoRevision, app.project.optJSONArray("selection")?.toString()) {
        app.call("boolean.state") { if (it is JSONObject) st = it }
    }
    if (!st.optBoolean("active")) return
    fun set(vararg kv: Pair<String, Any>) = app.call("boolean.set", jobj(*kv)) { if (it is JSONObject) { st = it; app.redraw() } }
    val c = desk()
    val labels = st.optJSONObject("labels") ?: JSONObject()
    val mode = st.optString("mode")
    @Composable fun tab(id: String, modifier: Modifier) {
        val sel = mode == id
        Box(modifier.height(34.dp).background(if (sel) OrcaColors.accent else Color.Transparent).border(1.dp, c.border)
            .clickable { set("mode" to id) }, Alignment.Center) {
            Text(labels.optString(id), style = deskText(13, color = if (sel) Color.White else c.text))
        }
    }
    @Composable fun part(cap: String, which: String, name: String) = Row(verticalAlignment = Alignment.CenterVertically) {
        Text(cap, Modifier.width(110.dp), style = deskText(13))
        val sel = st.optString("selecting") == which
        Box(Modifier.height(32.dp).clip(RoundedCornerShape(4.dp)).background(if (sel) OrcaColors.accent else c.panel).border(1.dp, c.border, RoundedCornerShape(4.dp))
            .clickable { set("selecting" to which) }.padding(horizontal = 12.dp), Alignment.Center) {
            Text(if (name.isNotEmpty()) labels.optString("selected") else labels.optString("select"), style = deskText(13, color = if (sel) Color.White else c.text))
        }
        if (name.isNotEmpty()) {
            Text(name, Modifier.padding(start = 8.dp).weight(1f, fill = false), style = deskText(13), maxLines = 1)
            Box(Modifier.size(28.dp).clickable { set((if (which == "source") "reset_src" else "reset_tool") to true) }, Alignment.Center) { Text("✕", style = deskText(13, color = c.textDim)) }
        }
    }
    Column(modifier.widthIn(max = 400.dp).shadow(4.dp, RoundedCornerShape(8.dp)).clip(RoundedCornerShape(8.dp)).background(c.panel)
        .padding(horizontal = 12.dp, vertical = 10.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Row { tab("union", Modifier.weight(1f)); tab("difference", Modifier.weight(1f)); tab("intersection", Modifier.weight(1f)) }
        part(labels.optString("cap1"), "source", st.optString("src"))
        part(labels.optString("cap2"), "tool", st.optString("tool"))
        if (mode != "union") Row(verticalAlignment = Alignment.CenterVertically) {
            DeskCheck(st.optBoolean("delete_input")) { set("delete_input" to it) }
            Text(labels.optString("delete_input"), style = deskText(13))
        }
        val enabled = st.optString("src").isNotEmpty() && st.optString("tool").isNotEmpty()
        PanelButton(labels.optString(mode), primary = true, enabled = enabled) {
            app.call("boolean.apply") { r ->
                if (r is JSONObject) {
                    if (r.has("objects")) app.project = r
                    r.optString("warning").takeIf { it.isNotEmpty() }?.let { app.toast = it }
                    r.optJSONObject("boolean")?.let { st = it }
                }
                app.redraw()
            }
        }
    }
}
