package org.orcaport.mobile.settings

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import kotlinx.coroutines.launch
import org.json.JSONArray
import org.json.JSONObject
import org.orcaport.mobile.AppState
import org.orcaport.mobile.engine.jobj
import org.orcaport.mobile.ui.*

private val MODES = listOf("simple", "advanced", "expert", "develop")
private fun modeRank(m: String) = MODES.indexOf(m).coerceAtLeast(0)

/** The edited preset as the engine reports it (Settings.cpp config_state). */
class ConfigState(val j: JSONObject) {
    val values: JSONObject = j.optJSONObject("values") ?: JSONObject()
    val enabled: JSONObject = j.optJSONObject("enabled") ?: JSONObject()
    val visible: JSONObject = j.optJSONObject("visible") ?: JSONObject()
    val labels: JSONObject = j.optJSONObject("labels") ?: JSONObject()
    val enums: JSONObject = j.optJSONObject("enums") ?: JSONObject()
    val dirty: Set<String> = j.optJSONArray("dirty").strings().toSet()
    val sysDiff: Set<String> = j.optJSONArray("sys_diff").strings().toSet()
    val mode: Int = j.optInt("mode", 2)
    val extruders: Int = j.optInt("extruders", 1)

    fun value(key: String, idx: Int): String? {
        val v = values.opt(key) ?: return null
        return when (v) {
            is JSONArray -> if (v.length() == 0) "" else v.optString(idx.coerceIn(0, v.length() - 1))
            else -> v.toString()
        }
    }
    fun isEnabled(key: String, idx: Int) = enabled.optBoolean("$key#$idx", enabled.optBoolean(key, true))
    fun isVisible(key: String, idx: Int) = visible.optBoolean("$key#$idx", visible.optBoolean(key, true))
    fun isDirty(key: String, idx: Int) = key in dirty || "$key#$idx" in dirty
    fun isSysDiff(key: String, idx: Int) = key in sysDiff || "$key#$idx" in sysDiff
}

fun JSONArray?.strings(): List<String> = if (this == null) emptyList() else (0 until length()).map { optString(it) }

/** The state and actions of one settings tab (desktop Tab), shared by the sidebar panel and the dialogs. */
class ParamsModel(val app: AppState, val type: String) {
    var state by mutableStateOf<ConfigState?>(null)
    var page by mutableIntStateOf(0)
    var extruder by mutableIntStateOf(0)
    var search by mutableStateOf("")
    var searching by mutableStateOf(false)
    var codeEdit by mutableStateOf<Pair<LayoutOption, Int>?>(null)
    var customEdit by mutableStateOf<String?>(null)
    var saveDialog by mutableStateOf(false)
    var confirmDelete by mutableStateOf(false)
    /** Sidebar::jump_to_option: the option shown highlighted for a moment */
    var highlight by mutableStateOf<String?>(null)
    /** the list item the next page change scrolls to (else the top) */
    var scrollTo: Int? = null

    /** Objects mode: the object / part whose overrides are edited ({"obj", "vol"}), null for the preset itself */
    var target by mutableStateOf<JSONObject?>(null)
    private fun args(vararg kv: Pair<String, Any?>): JSONObject {
        val a = jobj(*kv)
        target?.let { t ->
            a.put("obj", t.optInt("obj")).put("vol", t.optInt("vol", -1))
            t.optJSONArray("range")?.let { a.put("range", it) }
        } ?: a.put("type", type)
        return a
    }
    private val prefix get() = if (target != null) "objconfig" else "config"

    suspend fun reload() { state = ConfigState(app.engine.obj("$prefix.get", args())) }
    fun set(key: String, idx: Int, value: Any) = app.scope.launch {
        try {
            val a = args("key" to key, "value" to value)
            if (idx >= 0) a.put("index", idx)
            state = ConfigState(app.engine.obj("$prefix.set", a))
            if (target == null) app.presets = app.engine.obj("presets.state") else app.project = app.engine.obj("project.state")
            app.redraw()   // the 3D view shows printer changes (bed shape, texture, model)
        } catch (e: Exception) { app.toast = e.message; reload() }
    }
    fun reset(key: String, to: String) = app.scope.launch {
        state = ConfigState(app.engine.obj("$prefix.reset", args("key" to key, "to" to to)))
        if (target == null) app.presets = app.engine.obj("presets.state") else app.project = app.engine.obj("project.state")
        app.redraw()
    }
}

@Composable
fun rememberParams(app: AppState, type: String): ParamsModel {
    val m = remember(type) { ParamsModel(app, type) }
    LaunchedEffect(type, app.presets, app.modeVersion) { m.reload() }
    return m
}

/** the pages of a tab that show something in the current mode (pages left empty are hidden, as on the desktop) */
fun visiblePages(m: ParamsModel): List<LayoutPage> {
    val app = m.app
    val st = m.state ?: return emptyList()
    val layout = app.layout ?: return emptyList()
    val flavor = st.value("gcode_flavor", 0) ?: ""
    val marlin = flavor in setOf("marlin", "marlin2", "klipper", "reprapfirmware", "repetier")
    // TabPrintModel::build: a "Frequent" page first for objects and parts
    val frequent = if (m.target == null) emptyList() else listOf(LayoutPage("Frequent", "empty", listOf(LayoutGroup("", "",
        listOf("layer_height", "sparse_infill_density", "wall_loops", "enable_support").map { k ->
            LayoutLine.Single(LayoutOption(k, null, JSONObject(), null), null, false, false) as LayoutLine }, false, false)), false))
    return (frequent + (layout.tabs[m.type] ?: emptyList())).filter { p ->
        (!p.marlinOnly || marlin) && p.groups.any { g -> g.lines.any { l -> rowFor(l, st, app.schema, layout, m.extruder, "") != null } }
    }
}

