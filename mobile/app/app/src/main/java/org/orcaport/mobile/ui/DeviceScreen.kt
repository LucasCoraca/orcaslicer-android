package org.orcaport.mobile.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import org.json.JSONArray
import org.json.JSONObject
import org.orcaport.mobile.AppState
import org.orcaport.mobile.engine.jobj
import org.orcaport.mobile.prepare.DeskDialog
import org.orcaport.mobile.prepare.PanelButton

/**
 * The Device tab for printers driven through a print host (MainFrame::load_printer_url + PrinterWebView): the host's web
 * interface, with the API key added to the page's fetch calls like PrinterWebView::SendAPIKey.
 */
@Composable
fun DeviceScreen(app: AppState) {
    val url = app.device.optString("url")
    val c = desk()
    if (url.isEmpty()) {
        Box(Modifier.fillMaxSize().background(c.header), Alignment.Center) {
            Column(horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(12.dp)) {
                Text(app.device.optString("no_url"), Modifier.widthIn(max = 520.dp), style = deskText(14, color = c.textDim))
                PanelButton(tr("Connection"), primary = true) { app.openPrinterHost() }
            }
        }
        return
    }
    val key = app.device.optString("apikey")
    key(url, key) { PrinterWebView(url, key, app.deviceReload) }
}

/**
 * PrinterWebView: the host's page in a WebView that stays in the app (links included). Once a page has loaded, the API
 * key is added to the page's fetch calls by a document-start script and the page reloaded (PrinterWebView::SendAPIKey).
 */
@android.annotation.SuppressLint("SetJavaScriptEnabled")
@Composable
fun PrinterWebView(url: String, apikey: String, reload: Int = 0) {
    val reloaded = remember { intArrayOf(reload) }
    androidx.compose.ui.viewinterop.AndroidView(modifier = Modifier.fillMaxSize(), update = { v ->
        // F5 (MainFrame::load_printer_url): the page again
        if (reload != reloaded[0]) { reloaded[0] = reload; v.reload() }
    }, factory = { ctx ->
        android.webkit.WebView(ctx).apply {
            layoutParams = android.view.ViewGroup.LayoutParams(android.view.ViewGroup.LayoutParams.MATCH_PARENT, android.view.ViewGroup.LayoutParams.MATCH_PARENT)
            settings.javaScriptEnabled = true
            settings.domStorageEnabled = true
            var keySent = false
            webViewClient = object : android.webkit.WebViewClient() {
                override fun onPageFinished(view: android.webkit.WebView, u: String?) {
                    if (keySent || apikey.isEmpty() || u.isNullOrEmpty()) return
                    keySent = true
                    val script = """
                        if (window.fetch) {
                            const originalFetch = window.fetch;
                            window.fetch = function(input, init = {}) {
                                init.headers = init.headers || {};
                                init.headers['X-API-Key'] = '${apikey.replace("\\", "\\\\").replace("'", "\\'")}';
                                return originalFetch(input, init);
                            };
                        }
                    """.trimIndent()
                    if (androidx.webkit.WebViewFeature.isFeatureSupported(androidx.webkit.WebViewFeature.DOCUMENT_START_SCRIPT)) {
                        androidx.webkit.WebViewCompat.addDocumentStartJavaScript(view, script, setOf("*"))
                        view.reload()
                    } else view.evaluateJavascript(script, null)
                }
            }
            loadUrl(url)
        }
    })
}

