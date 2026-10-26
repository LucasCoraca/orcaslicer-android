package org.orcaport.mobile.settings

import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.text.TextRange
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.TextFieldValue
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import org.json.JSONArray
import org.json.JSONObject
import org.orcaport.mobile.AppState
import org.orcaport.mobile.engine.jobj
import org.orcaport.mobile.ui.*

/** the settings pages as EditGCodeDialog::add_presets_placeholders reads them from the Tabs: title, icon, option keys */
private fun AppState.gcodeTabs(): JSONArray {
    val out = JSONArray()
    val l = layout ?: return out
    for ((type, pages) in l.tabs) {
        val ps = JSONArray()
        for (p in pages) {
            val keys = JSONArray()
            for (g in p.groups) for (line in g.lines) when (line) {
                is LayoutLine.Single -> keys.put(line.option.key)
                is LayoutLine.Multi -> line.options.forEach { keys.put(it.key) }
                else -> {}
            }
            ps.put(jobj("title" to tr(p.title), "icon" to p.icon, "keys" to keys))
        }
        out.put(jobj("type" to type, "pages" to ps))
    }
    return out
}

private class PhNode(val text: String, val icon: String, val key: String?, val children: List<PhNode>, val expand: Boolean, val top: String)

private fun parse(a: JSONArray, top: String?): List<PhNode> = (0 until a.length()).map { i ->
    val o = a.getJSONObject(i)
    val t = top ?: o.optString("text")
    PhNode(o.optString("text"), o.optString("icon"), o.optString("key").takeIf { o.has("key") },
        o.optJSONArray("children")?.let { parse(it, t) } ?: emptyList(), o.optBoolean("expand"), t)
}

/**
 * EditGCodeDialog: the built-in placeholders (groups of params, searchable), the selected one's description, the Add
 * button (a double tap adds too) and the G-code text.
 */
