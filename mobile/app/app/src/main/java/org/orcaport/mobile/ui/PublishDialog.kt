package org.orcaport.mobile.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import org.json.JSONArray
import org.json.JSONObject
import org.orcaport.mobile.AppState
import org.orcaport.mobile.engine.jobj
import org.orcaport.mobile.prepare.PanelButton
import org.orcaport.mobile.settings.parseColor
import org.orcaport.mobile.settings.LayoutLine
import org.orcaport.mobile.settings.LayoutOption

/** the settings pages PublishSettingsDialog reads from the tabs: [{title, groups: [{title, icon, options: [[opt_id, key, index]]}]}] */
fun AppState.publishTabs(): JSONObject {
    val out = JSONObject()
    val l = layout ?: return out
    for ((type, pages) in l.tabs) {
        val arr = JSONArray()
        for (p in pages) {
            val groups = JSONArray()
            for (g in p.groups) {
                val opts = JSONArray()
                fun add(o: LayoutOption) {
                    // Tab.cpp's opt_id: the key, with "#<index>" for an element of a vector option
                    val idx = when (o.idx) { null -> null; "extruder_idx" -> 0; else -> o.idx.toIntOrNull() }
                    opts.put(JSONArray().put(if (idx == null) o.key else "${o.key}#$idx").put(o.key).put(idx ?: -1))
                }
                for (line in g.lines) when (line) {
                    is LayoutLine.Single -> if (line.option.customDef == null) add(line.option)
                    is LayoutLine.Multi -> line.options.filter { it.customDef == null }.forEach(::add)
                    else -> {}
                }
                groups.put(jobj("title" to g.title, "icon" to g.icon, "options" to opts))
            }
            arr.put(jobj("title" to p.title, "groups" to groups))
        }
        out.put(type, arr)
    }
    return out
}

/** MainFrame::publish_project: PublishSettingsDialog, then Plater::export_published_3mf into the file the user picks */
fun AppState.publishProject() {
    if (!hasObjects) return
    call("publish.open", publishTabs()) { if (it is JSONObject) publishDialog = it }
}

/**
 * PublishSettingsDialog: the filter bar (search, All / None or the active pseudo filter, the menu), the Printer / Filament /
 * Process tabs with their inner tabs (a filament slot's colour chip, the mixed slots' row, the extruder switch), a slot's
 * Enable and Full Publish, the rows under their group headings (dirty ones bold and pre-checked), the guides, OK / Cancel.
 */
@Composable
fun PublishDialog(app: AppState) {
    val st = app.publishDialog ?: return
    val s = st.optJSONObject("strings") ?: JSONObject()
    fun act(vararg kv: Pair<String, Any?>) = app.call("publish.action", jobj(*kv)) { r ->
        if (r is JSONObject) app.publishDialog = JSONObject(r.toString()).put("strings", s).put("file_name", st.optString("file_name"))
    }
    val close: () -> Unit = { app.publishDialog = null; app.call("publish.cancel") }
    val c = desk()
    val sections = st.optJSONArray("sections") ?: JSONArray()
    val sel = st.optInt("selected").coerceIn(0, (sections.length() - 1).coerceAtLeast(0))
    Dialog(close, DialogProperties(usePlatformDefaultWidth = false)) {
        Column(Modifier.widthIn(max = 820.dp).fillMaxWidth(0.92f).fillMaxHeight(0.9f).clip(RoundedCornerShape(6.dp)).background(c.panel)) {
            Row(Modifier.fillMaxWidth().background(FrameColors.topbar).padding(horizontal = 16.dp, vertical = 10.dp), verticalAlignment = Alignment.CenterVertically) {
                Text(s.optString("title"), Modifier.weight(1f), style = deskText(16, FontWeight.Bold, Color.White))
                Text("✕", Modifier.clickable(onClick = close).padding(6.dp), style = deskText(18, color = Color.White))
            }
            Column(Modifier.padding(horizontal = 14.dp, vertical = 10.dp).weight(1f)) {
                Text(s.optString("message"), style = deskText(14))
                Spacer(Modifier.height(8.dp))
                FilterBar(st, s) { kv -> act(*kv) }
                Spacer(Modifier.height(8.dp))
                // the outer tabs
                Row(Modifier.fillMaxWidth()) {
                    for (i in 0 until sections.length()) {
                        val sec = sections.getJSONObject(i)
                        TabItem(sec.optString("title"), i == sel, sec.optBoolean("indicator"), icon = sec.optString("icon")) { act("what" to "outer", "index" to i) }
                    }
                }
                HorizontalDivider(color = c.border)
                sections.optJSONObject(sel)?.let { sec -> SectionPage(app, sec, sel, s) { kv -> act(*kv) } }
            }
            HorizontalDivider(color = c.border)
            Row(Modifier.fillMaxWidth().padding(horizontal = 14.dp, vertical = 10.dp), verticalAlignment = Alignment.CenterVertically) {
                // the guides
                Box(Modifier.size(30.dp).clip(CircleShape).background(OrcaColors.accent).clickable { app.openUrl("https://www.orcaslicer.com/wiki/publishing_3mf/publish_3mf") }, Alignment.Center) {
                    OrcaIcon("toolbar_wiki", 16.dp, darkVariant = false, tint = Color.White)
                }
                Spacer(Modifier.width(10.dp))
                Box(Modifier.size(30.dp).clip(CircleShape).background(OrcaColors.accent).clickable { app.openUrl("https://www.youtube.com/watch?v=-xt1N29UIOg") }, Alignment.Center) {
                    OrcaIcon("toolbar_video_guide", 16.dp, darkVariant = false, tint = Color.White)
                }
                Spacer(Modifier.weight(1f))
                PanelButton("OK", primary = true) {
                    val name = st.optString("file_name")
                    app.call("publish.ok") { r ->
                        if ((r as? JSONObject)?.optBoolean("ok") == true) {
                            app.publishDialog = null
                            app.export("publish.export", JSONObject(), name, "")
                        }
                    }
                }
                Spacer(Modifier.width(8.dp))
                PanelButton(tr("Cancel"), onClick = close)
            }
        }
    }
}