/** PhysicalPrinterDialog: the printer preset's name and its print host settings, shown as upstream's update() leaves them. */
@Composable
fun PrinterHostDialog(app: AppState, onClose: () -> Unit) {
    val st = app.printerHost ?: return
    val c = desk()
    fun set(key: String, value: Any) = app.call("printhost.set", jobj("key" to key, "value" to value)) { if (it is JSONObject) app.printerHost = it }
    val buttons = st.optJSONObject("buttons") ?: JSONObject()
    fun btn(name: String) = buttons.optJSONObject(name) ?: JSONObject()
    val ok = btn("ok")
    DeskDialog(st.optString("title"), onDismiss = { app.call("printhost.close") { if (it is JSONObject) app.presets = it }; onClose() },
        ok = {
            if (!ok.optBoolean("enabled", true)) return@DeskDialog
            app.call("printhost.ok") { if (it is JSONObject) { app.presets = it; onClose(); app.call("device.state") { d -> if (d is JSONObject) app.device = d } } }
        }, okLabel = ok.optString("label", "OK"), cancelLabel = null) {
        Column(Modifier.heightIn(max = 560.dp).widthIn(min = 520.dp).verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(st.optString("name_label"), style = deskText(14))
            DeskInput(st.optString("name"), true, false, Modifier.fillMaxWidth()) { set("name", it) }
            st.optString("valid").takeIf { it.isNotEmpty() }?.let { Text(it, style = deskText(12, color = OrcaColors.warning)) }
            Text(st.optString("group"), style = deskText(14, androidx.compose.ui.text.font.FontWeight.Bold))
            val fields = st.optJSONArray("fields") ?: JSONArray()
            for (i in 0 until fields.length()) {
                val f = fields.getJSONObject(i)
                if (!f.optBoolean("shown", true)) continue
                val key = f.optString("key")
                val enabled = f.optBoolean("enabled", true)
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(f.optString("label"), Modifier.width(170.dp), style = deskText(13, color = if (enabled) c.text else c.textDisabled))
                    when (f.optString("kind")) {
                        "choice" -> {
                            val opts = f.optJSONArray("options") ?: JSONArray()
                            DeskChoice(List(opts.length()) { opts.optString(it) }, f.optInt("value", -1), Modifier.width(260.dp), enabled) { set(key, it) }
                        }
                        "check" -> DeskCheck(f.optBoolean("value"), enabled) { set(key, it) }
                        else -> DeskInput(f.optString("value"), enabled, false, Modifier.width(260.dp)) { set(key, it) }
                    }
                }
                // the buttons appended to the host / port / CA file lines, under the field
                val extra = when (key) {
                    "print_host" -> listOf("browse", "test", "generate_creds", "logout")
                    "printhost_port" -> listOf("port_browse")
                    "printhost_cafile" -> listOf("cafile_browse")
                    else -> emptyList()
                }.filter { btn(it).optBoolean("shown", true) }
                if (extra.isNotEmpty()) Row(Modifier.padding(start = 170.dp), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    for (b in extra) {
                        val bj = btn(b)
                        PanelButton(bj.optString("label"), enabled = bj.optBoolean("enabled", true)) {
                            when (b) {
                                "test" -> app.call("printhost.test")
                                "generate_creds" -> app.call("printhost.test")
                                "logout" -> app.call("printhost.logout") { if (it is JSONObject) app.printerHost = it }
                                "port_browse" -> app.call("printhost.refresh") { if (it is JSONObject) app.printerHost = it }
                                "browse" -> app.browsePrintHosts()
                                "cafile_browse" -> app.pickCaFile()
                            }
                        }
                    }
                }
                if (key == "printhost_cafile") Text(st.optString("ca_hint"), style = deskText(12, color = c.textDim))
            }
        }
    }
    app.printHostBrowse?.let { b ->
        val list = b.optJSONArray("printers") ?: JSONArray()
        val cols = b.optJSONArray("columns")
        var sel by remember(b) { mutableIntStateOf(-1) }
        DeskDialog(b.optString("title"), onDismiss = { app.printHostBrowse = null }, ok = {
            if (sel >= 0) list.optJSONObject(sel)?.let { p ->
                val args = jobj("host" to p.optString("host"))
                if (p.has("serial")) args.put("serial", p.optString("serial"))
                app.call("printhost.pick", args) { if (it is JSONObject) app.printerHost = it }
            }
            app.printHostBrowse = null
        }) {
            b.optString("prompt").takeIf { it.isNotEmpty() }?.let { Text(it, style = deskText(13)) }
            if (cols != null) Row { for (k in 0 until cols.length()) Text(cols.optString(k), Modifier.width(150.dp), style = deskText(12, color = c.textDim)) }
            Column(Modifier.heightIn(max = 300.dp).verticalScroll(rememberScrollState())) {
                for (i in 0 until list.length()) {
                    val p = list.getJSONObject(i)
                    val pc = p.optJSONArray("columns") ?: JSONArray()
                    Row(Modifier.fillMaxWidth().background(if (i == sel) c.accent.copy(alpha = 0.2f) else c.panel).clickable { sel = i }.padding(vertical = 6.dp)) {
                        for (k in 0 until pc.length()) Text(pc.optString(k), Modifier.width(150.dp), style = deskText(13))
                    }
                }
            }
            // BonjourDialog's label: "Searching for devices..." while the lookup runs
            Text(if (b.optBoolean("done")) b.optString("finished") else b.optString("searching") + "...", style = deskText(13, color = c.textDim))
        }
    }
}

