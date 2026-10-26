package org.orcaport.mobile.ui

import androidx.compose.foundation.layout.*
import androidx.compose.ui.unit.dp
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import kotlinx.coroutines.launch
import org.json.JSONObject
import org.orcaport.mobile.AppState
import org.orcaport.mobile.MainTab
import org.orcaport.mobile.engine.jobj

/**
 * The Home tab (WebViewPanel): Orca's home page (resources/web/homepage) — New Project, Open Project and the recent
 * projects with their thumbnails; its messages are handled by the core (Home.cpp, GUI_App::handle_web_request).
 */
@Composable
fun HomeScreen(app: AppState) {
    val scope = rememberCoroutineScope()
    var page by remember { mutableStateOf<JSONObject?>(null) }
    val handle = remember { WebPageHandle() }
    DisposableEffect(handle) { app.homeHandle = handle; onDispose { if (app.homeHandle === handle) app.homeHandle = null } }
    LaunchedEffect(Unit) { runCatching { page = app.engine.obj("home.open") } }
    // a project opened or saved elsewhere: the list follows (MainFrame::add_to_recent_projects -> SendRecentList)
    LaunchedEffect(app.project.optString("project")) {
        val list = runCatching { app.engine.call("home.recent", JSONObject()) }.getOrNull() ?: return@LaunchedEffect
        handle.run("window.postMessage(" + JSONObject().put("sequence_id", "").put("command", "get_recent_projects").put("response", list) + ")")
    }
    val p = page ?: return
    // WebViewPanel::set_cloud_mode: the page keeps its navigation column, the OrcaCloud pane takes the rest
    Row(Modifier.fillMaxSize()) {
    OrcaWebPage(p.getString("url"), p.optString("script"), if (app.cloudPane) Modifier.width(263.dp).fillMaxHeight() else Modifier.weight(1f).fillMaxHeight(), handle) { msg ->
        scope.launch {
            try {
                val r = app.engine.obj("home.message", jobj("message" to msg))
                r.optJSONArray("scripts")?.let { s -> for (i in 0 until s.length()) handle.run(s.getString(i)) }
                r.optString("notice").takeIf { it.isNotEmpty() }?.let { app.toast = it }
                r.optString("url").takeIf { it.isNotEmpty() }?.let { u ->
                    runCatching { app.appContext?.startActivity(android.content.Intent(android.content.Intent.ACTION_VIEW, android.net.Uri.parse(u)).addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK)) }
                }
                when (r.optString("action")) {
                    "new" -> app.newProject()
                    "open" -> app.openProject()
                    "open_recent" -> app.openRecent(r.optString("path"))
                    "login" -> if ("login" !in app.webDialogs) app.webDialogs.add("login")
                    "open_cloud" -> app.cloudPane = true
                    "close_cloud" -> { app.cloudPane = false; app.call("cloud.close_pane") }
                }
            } catch (e: Exception) { app.toast = e.message }
        }
    }
    if (app.cloudPane) CloudPane(app, Modifier.weight(1f).fillMaxHeight())
    }
}

/** the OrcaCloud pane: the Start URL, the page's request_web_session, and the URLs the core sends (Complete, sign-out) */
@Composable
private fun CloudPane(app: AppState, modifier: Modifier) {
    val handle = remember { WebPageHandle() }
    var start by remember { mutableStateOf<String?>(null) }
    LaunchedEffect(app.cloudNext) {
        app.call("cloud.open_pane", org.orcaport.mobile.engine.jobj("next" to app.cloudNext)) { if (it is JSONObject) start = it.optString("page").ifEmpty { it.optString("url") } }
    }
    LaunchedEffect(app.cloudPaneEvent) {
        val e = app.cloudPaneEvent ?: return@LaunchedEffect
        app.cloudPaneEvent = null
        val v = handle.view ?: return@LaunchedEffect
        e.optString("load").takeIf { it.isNotEmpty() }?.let { v.loadUrl(it) }
        e.optString("seed").takeIf { it.isNotEmpty() }?.let { v.loadUrl(it) }
        e.optString("signout").takeIf { it.isNotEmpty() }?.let {
            // the sign-out page ends the server session; the pane's stored session goes with it
            v.loadUrl(it)
            android.webkit.CookieManager.getInstance().removeAllCookies(null)
            android.webkit.WebStorage.getInstance().deleteAllData()
        }
    }
    val url = start ?: return
    OrcaWebPage(url, "", modifier, handle, remote = true, keepOrigin = true) { msg -> app.call("cloud.pane_message", jobj("message" to msg)) }
}
