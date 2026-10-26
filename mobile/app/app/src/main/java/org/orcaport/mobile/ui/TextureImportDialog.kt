package org.orcaport.mobile.ui

import android.graphics.BitmapFactory
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import org.json.JSONArray
import org.json.JSONObject
import org.orcaport.mobile.AppState
import org.orcaport.mobile.engine.Ask
import org.orcaport.mobile.prepare.FilamentSwatch
import org.orcaport.mobile.prepare.PanelButton
import org.orcaport.mobile.settings.parseColor
import kotlin.math.roundToInt
import kotlinx.coroutines.launch

/**
 * TextureImportDialog ("Import Model"): the preview (Original / Multi-Color, the mapped filaments by default; drag to
 * turn it), Color Count and Smooth Level with Apply, the filament mapping of every colour (auto-merge, one-click
 * CMYW / RYBW auto-mix), Skip Matching / Confirm. The engine keeps the dialog's state (Host::ask "texture_import").
 */
@Composable
fun TextureImportAsk(app: AppState, ask: Ask) {
    val r = ask.request
    val c = desk()
    val s = r.optJSONObject("strings") ?: JSONObject()
    fun reply(vararg kv: Pair<String, Any>) = ask.reply(JSONObject().apply { for ((k, v) in kv) put(k, v) })
    val filaments = r.optJSONArray("filaments") ?: JSONArray()
    val rows = r.optJSONArray("rows") ?: JSONArray()
    var colorCount by remember(ask) { mutableIntStateOf(r.optInt("color_count", 4)) }
    var smooth by remember(ask) { mutableIntStateOf(r.optInt("smooth", 5)) }
    var popupRow by remember(ask) { mutableIntStateOf(-1) }
    var pickColorRow by remember(ask) { mutableIntStateOf(-1) }
    var mixMenu by remember(ask) { mutableStateOf(false) }
    Dialog({ reply("action" to "cancel") }, DialogProperties(usePlatformDefaultWidth = false, dismissOnClickOutside = false)) {
        Column(Modifier.width(960.dp).fillMaxHeight(0.94f).clip(RoundedCornerShape(6.dp)).background(c.panel)) {
            Row(Modifier.fillMaxWidth().padding(start = 20.dp, end = 8.dp, top = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                Text(r.optString("title"), Modifier.weight(1f), style = deskText(15))
                Text("✕", Modifier.clickable { reply("action" to "cancel") }.padding(8.dp), style = deskText(15, color = c.textDim))
            }
            Row(Modifier.weight(1f).padding(16.dp), horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                // the preview: one frame per state, rendered by the engine; a drag turns the model
                Box(Modifier.weight(1.2f).fillMaxHeight().clip(RoundedCornerShape(6.dp)).background(if (desk() == DeskDark) Color(0xFF4C4C55) else Color(0xFFEEEEEE))) {
                    val path = r.optString("image")
                    val bmp = remember(path) { if (path.isNotEmpty()) runCatching { BitmapFactory.decodeFile(path)?.asImageBitmap() }.getOrNull() else null }
                    var yaw by remember { mutableFloatStateOf(0.6f) }
                    var pitch by remember { mutableFloatStateOf(0.5f) }
                    if (bmp != null) Image(bmp, null, Modifier.fillMaxSize().pointerInput(ask) {
                        detectDragGestures(onDragEnd = { reply("action" to "view", "mode" to r.optInt("view"), "yaw" to yaw.toDouble(), "pitch" to pitch.toDouble()) }) { ch, d ->
                            ch.consume(); yaw += d.x / 180f; pitch = (pitch + d.y / 180f).coerceIn(-1.5f, 1.5f)
                        }
                    }, contentScale = ContentScale.Fit)
                    // the view pills: Original, Multi-Color (the mapped filaments are shown while neither is chosen)
                    Row(Modifier.align(Alignment.TopStart).padding(8.dp), horizontalArrangement = Arrangement.spacedBy(2.dp)) {
                        for ((mode, label) in listOf(0 to s.optString("original"), 1 to s.optString("multicolor"))) {
                            val sel = r.optInt("view") == mode
                            Text(label, Modifier.clip(RoundedCornerShape(14.dp)).background(if (sel) OrcaColors.accent else c.panel)
                                .clickable { reply("action" to "view", "mode" to if (sel) 2 else mode) }.padding(horizontal = 12.dp, vertical = 5.dp),
                                style = deskText(13, FontWeight.Bold, color = if (sel) Color.White else Color(0xFF6B6B6A)))
                        }
                    }
                }
                Column(Modifier.weight(1f).fillMaxHeight()) {
                    // Color Count: 4 / 8 / 16 / Auto, the slider and the value
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text(s.optString("color_count"), Modifier.padding(end = 8.dp), style = deskText(14, FontWeight.Bold))
                        for (n in listOf(4, 8, 16)) Chip("$n", colorCount == n) { colorCount = n; reply("action" to "color_count", "value" to n) }
                        Chip(s.optString("auto"), false) { reply("action" to "auto") }
                    }
                    ValueSlider(colorCount, 1, r.optInt("max_colors", 32)) { colorCount = it; reply("action" to "color_count", "value" to it) }
                    Text(s.optString("smooth"), Modifier.padding(top = 4.dp), style = deskText(14, FontWeight.Bold))
                    ValueSlider(smooth, 0, 10) { smooth = it; reply("action" to "smooth", "value" to it) }
                    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) { PanelButton(s.optString("apply"), primary = r.optBoolean("dirty")) { reply("action" to "apply") } }
                    HorizontalDivider(Modifier.padding(vertical = 8.dp), color = c.border)
                    // Filament Mapping: the one-click auto-mix, its reset, auto-merge
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text(s.optString("mapping"), Modifier.weight(1f), style = deskText(14, FontWeight.Bold))
                        if (r.optBoolean("mix_applied")) Box(Modifier.size(32.dp).clickable { reply("action" to "reset_mix") }, Alignment.Center) { OrcaIcon("undo", 18.dp) }
                        Box {
                            val mode = r.optInt("mix_mode")
                            PanelButton(if (mode == 0) s.optString("cmyw") else s.optString("rybw")) { mixMenu = true }
                            DropdownMenu(mixMenu, { mixMenu = false }) {
                                DropdownMenuItem(text = { Text(s.optString("cmyw"), style = deskText(14)) }, onClick = { mixMenu = false; reply("action" to "auto_mix", "mode" to 0) })
                                DropdownMenuItem(text = { Text(s.optString("rybw"), style = deskText(14)) }, onClick = { mixMenu = false; reply("action" to "auto_mix", "mode" to 1) })
                            }
                        }
                    }
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        DeskCheck(r.optBoolean("auto_merge")) { reply("action" to "auto_merge", "value" to it) }
                        Text(s.optString("auto_merge"), style = deskText(13))
                    }
                    Column(Modifier.weight(1f).verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                        for (i in 0 until rows.length()) {
                            val row = rows.getJSONObject(i)
                            val target = filaments.optJSONObject(row.optInt("target", -1).coerceAtLeast(0))?.takeIf { row.optInt("target", -1) >= 0 }
                            Row(verticalAlignment = Alignment.CenterVertically) {
                                Box(Modifier.size(28.dp).clip(RoundedCornerShape(4.dp)).background(parseColor(row.optString("source")) ?: Color.Gray).border(1.dp, c.border, RoundedCornerShape(4.dp)))
                                Text(row.optString("source"), Modifier.width(80.dp).padding(start = 6.dp), style = deskText(12))
                                Text("→", Modifier.padding(horizontal = 6.dp), style = deskText(13, color = Color(0xFFACACAC)))
                                Box(Modifier.weight(1f)) {
                                    Row(Modifier.fillMaxWidth().height(36.dp).clip(RoundedCornerShape(4.dp)).background(c.field).border(1.dp, c.border, RoundedCornerShape(4.dp))
                                        .clickable { popupRow = i }.padding(horizontal = 6.dp), verticalAlignment = Alignment.CenterVertically) {
                                        if (target != null) {
                                            FilamentSwatch(target.optString("color"), "${target.optInt("number")}", 24.dp)
                                            Text(target.optString("name"), Modifier.weight(1f).padding(start = 6.dp), style = deskText(13), maxLines = 1, overflow = TextOverflow.Ellipsis)
                                        } else Text("—", Modifier.weight(1f), style = deskText(13))
                                        Text("˅", style = deskText(13, color = Color(0xFF6B6B6A)))
                                    }
                                    if (popupRow == i) FilamentPopup(r, s, filaments, onDismiss = { popupRow = -1 }, onSelect = { idx -> popupRow = -1; reply("action" to "select", "row" to i, "index" to idx) },
                                        onAdd = { popupRow = -1; pickColorRow = i }, onDecompose = { popupRow = -1; reply("action" to "decompose", "row" to i) })
                                }
                            }
                        }
                    }
                }
            }
            if (r.optBoolean("dropped")) Text(s.optString("dropped"), Modifier.padding(horizontal = 20.dp), style = deskText(13, color = Color(0xFFFF6F00)))
            if (r.optBoolean("dirty")) Text(s.optString("reminder"), Modifier.padding(horizontal = 20.dp), style = deskText(13, color = Color(0xFFFF6F00)))
            Row(Modifier.fillMaxWidth().padding(16.dp), horizontalArrangement = Arrangement.End) {
                PanelButton(s.optString("skip")) { reply("action" to "skip") }
                Spacer(Modifier.width(12.dp))
                PanelButton(s.optString("ok"), primary = true, enabled = r.optBoolean("ok")) { reply("action" to "ok") }
            }
        }
    }
    if (pickColorRow >= 0) ColorPickerDialog(null, onDismiss = { pickColorRow = -1 }) { hex ->
        val row = pickColorRow; pickColorRow = -1
        reply("action" to "add", "row" to row, "color" to hex)
    }
}