/** PrintHostSendDialog: file name (with the recent folder), Repetier group, PrusaLink storage, the post-upload action. */
@Composable
fun PrintHostSendDialog(app: AppState, d: JSONObject, onClose: () -> Unit) {
    val c = desk()
    var name by remember(d) { mutableStateOf(d.optString("filename")) }
    var switchTab by remember(d) { mutableStateOf(d.optBoolean("switch")) }
    val groups = d.optJSONArray("groups")
    var group by remember(d) { mutableStateOf(d.optString("group")) }
    val storages = d.optJSONArray("storage_names")
    var storage by remember(d) { mutableStateOf(d.optString("storage")) }
    var confirm by remember(d) { mutableStateOf<String?>(null) }
    // Elegoo / Creality / Flashforge: their dialog's options (SendDialogs.cpp), the answer goes back as "extras"
    val ex = d.optJSONObject("extras") ?: JSONObject()
    val elegoo = ex.optJSONObject("elegoo")
    val creality = ex.optJSONObject("creality")
    val ff = ex.optJSONObject("flashforge")
    val flags = remember(d) {
        mutableStateMapOf<String, Boolean>().apply {
            elegoo?.let { for (k in listOf("upload_print", "time_lapse", "leveling", "side_b")) put(k, it.optBoolean(k)) }
            creality?.let { put("self_test", it.optBoolean("self_test")) }
            ff?.let { for (k in listOf("leveling", "time_lapse", "ifs")) put(k, it.optBoolean(k)) }
        }
    }
    val selected = remember(d) {
        mutableStateListOf<Int>().apply {
            val f = (creality ?: ff)?.optJSONArray("filaments") ?: JSONArray()
            for (i in 0 until f.length()) add(f.getJSONObject(i).optInt("selected"))
        }
    }
    var error by remember(d) { mutableStateOf<String?>(null) }
    fun send(action: String) {
        // FlashforgePrintHostSendDialog::validate_before_close
        ff?.let {
            val n = it.optJSONArray("filaments")?.length() ?: 0
            if (flags["ifs"] != true && n > 1) { error = it.optString("error_multi"); return }
            if (flags["ifs"] == true && selected.any { s -> s <= 0 }) { error = it.optString("error_unassigned"); return }
        }
        val args = jobj("plate" to d.optInt("plate"), "filename" to name, "action" to action, "switch" to switchTab)
        if (ex.length() > 0) args.put("extras", JSONObject().apply {
            for ((k, v) in flags) put(k, v)
            put("selected", JSONArray(selected.toList()))
        })
        if (groups != null) args.put("group", group)
        if (storages != null) {
            val paths = d.optJSONArray("storage_paths") ?: JSONArray()
            val i = (0 until storages.length()).firstOrNull { storages.optString(it) == storage } ?: 0
            args.put("storage", paths.optString(i)); args.put("storage_name", storage)
        } else if (d.has("storage_preset")) args.put("storage", d.optString("storage_preset"))
        onClose()
        app.call("printhost.send", args) { r -> if (r is JSONObject) app.toast = r.optString("status") }
    }
    // the .gcode suffix control, then PrusaConnect's "Is the printer ready?" for Upload and Print
    fun validate(action: String) {
        val suffix = d.optString("valid_suffix")
        if (suffix.isNotEmpty() && !name.lowercase().endsWith(suffix.lowercase())) { confirm = "suffix:$action"; return }
        if (action == "print" && d.has("confirm_print")) { confirm = "ready:$action"; return }
        send(action)
    }
    AlertDialog(onDismissRequest = onClose, containerColor = c.panel,
        title = { Text(d.optString("title"), style = deskText(16)) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                Text(d.optString("message"), style = deskText(13))
                DeskInput(name, true, false, Modifier.fillMaxWidth()) { name = it }
                Text(d.optString("dir_hint"), style = deskText(12, color = c.textDim))
                if (groups != null) {
                    Text(d.optString("group_label"), style = deskText(13))
                    val gl = List(groups.length()) { groups.optString(it) }
                    DeskChoice(gl, gl.indexOf(group), Modifier.width(260.dp)) { group = gl[it] }
                }
                if (storages != null) {
                    Text(d.optString("storage_label"), style = deskText(13))
                    val sl = List(storages.length()) { storages.optString(it) }
                    DeskChoice(sl, sl.indexOf(storage), Modifier.width(260.dp)) { storage = sl[it] }
                } else d.optString("storage_info").takeIf { it.isNotEmpty() }?.let { Text(it, style = deskText(13)) }
                Row(verticalAlignment = Alignment.CenterVertically) {
                    DeskCheck(switchTab) { switchTab = it }
                    Text(d.optString("switch_label"), style = deskText(13))
                }
                @Composable fun check(key: String, label: String, indent: Boolean = false, enabled: Boolean = true) =
                    Row(Modifier.padding(start = if (indent) 16.dp else 0.dp), verticalAlignment = Alignment.CenterVertically) {
                        DeskCheck(flags[key] == true, enabled = enabled) { flags[key] = it }
                        Text(label, style = deskText(13, color = if (enabled) c.text else c.textDisabled))
                    }
                // ElegooPrintHostSendDialog: Upload and Print with its options, the bed side and its mismatch warning
                elegoo?.let { e ->
                    check("upload_print", e.optString("upload_print_label"))
                    if (flags["upload_print"] == true) {
                        check("time_lapse", e.optString("time_lapse_label"), true)
                        check("leveling", e.optString("leveling_label"), true)
                        for ((b, label) in listOf(false to e.optString("side_a_label"), true to e.optString("side_b_label")))
                            Row(Modifier.padding(start = 16.dp), verticalAlignment = Alignment.CenterVertically) {
                                RadioButton(flags["side_b"] == b, { flags["side_b"] = b }); Text(label, style = deskText(13))
                            }
                        if (flags["side_b"] != e.optBoolean("app_side_b"))
                            Text(e.optString("warning"), Modifier.padding(start = 16.dp), style = deskText(13, color = OrcaColors.error))
                    }
                }
                // the filament -> printer slot rows (Creality's CFS mapping, Flashforge's IFS mapping)
                @Composable fun mapping(f: JSONArray, options: (Int) -> List<Pair<Int, JSONObject>>, enabled: (Int) -> Boolean) {
                    for (i in 0 until f.length()) {
                        val fi = f.getJSONObject(i)
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Box(Modifier.size(16.dp).background(runCatching { Color(android.graphics.Color.parseColor(fi.optString("color"))) }.getOrDefault(Color.Gray)))
                            Text(" " + fi.optString("label", fi.optString("type")) + "  →  ", style = deskText(13))
                            val opts = options(i)
                            val idx = opts.indexOfFirst { it.first == selected[i] }
                            DeskChoice(opts.map { it.second.optString("label", it.second.optString("material")) }, idx, Modifier.width(220.dp), enabled = enabled(i)) {
                                selected[i] = opts[it].first
                            }
                        }
                    }
                }
                creality?.let { cr ->
                    Text(cr.optString("title"), style = deskText(14))
                    check("self_test", cr.optString("self_test_label"))
                    val slots = cr.optJSONArray("slots") ?: JSONArray()
                    val f = cr.optJSONArray("filaments") ?: JSONArray()
                    if (f.length() > 0) {
                        Text(cr.optString("mapping_label"), style = deskText(13))
                        // the external spool feeds one filament: choosing it locks the other rows
                        val extRow = (0 until f.length()).firstOrNull { slots.optJSONObject(selected[it])?.optBoolean("ext") == true }
                        mapping(f, { _ -> List(slots.length()) { it to slots.getJSONObject(it) } }, { i -> extRow == null || extRow == i })
                    }
                }
                ff?.let { fl ->
                    check("leveling", fl.optString("leveling_label"))
                    check("time_lapse", fl.optString("time_lapse_label"))
                    check("ifs", fl.optString("ifs_label"), enabled = fl.optBoolean("ifs_enabled"))
                    Text(fl.optString("status"), style = deskText(12, color = c.textDim))
                    if (flags["ifs"] == true && fl.optBoolean("ifs_enabled")) {
                        val slots = fl.optJSONArray("slots") ?: JSONArray()
                        val f = fl.optJSONArray("filaments") ?: JSONArray()
                        mapping(f, { i ->
                            val cand = f.getJSONObject(i).optJSONArray("candidates") ?: JSONArray()
                            val ids = List(cand.length()) { cand.getInt(it) }
                            (0 until slots.length()).map { slots.getJSONObject(it) }.filter { it.optInt("id") in ids }.map { s ->
                                s.optInt("id") to JSONObject(s.toString()).put("label", "${s.optInt("id")} - ${s.optString("material")}")
                            }
                        }, { true })
                    }
                }
            }
        },
        confirmButton = {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                PanelButton(d.optString("upload"), primary = true) { validate("upload") }
                // Elegoo's Upload and Print is the check box above
                if (elegoo == null) d.optString("upload_print").takeIf { it.isNotEmpty() }?.let { PanelButton(it) { validate("print") } }
                PanelButton(d.optString("cancel")) { onClose() }
            }
        })
    error?.let { msg -> DeskDialog("Error", onDismiss = { error = null }, ok = { error = null }, cancelLabel = null) { Text(msg, style = deskText(13)) } }
    confirm?.let { cf ->
        val (what, action) = cf.split(":")
        DeskDialog(if (what == "ready") "Upload and Print" else "OrcaSlicer", onDismiss = { confirm = null }, ok = {
            confirm = null
            if (what == "suffix" && action == "print" && d.has("confirm_print")) confirm = "ready:$action" else send(action)
        }, okLabel = if (what == "suffix") "Yes" else "OK", cancelLabel = if (what == "suffix") "No" else "Cancel") {
            Text(if (what == "ready") d.optString("confirm_print") else d.optString("suffix_warning"), style = deskText(13))
        }
    }
}