/** The page tab strip (Tab::m_tabctrl: underlined label of the selected page). */
@Composable
fun PageTabs(m: ParamsModel, pages: List<LayoutPage>) {
    val c = desk()
    Row(Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()).padding(horizontal = 6.dp)) {
        pages.forEachIndexed { i, p ->
            val sel = i == m.page
            Column(Modifier.clickable { m.page = i }.padding(horizontal = 10.dp, vertical = 6.dp), horizontalAlignment = Alignment.CenterHorizontally) {
                Text(tr(p.title), style = deskText(14, if (sel) FontWeight.Bold else FontWeight.Normal, if (sel) c.text else c.textDim), maxLines = 1)
                Spacer(Modifier.height(4.dp))
                Box(Modifier.height(3.dp).width(if (sel) 48.dp else 0.dp).clip(RoundedCornerShape(2.dp)).background(c.accent))
            }
        }
    }
}

/** the settings tab the fields belong to (the preset type of the plugin fields) */
val LocalParams = staticCompositionLocalOf<ParamsModel?> { null }

/** The option groups of the current page (or of every page while searching). */
@Composable
fun ParamsContent(m: ParamsModel, pages: List<LayoutPage>, modifier: Modifier = Modifier) {
    val app = m.app
    val st = m.state
    val layout = app.layout ?: return
    if (st == null) { Box(modifier, Alignment.Center) { CircularProgressIndicator(color = desk().accent) }; return }
    if (m.page >= pages.size) m.page = 0
    val listState = rememberLazyListState()
    LaunchedEffect(m.page, m.type) { listState.scrollToItem(m.scrollTo ?: 0); m.scrollTo = null }
    // Sidebar::jump_to_option (a Speed Dial setting): the option's page, scrolled to its group, the row highlighted
    LaunchedEffect(app.jumpOption, pages) {
        val j = app.jumpOption ?: return@LaunchedEffect
        if (j.optString("type") != m.type || m.target != null) return@LaunchedEffect
        val key = j.optString("key")
        fun has(l: LayoutLine) = when (l) {
            is LayoutLine.Single -> l.option.key == key
            is LayoutLine.Multi -> l.options.any { it.key == key }
            is LayoutLine.Custom -> l.key == key
            else -> false
        }
        // not on these pages yet (a mode switch first reloads them): tried again when they change
        val pi = pages.indexOfFirst { p -> p.groups.any { g -> g.lines.any(::has) && g.lines.any { l -> has(l) && rowFor(l, st, app.schema, layout, m.extruder, "") != null } } }
        if (pi < 0) return@LaunchedEffect
        m.searching = false; m.search = ""
        // the list item of the group: groups with rows, after the extruder switch
        var item = if (m.type == "printer" && pages[pi].title == "Extruder" && st.extruders > 1) 1 else 0
        for (g in pages[pi].groups) {
            if (g.lines.mapNotNull { l -> rowFor(l, st, app.schema, layout, m.extruder, "") }.isEmpty()) continue
            if (g.lines.any(::has)) break
            item++
        }
        m.highlight = key
        if (m.page != pi) { m.scrollTo = item; m.page = pi } else listState.scrollToItem(item)
        app.jumpOption = null
    }
    LaunchedEffect(m.highlight) { if (m.highlight != null) { kotlinx.coroutines.delay(2500); m.highlight = null } }
    val shown = if (m.searching && m.search.isNotBlank()) pages else listOfNotNull(pages.getOrNull(m.page))
    val showExtruderSwitch = !m.searching && m.type == "printer" && pages.getOrNull(m.page)?.title == "Extruder" && st.extruders > 1
    CompositionLocalProvider(LocalParams provides m) {
    LazyColumn(modifier.background(desk().panel), state = listState, contentPadding = PaddingValues(bottom = 24.dp)) {
        if (showExtruderSwitch) item {
            Row(Modifier.padding(8.dp)) {
                DeskSwitchN((1..st.extruders).map { "Extruder $it" }, m.extruder) { m.extruder = it }
            }
        }
        for (p in shown) for (g in p.groups) {
            val rows = g.lines.mapNotNull { l -> rowFor(l, st, app.schema, layout, m.extruder, m.search.trim()) }
            if (rows.isEmpty()) continue
            item(key = "${p.title}/${g.title}/${rows.size}") {
                Column {
                    GroupTitle(g.icon, if (m.searching) "${tr(p.title)} › ${tr(g.title)}" else g.title)
                    for (r in rows) {
                        if ((r.line as? LayoutLine.Custom)?.key == "widget:Modes") { IMEXModesEditor(m); continue }
                        val lit = m.highlight != null && (r.options.any { it.key == m.highlight } || (r.line as? LayoutLine.Custom)?.key == m.highlight)
                        Box(if (lit) Modifier.background(desk().accent.copy(alpha = 0.25f)) else Modifier) {
                            RowView(app, r, st, app.schema, layout, m.extruder, m::set, m::reset,
                                onCode = { o, i -> m.codeEdit = o to i }, onCustom = { m.customEdit = it })
                        }
                    }
                }
            }
        }
    }
    m.codeEdit?.let { (o, i) ->
        EditGCodeDialog(app, o.key, st.value(o.key, i) ?: "",
            onDismiss = { m.codeEdit = null }, onSave = { m.set(o.key, if (st.values.opt(o.key) is JSONArray) i else -1, it); m.codeEdit = null })
    }
    }
    m.customEdit?.let { key -> CustomEditor(app, m.type, key, st, onSet = { k, v -> m.set(k, -1, v) }, onDismiss = { m.customEdit = null }) }
    if (m.saveDialog) SavePresetDialog(app, m.type, onDone = { m.saveDialog = false })
    if (m.confirmDelete) AlertDialog(
        onDismissRequest = { m.confirmDelete = false },
        title = { Text(tr("Delete preset")) },
        text = { Text(tr("Are you sure to delete the selected preset?\nIf the preset corresponds to a filament currently in use on your printer, please reset the filament information for that slot.")) },
        confirmButton = { TextButton({ m.confirmDelete = false; app.call("presets.delete", jobj("type" to m.type)) { if (it is JSONObject) app.presets = it } }) { Text(tr("Delete")) } },
        dismissButton = { TextButton({ m.confirmDelete = false }) { Text(tr("Cancel")) } })
}

