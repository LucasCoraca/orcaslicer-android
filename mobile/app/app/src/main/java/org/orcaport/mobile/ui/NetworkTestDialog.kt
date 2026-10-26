package org.orcaport.mobile.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import org.json.JSONObject
import org.orcaport.mobile.AppState
import org.orcaport.mobile.engine.jobj
import org.orcaport.mobile.prepare.PanelButton

/** NetworkTestDialog (Help > Open Network Test): the start buttons, basic info, a row per tested site, the log */
@Composable
fun NetworkTestDialog(app: AppState, onClose: () -> Unit) {
    var d by remember { mutableStateOf<JSONObject?>(null) }
    LaunchedEffect(Unit) {
        app.networkTestResults.clear(); app.networkTestLog = ""
        app.call("nettest.open") { if (it is JSONObject) d = it }
    }
    val close = { app.call("nettest.close"); onClose() }
    val s = d ?: return
    fun start(what: String) = app.call("nettest.start", jobj("what" to what))
    Dialog(onDismissRequest = close, properties = DialogProperties(usePlatformDefaultWidth = false)) {
        Column(Modifier.widthIn(max = 1000.dp).fillMaxWidth(0.9f).heightIn(max = 700.dp).fillMaxHeight(0.85f)
            .background(desk().panel, RoundedCornerShape(6.dp)).padding(12.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(s.optString("title"), Modifier.weight(1f), style = deskText(15))
                Text("✕", Modifier.clickable(onClick = close).padding(8.dp), style = deskText(15, color = desk().textDim))
            }
            Row(Modifier.padding(5.dp), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                PanelButton(s.optString("start"), primary = true) { start("all") }
                PanelButton(s.optString("start_sequence")) { start("sequence") }
            }
            Text(s.optString("basic_info"), Modifier.padding(5.dp), style = deskText(13))
            Row(Modifier.padding(5.dp)) {
                Text(s.optString("version_title"), style = deskText(13)); Spacer(Modifier.width(10.dp)); Text(s.optString("version"), style = deskText(13))
            }
            Row(Modifier.padding(5.dp)) {
                Text(s.optString("sys_title"), style = deskText(13)); Spacer(Modifier.width(10.dp)); Text(s.optString("sys"), style = deskText(13))
            }
            val tests = s.optJSONArray("tests")
            for (i in 0 until (tests?.length() ?: 0)) {
                val t = tests!!.getJSONObject(i)
                val job = t.optInt("job")
                Row(Modifier.padding(5.dp), verticalAlignment = Alignment.CenterVertically) {
                    Box(Modifier.width(220.dp)) { PanelButton(t.optString("button")) { start(if (job == 1) "github" else "bing") } }
                    Spacer(Modifier.width(10.dp))
                    Text(t.optString("title"), style = deskText(13))
                    Spacer(Modifier.width(10.dp))
                    Text(app.networkTestResults[job] ?: s.optString("na"), style = deskText(13))
                }
            }
            Text(s.optString("log_title"), Modifier.padding(5.dp), style = deskText(13))
            val scroll = rememberScrollState()
            LaunchedEffect(app.networkTestLog) { scroll.animateScrollTo(scroll.maxValue) }
            Box(Modifier.fillMaxWidth().weight(1f).padding(5.dp).background(desk().field, RoundedCornerShape(4.dp)).border(1.dp, desk().border, RoundedCornerShape(4.dp)).verticalScroll(scroll).padding(8.dp)) {
                Text(app.networkTestLog, style = deskText(12).copy(fontFamily = FontFamily.Monospace))
            }
        }
    }
}
