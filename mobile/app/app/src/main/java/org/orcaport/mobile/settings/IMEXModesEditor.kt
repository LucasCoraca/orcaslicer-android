package org.orcaport.mobile.settings

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
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import org.json.JSONArray
import org.json.JSONObject
import org.orcaport.mobile.engine.jobj
import org.orcaport.mobile.prepare.PanelButton
import org.orcaport.mobile.ui.*

private fun color(s: String) = runCatching { Color(android.graphics.Color.parseColor(s)) }.getOrDefault(Color.Gray)

/**
 * IMEXModesCtrl (Printer > Multimaterial > IDEX/IQEX Configuration > Modes): Add Mode with the overview, the role legend,
 * then a row per mode: its name (Primary fixed), the tool tiles in the bed's layout (tap to cycle a role), its G-code
 * with the placeholder editor, remove and reset. The control's logic runs in the engine (imex.state / imex.edit).
 */
@Composable
fun IMEXModesEditor(m: ParamsModel) {
    val app = m.app
    val c = desk()
    var st by remember { mutableStateOf<JSONObject?>(null) }
    var help by remember { mutableStateOf(false) }
    var editGcode by remember { mutableStateOf<Int?>(null) }
    LaunchedEffect(m.state) { app.call("imex.state") { if (it is JSONObject) st = it } }
    fun edit(vararg kv: Pair<String, Any?>) = app.call("imex.edit", jobj(*kv)) { r ->
        if (r !is JSONObject) return@call
        st = r
        r.optJSONObject("config")?.let { m.state = ConfigState(it) }
        app.call("presets.state") { p -> if (p is JSONObject) app.presets = p }
    }
    val s = st ?: return
    val str = s.optJSONObject("strings") ?: JSONObject()
    val cols = s.optInt("cols", 1)
    Column(Modifier.fillMaxWidth().padding(start = 30.dp, end = 10.dp, top = 4.dp, bottom = 8.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            PanelButton(str.optString("add"), primary = true) { edit("what" to "add") }
            Spacer(Modifier.width(6.dp))
            Box(Modifier.size(28.dp).clickable { help = !help }, Alignment.Center) { OrcaIcon("icon_qusetion", 20.dp) }
        }
        if (help) Text(str.optString("help"), style = deskText(12, color = c.textDim))
        // the legend: a swatch per role this printer offers, its hint under a tap
        var hint by remember { mutableStateOf<String?>(null) }
        Row(verticalAlignment = Alignment.CenterVertically) {
            val legend = s.optJSONArray("legend") ?: JSONArray()
            for (i in 0 until legend.length()) {
                val l = legend.getJSONObject(i)
                Row(Modifier.clickable { hint = if (hint == l.optString("hint")) null else l.optString("hint") }.padding(end = 16.dp),
                    verticalAlignment = Alignment.CenterVertically) {
                    Box(Modifier.size(14.dp).background(color(l.optString("color"))))
                    Spacer(Modifier.width(4.dp))
                    Text(l.optString("label"), style = deskText(13))
                }
            }
        }
        hint?.let { Text(it, style = deskText(12, color = c.textDim)) }
        Row {
            Text(str.optString("mode"), Modifier.width(180.dp), style = deskText(13))
            Text(str.optString("tools"), Modifier.width((cols * 40).dp + 8.dp), style = deskText(13))
            Text(str.optString("gcode"), style = deskText(13))
        }
        val rows = s.optJSONArray("rows") ?: JSONArray()
        for (i in 0 until rows.length()) {
            val r = rows.getJSONObject(i)
            key(i, rows.length()) {
                Row(verticalAlignment = Alignment.Top) {
                    Column(Modifier.width(180.dp)) {
                        if (r.optBoolean("primary")) Text(r.optString("name"), Modifier.padding(top = 8.dp), style = deskText(14, FontWeight.Bold))
                        else {
                            var name by remember(r.optString("name")) { mutableStateOf(r.optString("name")) }
                            var menu by remember { mutableStateOf(false) }
                            var focused by remember { mutableStateOf(false) }
                            Box {
                                OutlinedTextField(name, { name = it; edit("what" to "name", "row" to i, "value" to it) },
                                    Modifier.width(172.dp).onFocusChanged { f ->
                                        // the name field's kill-focus: fix_row_name
                                        if (focused && !f.isFocused) edit("what" to "name_commit", "row" to i)
                                        focused = f.isFocused
                                    },
                                    singleLine = true, textStyle = deskText(13),
                                    placeholder = { Text(str.optString("name_hint"), style = deskText(12, color = c.textDim)) },
                                    trailingIcon = {
                                        val sug = r.optJSONArray("suggestions")
                                        if (sug != null && sug.length() > 0) Text("▾", Modifier.clickable { menu = true }.padding(6.dp), style = deskText(14))
                                    })
                                DropdownMenu(menu, { menu = false }) {
                                    val sug = r.optJSONArray("suggestions") ?: JSONArray()
                                    for (k in 0 until sug.length()) DropdownMenuItem({ Text(sug.getString(k)) }, {
                                        menu = false; name = sug.getString(k)
                                        edit("what" to "name", "row" to i, "value" to sug.getString(k))
                                    })
                                }
                            }
                            Text(tr("Remove mode"), Modifier.clickable { edit("what" to "remove", "row" to i) }.padding(top = 2.dp),
                                style = deskText(12, color = OrcaColors.error))
                        }
                    }
                    // the tiles, row by row as on the bed (GridSizer n_rows x n_cols)
                    val tiles = r.optJSONArray("tiles") ?: JSONArray()
                    Column(Modifier.padding(end = 8.dp), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                        for (rowStart in 0 until tiles.length() step cols) Row(horizontalArrangement = Arrangement.spacedBy(2.dp)) {
                            for (k in rowStart until minOf(rowStart + cols, tiles.length())) {
                                val t = tiles.getJSONObject(k)
                                Box(Modifier.size(38.dp).clip(RoundedCornerShape(4.dp)).background(color(t.optString("bg")))
                                    .clickable(enabled = t.optBoolean("enabled")) { edit("what" to "tile", "row" to i, "tile" to k) }, Alignment.Center) {
                                    Text(t.optString("label"), style = deskText(12, FontWeight.Bold, color(t.optString("fg"))))
                                }
                            }
                        }
                    }
                    var gcode by remember(r.optString("gcode")) { mutableStateOf(r.optString("gcode")) }
                    OutlinedTextField(gcode, { gcode = it; edit("what" to "gcode", "row" to i, "value" to it) },
                        Modifier.weight(1f).heightIn(min = 60.dp), textStyle = monoStyle.copy(color = c.text))
                    Column(Modifier.padding(start = 4.dp)) {
                        Box(Modifier.size(32.dp).clickable { editGcode = i }, Alignment.Center) { OrcaIcon("edit", 20.dp) }
                        val reset = r.optString("reset")
                        if (reset.isNotEmpty()) Box(Modifier.size(32.dp).clickable(enabled = reset == "undo") { edit("what" to "reset", "row" to i) }, Alignment.Center) {
                            OrcaIcon(reset, 20.dp)
                        }
                    }
                }
            }
        }
    }
    editGcode?.let { i ->
        val r = (s.optJSONArray("rows") ?: JSONArray()).optJSONObject(i)
        if (r == null) editGcode = null
        else EditGCodeDialog(app, "imex_mode_gcode", r.optString("gcode"), onDismiss = { editGcode = null }) {
            editGcode = null
            edit("what" to "gcode", "row" to i, "value" to it)
        }
    }
}