@Composable
private fun FilterBar(st: JSONObject, s: JSONObject, act: (Array<Pair<String, Any?>>) -> Unit) {
    val c = desk()
    var text by remember { mutableStateOf(st.optString("filter")) }
    var menu by remember { mutableStateOf(false) }
    val pseudo = st.optString("pseudo")
    val filtering = text.isNotEmpty() || pseudo.isNotEmpty()
    Row(verticalAlignment = Alignment.CenterVertically) {
        Row(Modifier.weight(1f).height(34.dp).clip(RoundedCornerShape(4.dp)).background(c.field).border(1.dp, c.border, RoundedCornerShape(4.dp))
            .padding(horizontal = 8.dp), verticalAlignment = Alignment.CenterVertically) {
            OrcaIcon("search", 16.dp); Spacer(Modifier.width(6.dp))
            Box(Modifier.weight(1f)) {
                if (text.isEmpty()) Text(s.optString("hint"), style = deskText(13, color = c.textDim))
                BasicTextField(text, { text = it; act(arrayOf("what" to "filter", "text" to it)) }, Modifier.fillMaxWidth(), singleLine = true,
                    textStyle = deskText(13), cursorBrush = SolidColor(c.accent))
            }
        }
        if (pseudo.isEmpty()) {
            Text(s.optString("all"), Modifier.clickable { act(arrayOf("what" to "select_all", "value" to true)) }.padding(start = 10.dp), style = deskText(13, color = OrcaColors.accent))
            Text(s.optString("none"), Modifier.clickable { act(arrayOf("what" to "select_all", "value" to false)) }.padding(start = 10.dp), style = deskText(13, color = OrcaColors.accent))
        } else
            Text(pseudo, Modifier.clickable { act(arrayOf("what" to "text_mode")) }.padding(start = 10.dp), style = deskText(13, color = OrcaColors.accent))
        Box {
            Box(Modifier.padding(start = 6.dp).size(32.dp).clickable { menu = true }, Alignment.Center) { OrcaIcon("filter", 16.dp) }
            // show_menu
            DropdownMenu(menu, { menu = false }) {
                @Composable fun item(label: String, enabled: Boolean, checked: Boolean? = null, run: () -> Unit) =
                    DropdownMenuItem(text = { Text(label, style = deskText(14, color = if (enabled) c.text else c.textDisabled)) }, enabled = enabled,
                        leadingIcon = checked?.let { { Text(if (it) "✓" else "", Modifier.width(14.dp), style = deskText(14)) } },
                        onClick = { menu = false; run() })
                item(s.optString("select_all"), !filtering) { act(arrayOf("what" to "select_all", "value" to true)) }
                item(s.optString("deselect_all"), !filtering) { act(arrayOf("what" to "select_all", "value" to false)) }
                HorizontalDivider()
                item(s.optString("select_visible"), filtering) { act(arrayOf("what" to "select_visible", "value" to true)) }
                item(s.optString("deselect_visible"), filtering) { act(arrayOf("what" to "select_visible", "value" to false)) }
                HorizontalDivider()
                item(s.optString("filter_selected"), true, st.optInt("filter_mode") == 1) { act(arrayOf("what" to "pseudo", "selected" to true)) }
                item(s.optString("filter_nonselected"), true, st.optInt("filter_mode") == 2) { act(arrayOf("what" to "pseudo", "selected" to false)) }
            }
        }
    }
}