@OptIn(ExperimentalFoundationApi::class)
@Composable
fun EditGCodeDialog(app: AppState, key: String, value: String, onDismiss: () -> Unit, onSave: (String) -> Unit) {
    val c = desk()
    var text by remember { mutableStateOf(TextFieldValue(value, TextRange(value.length))) }
    var info by remember { mutableStateOf<JSONObject?>(null) }
    var tree by remember { mutableStateOf<List<PhNode>>(emptyList()) }
    val expanded = remember { mutableStateMapOf<PhNode, Boolean>() }
    var search by remember { mutableStateOf("") }
    var selected by remember { mutableStateOf<PhNode?>(null) }
    var desc by remember { mutableStateOf(JSONObject()) }
    val editorFocus = remember { FocusRequester() }
    LaunchedEffect(key) {
        app.call("gcode.placeholders", jobj("key" to key, "tabs" to app.gcodeTabs())) { r ->
            if (r !is JSONObject) return@call
            info = r
            tree = parse(r.optJSONArray("tree") ?: JSONArray(), null)
        }
    }
    fun select(n: PhNode) {
        selected = n
        if (n.key != null) app.call("gcode.describe", jobj("key" to n.key, "category" to n.top)) { if (it is JSONObject) desc = it }
    }
    // add_selected_value_to_gcode: at the cursor (on a new line at the end); into "[]", or "current_extruder" selected
    fun add() {
        val v = selected?.takeIf { it.key != null }?.text ?: return
        val atEnd = text.selection.end == text.text.length
        val ins = if (atEnd) "\n" + v else v
        val start = text.selection.start
        val s = text.text.substring(0, start) + ins + text.text.substring(text.selection.end)
        val end = start + ins.length
        val sel = when {
            v.endsWith("[]") -> TextRange(end - 1)
            v.endsWith("]") -> TextRange(end - 17, end - 1)
            else -> TextRange(end)
        }
        text = TextFieldValue(s, sel)
        runCatching { editorFocus.requestFocus() }
    }
    // the visible rows: the tree as expanded, or while searching every matching param under its expanded groups
    val rows = remember(tree, expanded.toMap(), search) {
        val out = mutableListOf<Pair<PhNode, Int>>()
        val q = search.trim().lowercase()
        fun matches(n: PhNode): Boolean = if (n.key != null) n.text.lowercase().contains(q) else n.children.any(::matches)
        fun walk(ns: List<PhNode>, depth: Int) {
            for (n in ns) {
                if (q.isNotEmpty() && !matches(n)) continue
                out += n to depth
                if (n.key == null && (q.isNotEmpty() || expanded[n] ?: n.expand)) walk(n.children, depth + 1)
            }
        }
        walk(tree, 0)
        out
    }
    val i = info
    Dialog(onDismissRequest = onDismiss, properties = DialogProperties(usePlatformDefaultWidth = false, decorFitsSystemWindows = false)) {
        Surface(Modifier.fillMaxSize().systemBarsPadding().imePadding(), color = c.panel) {
            Column {
                Row(Modifier.fillMaxWidth().background(c.header).height(44.dp).padding(start = 14.dp), verticalAlignment = Alignment.CenterVertically) {
                    Text(i?.optString("title") ?: "", Modifier.weight(1f), style = deskText(15))
                    TextButton(onClick = onDismiss) { Text(tr("Cancel"), style = deskText(14, color = c.textDim)) }
                    TextButton(onClick = { onSave(text.text) }) { Text(tr("OK"), style = deskText(14, FontWeight.Bold, c.accent)) }
                }
                Text(i?.optString("label_top") ?: "", Modifier.padding(start = 10.dp, top = 10.dp), style = deskText(13))
                Row(Modifier.weight(1f).padding(10.dp), verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f).fillMaxHeight()) {
                        OutlinedTextField(search, { search = it }, Modifier.fillMaxWidth(), singleLine = true, textStyle = deskText(13),
                            placeholder = { Text(i?.optString("search") ?: "", style = deskText(13, color = c.textDim)) })
                        Spacer(Modifier.height(8.dp))
                        LazyColumn(Modifier.fillMaxSize().border(1.dp, c.border)) {
                            items(rows) { (n, depth) ->
                                val sel = n == selected
                                Row(Modifier.fillMaxWidth().background(if (sel) c.selection else c.panel)
                                    .combinedClickable(
                                        onClick = {
                                            if (n.key == null) expanded[n] = !(expanded[n] ?: n.expand)
                                            select(n)
                                        },
                                        onDoubleClick = { if (n.key != null) { select(n); add() } })
                                    .padding(start = (8 + 16 * depth).dp, top = 4.dp, bottom = 4.dp, end = 8.dp),
                                    verticalAlignment = Alignment.CenterVertically) {
                                    if (n.key == null) Text(if (search.isNotBlank() || (expanded[n] ?: n.expand)) "▾" else "▸", Modifier.width(14.dp), style = deskText(12, color = c.textDim))
                                    else Spacer(Modifier.width(14.dp))
                                    OrcaIcon(n.icon, 16.dp)
                                    Spacer(Modifier.width(6.dp))
                                    Text(n.text, style = deskText(13, if (n.key == null) FontWeight.Bold else FontWeight.Normal).copy(fontFamily = if (n.key != null) monoStyle.fontFamily else null))
                                }
                            }
                        }
                    }
                    Box(Modifier.padding(horizontal = 12.dp).size(40.dp).clickable { add() }, Alignment.Center) { OrcaIcon("add_copies", 28.dp) }
                    OutlinedTextField(text, { text = it }, Modifier.weight(2f).fillMaxHeight().focusRequester(editorFocus), textStyle = monoStyle.copy(color = c.text))
                }
                // m_param_label / m_param_description
                Column(Modifier.padding(start = 10.dp, end = 10.dp, bottom = 10.dp).heightIn(max = 160.dp).verticalScroll(rememberScrollState())) {
                    Text(desc.optString("label").ifEmpty { i?.optString("select") ?: "" }, style = deskText(14, FontWeight.Bold))
                    if (desc.optString("description").isNotEmpty()) Text(desc.optString("description"), style = deskText(13))
                }
            }
        }
    }
}