@Composable
fun DeskSwitchN(labels: List<String>, selected: Int, onSelect: (Int) -> Unit) {
    val c = desk()
    Row(Modifier.height(30.dp).clip(RoundedCornerShape(15.dp)).border(1.dp, c.border, RoundedCornerShape(15.dp)).padding(2.dp)) {
        labels.forEachIndexed { i, l ->
            Box(Modifier.fillMaxHeight().clip(RoundedCornerShape(13.dp)).background(if (i == selected) c.accent else Color.Transparent)
                .clickable { onSelect(i) }.padding(horizontal = 12.dp), Alignment.Center) {
                Text(l, style = deskText(12, color = if (i == selected) Color.White else c.textDim))
            }
        }
    }
}

/** The preset row of a tab: combo + save / delete / discard / search (Tab::m_presets_choice and its buttons). */
@Composable
fun PresetRow(m: ParamsModel, showSearch: Boolean = true) {
    val app = m.app
    val st = m.state ?: return
    var list by remember { mutableStateOf<List<JSONObject>>(emptyList()) }
    var open by remember { mutableStateOf(false) }
    val dirty = st.dirty.isNotEmpty()
    Row(Modifier.fillMaxWidth().padding(horizontal = 10.dp, vertical = 6.dp), verticalAlignment = Alignment.CenterVertically) {
        val name = st.j.optString("preset")
        DeskCombo(st.j.optString("label", name) + if (dirty) "*" else "", Modifier.weight(1f), textColor = if (dirty) desk().modified else null) {
            app.presetList(m.type) { a -> list = (0 until a.length()).map { a.getJSONObject(it) }; open = true }
        }
        Spacer(Modifier.width(6.dp))
        BoxedIcon("save", "Save current preset") { m.saveDialog = true }
        if (st.j.optBoolean("is_user")) BoxedIcon("cross", "Delete this preset") { m.confirmDelete = true }
        if (dirty) BoxedIcon("undo", "Discard all changes") { app.call("presets.discard", jobj("type" to m.type)) { if (it is JSONObject) app.presets = it } }
        if (showSearch) BoxedIcon("search", "Search in preset") { m.searching = !m.searching; if (!m.searching) m.search = "" }
        if (open) PresetPicker(list, name, { open = false }) { open = false; app.selectPreset(m.type, it) }
    }
    if (m.searching) Box(Modifier.padding(horizontal = 10.dp, vertical = 4.dp)) {
        DeskInput(m.search, true, false, Modifier.fillMaxWidth()) { m.search = it }
    }
}

/**
 * The printer / filament / process settings dialog (desktop ParamsDialog with its Tab): title bar, preset row, the
 * page tabs and the option groups. Back or the close button closes it; edits stay in the edited preset.
 */
@Composable
fun SettingsEditor(app: AppState, type: String, onClose: () -> Unit) {
    val m = rememberParams(app, type)
    val c = desk()
    BackHandler { if (m.searching) { m.searching = false; m.search = "" } else onClose() }
    val title = tr(when (type) { "printer" -> "Printer settings"; "filament" -> "Filament settings"; else -> "Print settings" })
    val compact = isCompact()
    Box(Modifier.fillMaxSize().background(Color(0x80000000)).systemBarsPadding().imePadding(), Alignment.Center) {
        Surface(if (compact) Modifier.fillMaxSize() else Modifier.widthIn(max = 980.dp).fillMaxWidth(0.92f).fillMaxHeight(0.94f),
            shape = if (compact) androidx.compose.ui.graphics.RectangleShape else RoundedCornerShape(6.dp), color = c.panel) {
            Column {
                Row(Modifier.fillMaxWidth().background(c.header).height(44.dp).padding(start = 14.dp), verticalAlignment = Alignment.CenterVertically) {
                    Text(title, Modifier.weight(1f), style = deskText(15))
                    ModeSwitch(m)
                    IconButton(onClick = onClose) { OrcaIcon("topbar_close", 18.dp, darkVariant = false, alpha = 0.7f) }
                }
                PresetRow(m)
                val pages = visiblePages(m)
                if (!m.searching) PageTabs(m, pages)
                HorizontalDivider(color = c.border)
                ParamsContent(m, pages, Modifier.fillMaxSize())
            }
        }
    }
}