@Composable
private fun TabItem(title: String, selected: Boolean, indicator: Boolean, icon: String = "", chip: String? = null, slot: Int = -1, onClick: () -> Unit) {
    val c = desk()
    Column(Modifier.clickable(onClick = onClick).padding(horizontal = 10.dp), horizontalAlignment = Alignment.CenterHorizontally) {
        Row(Modifier.padding(vertical = 6.dp), verticalAlignment = Alignment.CenterVertically) {
            if (icon.isNotEmpty()) { OrcaIcon(icon, 16.dp); Spacer(Modifier.width(4.dp)) }
            if (chip != null) { SlotChip(chip, slot); Spacer(Modifier.width(4.dp)) }
            Text(title, style = deskText(14, if (selected) FontWeight.Bold else FontWeight.Normal, if (selected) OrcaColors.accent else c.text), maxLines = 1)
            // TabCtrl::SetItemIndicator
            if (indicator) { Spacer(Modifier.width(4.dp)); Box(Modifier.size(6.dp).clip(CircleShape).background(OrcaColors.accent)) }
        }
        Box(Modifier.height(2.dp).width(36.dp).background(if (selected) OrcaColors.accent else Color.Transparent))
    }
}

/** get_extruder_color_icon: the slot's colour with its 1-based number */
@Composable
private fun SlotChip(hex: String, slot: Int) {
    val col = parseColor(hex) ?: Color(0xFFD9D9D9)
    val light = (col.red * 0.299f + col.green * 0.587f + col.blue * 0.114f) > 0.6f
    Box(Modifier.size(20.dp).background(col, RoundedCornerShape(3.dp)).border(1.dp, Color(0x33000000), RoundedCornerShape(3.dp)), Alignment.Center) {
        if (slot >= 0) Text("${slot + 1}", style = deskText(11, FontWeight.Bold, if (light) Color.Black else Color.White))
    }
}

