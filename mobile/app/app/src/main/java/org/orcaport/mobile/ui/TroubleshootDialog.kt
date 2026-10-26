package org.orcaport.mobile.ui

import android.content.Context
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import org.json.JSONArray
import org.json.JSONObject
import org.orcaport.mobile.AppState
import org.orcaport.mobile.engine.jobj
import org.orcaport.mobile.prepare.GLInfo
import org.orcaport.mobile.prepare.PanelButton

/** what TroubleshootDialog reads from the system: the GL renderer, how the app was installed, the displays */
private fun deviceInfo(ctx: Context): JSONObject {
    val installer = runCatching {
        if (android.os.Build.VERSION.SDK_INT >= 30) ctx.packageManager.getInstallSourceInfo(ctx.packageName).installingPackageName
        else @Suppress("DEPRECATION") ctx.packageManager.getInstallerPackageName(ctx.packageName)
    }.getOrNull()
    val displays = JSONArray()
    val dm = ctx.getSystemService(Context.DISPLAY_SERVICE) as android.hardware.display.DisplayManager
    for (d in dm.displays) {
        val m = android.util.DisplayMetrics()
        @Suppress("DEPRECATION") d.getRealMetrics(m)
        displays.put(jobj("width" to m.widthPixels, "height" to m.heightPixels, "scale" to m.density.toDouble()))
    }
    return jobj("gpu" to GLInfo.renderer, "glsl" to GLInfo.glsl, "displays" to displays,
        "package" to when (installer) { "com.android.vending" -> "Google Play"; null -> "APK"; else -> "APK ($installer)" })
}

private fun openUrl(ctx: Context, url: String) = runCatching {
    ctx.startActivity(android.content.Intent(android.content.Intent.ACTION_VIEW, android.net.Uri.parse(url)).addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK))
}

/** TroubleshootDialog (Help > Troubleshoot Center): the logo, version, build and system info on the left; information,
 *  profiles and logs on the right */