/** the Simple / Advanced / Expert switch (desktop: the mode switch in the process title bar / preferences) */
@Composable
fun ModeSwitch(m: ParamsModel) {
    val st = m.state ?: return
    var open by remember { mutableStateOf(false) }
    // the mode names with the "Settings Mode" context (SpeedDialDialog mode_label, Sidebar mode switch)
    val names = listOf("Simple", "Advanced", "Expert").map { trc(it, "Settings Mode") }
    Box {
        Row(Modifier.clickable { open = true }.padding(horizontal = 10.dp, vertical = 6.dp), verticalAlignment = Alignment.CenterVertically) {
            OrcaIcon("advanced", 20.dp); Spacer(Modifier.width(6.dp))
            Text(names.getOrElse(st.mode.coerceIn(0, 2)) { "Expert" }, style = deskText(13, color = desk().textDim))
        }
        DropdownMenu(open, { open = false }) {
            names.forEachIndexed { i, n -> DropdownMenuItem(text = { Text(n) }, onClick = {
                open = false
                m.app.scope.launch { m.app.engine.call("config.set_mode", jobj("mode" to MODES[i])); m.reload() }
            }) }
        }
    }
}

/** A preset list grouped like the desktop combos: project / user presets first, then system ones by vendor; [actions]
 *  are the combo's last items (PlaterPresetComboBox: "Add/Remove filament", "Create printer", ...). */
@Composable
fun PresetPicker(list: List<JSONObject>, selected: String, onDismiss: () -> Unit, actions: List<Pair<String, () -> Unit>> = emptyList(),
                 onSelect: (String) -> Unit) {
    var filter by remember { mutableStateOf("") }
    val c = desk()
    Dialog(onDismissRequest = onDismiss, properties = DialogProperties(usePlatformDefaultWidth = false)) {
        Surface(Modifier.widthIn(max = 560.dp).fillMaxWidth(0.94f).fillMaxHeight(0.85f), shape = RoundedCornerShape(6.dp), color = c.panel) {
            Column {
                Box(Modifier.padding(10.dp)) { DeskInput(filter, true, false, Modifier.fillMaxWidth()) { filter = it } }
                val shown = list.filter { filter.isBlank() || it.optString("label").contains(filter, true) }
                val groups = shown.groupBy {
                    when {
                        it.optBoolean("project") -> "Project-inside presets"
                        it.optBoolean("user") -> "User presets"
                        it.optBoolean("bundle") -> "Bundle presets"
                        it.optBoolean("default") -> "Default"
                        else -> "System presets"
                    }
                }.toSortedMap(compareBy<String> { when (it) { "Project-inside presets" -> 0; "User presets" -> 1; "Bundle presets" -> 2; "System presets" -> 3; else -> 4 } })
                LazyColumn(Modifier.weight(1f)) {
                    for ((g, items) in groups) {
                        item { Text(tr(g), Modifier.fillMaxWidth().background(c.header).padding(horizontal = 14.dp, vertical = 6.dp), style = deskText(13, FontWeight.Bold, c.textDim)) }
                        items(items, key = { "$g/" + it.optString("name") }) { p ->
                            val name = p.optString("name")
                            Row(Modifier.fillMaxWidth().background(if (name == selected) c.selection else Color.Transparent)
                                .clickable { onSelect(name) }.padding(horizontal = 14.dp, vertical = 11.dp), verticalAlignment = Alignment.CenterVertically) {
                                p.optString("color").takeIf { it.isNotEmpty() }?.let { col ->
                                    parseColor(col)?.let { Box(Modifier.size(16.dp).background(it, RoundedCornerShape(3.dp)).border(1.dp, c.border, RoundedCornerShape(3.dp))); Spacer(Modifier.width(8.dp)) }
                                }
                                Text(p.optString("label"), Modifier.weight(1f), style = deskText(14, color = if (!p.optBoolean("compatible", true)) OrcaColors.error else c.text))
                            }
                        }
                    }
                    if (filter.isBlank()) items(actions, key = { "action/" + it.first }) { (label, run) ->
                        Row(Modifier.fillMaxWidth().clickable { onDismiss(); run() }.padding(horizontal = 14.dp, vertical = 11.dp),
                            verticalAlignment = Alignment.CenterVertically) {
                            OrcaIcon("edit", 16.dp); Spacer(Modifier.width(8.dp))
                            Text(label, style = deskText(14))
                        }
                    }
                }
                Row(Modifier.align(Alignment.End).padding(8.dp)) { TextButton(onClick = onDismiss) { Text(tr("Cancel"), style = deskText(14, color = c.accent)) } }
            }
        }
    }
}