@Composable
private fun Chip(label: String, selected: Boolean, onClick: () -> Unit) {
    val c = desk()
    Text(label, Modifier.padding(end = 6.dp).clip(RoundedCornerShape(4.dp)).background(if (selected) OrcaColors.accent else c.field)
        .border(1.dp, if (selected) OrcaColors.accent else c.border, RoundedCornerShape(4.dp)).clickable(onClick = onClick).padding(horizontal = 10.dp, vertical = 5.dp),
        style = deskText(13, color = if (selected) Color.White else c.text))
}

/** AccentSlider + SpinInput: the value is sent when the drag ends */
@Composable
private fun ValueSlider(value: Int, min: Int, max: Int, onChange: (Int) -> Unit) {
    var v by remember(value) { mutableFloatStateOf(value.toFloat()) }
    Row(verticalAlignment = Alignment.CenterVertically) {
        Slider(v, { v = it }, Modifier.weight(1f), valueRange = min.toFloat()..max.coerceAtLeast(min + 1).toFloat(),
            onValueChangeFinished = { onChange(v.roundToInt()) }, colors = SliderDefaults.colors(thumbColor = OrcaColors.accent, activeTrackColor = OrcaColors.accent))
        Text("${v.roundToInt()}", Modifier.width(36.dp).padding(start = 8.dp), style = deskText(13))
    }
}