@Composable
fun TroubleshootDialog(app: AppState, onClose: () -> Unit) {
    val ctx = LocalContext.current
    var s by remember { mutableStateOf<JSONObject?>(null) }
    var full by remember { mutableStateOf(true) }
    LaunchedEffect(Unit) { app.call("troubleshoot.open", deviceInfo(ctx)) { if (it is JSONObject) s = it } }
    val t = s ?: return
    val dim = Color(0xFF363636).let { if (androidx.compose.foundation.isSystemInDarkTheme()) desk().text else it }
    fun act(what: String, extra: JSONObject = JSONObject()) = app.call("troubleshoot.action", extra.put("what", what)) { r ->
        if (r !is JSONObject) return@call
        if (r.has("url")) { openUrl(ctx, r.optString("url")); return@call }
        s = r
        r.optJSONObject("save")?.let { e -> app.export("troubleshoot.write", jobj("export" to e), e.optString("name"), tr("Export successful")) }
        if (r.optBoolean("close")) onClose()
    }
    Dialog(onDismissRequest = onClose, properties = DialogProperties(usePlatformDefaultWidth = false)) {
        Column(Modifier.widthIn(max = 900.dp).fillMaxWidth(0.95f).background(desk().panel, RoundedCornerShape(6.dp))) {
            Row(Modifier.fillMaxWidth().padding(start = 16.dp, top = 8.dp, end = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                Text(t.optString("title"), Modifier.weight(1f), style = deskText(15))
                Text("✕", Modifier.clickable(onClick = onClose).padding(8.dp), style = deskText(15, color = desk().textDim))
            }
            Row(Modifier.heightIn(max = 640.dp).verticalScroll(rememberScrollState()).padding(15.dp)) {
                // LEFT: header, system info, buttons
                Column(Modifier.width(300.dp), horizontalAlignment = Alignment.CenterHorizontally) {
                    OrcaIcon(if (t.optString("logo").endsWith("_dark.svg")) "OrcaSlicer_horizontal_dark" else "OrcaSlicer_horizontal_light",
                        171.dp, height = 64.dp, darkVariant = false)
                    Spacer(Modifier.height(12.dp))
                    Box(Modifier.fillMaxWidth().height(2.dp).background(OrcaColors.accent))
                    Spacer(Modifier.height(6.dp))
                    Text(t.optString("version"), Modifier.fillMaxWidth(), style = deskText(23, color = dim), textAlign = TextAlign.Center)
                    Spacer(Modifier.height(4.dp))
                    PanelButton(t.optString("build")) { openUrl(ctx, t.optString("build_url")) }
                    Spacer(Modifier.height(15.dp))
                    val lines = t.optJSONArray(if (full) "sys_full" else "sys_short") ?: JSONArray()
                    for (i in 0 until lines.length())
                        Text(lines.optString(i), Modifier.fillMaxWidth().padding(bottom = 6.dp), style = deskText(14, color = dim), textAlign = TextAlign.Center)
                    Spacer(Modifier.height(15.dp))
                    Row(Modifier.fillMaxWidth().padding(horizontal = 5.dp)) {
                        PanelButton(t.optString(if (full) "hide" else "show")) { full = !full }
                        Spacer(Modifier.weight(1f))
                        PanelButton(t.optString("copy")) {
                            app.call("troubleshoot.action", jobj("what" to "copy")) { r ->
                                val cm = ctx.getSystemService(Context.CLIPBOARD_SERVICE) as android.content.ClipboardManager
                                cm.setPrimaryClip(android.content.ClipData.newPlainText("OrcaSlicer", r as? String ?: ""))
                            }
                        }
                    }
                    Spacer(Modifier.height(15.dp))
                    Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                        for ((icon, url) in listOf("toolbar_wiki" to "wiki_url", "toolbar_video_guide" to "video_url"))
                            Box(Modifier.size(40.dp).clip(CircleShape).background(OrcaColors.accent).clickable { openUrl(ctx, t.optString(url)) }, Alignment.Center) {
                                OrcaIcon(icon, 22.dp, darkVariant = false, tint = Color.White)
                            }
                    }
                }
                Spacer(Modifier.width(20.dp))
                // RIGHT: information, profiles, more
                Column(Modifier.weight(1f)) {
                    @Composable fun Title(text: String, top: Int) {
                        Row(Modifier.padding(top = top.dp), verticalAlignment = Alignment.CenterVertically) {
                            Text(text, style = deskText(16, color = dim))
                            Spacer(Modifier.width(8.dp))
                            Box(Modifier.weight(1f).height(1.dp).background(desk().border))
                        }
                    }
                    @Composable fun Line(label: String, top: Int, content: @Composable () -> Unit) {
                        Row(Modifier.fillMaxWidth().padding(top = top.dp), verticalAlignment = Alignment.CenterVertically) {
                            Text(label, Modifier.width(275.dp).padding(end = 15.dp), style = deskText(14, color = dim))
                            content()
                        }
                    }
                    Title(t.optString("information"), 0)
                    val info = t.optJSONArray("info") ?: JSONArray()
                    for (i in 0 until info.length())
                        Text(info.optString(i), Modifier.widthIn(max = 400.dp).padding(top = if (i == 0) 5.dp else 8.dp), style = deskText(14, color = dim))
                    Row(Modifier.fillMaxWidth().padding(top = 12.dp), verticalAlignment = Alignment.CenterVertically) {
                        Text(t.optString("report"), Modifier.clickable { act("report") },
                            style = deskText(14, color = OrcaColors.accent).copy(textDecoration = TextDecoration.Underline))
                        Spacer(Modifier.weight(1f))
                        PanelButton(t.optString("pack")) { act("pack") }
                    }
                    Title(t.optString("profiles"), 12)
                    Line(t.optString("clean_cache"), 8) { PanelButton(t.optString("clean")) { act("clean") } }
                    Line(t.optString("loaded"), 5) { PanelButton(t.optString("export")) { act("export_profiles") } }
                    // create_item_loaded_profiles: Active / System + User
                    val cols = t.optJSONArray("columns") ?: JSONArray()
                    val rows = t.optJSONArray("rows") ?: JSONArray()
                    Column(Modifier.padding(top = 5.dp)) {
                        @Composable fun Cell(text: String, w: Int, size: Int = 14) =
                            Text(text, Modifier.width(w.dp), style = deskText(size, color = dim), textAlign = TextAlign.Center)
                        Row { Spacer(Modifier.width(90.dp)); Cell(cols.optString(0), 60, 12); Cell("", 20); Cell(cols.optString(1), 60, 12); Cell("", 20); Cell(cols.optString(2), 60, 12) }
                        for (i in 0 until rows.length()) {
                            val r = rows.getJSONArray(i)
                            Row(Modifier.padding(top = 3.dp)) {
                                Text(r.optString(0), Modifier.width(90.dp), style = deskText(14, color = dim))
                                Cell(r.optInt(1).toString(), 60); Cell("/", 20, 12); Cell(r.optInt(2).toString(), 60); Cell("+", 20, 12); Cell(r.optInt(3).toString(), 60)
                            }
                        }
                    }
                    Title(t.optString("more"), 12)
                    Line(t.optString("cfg_folder"), 8) { PanelButton(t.optString("browse")) { app.showConfigFolder() } }
                    Line(t.optString("log_level"), 5) {
                        val levels = t.optJSONArray("levels")?.let { a -> List(a.length()) { a.optString(it) } } ?: emptyList()
                        DeskChoice(levels, t.optInt("level", -1), Modifier.width(120.dp)) { i -> act("log_level", jobj("index" to i)) }
                    }
                    Line(t.optString("stored_logs"), 5) { PanelButton(t.optString("pack")) { act("pack_logs") } }
                    Line(t.optString("logs_storage"), 5) { PanelButton(t.optString("clear")) { act("clear_logs") } }
                }
            }
        }
    }
}