/** what a layout line becomes on screen, after mode / visibility / search filtering */
class Row(val line: LayoutLine, val options: List<LayoutOption>, val label: String, val tooltip: String)

private fun idxOf(o: LayoutOption, extruder: Int) = when (o.idx) { null -> 0; "extruder_idx" -> extruder; else -> o.idx.toIntOrNull() ?: 0 }

private fun def(schema: JSONObject, layout: Layout, o: LayoutOption): JSONObject? =
    o.customDef?.takeIf { it.length() > 0 }?.let { JSONObject(it.toString()).apply { layout.customDefs.optJSONObject(o.key)?.let { c -> for (k in c.keys()) put(k, c.get(k)) } } }
        ?: schema.optJSONObject(o.key) ?: layout.customDefs.optJSONObject(o.key)

fun rowFor(l: LayoutLine, st: ConfigState, schema: JSONObject, layout: Layout, extruder: Int, search: String): Row? {
    val mode = st.mode
    fun shown(o: LayoutOption): Boolean {
        val d = def(schema, layout, o) ?: return false
        if (modeRank(d.optString("mode", "simple")) > mode) return false
        if (!st.values.has(o.key) && o.customDef == null && !layout.customDefs.has(o.key)) return false
        return st.isVisible(o.key, idxOf(o, extruder))
    }
    val row = when (l) {
        is LayoutLine.Single -> {
            if (!shown(l.option)) return null
            val d = def(schema, layout, l.option)!!
            val label = l.option.overrides.optString("label").ifEmpty { st.labels.optString(l.option.key).ifEmpty { d.optString("label").ifEmpty { d.optString("full_label") } } }
            Row(l, listOf(l.option), label, l.option.overrides.optString("tooltip").ifEmpty { d.optString("tooltip") })
        }
        is LayoutLine.Multi -> {
            val opts = l.options.filter { shown(it) }
            if (opts.isEmpty()) return null
            Row(l, opts, l.label, l.tooltip ?: l.tooltipOf?.let { schema.optJSONObject(it)?.optString("tooltip") } ?: "")
        }
        is LayoutLine.Custom -> {
            // IMEXModesCtrl::set_applicable: only on an IDEX/IQEX printer
            if (l.key == "widget:Modes" && st.value("is_imex", 0) != "1") return null
            val d = schema.optJSONObject(l.key) ?: layout.customDefs.optJSONObject(l.key)
            if (d != null && modeRank(d.optString("mode", "simple")) > mode) return null
            if (!st.isVisible(l.key, 0)) return null
            Row(l, emptyList(), l.label ?: d?.optString("label") ?: l.key, d?.optString("tooltip") ?: "")
        }
        LayoutLine.Separator -> Row(l, emptyList(), "", "")
    }
    if (search.isNotEmpty() && l !is LayoutLine.Separator) {
        val hay = row.label + " " + row.options.joinToString(" ") { it.key + " " + (schema.optJSONObject(it.key)?.optString("label") ?: "") }
        if (!hay.contains(search, true)) return null
    }
    return row
}

private val FIELD_W = 128.dp