@Composable
private fun ColumnScope.SectionPage(app: AppState, sec: JSONObject, sectionIndex: Int, s: JSONObject, act: (Array<Pair<String, Any?>>) -> Unit) {
    val c = desk()
    val cats = sec.optJSONArray("categories") ?: JSONArray()
    val mixed = sec.optJSONArray("mixed") ?: JSONArray()
    val selInner = sec.optInt("selected_inner", -1)
    val selMixed = sec.optInt("selected_mixed", -1)
    val switch = sec.optJSONArray("switch")
    if (switch != null) {
        // the extruder switch of the printer tab (MultiSwitchButton) in place of the tab strip
        Row(Modifier.fillMaxWidth().padding(vertical = 6.dp), horizontalArrangement = Arrangement.Center) {
            Row(Modifier.clip(RoundedCornerShape(15.dp)).border(1.dp, c.border, RoundedCornerShape(15.dp)).padding(2.dp)) {
                for (i in 0 until switch.length()) {
                    val on = i == selInner
                    Box(Modifier.clip(RoundedCornerShape(13.dp)).background(if (on) c.accent else Color.Transparent)
                        .clickable { act(arrayOf("what" to "inner", "section" to sectionIndex, "index" to i)) }.padding(horizontal = 14.dp, vertical = 5.dp)) {
                        Text(switch.getString(i), style = deskText(13, color = if (on) Color.White else c.text))
                    }
                }
            }
        }
    } else Row(Modifier.fillMaxWidth().horizontalScroll(rememberScrollState())) {
        for (i in 0 until cats.length()) {
            val cat = cats.getJSONObject(i)
            TabItem(cat.optString("title"), i == selInner, cat.optBoolean("indicator"), chip = cat.optString("color").takeIf { cat.has("slot") },
                slot = cat.optInt("slot", -1)) { act(arrayOf("what" to "inner", "section" to sectionIndex, "index" to i)) }
        }
    }
    if (mixed.length() > 0) Row(Modifier.fillMaxWidth().horizontalScroll(rememberScrollState())) {
        for (i in 0 until mixed.length()) {
            val cat = mixed.getJSONObject(i)
            TabItem(cat.optString("title"), i == selMixed, cat.optBoolean("indicator"), chip = cat.optString("color"), slot = cat.optInt("slot", -1)) {
                act(arrayOf("what" to "mixed", "section" to sectionIndex, "index" to i))
            }
        }
    }
    val cat = (if (selMixed >= 0) mixed.optJSONObject(selMixed) else cats.optJSONObject(selInner)) ?: return
    val ci = cat.optInt("index")
    Column(Modifier.weight(1f).fillMaxWidth().verticalScroll(rememberScrollState()).padding(top = 8.dp)) {
        if (cat.has("enable")) {
            // the slot's header: [chip] [title] [Enable], then Full Publish while enabled
            Row(verticalAlignment = Alignment.CenterVertically) {
                if (!cat.optBoolean("mixed")) {
                    SlotChip(cat.optString("color"), cat.optInt("slot", -1)); Spacer(Modifier.width(8.dp))
                    Text(cat.optString("title"), Modifier.weight(1f), style = deskText(14, FontWeight.Bold))
                }
                DeskCheck(cat.optBoolean("enable")) { act(arrayOf("what" to "enable", "category" to ci, "value" to it)) }
                Text(s.optString("enable"), Modifier.clickable { act(arrayOf("what" to "enable", "category" to ci, "value" to !cat.optBoolean("enable"))) }, style = deskText(14))
            }
            if (cat.has("full") && cat.optBoolean("enable")) Row(verticalAlignment = Alignment.CenterVertically) {
                DeskCheck(cat.optBoolean("full")) { act(arrayOf("what" to "full", "category" to ci, "value" to it)) }
                Text(s.optString("full"), Modifier.clickable { act(arrayOf("what" to "full", "category" to ci, "value" to !cat.optBoolean("full"))) }, style = deskText(14))
            }
        }
        cat.optJSONObject("visual")?.let { v -> MixedVisual(app, v) }
        if (cat.optBoolean("shown_rows")) {
            val subs = cat.optJSONArray("subs") ?: JSONArray()
            for (k in 0 until subs.length()) {
                val sub = subs.getJSONObject(k)
                if (sub.optString("title").isNotEmpty()) Row(Modifier.padding(top = 10.dp, bottom = 2.dp), verticalAlignment = Alignment.CenterVertically) {
                    if (sub.optString("icon").isNotEmpty()) { OrcaIcon(sub.optString("icon"), 18.dp); Spacer(Modifier.width(6.dp)) }
                    Text(sub.optString("title"), style = deskText(14, FontWeight.Bold))
                    Spacer(Modifier.width(8.dp)); HorizontalDivider(Modifier.weight(1f), color = c.border)
                }
                val rows = sub.optJSONArray("rows") ?: JSONArray()
                for (r in 0 until rows.length()) PublishRow(rows.getJSONObject(r), cat.optInt("slot", -1)) { v -> act(arrayOf("what" to "check", "row" to rows.getJSONObject(r).optInt("index"), "value" to v)) }
            }
            cat.optString("info").takeIf { it.isNotEmpty() }?.let { Text(it, Modifier.padding(16.dp), style = deskText(13, color = c.textDim)) }
        }
    }
}

@Composable
private fun PublishRow(r: JSONObject, slot: Int, onCheck: (Boolean) -> Unit) {
    val c = desk()
    val enabled = r.optBoolean("enabled", true)
    Row(Modifier.fillMaxWidth().heightIn(min = 34.dp), verticalAlignment = Alignment.CenterVertically) {
        DeskCheck(r.optBoolean("checked"), enabled, onCheck)
        Text(r.optString("label") + ":", Modifier.width(260.dp).clickable(enabled = enabled) { onCheck(!r.optBoolean("checked")) },
            style = deskText(13, if (r.optBoolean("bold")) FontWeight.Bold else FontWeight.Normal, if (enabled) c.text else c.textDisabled), maxLines = 2)
        if (r.optString("kind") == "color" && r.optString("value").isNotEmpty()) { SlotChip(r.optString("value"), slot); Spacer(Modifier.width(6.dp)) }
        Text(r.optString("value"), Modifier.weight(1f, fill = false), style = deskText(13, color = if (enabled) c.text else c.textDisabled), maxLines = 1, overflow = TextOverflow.Ellipsis)
        if (r.optString("unit").isNotEmpty()) Text(" " + r.optString("unit"), style = deskText(13, color = c.textDim))
    }
}
