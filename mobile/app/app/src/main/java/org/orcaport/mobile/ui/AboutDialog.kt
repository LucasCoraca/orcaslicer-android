package org.orcaport.mobile.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import org.json.JSONObject
import org.orcaport.mobile.AppState
import org.orcaport.mobile.prepare.PanelButton

private fun openUrl(app: AppState, url: String) = runCatching {
    app.appContext?.startActivity(android.content.Intent(android.content.Intent.ACTION_VIEW, android.net.Uri.parse(url))
        .addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK))
}

/** AboutDialog: the logo with the version and build, the history text, the copyright, the site and License Info */
@Composable
fun AboutDialog(app: AppState, onClose: () -> Unit) {
    var about by remember { mutableStateOf<JSONObject?>(null) }
    var license by remember { mutableStateOf(false) }
    LaunchedEffect(Unit) { app.call("app.about") { if (it is JSONObject) about = it } }
    val a = about ?: return
    val grey = Color(0xFF949494)
    Dialog(onDismissRequest = onClose) {
        Column(Modifier.width(560.dp).background(desk().panel, RoundedCornerShape(6.dp))) {
            Row(Modifier.fillMaxWidth().padding(start = 16.dp, top = 12.dp, end = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                Text(a.optString("title"), Modifier.weight(1f), style = deskText(15))
                Text("✕", Modifier.clickable(onClick = onClose).padding(8.dp), style = deskText(15, color = desk().textDim))
            }
            Box(Modifier.fillMaxWidth().height(125.dp)) {
                OrcaIcon(if (a.optString("logo").endsWith("_dark.svg")) "OrcaSlicer_about_dark" else "OrcaSlicer_about", 560.dp, height = 125.dp, darkVariant = false)
                Column(Modifier.align(Alignment.CenterEnd).padding(end = 20.dp), horizontalAlignment = Alignment.End) {
                    Text(a.optString("version"), style = deskText(24, color = grey))
                    Spacer(Modifier.height(5.dp))
                    Text(a.optString("build"), style = deskText(12, color = grey))
                }
            }
            Column(Modifier.padding(start = 20.dp, end = 20.dp, top = 33.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                val t = a.optJSONArray("text")
                for (i in 0 until (t?.length() ?: 0)) Text(t!!.optString(i), style = deskText(12, color = Color(0xFF6B6B6B)))
            }
            Spacer(Modifier.height(43.dp))
            Row(Modifier.fillMaxWidth().padding(horizontal = 20.dp), verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text(a.optString("copyright"), style = deskText(12, color = Color(0xFF6B6B6B)))
                    Text(a.optString("site"), Modifier.clickable { openUrl(app, a.optString("site")) }, style = deskText(12, color = Color(0xFF009789)))
                }
                PanelButton(a.optString("license_button")) { license = true }
            }
            Spacer(Modifier.height(30.dp))
        }
    }
    if (license) CopyrightsDialog(app, a.optJSONObject("license") ?: JSONObject()) { license = false }
}

/** CopyrightsDialog: the license and the libraries with their links */
@Composable
private fun CopyrightsDialog(app: AppState, l: JSONObject, onClose: () -> Unit) {
    Dialog(onDismissRequest = onClose) {
        Column(Modifier.width(560.dp).heightIn(max = 600.dp).background(desk().panel, RoundedCornerShape(6.dp)).padding(16.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(l.optString("title"), Modifier.weight(1f), style = deskText(18, FontWeight.Bold))
                Text("✕", Modifier.clickable(onClick = onClose).padding(8.dp), style = deskText(15, color = desk().textDim))
            }
            Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text(l.optString("licensed"), style = deskText(15))
                Text(l.optString("gpl") + ".", Modifier.clickable { openUrl(app, l.optString("gpl_link")) }, style = deskText(15, color = OrcaColors.accent))
                Text(l.optString("based") + ".", style = deskText(15))
                Spacer(Modifier.height(16.dp))
                Text(l.optString("libraries"), style = deskText(15, FontWeight.Bold))
                Text(l.optString("uses") + ":", style = deskText(15))
                Spacer(Modifier.height(8.dp))
                val e = l.optJSONArray("entries")
                for (i in 0 until (e?.length() ?: 0)) {
                    val lib = e!!.getJSONObject(i)
                    Text(lib.optString("name"), style = deskText(13))
                    Text(lib.optString("link"), Modifier.clickable { openUrl(app, lib.optString("link")) }.padding(bottom = 10.dp), style = deskText(13, color = OrcaColors.accent))
                }
            }
        }
    }
}
