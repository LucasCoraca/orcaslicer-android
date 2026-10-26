package org.orcaport.mobile.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import org.json.JSONObject
import org.orcaport.mobile.AppState
import org.orcaport.mobile.MainTab
import org.orcaport.mobile.engine.jobj

/**
 * PluginPages::relayout: the plugins' pages after the built-in tabs, as many as Preferences allows; with more, the last
 * slot shows the page picked from the ▾ overflow menu.
 */
@Composable
fun PluginPageTabs(app: AppState, compact: Boolean) {
    val pages = (0 until app.pluginPages.length()).map { app.pluginPages.getJSONObject(it) }
    if (pages.isEmpty()) return
    val slots = app.pluginPagesVisible.coerceAtLeast(1)
    val overflow = pages.size > slots
    var swapped by remember { mutableStateOf<String?>(null) }
    val shown = if (!overflow) pages else {
        val rest = pages.drop(slots - 1)
        // the selected page sits in the swappable slot when it is one of the overflowing ones
        val pick = rest.firstOrNull { it.optString("id") == (if (app.tab == MainTab.Plugin) app.pluginPage else null) }
            ?: rest.firstOrNull { it.optString("id") == swapped } ?: rest.first()
        swapped = pick.optString("id")
        pages.take(slots - 1) + pick
    }
    for (p in shown) {
        val id = p.optString("id")
        val sel = app.tab == MainTab.Plugin && app.pluginPage == id
        Row(Modifier.fillMaxHeight().background(if (sel) FrameColors.tabSelected else Color.Transparent)
            .clickable { app.pluginPage = id; app.tab = MainTab.Plugin }.padding(horizontal = if (compact) 14.dp else 20.dp),
            verticalAlignment = Alignment.CenterVertically) {
            Text(p.optString("name"), color = FrameColors.text, fontSize = 14.sp, fontWeight = if (sel) FontWeight.Bold else FontWeight.Normal, maxLines = 1)
        }
    }
    if (overflow) Box {
        var open by remember { mutableStateOf(false) }
        Box(Modifier.fillMaxHeight().clickable { open = true }.padding(horizontal = 10.dp), Alignment.Center) {
            Text("\u25BE", color = FrameColors.text, fontSize = 16.sp)
        }
        DropdownMenu(open, { open = false }) {
            for (p in pages.drop(slots - 1)) DropdownMenuItem(
                leadingIcon = { RadioButton(p.optString("id") == swapped, null) },
                text = { Text(p.optString("name"), style = deskText(14)) },
                onClick = { open = false; swapped = p.optString("id"); app.pluginPage = p.optString("id"); app.tab = MainTab.Plugin })
        }
    }
}

/** PluginPage: the page's HTML from the plugin with the page bridge; its window.orca messages go to the capability */
@Composable
fun PluginPageScreen(app: AppState, id: String) {
    var page by remember { mutableStateOf<JSONObject?>(null) }
    var error by remember { mutableStateOf<String?>(null) }
    val handle = remember { WebPageHandle() }
    DisposableEffect(id) {
        onDispose { app.call("plugins.page_close", jobj("id" to id)) }
    }
    LaunchedEffect(id) {
        runCatching { page = app.engine.obj("plugins.page_open", jobj("id" to id)) }.onFailure { error = it.message }
    }
    LaunchedEffect(id) {
        app.pageScripts.collect { r -> if (r.optString("target") == "plugin_page" && r.optString("id") == id) handle.run(r.optString("script")) }
    }
    error?.let { Box(Modifier.fillMaxSize(), Alignment.Center) { Text(it, style = deskText(14)) } }
    val p = page ?: return
    OrcaWebPage(p.optString("base_url"), p.optString("script"), Modifier.fillMaxSize().background(desk().panel), handle, html = p.optString("html")) { msg ->
        app.post("plugins.page_message", jobj("id" to id, "message" to msg))
    }
}
