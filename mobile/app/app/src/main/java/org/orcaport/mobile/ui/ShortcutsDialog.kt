package org.orcaport.mobile.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.focusable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.key.onPreviewKeyEvent
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import org.json.JSONArray
import org.json.JSONObject
import org.orcaport.mobile.AppState
import org.orcaport.mobile.engine.jobj
import org.orcaport.mobile.prepare.PanelButton

/**
 * KBShortcutsDialog: a tab per context (Global, Prepare, Painting, Objects list, Preview), the page's note, the rows under
 * their section headings with the keys right-aligned, Edit / Reset on the assignable ones, the Preferences button on the
 * mouse rows and a lock on the fixed keys. The pages, the rules and the conflict question are upstream's (KeyShortcuts.cpp).
 */
@Composable
fun ShortcutsDialog(app: AppState, context: String, onClose: () -> Unit) {
    var st by remember { mutableStateOf<JSONObject?>(null) }
    var tab by remember { mutableIntStateOf(-1) }
    var capture by remember { mutableStateOf<Int?>(null) }
    fun load() = app.call("shortcuts.pages") { if (it is JSONObject) st = it }
    LaunchedEffect(Unit) { load() }
    // the bindings changed (an edit, a reset, the conflict question answered): the rows follow
    LaunchedEffect(app.shortcutLabels) { load() }
    val c = desk()
    Dialog(onClose, DialogProperties(usePlatformDefaultWidth = false)) {
        val s = st ?: return@Dialog
        val pages = s.optJSONArray("pages") ?: JSONArray()
        if (tab < 0) tab = (0 until pages.length()).firstOrNull { pages.getJSONObject(it).optString("context") == context } ?: 0
        val page = pages.optJSONObject(tab) ?: return@Dialog
        Column(Modifier.widthIn(max = 760.dp).fillMaxWidth(0.9f).fillMaxHeight(0.88f).clip(RoundedCornerShape(6.dp)).background(c.panel)) {
            Row(Modifier.fillMaxWidth().background(FrameColors.topbar).padding(horizontal = 16.dp, vertical = 10.dp), verticalAlignment = Alignment.CenterVertically) {
                Text(s.optString("title"), Modifier.weight(1f), style = deskText(16, FontWeight.Bold, Color.White))
                Text("✕", Modifier.clickable(onClick = onClose).padding(6.dp), style = deskText(18, color = Color.White))
            }
            // TabCtrl
            Row(Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()).padding(horizontal = 12.dp, vertical = 6.dp)) {
                for (i in 0 until pages.length()) {
                    val sel = i == tab
                    Column(Modifier.clickable { tab = i }.padding(horizontal = 12.dp), horizontalAlignment = Alignment.CenterHorizontally) {
                        Text(pages.getJSONObject(i).optString("title"), Modifier.padding(vertical = 6.dp),
                            style = deskText(14, if (sel) FontWeight.Bold else FontWeight.Normal, if (sel) OrcaColors.accent else c.text))
                        Box(Modifier.height(2.dp).width(40.dp).background(if (sel) OrcaColors.accent else Color.Transparent))
                    }
                }
            }
            HorizontalDivider(color = c.border)
            Column(Modifier.weight(1f).verticalScroll(rememberScrollState()).padding(horizontal = 20.dp, vertical = 12.dp)) {
                // the note: when the page's keys apply
                Row(Modifier.fillMaxWidth().clip(RoundedCornerShape(4.dp)).background(c.field).padding(10.dp), verticalAlignment = Alignment.CenterVertically) {
                    OrcaIcon("help", 16.dp); Spacer(Modifier.width(8.dp))
                    Text(page.optString("caption"), style = deskText(13, color = c.textDim))
                }
                val rows = page.optJSONArray("rows") ?: JSONArray()
                var section: String? = null
                for (i in 0 until rows.length()) {
                    val r = rows.getJSONObject(i)
                    if (r.optString("section") != section) {
                        section = r.optString("section")
                        Row(Modifier.padding(top = 14.dp, bottom = 4.dp), verticalAlignment = Alignment.CenterVertically) {
                            Text(section, style = deskText(14, FontWeight.Bold))
                            Spacer(Modifier.width(8.dp))
                            HorizontalDivider(Modifier.weight(1f), color = c.border)
                        }
                    }
                    ShortcutRow(app, s, r, onEdit = { capture = r.optInt("shortcut") }, onClose = onClose)
                }
            }
        }
    }
    capture?.let { id -> CaptureDialog(app, id) { capture = null } }
}

