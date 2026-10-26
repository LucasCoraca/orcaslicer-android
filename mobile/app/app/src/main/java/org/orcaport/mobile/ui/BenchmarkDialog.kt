package org.orcaport.mobile.ui

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import org.json.JSONObject
import org.orcaport.mobile.AppState
import org.orcaport.mobile.prepare.PanelButton

/** SceneBenchmarkDialog: while it runs, the status and progress in a corner of the 3D view with the rest of the window
 *  locked (wxWindowDisabler); when done, the report with Copy / OK */
@Composable
fun BoxScope.BenchmarkDialog(app: AppState, b: JSONObject) {
    val ctx = LocalContext.current
    if (!b.optBoolean("done")) {
        // the main window stays locked until it finishes
        Box(Modifier.matchParentSize().clickable(remember { MutableInteractionSource() }, null) {})
        val wide = LocalConfiguration.current.screenWidthDp >= 600
        // a corner of the 3D view, away from the model (next to the sidebar on a tablet)
        Column(Modifier.align(Alignment.BottomStart).navigationBarsPadding().padding(start = if (wide) 412.dp else 16.dp, bottom = 20.dp).width(360.dp)
            .background(desk().panel, RoundedCornerShape(6.dp)).border(1.dp, desk().border, RoundedCornerShape(6.dp))) {
            Text(b.optString("title"), Modifier.padding(start = 15.dp, top = 10.dp), style = deskText(13, color = desk().textDim))
            Text(b.optString("status"), Modifier.padding(start = 15.dp, end = 15.dp, top = 15.dp), style = deskText(14))
            LinearProgressIndicator({ b.optInt("progress") / 100f }, Modifier.padding(15.dp).fillMaxWidth().height(8.dp).clip(RoundedCornerShape(4.dp)),
                color = OrcaColors.accent)
            Row(Modifier.fillMaxWidth().padding(start = 15.dp, end = 15.dp, bottom = 15.dp), horizontalArrangement = Arrangement.End) {
                PanelButton(b.optString("cancel").ifEmpty { tr("Cancel") }) { app.call("benchmark.cancel"); app.redraw() }
            }
        }
        return
    }
    // show_report
    Dialog(onDismissRequest = { app.benchmark = null }, properties = DialogProperties(usePlatformDefaultWidth = false)) {
        Column(Modifier.widthIn(max = 900.dp).background(desk().panel, RoundedCornerShape(6.dp))) {
            Text(b.optString("title"), Modifier.padding(start = 16.dp, top = 12.dp), style = deskText(15))
            if (b.optString("error").isNotEmpty())
                Text(b.optString("error"), Modifier.padding(start = 15.dp, end = 15.dp, top = 15.dp).widthIn(max = 500.dp), style = deskText(14))
            val report = b.optString("report")
            if (report.isNotEmpty())
                Box(Modifier.padding(start = 15.dp, end = 15.dp, top = 15.dp).heightIn(max = 560.dp).border(1.dp, desk().border)
                    .verticalScroll(rememberScrollState()).horizontalScroll(rememberScrollState()).padding(10.dp)) {
                    Text(report, style = deskText(12).copy(fontFamily = FontFamily.Monospace), softWrap = false)
                }
            Row(Modifier.fillMaxWidth().padding(15.dp), horizontalArrangement = Arrangement.End) {
                if (report.isNotEmpty()) {
                    PanelButton(b.optString("copy")) {
                        (ctx.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager).setPrimaryClip(ClipData.newPlainText("benchmark", report))
                    }
                    Spacer(Modifier.width(10.dp))
                }
                PanelButton(b.optString("ok"), primary = true) { app.benchmark = null }
            }
        }
    }
}