@Composable
private fun RowView(
    app: AppState, r: Row, st: ConfigState, schema: JSONObject, layout: Layout, extruder: Int,
    set: (String, Int, Any) -> Unit, reset: (String, String) -> Unit,
    onCode: (LayoutOption, Int) -> Unit, onCustom: (String) -> Unit,
) {
    val c = desk()
    var help by remember { mutableStateOf(false) }
    when (val l = r.line) {
        LayoutLine.Separator -> { Box(Modifier.padding(horizontal = 30.dp, vertical = 4.dp).fillMaxWidth().height(1.dp).background(c.border)); return }
        is LayoutLine.Custom -> {
            // a check box kept in the app config, not the preset (Tab.cpp's "Pre-slice warnings" widget)
            val appKey = layout.customDefs.optJSONObject(l.key)?.takeIf { it.optString("type") == "app_bool" }?.optString("app_config")
            if (appKey != null) {
                var on by remember(appKey) { mutableStateOf(true) }
                LaunchedEffect(appKey) { app.call("app.get", org.orcaport.mobile.engine.jobj("key" to appKey)) { on = it != "false" } }
                Row(Modifier.fillMaxWidth().heightIn(min = 40.dp).padding(start = 30.dp, end = 10.dp), verticalAlignment = Alignment.CenterVertically) {
                    Text(r.label, Modifier.weight(1f).clickable { help = !help }, style = deskText(14))
                    DeskCheck(on) { v -> on = v; app.call("app.set", org.orcaport.mobile.engine.jobj("key" to appKey, "value" to if (v) "true" else "false")) }
                }
                if (help) layout.customDefs.optJSONObject(l.key)?.optString("tooltip")?.takeIf { it.isNotEmpty() }?.let {
                    Text(tr(it), Modifier.padding(start = 30.dp, end = 10.dp, bottom = 6.dp), style = deskText(12, color = c.textDim))
                }
                return
            }
            Row(Modifier.fillMaxWidth().heightIn(min = 40.dp).padding(start = 30.dp, end = 10.dp), verticalAlignment = Alignment.CenterVertically) {
                Text(r.label, Modifier.weight(1f), style = deskText(14, color = if (st.isDirty(l.key, 0)) c.modified else c.text))
                Box(Modifier.clip(RoundedCornerShape(4.dp)).border(1.dp, c.border, RoundedCornerShape(4.dp)).clickable { onCustom(l.key) }
                    .padding(horizontal = 14.dp, vertical = 7.dp)) { Text(tr("Set") + " …", style = deskText(14)) }
            }
            return
        }
        else -> {}
    }
    val keys = r.options.map { it.key to idxOf(it, extruder) }
    val dirty = keys.any { (k, i) -> st.isDirty(k, i) }
    val sysDiff = keys.any { (k, i) -> st.isSysDiff(k, i) }
    val single = r.line as? LayoutLine.Single
    val first = r.options.first()
    val firstDef = def(schema, layout, first) ?: JSONObject()
    val fullWidth = first.overrides.optBoolean("full_width", firstDef.optBoolean("full_width")) ||
        first.overrides.optBoolean("is_code", firstDef.optBoolean("is_code")) || firstDef.optBoolean("multiline")
    val resetKey = { k: String, i: Int -> if (st.values.opt(k) is JSONArray && first.idx != null) "$k#$i" else k }
    Column(Modifier.fillMaxWidth().padding(start = 8.dp, end = 10.dp, top = 2.dp, bottom = 2.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            // Tab::update_changed_ui: an orange label and the undo arrow for a value changed from the saved preset,
            // the open lock for a value that differs from the system preset
            Box(Modifier.size(22.dp), Alignment.Center) {
                when {
                    dirty -> Box(Modifier.size(22.dp).clickable { keys.forEach { (k, i) -> reset(resetKey(k, i), "saved") } }, Alignment.Center) { OrcaIcon("undo", 14.dp) }
                    sysDiff && st.j.optBoolean("has_parent") -> Box(Modifier.size(22.dp).clickable { keys.forEach { (k, i) -> reset(resetKey(k, i), "system") } }, Alignment.Center) { OrcaIcon("lock_open", 14.dp) }
                }
            }
            if (single?.override == true) {   // filament override: checked = own value, unchecked = nil (printer value)
                val v = st.value(first.key, idxOf(first, extruder))
                DeskCheck(v != null && v != "nil") { on ->
                    set(first.key, idxOf(first, extruder), if (on) (firstDef.optString("default").split(",").firstOrNull()?.takeIf { it != "nil" } ?: "0") else "nil")
                }
            }
            Text(r.label, Modifier.weight(1f).clickable { help = !help }.padding(vertical = 8.dp, horizontal = 4.dp),
                style = deskText(14, color = if (dirty) c.modified else c.text), maxLines = 3)
            if (!fullWidth) {
                Row(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalAlignment = Alignment.CenterVertically) {
                    for (o in r.options) Editor(o, st, schema, layout, extruder, Modifier.width(if (r.options.size > 1) 104.dp else FIELD_W), set, onCode, showLabel = r.options.size > 1)
                }
            }
        }
        if (fullWidth) Editor(first, st, schema, layout, extruder, Modifier.fillMaxWidth().padding(start = 26.dp), set, onCode, showLabel = false)
        if (help && r.tooltip.isNotEmpty())
            Text(r.tooltip, Modifier.padding(start = 26.dp, end = 4.dp, bottom = 6.dp), style = deskText(12, color = c.textDim))
    }
}