@Composable
private fun ShortcutRow(app: AppState, s: JSONObject, r: JSONObject, onEdit: () -> Unit, onClose: () -> Unit) {
    val c = desk()
    Row(Modifier.fillMaxWidth().heightIn(min = 34.dp).padding(start = 8.dp), verticalAlignment = Alignment.CenterVertically) {
        Text(r.optString("description"), Modifier.weight(1f), style = deskText(13))
        Spacer(Modifier.width(12.dp))
        KeyCaps(r.optJSONArray("keys") ?: JSONArray())
        Spacer(Modifier.width(12.dp))
        Row(Modifier.width(72.dp), verticalAlignment = Alignment.CenterVertically) {
            when (r.optString("kind")) {
                "shortcut" -> {
                    IconButton("edit", s.optString("edit"), onEdit)
                    if (r.optBoolean("customized")) IconButton("undo", s.optString("reset")) { app.call("shortcuts.reset", jobj("shortcut" to r.optInt("shortcut"))) }
                }
                // open_mouse_preferences: Preferences on the Control tab, this dialog closed
                "mouse" -> IconButton("settings", s.optString("preferences")) { onClose(); app.preferences = true }
                else -> Box(Modifier.size(32.dp), Alignment.Center) { OrcaIcon("printer_status_lock", 16.dp) }
            }
        }
    }
}

/** set_chord_labels: the modifiers joined with " + ", then the key */
@Composable
private fun KeyCaps(keys: JSONArray) {
    val c = desk()
    if (keys.length() == 0) return
    val parts = (0 until keys.length()).map { keys.getString(it) }
    Row(verticalAlignment = Alignment.CenterVertically) {
        parts.forEachIndexed { i, p ->
            if (i > 0) Text(" + ", style = deskText(12, color = c.textDim))
            Text(p, Modifier.border(1.dp, c.border, RoundedCornerShape(3.dp)).background(c.field, RoundedCornerShape(3.dp))
                .padding(horizontal = 6.dp, vertical = 2.dp).widthIn(min = 14.dp), style = deskText(12, FontWeight.Bold))
        }
    }
}

@Composable
private fun IconButton(icon: String, tooltip: String, onClick: () -> Unit) =
    Box(Modifier.size(32.dp).clip(RoundedCornerShape(4.dp)).clickable(onClick = onClick), Alignment.Center) { OrcaIcon(icon, 18.dp) }

/**
 * ShortcutCaptureDialog: the next key chord pressed (Esc cancels, Enter confirms), what it would take from other
 * shortcuts or why it cannot be used, and Unbind / OK / Cancel.
 */
@Composable
private fun CaptureDialog(app: AppState, shortcut: Int, onClose: () -> Unit) {
    var st by remember { mutableStateOf<JSONObject?>(null) }
    LaunchedEffect(shortcut) { app.call("shortcuts.capture_open", jobj("shortcut" to shortcut)) { if (it is JSONObject) st = it } }
    val s = st
    val c = desk()
    val focus = remember { FocusRequester() }
    fun apply(chord: String) { onClose(); app.call("shortcuts.apply", jobj("shortcut" to shortcut, "chord" to chord)) }
    Dialog(onClose) {
        if (s == null) return@Dialog
        LaunchedEffect(Unit) { runCatching { focus.requestFocus() } }
        Column(Modifier.width(480.dp).clip(RoundedCornerShape(6.dp)).background(c.panel).padding(20.dp)
            .focusRequester(focus).focusable()
            .onPreviewKeyEvent { ke ->
                val e = ke.nativeKeyEvent
                if (e.action != android.view.KeyEvent.ACTION_DOWN) return@onPreviewKeyEvent true
                val chord = keyChord(e) ?: return@onPreviewKeyEvent true
                if (chord.second == 0 && chord.first == Wx.ESCAPE) { onClose(); return@onPreviewKeyEvent true }
                if (chord.second == 0 && chord.first == Wx.RETURN) { if (s.optBoolean("ok")) apply(s.optString("chord")); return@onPreviewKeyEvent true }
                // a symbol typed without Ctrl / Alt arrives as the character (on_char), anything else as the key (on_key)
                val symbol = chord.first in 33..126 && !Character.isLetterOrDigit(chord.first)
                val asChar = symbol && chord.second and (Wx.MOD_CONTROL or Wx.MOD_ALT) == 0
                app.call("shortcuts.capture", jobj("shortcut" to shortcut, "key" to chord.first, "mods" to chord.second, "char" to asChar)) { r ->
                    if (r is JSONObject) st = JSONObject(s.toString()).apply { for (k in r.keys()) put(k, r.get(k)) }
                }
                true
            }, verticalArrangement = Arrangement.spacedBy(14.dp)) {
            Text(s.optString("title"), style = deskText(16, FontWeight.Bold))
            Text(s.optString("prompt"), style = deskText(14))
            Box(Modifier.fillMaxWidth().height(60.dp).border(1.dp, OrcaColors.accent, RoundedCornerShape(4.dp)).clickable { runCatching { focus.requestFocus() } },
                Alignment.Center) {
                Text(s.optString("label"), style = deskText(16, FontWeight.Bold))
            }
            Text(s.optString("status"), Modifier.heightIn(min = 54.dp), style = deskText(13, color = if (s.optBoolean("error")) Color(0xFFE14747) else c.textDim))
            Row(verticalAlignment = Alignment.CenterVertically) {
                PanelButton(s.optString("unbind")) { apply("") }
                Spacer(Modifier.weight(1f))
                PanelButton("OK", primary = true, enabled = s.optBoolean("ok")) { apply(s.optString("chord")) }
                Spacer(Modifier.width(8.dp))
                PanelButton(tr("Cancel"), onClick = onClose)
            }
        }
    }
}
