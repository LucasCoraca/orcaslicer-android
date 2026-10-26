package org.orcaport.mobile.ui

import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import kotlinx.coroutines.launch
import org.json.JSONObject
import org.orcaport.mobile.AppState
import org.orcaport.mobile.engine.jobj

/**
 * The Project tab (ProjectPanel): Orca's model page (resources/web/model) with the 3MF's model and profile information
 * and its accessories; an accessory opens in the app the system picks for it.
 */
@Composable
fun ProjectInfoScreen(app: AppState) {
    val scope = rememberCoroutineScope()
    var page by remember { mutableStateOf<JSONObject?>(null) }
    val handle = remember { WebPageHandle() }
    LaunchedEffect(Unit) { runCatching { page = app.engine.obj("projectinfo.open") } }
    // ProjectPanel::Show -> update_model_data: a newly opened project
    LaunchedEffect(app.project.optString("project")) {
        val r = runCatching { app.engine.obj("projectinfo.reload") }.getOrNull() ?: return@LaunchedEffect
        r.optJSONArray("scripts")?.let { s -> for (i in 0 until s.length()) handle.run(s.getString(i)) }
    }
    var editing by remember { mutableStateOf(false) }
    if (editing) {   // ProjectPanel::show_info_editor
        ProjectInfoEditor(app) {
            editing = false
            scope.launch { runCatching { app.engine.obj("projectinfo.reload") }.getOrNull()?.optJSONArray("scripts")?.let { s -> for (i in 0 until s.length()) handle.run(s.getString(i)) } }
        }
        return
    }
    val p = page ?: return
    OrcaWebPage(p.getString("url"), p.optString("script"), Modifier.fillMaxSize(), handle) { msg ->
        scope.launch {
            try {
                val r = app.engine.obj("projectinfo.message", jobj("message" to msg))
                r.optJSONArray("scripts")?.let { s -> for (i in 0 until s.length()) handle.run(s.getString(i)) }
                r.optString("open").takeIf { it.isNotEmpty() }?.let { path -> openAttachment(app, path) }
                if (r.optBoolean("edit")) editing = true
            } catch (e: Exception) { app.toast = e.message }
        }
    }
}

/** desktop_open_project_attachment: the file shared through the app's FileProvider to a viewer */
private fun openAttachment(app: AppState, path: String) {
    val ctx = app.appContext ?: return
    runCatching {
        val f = java.io.File(path)
        val uri = androidx.core.content.FileProvider.getUriForFile(ctx, ctx.packageName + ".files", f)
        val mime = android.webkit.MimeTypeMap.getSingleton().getMimeTypeFromExtension(f.extension.lowercase()) ?: "*/*"
        ctx.startActivity(android.content.Intent(android.content.Intent.ACTION_VIEW).setDataAndType(uri, mime)
            .addFlags(android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION or android.content.Intent.FLAG_ACTIVITY_NEW_TASK))
    }.onFailure { app.toast = it.message }
}