/** The upload notification of each job (NotificationManager's upload job notification: progress, cancel, result). */
@Composable
fun UploadNotifications(app: AppState, modifier: Modifier = Modifier) {
    val jobs = app.uploadJobs
    val c = desk()
    Column(modifier, verticalArrangement = Arrangement.spacedBy(6.dp)) {
        for (i in 0 until jobs.length()) {
            val j = jobs.getJSONObject(i)
            val id = j.optInt("id")
            if (id in app.dismissedUploads) continue
            Column(Modifier.width(380.dp).background(c.panel).padding(10.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text("${j.optString("file")} → ${j.optString("host")}", Modifier.weight(1f), style = deskText(13), maxLines = 1)
                    Text("✕", Modifier.clickable { app.dismissedUploads += id }.padding(4.dp), style = deskText(13, color = c.textDim))
                }
                when (j.optString("state")) {
                    "queued", "progress" -> Row(verticalAlignment = Alignment.CenterVertically) {
                        LinearProgressIndicator(progress = { j.optInt("progress") / 100f }, Modifier.weight(1f), color = OrcaColors.accent)
                        Text("${j.optInt("progress")}%", Modifier.padding(horizontal = 8.dp), style = deskText(12))
                        Text(tr("Cancel"), Modifier.clickable { app.call("upload.cancel", jobj("id" to id)) }, style = deskText(13, color = OrcaColors.accent))
                    }
                    "completed" -> Text(tr("Completed"), style = deskText(12, color = c.textDim))
                    "cancelled" -> Text(tr("Canceled"), style = deskText(12, color = c.textDim))
                    "error" -> Text(tr("Upload failed"), style = deskText(12, color = OrcaColors.warning))
                }
            }
        }
    }
}