/** FilamentSelectPopup: the filaments by section, Decompose Color and + Add Material at the end */
@Composable
private fun FilamentPopup(r: JSONObject, s: JSONObject, filaments: JSONArray, onDismiss: () -> Unit, onSelect: (Int) -> Unit, onAdd: () -> Unit, onDecompose: () -> Unit) {
    val c = desk()
    DropdownMenu(true, onDismiss, Modifier.heightIn(max = 480.dp)) {
        for ((kind, key) in listOf(0 to "physical", 2 to "new_physical", 1 to "mixed", 3 to "new_mixed")) {
            val items = (0 until filaments.length()).map { filaments.getJSONObject(it) }.filter { it.optInt("kind") == kind }
            if (items.isEmpty()) continue
            Text(s.optString(key), Modifier.padding(horizontal = 12.dp, vertical = 4.dp), style = deskText(12, color = c.textDim))
            for (f in items) DropdownMenuItem(onClick = { onSelect(f.optInt("index")) }, text = {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    FilamentSwatch(f.optString("color"), "${f.optInt("number")}", 22.dp)
                    val comps = f.optJSONArray("components")
                    if (comps != null && comps.length() > 0) {
                        for (k in 0 until comps.length()) {
                            val o = comps.getJSONObject(k)
                            Text(if (k == 0) " = " else " + ", style = deskText(12))
                            FilamentSwatch(o.optString("color"), "${o.optInt("number")}", 18.dp)
                        }
                    } else Text(f.optString("name"), Modifier.padding(start = 6.dp), style = deskText(13), maxLines = 1, overflow = TextOverflow.Ellipsis)
                }
            })
        }
        HorizontalDivider()
        DropdownMenuItem(text = { Text(s.optString("decompose"), style = deskText(13, color = OrcaColors.accent)) }, onClick = onDecompose)
        DropdownMenuItem(text = { Text(s.optString("add"), style = deskText(13, color = if (r.optBoolean("can_add")) OrcaColors.accent else c.textDisabled)) },
            onClick = onAdd, enabled = r.optBoolean("can_add"))
    }
}

/** the clustering's progress (TexturePaintingSettings' callback), Cancel stops it (texture.cancel) */
@Composable
fun TextureImportProgress(app: AppState) {
    val p = app.textureProgress ?: return
    val c = desk()
    Dialog({}, DialogProperties(dismissOnBackPress = false, dismissOnClickOutside = false)) {
        Column(Modifier.width(420.dp).clip(RoundedCornerShape(6.dp)).background(c.panel).padding(20.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Text(p.optString("text"), style = deskText(14))
            LinearProgressIndicator({ p.optInt("percent").coerceIn(0, 100) / 100f }, Modifier.fillMaxWidth(), color = OrcaColors.accent)
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                PanelButton(tr("Cancel")) { app.scope.launch { runCatching { app.engine.pure("texture.cancel") } } }
            }
        }
    }
}
