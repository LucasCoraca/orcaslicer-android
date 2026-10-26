package org.orcaport.mobile.prepare

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import org.json.JSONArray
import org.json.JSONObject
import org.orcaport.mobile.AppState
import org.orcaport.mobile.engine.jobj
import org.orcaport.mobile.ui.*

/**
 * ObjectTableDialog: the parameter table of every object and part — printable, plate, name, filament, layer height,
 * wall loops, infill density, support, brim type, outer wall speed — with the category list on the left (All /
 * Quality / Strength / Support / Speed), the process value as default, a reset per override, Reset All, and sorting by
 * plate / name / filament.
 */
@Composable
fun ObjectTableDialog(app: AppState, onClose: () -> Unit) {
    var st by remember { mutableStateOf(JSONObject()) }
    var category by remember { mutableStateOf("all") }
    var sort by remember { mutableStateOf<Pair<String, Boolean>?>(null) }
    LaunchedEffect(Unit) { app.call("table.get") { if (it is JSONObject) st = it } }
    fun apply(r: Any?) { if (r is JSONObject) { st = r; app.call("project.state") { p -> if (p is JSONObject) app.project = p }; app.redraw() } }
    fun set(obj: Int, vol: Int, key: String, value: Any) = app.call("table.set", jobj("obj" to obj, "vol" to vol, "key" to key, "value" to value)) { apply(it) }
    fun reset(obj: Int, vol: Int, key: String) = app.call("table.reset", jobj("obj" to obj, "vol" to vol, "key" to key)) { apply(it) }
    val c = desk()
    val cols = st.optJSONArray("columns")?.let { a -> List(a.length()) { a.getJSONObject(it) } } ?: emptyList()
    val shown = cols.filter { category == "all" || it.optString("category") == "all" || it.optString("category") == category }
    val cats = st.optJSONArray("categories")?.let { a -> List(a.length()) { a.getJSONObject(it) } } ?: emptyList()
    val brim = st.optJSONArray("brim_types")?.let { a -> List(a.length()) { a.getJSONObject(it) } } ?: emptyList()
    val fil = st.optJSONArray("filaments")?.let { a -> List(a.length()) { a.optString(it) } } ?: emptyList()
    var rows = st.optJSONArray("rows")?.let { a -> List(a.length()) { a.getJSONObject(it) } } ?: emptyList()
    // ObjectGridTable::sort_column: objects keep their parts below them
    sort?.let { (key, asc) ->
        val groups = rows.groupBy { it.optInt("obj") }.values.sortedWith(compareBy<List<JSONObject>> { g ->
            val r = g.first()
            when (key) { "plate_index" -> r.optString("plate").toIntOrNull() ?: 999; "extruder" -> r.optJSONObject("values")?.optInt("extruder") ?: 0; else -> 0 }
        }.thenBy { g -> if (key == "name") g.first().optString("name").lowercase() else "" })
        rows = (if (asc) groups else groups.reversed()).flatten()
    }
    fun width(key: String): Dp = when (key) { "name" -> 200.dp; "printable" -> 80.dp; "plate_index" -> 70.dp; "brim_type" -> 170.dp; "extruder" -> 120.dp; else -> 130.dp }
    fun fmt(v: Double) = if (v == Math.floor(v)) v.toLong().toString() else String.format(java.util.Locale.ROOT, "%.2f", v)

    Dialog(onClose, DialogProperties(usePlatformDefaultWidth = false)) {
        Column(Modifier.fillMaxSize(0.95f).background(c.panel)) {
            Row(Modifier.fillMaxWidth().background(FrameColors.topbar).padding(horizontal = 16.dp, vertical = 10.dp), verticalAlignment = Alignment.CenterVertically) {
                Text(st.optString("title"), Modifier.weight(1f), style = deskText(16, FontWeight.Bold, Color.White))
                Text("✕", Modifier.clickable(onClick = onClose).padding(6.dp), style = deskText(18, color = Color.White))
            }
            Row(Modifier.weight(1f)) {
                // the category list (m_side_window) with Reset All
                Column(Modifier.width(140.dp).fillMaxHeight().background(c.field).padding(8.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    for (cat in cats) {
                        val sel = cat.optString("key") == category
                        Text(cat.optString("label"), Modifier.fillMaxWidth().background(if (sel) c.selection else Color.Transparent).clickable { category = cat.optString("key") }
                            .padding(horizontal = 8.dp, vertical = 8.dp), style = deskText(14, if (sel) FontWeight.Bold else FontWeight.Normal))
                    }
                    Spacer(Modifier.weight(1f))
                    PanelButton(st.optString("reset_all")) { app.call("table.reset", jobj("category" to category)) { apply(it) } }
                }
                val h = rememberScrollState()
                Column(Modifier.weight(1f).horizontalScroll(h)) {
                    Row(Modifier.background(c.field).padding(vertical = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                        for (col in shown) {
                            val k = col.optString("key")
                            val sortable = k in listOf("plate_index", "name", "extruder")
                            Text(col.optString("label") + if (sortable) " ↑↓" else "", Modifier.width(width(k)).padding(horizontal = 6.dp)
                                .clickable(enabled = sortable) { sort = k to !(sort?.first == k && sort?.second == true) },
                                style = deskText(13, FontWeight.Bold), maxLines = 1, overflow = TextOverflow.Ellipsis)
                        }
                    }
                    HorizontalDivider(color = c.border)
                    Column(Modifier.verticalScroll(rememberScrollState())) {
                        for (r in rows) {
                            val obj = r.optInt("obj"); val vol = r.optInt("vol", -1)
                            val values = r.optJSONObject("values") ?: JSONObject()
                            val overrides = r.optJSONArray("overrides")?.let { a -> List(a.length()) { a.optString(it) } } ?: emptyList()
                            Row(Modifier.background(if (vol >= 0) c.panel else c.field.copy(alpha = 0.4f)).padding(vertical = 2.dp), verticalAlignment = Alignment.CenterVertically) {
                                for (col in shown) {
                                    val k = col.optString("key")
                                    Box(Modifier.width(width(k)).padding(horizontal = 4.dp), Alignment.CenterStart) {
                                        when {
                                            k == "printable" -> if (vol < 0) DeskCheck(r.optBoolean("printable")) { set(obj, vol, k, it) }
                                            k == "plate_index" -> if (vol < 0) Text(r.optString("plate"), style = deskText(13))
                                            k == "name" -> Row(verticalAlignment = Alignment.CenterVertically) {
                                                if (vol >= 0) Spacer(Modifier.width(16.dp))
                                                DeskInput(r.optString("name"), true, false, Modifier.weight(1f)) { s -> if (s.isNotBlank()) set(obj, vol, k, s) }
                                            }
                                            col.optBoolean("only_object") && vol >= 0 -> {}
                                            else -> Row(verticalAlignment = Alignment.CenterVertically) {
                                                Box(Modifier.weight(1f)) {
                                                    when (k) {
                                                        "extruder" -> {
                                                            val labels = (if (vol >= 0) listOf(st.optString("default")) else emptyList()) + fil
                                                            val e = values.optInt(k)
                                                            DeskChoice(labels, if (vol >= 0) e else (e - 1).coerceAtLeast(0), Modifier.fillMaxWidth()) { i -> set(obj, vol, k, if (vol >= 0) i else i + 1) }
                                                        }
                                                        "brim_type" -> {
                                                            val i = brim.indexOfFirst { it.optString("value") == values.optString(k) }
                                                            DeskChoice(brim.map { it.optString("label") }, i, Modifier.fillMaxWidth()) { j -> set(obj, vol, k, brim[j].optString("value")) }
                                                        }
                                                        "enable_support" -> DeskCheck(values.optBoolean(k)) { set(obj, vol, k, it) }
                                                        else -> DeskInput(fmt(values.optDouble(k)), true, true, Modifier.fillMaxWidth()) { s ->
                                                            s.replace(',', '.').toDoubleOrNull()?.let { v -> set(obj, vol, k, if (col.optString("type") == "int") v.toInt() else v) }
                                                        }
                                                    }
                                                }
                                                if (k in overrides) Box(Modifier.size(28.dp).clickable { reset(obj, vol, k) }, Alignment.Center) { OrcaIcon("undo", 16.dp) }
                                                else Spacer(Modifier.width(28.dp))
                                            }
                                        }
                                    }
                                }
                            }
                            HorizontalDivider(color = c.border.copy(alpha = 0.5f))
                        }
                    }
                }
            }
        }
    }
}