@Composable
private fun Editor(
    o: LayoutOption, st: ConfigState, schema: JSONObject, layout: Layout, extruder: Int, modifier: Modifier,
    set: (String, Int, Any) -> Unit, onCode: (LayoutOption, Int) -> Unit, showLabel: Boolean,
) {
    val c = desk()
    val d = def(schema, layout, o) ?: JSONObject()
    val idx = idxOf(o, extruder)
    val raw = st.values.opt(o.key)
    val isVector = raw is JSONArray
    val setIdx = if (isVector) idx else -1
    val v = st.value(o.key, idx) ?: d.optString("default")
    val nil = v == "nil"
    val enabled = st.isEnabled(o.key, idx) && !d.optBoolean("readonly") && !nil
    val type = d.optString("type")
    val side = o.overrides.optString("sidetext").ifEmpty { d.optString("sidetext") }
    Column(modifier) {
        if (showLabel) Text(o.overrides.optString("label").ifEmpty { d.optString("label") }, style = deskText(11, color = c.textDim), maxLines = 1)
        when {
            type == "app_bool" -> DeskCheck(v != "false", enabled = false) {}
            type == "bool" || type == "bools" -> DeskCheck(v == "1" || v == "true", enabled) { set(o.key, setIdx, if (it) "1" else "0") }
            type == "enum" || type == "enums" -> {
                val all = d.optJSONArray("enum_values").strings()
                val labels = d.optJSONArray("enum_labels").strings().ifEmpty { all }
                val allowed = st.enums.optJSONArray(o.key)?.strings()?.toSet()
                val pairs = all.indices.filter { allowed == null || all[it] in allowed }.map { all[it] to labels.getOrElse(it) { all[it] } }
                DeskChoice(pairs.map { it.second }, pairs.indexOfFirst { it.first == v }, Modifier.fillMaxWidth(), enabled,
                    display = pairs.firstOrNull { it.first == v }?.second ?: v) { set(o.key, setIdx, pairs[it].first) }
            }
            // PluginField: a row per picked capability (tap to change it, ✕ to remove), Add plugin
            d.optString("gui_type") == "plugin_picker" -> PluginPickerField(o.key, raw as? JSONArray ?: JSONArray(), enabled) { set(o.key, -1, it) }
            // PluginConfigField: Configure... (n) opens PluginsConfigDialog on the preset's overrides
            d.optString("gui_type") == "plugin_config" -> PluginConfigField(v, enabled) { set(o.key, -1, it) }
            d.optString("gui_type") == "color" || o.key.endsWith("_colour") || o.key.endsWith("_color") -> ColorField(v, enabled) { set(o.key, setIdx, it) }
            d.optString("gui_type").let { it == "i_enum_open" || it == "f_enum_open" } -> {
                val values = d.optJSONArray("enum_values").strings()
                val labels = d.optJSONArray("enum_labels").strings().ifEmpty { values }
                var open by remember { mutableStateOf(false) }
                Box {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Box(Modifier.size(28.dp, 36.dp).clickable(enabled = enabled) { open = true }, Alignment.Center) { OrcaIcon("drop_down", 12.dp) }
                        DeskInput(v, enabled, true, Modifier.weight(1f), side) { set(o.key, setIdx, it) }
                    }
                    DropdownMenu(open, { open = false }) { values.forEachIndexed { i, x -> DropdownMenuItem(text = { Text(labels[i]) }, onClick = { open = false; set(o.key, setIdx, x) }) } }
                }
            }
            type == "string" || type == "strings" -> {
                val code = o.overrides.optBoolean("is_code", d.optBoolean("is_code")) || d.optBoolean("multiline") || o.overrides.optBoolean("multiline")
                if (code) {
                    Box(Modifier.fillMaxWidth().heightIn(min = 48.dp, max = 180.dp).clip(RoundedCornerShape(4.dp)).background(c.field)
                        .border(1.dp, c.border, RoundedCornerShape(4.dp)).clickable(enabled = enabled) { onCode(o, idx) }.padding(8.dp)) {
                        Text(v.ifEmpty { " " }, style = monoStyle.copy(fontSize = 11.sp, color = c.text), maxLines = 10, overflow = TextOverflow.Ellipsis)
                    }
                } else DeskInput(v, enabled, false, Modifier.fillMaxWidth(), side) { set(o.key, setIdx, it) }
            }
            type == "points" || type == "point" || type == "point3" ->
                DeskInput(if (isVector && o.idx == null) (raw as JSONArray).strings().joinToString(", ") else v, enabled, false, Modifier.fillMaxWidth(), side) { t ->
                    if (isVector && o.idx == null) set(o.key, -1, JSONArray(t.split(",").map { it.trim() }.filter { it.isNotEmpty() })) else set(o.key, setIdx, t)
                }
            type == "int" || type == "ints" -> SpinField(if (nil) "N/A" else v, enabled, side, d) { set(o.key, setIdx, it) }
            else -> DeskInput(if (nil) "N/A" else v, enabled, true, Modifier.fillMaxWidth(), side) { set(o.key, setIdx, it) }
        }
    }
}

/** SpinInput: the up / down arrows on the left of the value */
@Composable
private fun SpinField(v: String, enabled: Boolean, side: String, d: JSONObject, onSet: (String) -> Unit) {
    val n = v.toIntOrNull()
    val min = if (d.has("min")) d.optDouble("min").toInt() else Int.MIN_VALUE
    val max = if (d.has("max")) d.optDouble("max").toInt() else Int.MAX_VALUE
    Row(verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.width(24.dp)) {
            Box(Modifier.size(24.dp, 18.dp).clickable(enabled = enabled && n != null) { onSet(((n ?: 0) + 1).coerceIn(min, max).toString()) }, Alignment.Center) {
                Text("▲", style = deskText(9, color = desk().textDim))
            }
            Box(Modifier.size(24.dp, 18.dp).clickable(enabled = enabled && n != null) { onSet(((n ?: 0) - 1).coerceIn(min, max).toString()) }, Alignment.Center) {
                Text("▼", style = deskText(9, color = desk().textDim))
            }
        }
        DeskInput(v, enabled, true, Modifier.weight(1f), side, onCommit = onSet)
    }
}

@Composable
private fun ColorField(v: String, enabled: Boolean, onSet: (String) -> Unit) {
    var open by remember { mutableStateOf(false) }
    Row(verticalAlignment = Alignment.CenterVertically) {
        Box(Modifier.size(36.dp).background(parseColor(v) ?: desk().field, RoundedCornerShape(4.dp))
            .border(1.dp, desk().border, RoundedCornerShape(4.dp)).clickable(enabled = enabled) { open = true })
        Spacer(Modifier.width(8.dp))
        Text(v, style = deskText(12, color = desk().textDim))
    }
    if (open) ColorPickerDialog(parseColor(v), onDismiss = { open = false }) { open = false; onSet(it) }
}

