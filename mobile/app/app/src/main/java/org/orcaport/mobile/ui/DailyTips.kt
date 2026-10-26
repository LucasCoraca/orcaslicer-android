package org.orcaport.mobile.ui

import android.graphics.BitmapFactory
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import org.json.JSONObject
import org.orcaport.mobile.AppState
import org.orcaport.mobile.engine.jobj

/** ImGui's colour markers (\u0002 ... \u0003) in hint texts: the marked part in the highlight colour */
fun markedText(s: String, highlight: androidx.compose.ui.graphics.Color, bold: Boolean = false) = buildAnnotatedString {
    var on = false
    val sb = StringBuilder()
    fun flush() {
        if (sb.isEmpty()) return
        if (on) withStyle(SpanStyle(color = highlight, fontWeight = if (bold) FontWeight.Bold else null)) { append(sb) } else append(sb)
        sb.clear()
    }
    for (c in s) when (c) {
        '\u0002' -> { flush(); on = true }
        '\u0003' -> { flush(); on = false }
        else -> sb.append(c)
    }
    flush()
}

/** DailyTipsDataRenderer (vertical layout): the 16:9 image, the headline, the text with its link, the Wiki line */
@Composable
fun DailyTipContent(app: AppState, tip: JSONObject, showImage: Boolean = true) {
    val c = desk()
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        if (showImage) {
            val path = tip.optString("image")
            val bmp = remember(path) { runCatching { BitmapFactory.decodeFile(path)?.asImageBitmap() }.getOrNull() }
            if (bmp != null) Image(bmp, null, Modifier.fillMaxWidth().aspectRatio(16f / 9f), contentScale = ContentScale.Fit)
        }
        Text(markedText("\u0002" + tip.optString("title") + "\u0003", OrcaColors.accent), style = deskText(15, FontWeight.Bold))
        // HintData: text, the hypertext that runs the hint's action, and the text after it
        val body = buildAnnotatedString {
            append(markedText(tip.optString("text"), OrcaColors.accent))
            val hyper = tip.optString("hypertext")
            if (hyper.isNotEmpty()) {
                pushStringAnnotation("action", "1")
                withStyle(SpanStyle(color = OrcaColors.accent, textDecoration = TextDecoration.Underline)) { append(hyper) }
                pop()
                append(markedText(tip.optString("follow_text"), OrcaColors.accent))
            }
        }
        @Suppress("DEPRECATION")
        androidx.compose.foundation.text.ClickableText(body, style = deskText(13)) { off ->
            if (body.getStringAnnotations("action", off, off).isNotEmpty()) app.call("tips.action")
        }
        val wiki = tip.optString("wiki")
        if (wiki.isNotEmpty()) {
            val line = tip.optString("wiki_line"); val part = tip.optString("wiki_part")
            val at = line.indexOf(part).coerceAtLeast(0)
            Row {
                Text(line.substring(0, at), style = deskText(13))
                Text(part, Modifier.clickable {
                    runCatching {
                        app.appContext?.startActivity(android.content.Intent(android.content.Intent.ACTION_VIEW, android.net.Uri.parse(wiki))
                            .addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK))
                    }
                }, style = deskText(13, color = OrcaColors.accent))
            }
        }
    }
}

/** DailyTipsPanel::render_controller_buttons: the page index and the previous / next arrows */
@Composable
fun DailyTipNav(tip: JSONObject, onNav: (String) -> Unit) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text("${tip.optInt("page")}/${tip.optInt("pages")}", style = deskText(13, color = desk().textDim))
        Spacer(Modifier.width(8.dp))
        Box(Modifier.size(38.dp).clickable { onNav("prev") }, Alignment.Center) { Text("‹", style = deskText(20)) }
        Box(Modifier.size(38.dp).clickable { onNav("next") }, Alignment.Center) { Text("›", style = deskText(20)) }
    }
}

/** DailyTipsWindow: Help > Show Tip of the Day */
@Composable
fun DailyTipsWindow(app: AppState, onClose: () -> Unit) {
    var tip by remember { mutableStateOf<JSONObject?>(null) }
    fun load(nav: String) = app.call("tips.get", jobj("nav" to nav)) { if (it is JSONObject) tip = it }
    LaunchedEffect(Unit) { load("curr") }
    val t = tip ?: return
    Dialog(onDismissRequest = onClose) {
        Column(Modifier.width(520.dp).background(desk().panel, RoundedCornerShape(8.dp)).padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(t.optJSONObject("strings")?.optString("daily_tips") ?: "Daily Tips", Modifier.weight(1f), style = deskText(16))
                Text("✕", Modifier.clickable(onClick = onClose).padding(6.dp), style = deskText(16, color = desk().textDim))
            }
            DailyTipContent(app, t)
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) { DailyTipNav(t) { load(it) } }
        }
    }
}
