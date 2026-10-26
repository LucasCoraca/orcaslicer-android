package org.orcaport.mobile.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import org.json.JSONArray
import org.json.JSONObject
import org.orcaport.mobile.engine.Ask
import org.orcaport.mobile.prepare.PanelButton
import org.orcaport.mobile.settings.parseColor

/**
 * ColorDecomposeDialog: the material type, the target colour and the closest mix, the decompositions (Material List of
 * the project's filaments; CMYW / RYBW of the standard bases). The engine computes each state (Host::ask
 * "color_decompose"); a change of type or mode is answered and asked again.
 */
@Composable
fun ColorDecomposeAsk(ask: Ask) {
    val r = ask.request
    val c = desk()
    val str = r.optJSONObject("strings") ?: JSONObject()
    val items = r.optJSONArray("items") ?: JSONArray()
    val cards = r.optJSONArray("cards") ?: JSONArray()
    fun reply(vararg kv: Pair<String, Any>) = ask.reply(JSONObject().apply { for ((k, v) in kv) put(k, v) })
    Dialog({ reply("action" to "cancel") }, DialogProperties(usePlatformDefaultWidth = false)) {
        Column(Modifier.widthIn(min = 477.dp, max = 640.dp).clip(RoundedCornerShape(6.dp)).background(c.panel).padding(horizontal = 30.dp, vertical = 22.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(r.optString("title"), Modifier.weight(1f), style = deskText(15))
                Text("✕", Modifier.clickable { reply("action" to "cancel") }.padding(6.dp), style = deskText(15, color = c.textDim))
            }
            Spacer(Modifier.height(12.dp))
            // the material type: the project's types and the standard recommendations, under disabled group titles
            var open by remember { mutableStateOf(false) }
            val sel = r.optInt("selection", -1)
            Box {
                DeskCombo(items.optJSONObject(sel)?.optString("label") ?: "", Modifier.fillMaxWidth()) { open = true }
                DropdownMenu(open, { open = false }) {
                    for (i in 0 until items.length()) {
                        val it = items.getJSONObject(i)
                        if (it.optBoolean("group")) Text(it.optString("label"), Modifier.padding(horizontal = 12.dp, vertical = 6.dp), style = deskText(12, color = c.textDim))
                        else DropdownMenuItem(text = { Text(it.optString("label"), style = deskText(14, color = if (i == sel) c.accent else c.text)) },
                            onClick = { open = false; if (i != sel) reply("action" to "type", "selection" to i) })
                    }
                }
            }
            Spacer(Modifier.height(18.dp))
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(str.optString("target"), Modifier.padding(end = 19.dp), style = deskText(14, FontWeight.Bold))
                ColorChip(r.optString("target"))
                Text(rgb(r.optString("target")), Modifier.padding(horizontal = 12.dp), style = deskText(13))
                Text("→", Modifier.padding(end = 12.dp), style = deskText(13))
                ColorChip(r.optString("matched"))
                Text(rgb(r.optString("matched")), Modifier.padding(start = 12.dp), style = deskText(13, FontWeight.Bold))
            }
            Box(Modifier.padding(vertical = 16.dp).fillMaxWidth().height(1.dp).background(c.border))
            Text(str.optString("select"), Modifier.padding(bottom = 4.dp), style = deskText(14, FontWeight.Bold))
            val mode = r.optInt("mode")
            Row {
                if (r.optBoolean("arbitrary")) Column(Modifier.padding(end = 16.dp)) {
                    GroupLabel(str.optString("arbitrary"))
                    DecomposeCard(str.optString("material_list"), cards.optJSONObject(0), mode == 0) { reply("action" to "mode", "mode" to 0) }
                }
                val std = (1..2).filter { cards.optJSONObject(it)?.optBoolean("visible") == true }
                if (std.isNotEmpty()) Column {
                    GroupLabel(str.optString("standard"))
                    Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                        for (m in std) DecomposeCard(if (m == 1) "CMYW" else "RYBW", cards.optJSONObject(m), mode == m) { reply("action" to "mode", "mode" to m) }
                    }
                }
            }
            r.optString("hint").takeIf { it.isNotEmpty() }?.let { Text(it, Modifier.padding(top = 8.dp).widthIn(max = 400.dp), style = deskText(13, color = Color(0xFF6B6A6A))) }
            r.optString("warning").takeIf { it.isNotEmpty() }?.let {
                Row(Modifier.padding(top = 8.dp)) {
                    OrcaIcon("obj_warning", 16.dp); Spacer(Modifier.width(6.dp))
                    Text(it, style = deskText(13, color = Color(0xFFD01B1B)))
                }
            }
            Row(Modifier.fillMaxWidth().padding(top = 16.dp), horizontalArrangement = Arrangement.End) {
                PanelButton(str.optString("cancel", "Cancel")) { reply("action" to "cancel") }
                Spacer(Modifier.width(12.dp))
                PanelButton(str.optString("ok", "OK"), primary = true, enabled = r.optBoolean("ok")) { reply("action" to "ok") }
            }
        }
    }
}

private fun rgb(hex: String): String {
    val v = hex.removePrefix("#").take(6).toIntOrNull(16) ?: 0
    return "RGB: ${(v shr 16) and 0xFF}, ${(v shr 8) and 0xFF}, ${v and 0xFF}"
}

@Composable
private fun ColorChip(hex: String, size: androidx.compose.ui.unit.Dp = 28.dp) =
    Box(Modifier.size(size).clip(RoundedCornerShape(4.dp)).background(parseColor(hex) ?: Color.Gray).border(1.dp, desk().border, RoundedCornerShape(4.dp)))

@Composable
private fun GroupLabel(text: String) = Row(Modifier.padding(bottom = 8.dp), verticalAlignment = Alignment.CenterVertically) {
    Text(text, Modifier.padding(end = 5.dp), style = deskText(11, color = Color(0xFFACACAC)))
    Box(Modifier.width(88.dp).height(1.dp).background(desk().border))
}

/** a mode card: title and check, the components (swatch, ratio, "+" between), brand border when selected */
@Composable
private fun DecomposeCard(title: String, card: JSONObject?, selected: Boolean, onSelect: () -> Unit) {
    val c = desk()
    val comps = card?.optJSONArray("components") ?: JSONArray()
    val width = (128 + (if (comps.length() > 2) (comps.length() - 2) * 31 else 0)).dp
    Column(Modifier.width(width).height(111.dp).clip(RoundedCornerShape(8.dp)).background(c.field)
        .border(if (selected) 2.dp else 1.dp, if (selected) Color(0xFF009688) else c.border, RoundedCornerShape(8.dp)).clickable(onClick = onSelect).padding(12.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(title, Modifier.weight(1f), style = deskText(14, color = Color(0xFF6B6A6A)))
            DeskCheck(selected) { onSelect() }
        }
        Box(Modifier.padding(vertical = 4.dp).fillMaxWidth().height(1.dp).background(c.border))
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
            for (i in 0 until comps.length()) {
                val o = comps.getJSONObject(i)
                if (i > 0) Text("+", Modifier.padding(top = 4.dp), style = deskText(13))
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    ColorChip(o.optString("color"), 24.dp)
                    Text("${o.optInt("ratio")}%", Modifier.padding(top = 4.dp), style = deskText(13))
                }
            }
        }
    }
}