@Composable
private fun SavePresetDialog(app: AppState, type: String, onDone: () -> Unit) {
    val scope = rememberCoroutineScope()
    var name by remember { mutableStateOf("") }
    var check by remember { mutableStateOf(JSONObject()) }
    var toProject by remember { mutableStateOf(false) }
    LaunchedEffect(Unit) { name = app.engine.call("presets.suggest_name", jobj("type" to type)) as String }
    LaunchedEffect(name) { check = app.engine.obj("presets.validate_name", jobj("type" to type, "name" to name)) }
    val level = check.optString("level", "ok")
    AlertDialog(
        onDismissRequest = onDone,
        title = { Text(tr("Save preset"), style = deskText(16, FontWeight.Bold)) },
        text = {
            Column {
                Text(tr("Save %s as").replace("%s", tr(when (type) { "printer" -> "Printer"; "filament" -> "Filament"; else -> "Process" })), style = deskText(14))
                Spacer(Modifier.height(6.dp))
                DeskInput(name, true, false, Modifier.fillMaxWidth()) { name = it }
                if (check.optString("info").isNotEmpty())
                    Text(check.optString("info"), color = if (level == "error") OrcaColors.error else OrcaColors.warning,
                        style = deskText(12, color = if (level == "error") OrcaColors.error else OrcaColors.warning), modifier = Modifier.padding(top = 6.dp))
                Row(verticalAlignment = Alignment.CenterVertically) {
                    RadioButton(!toProject, { toProject = false }); Text(tr("User Preset"), style = deskText(14))
                    Spacer(Modifier.width(12.dp))
                    RadioButton(toProject, { toProject = true }); Text(tr("Preset Inside Project"), style = deskText(14))
                }
            }
        },
        confirmButton = {
            TextButton(enabled = level != "error", onClick = {
                scope.launch {
                    try { app.presets = app.engine.obj("presets.save", jobj("type" to type, "name" to name, "to_project" to toProject)) }
                    catch (e: Exception) { app.toast = e.message }
                    onDone()
                }
            }) { Text(tr("Save")) }
        },
        dismissButton = { TextButton(onDone) { Text(tr("Cancel")) } })
}

@Composable
private fun PluginPickerField(key: String, values: JSONArray, enabled: Boolean, onSet: (JSONArray) -> Unit) {
    val app = LocalParams.current?.app ?: return
    val c = desk()
    val list = (0 until values.length()).map { values.optString(it) }.filter { it.isNotEmpty() }
    fun pick(then: (String) -> Unit) = app.call("plugins.pick", org.orcaport.mobile.engine.jobj("key" to key)) { r -> (r as? String)?.takeIf { it.isNotEmpty() }?.let(then) }
    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Box(Modifier.fillMaxWidth().height(36.dp).clip(RoundedCornerShape(4.dp)).border(1.dp, c.border, RoundedCornerShape(4.dp))
            .clickable(enabled = enabled) { pick { onSet(JSONArray(list + it)) } }.padding(horizontal = 10.dp), Alignment.CenterStart) {
            Row(verticalAlignment = Alignment.CenterVertically) { OrcaIcon("param_add", 16.dp); Spacer(Modifier.width(6.dp)); Text(tr("Add plugin"), style = deskText(14)) }
        }
        list.forEachIndexed { i, name ->
            Row(verticalAlignment = Alignment.CenterVertically) {
                Row(Modifier.weight(1f).height(36.dp).clip(RoundedCornerShape(4.dp)).background(c.field).border(1.dp, c.border, RoundedCornerShape(4.dp))
                    .clickable(enabled = enabled) { pick { p -> onSet(JSONArray(list.toMutableList().also { it[i] = p })) } }.padding(horizontal = 10.dp),
                    verticalAlignment = Alignment.CenterVertically) {
                    Text(name, Modifier.weight(1f), style = deskText(14), maxLines = 1)
                    OrcaIcon("edit", 14.dp)
                }
                Box(Modifier.size(32.dp).clickable(enabled = enabled) { onSet(JSONArray(list.filterIndexed { k, _ -> k != i })) }, Alignment.Center) { OrcaIcon("cross", 14.dp) }
            }
        }
    }
}

@Composable
private fun PluginConfigField(v: String, enabled: Boolean, onSet: (String) -> Unit) {
    val m = LocalParams.current ?: return
    val count = runCatching { JSONArray(v.ifEmpty { "[]" }).length() }.getOrDefault(0)
    var open by remember { mutableStateOf(false) }
    val c = desk()
    Box(Modifier.clip(RoundedCornerShape(4.dp)).border(1.dp, c.border, RoundedCornerShape(4.dp)).clickable(enabled = enabled) { open = true }
        .padding(horizontal = 14.dp, vertical = 7.dp)) {
        Text(if (count == 0) tr("Configure") + "..." else tr("Configure (%d)").replace("%d", count.toString()), style = deskText(14))
    }
    if (open) org.orcaport.mobile.ui.WebDialog(m.app, "plugins_config", org.orcaport.mobile.engine.jobj("type" to m.type, "overrides" to v),
        onResult = { r ->
            val edited = r.optString("overrides")
            // compared as documents: a reformatted serialization alone does not dirty the preset
            val same = runCatching { JSONArray(edited.ifEmpty { "[]" }).toString() == JSONArray(v.ifEmpty { "[]" }).toString() }.getOrDefault(edited == v)
            if (r.has("overrides") && !same) onSet(edited)
        }) { open = false }
}
