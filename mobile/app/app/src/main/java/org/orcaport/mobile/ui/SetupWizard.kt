package org.orcaport.mobile.ui

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.compose.material.icons.filled.Close
import kotlinx.coroutines.launch
import org.json.JSONObject
import org.orcaport.mobile.AppState
import org.orcaport.mobile.engine.jobj

/** The setup wizard (desktop GuideFrame): Orca's guide pages, messages handled by the core (Guide.cpp). */
@Composable
fun SetupWizard(app: AppState, page: String, onClose: () -> Unit) {
    val scope = rememberCoroutineScope()
    var open by remember { mutableStateOf<JSONObject?>(null) }
    var error by remember { mutableStateOf<String?>(null) }
    val handle = remember { WebPageHandle() }
    LaunchedEffect(page) {
        try { open = app.engine.obj("guide.open", jobj("page" to page)) } catch (e: Exception) { error = e.message }
    }
    fun finish(result: String) = scope.launch {
        if (result != "ok") runCatching { app.presets = app.engine.obj("guide.cancelled") }
        else app.presets = app.engine.obj("presets.state")
        onClose()
        if (result == "create_printer") app.editing = "printer"
    }
    BackHandler { finish("cancel") }
    // GuideFrame: an 820 x 660 dialog titled "Setup Wizard" (full screen when the display is smaller)
    val compact = isCompact()
    Box(Modifier.fillMaxSize().background(androidx.compose.ui.graphics.Color(0x80000000)).systemBarsPadding().imePadding(), Alignment.Center) {
      Surface(if (compact) Modifier.fillMaxSize() else Modifier.widthIn(max = 820.dp).heightIn(max = 700.dp).fillMaxWidth().fillMaxHeight(),
          shape = if (compact) androidx.compose.ui.graphics.RectangleShape else androidx.compose.foundation.shape.RoundedCornerShape(6.dp),
          color = androidx.compose.ui.graphics.Color.White) {
       Column {
        Row(Modifier.fillMaxWidth().height(40.dp).padding(start = 16.dp), verticalAlignment = Alignment.CenterVertically) {
            Text(tr("Setup Wizard"), Modifier.weight(1f), color = androidx.compose.ui.graphics.Color(0xFF262E30))
            IconButton(onClick = { finish("cancel") }) { Icon(androidx.compose.material.icons.Icons.Filled.Close, "Close", tint = androidx.compose.ui.graphics.Color(0xFF6B6A6A)) }
        }
        val o = open
        when {
            error != null -> Box(Modifier.fillMaxSize(), Alignment.Center) { Text(error ?: "", color = MaterialTheme.colorScheme.error) }
            o == null -> Box(Modifier.fillMaxSize(), Alignment.Center) {
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    CircularProgressIndicator(); Spacer(Modifier.height(12.dp)); Text(app.progress ?: tr("Loading printer & filament profiles"))
                }
            }
            else -> OrcaWebPage(o.getString("url"), o.optString("script"), Modifier.fillMaxSize(), handle) { msg ->
                scope.launch {
                    try {
                        val r = app.engine.obj("guide.message", jobj("message" to msg))
                        val scripts = r.optJSONArray("scripts")
                        if (scripts != null) for (i in 0 until scripts.length()) handle.run(scripts.getString(i))
                        if (!r.isNull("close")) finish(r.getString("close"))
                    } catch (e: Exception) { app.toast = e.message }
                }
            }
        }
    }
       }
      }
    }
