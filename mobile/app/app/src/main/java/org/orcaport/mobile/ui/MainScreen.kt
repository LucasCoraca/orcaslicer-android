package org.orcaport.mobile.ui

import android.app.Activity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import org.orcaport.mobile.AppState
import org.orcaport.mobile.MainTab
import org.json.JSONObject
import org.orcaport.mobile.engine.jobj
import org.orcaport.mobile.prepare.PrepareScreen
import org.orcaport.mobile.settings.SettingsEditor

/** phone layout: the desktop port's compact mode (window narrower than 600 dp) */
@Composable
fun isCompact() = LocalConfiguration.current.screenWidthDp < 600

/** MainFrame: the title bar, the tab bar with the slice / export buttons, and the selected page. */
@Composable
fun MainScreen(app: AppState, activity: Activity) {
    if (!app.ready) { Startup(app); return }
    val compact = isCompact()
    val ctx = LocalContext.current
    val asks by app.engine.host.asks.collectAsState()
    var fileMenu by remember { mutableStateOf(false) }
    var mainMenu by remember { mutableStateOf(false) }
    val importer = rememberLauncherForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris ->
        if (uris.isNotEmpty()) app.importUris(ctx, uris)
    }
    var pendingSave by remember { mutableStateOf<((android.net.Uri) -> Unit)?>(null) }
    val creator = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/octet-stream")) { uri ->
        val then = pendingSave; pendingSave = null
        if (uri != null && then != null) then(uri) else app.saveCanceled?.invoke()
    }
    var pendingOpen by remember { mutableStateOf<((android.net.Uri) -> Unit)?>(null) }
    val opener = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        val then = pendingOpen; pendingOpen = null
        if (uri != null && then != null) then(uri)
    }
    var pendingFolder by remember { mutableStateOf<((android.net.Uri) -> Unit)?>(null) }
    val folder = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
        val then = pendingFolder; pendingFolder = null
        if (uri != null && then != null) then(uri)
    }
    // wxFileDialog (Install local plugin): the document copied to the cache under its name, its path answered; none when canceled
    var fileAsk by remember { mutableStateOf<org.orcaport.mobile.engine.Ask?>(null) }
    val fileAskLauncher = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        val a = fileAsk; fileAsk = null
        if (a == null) return@rememberLauncherForActivityResult
        val ctx = app.appContext
        if (uri == null || ctx == null) { a.reply(JSONObject()); return@rememberLauncherForActivityResult }
        app.scope.launch(kotlinx.coroutines.Dispatchers.IO) {
            val path = runCatching {
                val f = java.io.File(java.io.File(ctx.cacheDir, "picked").apply { mkdirs() }, app.displayName(ctx, uri))
                ctx.contentResolver.openInputStream(uri)?.use { i -> f.outputStream().use { i.copyTo(it) } }
                f.absolutePath
            }.getOrDefault("")
            a.reply(JSONObject().put("path", path))
        }
    }
    var pendingMany by remember { mutableStateOf<((List<android.net.Uri>) -> Unit)?>(null) }
    val many = rememberLauncherForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris ->
        val then = pendingMany; pendingMany = null
        if (uris.isNotEmpty() && then != null) then(uris)
    }
    // Plater::priv::is_preview_shown, for the automatic re-slice
    LaunchedEffect(app.tab, app.ready) { if (app.ready) app.call("slice.preview_shown", jobj("shown" to (app.tab == MainTab.Preview))) }
    // another tab leaves the Assembly view (its canvas is a panel of its own on the desktop)
    LaunchedEffect(app.tab) { if (app.tab != MainTab.Prepare && app.assembleView) app.setAssembleView(false) }
    // the Device tab's page and the print actions follow the printer preset (MainFrame::load_printer_url / update_side_button_style)
    LaunchedEffect(app.presets, app.ready) { if (app.ready) app.call("device.state") { if (it is org.json.JSONObject) app.device = it } }
    LaunchedEffect(Unit) {
        app.openDocuments = { then -> pendingMany = then; many.launch(arrayOf("*/*")) }
        app.openFolder = { then -> pendingFolder = then; folder.launch(null) }
        app.openImporter = { importer.launch(arrayOf("*/*")) }
        app.openDocument = { mimes, then -> pendingOpen = then; opener.launch(mimes) }
        app.saveDocument = { name, then -> pendingSave = then; creator.launch(name) }
    }
    Box(Modifier.fillMaxSize().background(desk().panel)) {
        Column(Modifier.fillMaxSize()) {
            TitleBar(app, compact, onFile = { fileMenu = true }, onMenu = { mainMenu = true })
            Box {
                FileMenu(app, fileMenu, { fileMenu = false })
                MainMenu(app, mainMenu, { mainMenu = false })
            }
            TabBar(app, compact,
                onSlice = { all -> app.slice(if (all) -1 else app.project.optInt("current_plate")); app.tab = MainTab.Preview },
                onExport = { what ->
                    when (what) {
                        "gcode" -> app.exportGcode()
                        "plate_3mf", "all_3mf" -> {
                            val base = app.project.optString("project").ifEmpty { "Untitled" }
                            app.export("project.export_sliced", if (what == "all_3mf") jobj("all" to true) else jobj("plate" to app.project.optInt("current_plate")),
                                "$base.gcode.3mf", "Sliced file exported")
                        }
                        "send" -> app.sendGcode()
                        else -> app.toast = tr("Not available")
                    }
                })
            Box(Modifier.weight(1f).fillMaxWidth().navigationBarsPadding()) {
                when (app.tab) {
                    MainTab.Prepare, MainTab.Preview -> PrepareScreen(app, activity, preview = app.tab == MainTab.Preview)
                    MainTab.Home -> HomeScreen(app)
                    MainTab.Project -> ProjectInfoScreen(app)
                    MainTab.Device -> DeviceScreen(app)
                    MainTab.Plugin -> app.pluginPage?.let { key(it) { PluginPageScreen(app, it) } }
                    else -> Box(Modifier.fillMaxSize(), Alignment.Center) { Text(tr(app.tab.label), style = deskText(16)) }
                }
            }
        }
        app.editing?.let { type -> SettingsEditor(app, type) { app.editing = null } }
        app.wizard?.let { page -> SetupWizard(app, page) { app.wizard = null } }
        org.orcaport.mobile.prepare.MenuDialogs(app)
        if (app.objectTable) org.orcaport.mobile.prepare.ObjectTableDialog(app) { app.objectTable = false }
        app.calibration?.let { k -> CalibrationDialog(app, k) { app.calibration = null } }
        if (app.exportBundle) ExportBundleDialog(app) { app.exportBundle = false }
        if (app.createPrinter) CreatePrinterDialog(app) { app.createPrinter = false }
        app.shortcutsDialog?.let { ctx -> ShortcutsDialog(app, ctx) { app.shortcutsDialog = null } }
        PublishDialog(app)
        if (app.preferences) PreferencesDialog(app) { app.preferences = false; app.loadPluginPages(); app.call("project.state") { p -> if (p is org.json.JSONObject) app.project = p } }
        if (app.printerHost != null) PrinterHostDialog(app) { app.printerHost = null }
        if (app.dailyTips) DailyTipsWindow(app) { app.dailyTips = false }
        if (app.about) AboutDialog(app) { app.about = false }
        if (app.networkTest) NetworkTestDialog(app) { app.networkTest = false }
        if (app.troubleshoot) TroubleshootDialog(app) { app.troubleshoot = false }
        app.benchmark?.let { b -> BenchmarkDialog(app, b) }
        app.updateAvailable?.let { u -> UpdateVersionDialog(app, u) { app.updateAvailable = null } }
        for (d in app.webDialogs.toList()) key(d) { WebDialog(app, d) { app.webDialogs.remove(d) } }
        // OAuthDialog / TokenAuthDialog
        app.authorizing?.let { a ->
            AlertDialog(onDismissRequest = { }, title = { Text(a.optString("title")) }, text = { Text(a.optString("text")) },
                confirmButton = { TextButton(onClick = { app.call("printhost.auth_cancel") }) { Text(tr("Cancel")) } })
        }
        app.sendDialog?.let { d -> PrintHostSendDialog(app, d) { app.sendDialog = null } }
        UploadNotifications(app, Modifier.align(Alignment.BottomEnd).padding(16.dp).navigationBarsPadding())
        PluginNotifications(app, Modifier.align(Alignment.BottomStart).padding(start = 16.dp, bottom = 72.dp).navigationBarsPadding())
        PluginWindows(app)
        PluginProgressDialogs(app)
        TextureImportProgress(app)
        asks.firstOrNull()?.takeIf { it.request.optString("kind") == "save_project" }?.let { a -> LaunchedEffect(a) { app.answerSaveProject(a) } }
            ?: asks.firstOrNull()?.takeIf { it.request.optString("kind") == "open_file" }?.let { a ->
                LaunchedEffect(a) { fileAsk = a; fileAskLauncher.launch(arrayOf("*/*")) }
            }
            ?: asks.firstOrNull()?.takeIf { it.request.optString("kind") == "mixed_filament" }?.let { a -> MixedFilamentAsk(app, a) }
            ?: asks.firstOrNull()?.takeIf { it.request.optString("kind") == "color_decompose" }?.let { a -> ColorDecomposeAsk(a) }
            ?: asks.firstOrNull()?.takeIf { it.request.optString("kind") == "texture_import" }?.let { a -> TextureImportAsk(app, a) }
            ?: AskHost(asks)
        app.toast?.let { msg ->
            LaunchedEffect(msg) { delay(3500); app.toast = null }
            Snackbar(Modifier.align(Alignment.BottomCenter).padding(16.dp).navigationBarsPadding()) { Text(msg) }
        }
    }
}

@Composable
private fun Startup(app: AppState) {
    Column(Modifier.fillMaxSize().background(FrameColors.topbar).padding(32.dp),
        verticalArrangement = Arrangement.Center, horizontalAlignment = Alignment.CenterHorizontally) {
        Text("Orca Slicer", style = MaterialTheme.typography.headlineMedium, color = OrcaColors.accent)
        Spacer(Modifier.height(24.dp))
        val err = app.startupError
        if (err != null) Text(err, color = MaterialTheme.colorScheme.error, textAlign = TextAlign.Center)
        else {
            CircularProgressIndicator(color = OrcaColors.accent)
            Spacer(Modifier.height(16.dp))
            Text(app.startup, color = FrameColors.textDim)
        }
    }
}

/** BBLTopbar's File menu (MainFrame::init_menubar_as_editor) */
@Composable
private fun FileMenu(app: AppState, open: Boolean, onDismiss: () -> Unit) {
    val p = app.project
    val hasObjects = (p.optJSONArray("objects")?.length() ?: 0) > 0
    val base = p.optString("project").ifEmpty { "Untitled" }
    DropdownMenu(open, onDismiss) {
        MenuItem(tr("New Project"), keys = app.keyOf("new_project")) { onDismiss(); app.newProject() }
        MenuItem(tr("Open Project"), keys = app.keyOf("open_project")) { onDismiss(); app.openProject() }
        HorizontalDivider()
        MenuItem(tr("Save Project"), keys = app.keyOf("save_project")) { onDismiss(); app.saveProject() }
        MenuItem(tr("Save Project as"), keys = app.keyOf("save_project_as")) { onDismiss(); app.saveProject(saveAs = true) }
        HorizontalDivider()
        // BBS: publish
        MenuItem(tr("Publish 3MF") + "...", keys = app.keyOf("publish_3mf"), enabled = hasObjects) { onDismiss(); app.publishProject() }
        HorizontalDivider()
        SubMenu(tr("Import")) { close ->
            MenuItem(tr("Import 3MF/STL/STEP/SVG/OBJ/AMF"), keys = app.keyOf("import_model")) { close(); onDismiss(); app.openImporter() }
            MenuItem(tr("Import ZIP Archive")) { close(); onDismiss(); app.openImporter() }
            MenuItem(tr("Import Configs")) { close(); onDismiss(); app.appContext?.let { app.importConfigs(it) } }
        }
        SubMenu(tr("Export")) { close ->
            MenuItem(tr("Export all objects as one STL"), enabled = hasObjects) {
                close(); onDismiss(); app.export("export.stl", JSONObject(), "$base.stl", "STL exported")
            }
            MenuItem(tr("Export all objects as STLs"), enabled = hasObjects) { close(); onDismiss(); app.exportToFolder("export.stl", JSONObject(), "STLs exported") }
            MenuItem(tr("Export all objects as one DRC"), enabled = hasObjects) {
                close(); onDismiss(); app.export("export.stl", jobj("format" to "drc"), "$base.drc", "DRC exported")
            }
            MenuItem(tr("Export all objects as DRCs"), enabled = hasObjects) { close(); onDismiss(); app.exportToFolder("export.stl", jobj("format" to "drc"), "DRCs exported") }
            MenuItem(tr("Export Generic 3MF"), enabled = hasObjects) {
                close(); onDismiss(); app.export("export.generic_3mf", JSONObject(), "$base.3mf", "3MF exported")
            }
            MenuItem(tr("Export plate sliced file"), keys = app.keyOf("export_sliced_file"), enabled = app.canExport) {
                close(); onDismiss(); app.export("project.export_sliced", jobj("plate" to p.optInt("current_plate")), "$base.gcode.3mf", "Sliced file exported")
            }
            MenuItem(tr("Export all plate sliced file"), enabled = app.canExport) {
                close(); onDismiss(); app.export("project.export_sliced", jobj("all" to true), "$base.gcode.3mf", "Sliced file exported")
            }
            MenuItem(tr("Export G-code"), enabled = app.canExport) { close(); onDismiss(); app.exportGcode() }
            MenuItem(tr("Export Preset Bundle")) { close(); onDismiss(); app.exportBundle = true }
            MenuItem(tr("Export toolpaths as OBJ"), enabled = app.canExport) {
                close(); onDismiss(); app.exportToFolder("preview.export_obj", JSONObject(), "Toolpaths exported", "$base.obj")
            }
        }
    }
}

/** the dropdown next to File: Edit, View, Preferences, Help (MainFrame's menus on the topbar) */
@Composable
private fun MainMenu(app: AppState, open: Boolean, onDismiss: () -> Unit) {
    val prepare = app.tab == MainTab.Prepare
    val prepareOrPreview = prepare || app.tab == MainTab.Preview
    val hasSel = (app.project.optJSONArray("selection")?.length() ?: 0) > 0
    val o = app.viewOptions
    @Composable
    fun check(label: String, key: String, enabled: Boolean, close: () -> Unit, keys: String = "") =
        DropdownMenuItem(enabled = enabled, onClick = { close(); onDismiss(); app.setViewOption(key, !o.optBoolean(key)) },
            trailingIcon = if (keys.isNotEmpty()) ({ Text(keys, style = deskText(13, color = desk().textDim)) }) else null,
            leadingIcon = { if (o.optBoolean(key)) Text("✓", style = deskText(14)) else Spacer(Modifier.width(14.dp)) },
            text = { Text(label, style = deskText(14, color = if (enabled) desk().text else desk().textDisabled)) })
    DropdownMenu(open, onDismiss) {
        // MainFrame::init_menubar_as_editor: editMenu
        SubMenu(tr("Edit")) { close ->
            MenuItem(tr("Undo"), keys = app.keyOf("undo"), enabled = app.project.optBoolean("can_undo")) { close(); onDismiss(); app.projectCall("project.undo") }
            MenuItem(tr("Redo"), keys = app.keyOf("redo"), enabled = app.project.optBoolean("can_redo")) { close(); onDismiss(); app.projectCall("project.redo") }
            HorizontalDivider()
            MenuItem(tr("Cut"), keys = app.keyOf("cut"), enabled = prepare && hasSel) { close(); onDismiss(); app.projectCall("clipboard.cut") }
            MenuItem(tr("Copy"), keys = app.keyOf("copy"), enabled = prepare && hasSel) { close(); onDismiss(); app.call("clipboard.copy") }
            MenuItem(tr("Paste"), keys = app.keyOf("paste"), enabled = prepare) { close(); onDismiss(); app.projectCall("clipboard.paste") }
            MenuItem(tr("Delete Selected"), keys = app.keyOf("delete_selected"), enabled = prepare && hasSel) { close(); onDismiss(); app.projectCall("selection.delete") }
            MenuItem(tr("Delete All"), keys = app.keyOf("delete_all"), enabled = prepare && app.hasObjects) { close(); onDismiss(); app.projectCall("objects.delete_all") }
            HorizontalDivider()
            MenuItem(tr("Clone Selected"), keys = app.keyOf("clone_selected"), enabled = prepare && hasSel) { close(); onDismiss(); org.orcaport.mobile.prepare.runMenu(app, "clone") }
            HorizontalDivider()
            MenuItem(tr("Duplicate Current Plate"), enabled = prepare) { close(); onDismiss(); app.projectCall("plate.duplicate") }
            HorizontalDivider()
            MenuItem(tr("Select All"), keys = app.keyOf("select_all"), enabled = prepare) { close(); onDismiss(); app.projectCall("selection.all") }
            MenuItem(tr("Deselect All"), enabled = prepare) { close(); onDismiss(); app.projectCall("selection.set", jobj("items" to org.json.JSONArray())) }
        }
        // viewMenu (add_common_view_menu_items + the camera, window and scene toggles)
        SubMenu(tr("View")) { close ->
            for ((n, l) in listOf("iso" to "Default View", "top" to "Top View", "bottom" to "Bottom View", "front" to "Front View", "rear" to "Rear View", "left" to "Left View", "right" to "Right View"))
                MenuItem(tr(l), keys = app.keyOf("view_" + if (n == "iso") "default" else n), enabled = prepareOrPreview) { close(); onDismiss(); org.orcaport.mobile.engine.Native.vpView(n); org.orcaport.mobile.engine.Native.vpZoomTo("plate"); app.redraw() }
            HorizontalDivider()
            val persp = o.optBoolean("use_perspective_camera")
            DropdownMenuItem(onClick = { close(); onDismiss(); app.setViewOption("use_perspective_camera", true) },
                leadingIcon = { RadioButton(persp, null, Modifier.size(14.dp)) }, text = { Text(tr("Use Perspective View"), style = deskText(14)) })
            DropdownMenuItem(onClick = { close(); onDismiss(); app.setViewOption("use_perspective_camera", false) },
                leadingIcon = { RadioButton(!persp, null, Modifier.size(14.dp)) }, text = { Text(tr("Use Orthogonal View"), style = deskText(14)) })
            check(tr("Auto Perspective"), "auto_perspective", prepareOrPreview, close)
            HorizontalDivider()
            check(tr("Show &G-code Window").replace("&", ""), "show_gcode_window", app.tab == MainTab.Preview, close, app.keyOf("toggle_gcode_window"))
            check(tr("Show 3D Navigator"), "show_3d_navigator", prepareOrPreview, close)
            check(tr("Show Gridlines"), "show_plate_gridlines", prepareOrPreview, close)
            check(tr("Show IDEX/IQEX Toolhead"), "show_imex_toolhead_boxes", app.tab == MainTab.Preview && o.optBoolean("is_imex"), close)
            HorizontalDivider()
            check(tr("Show Labels"), "labels", prepare, close, app.keyOf("show_labels"))
            check(tr("Show &Overhang").replace("&", ""), "overhang", prepare, close)
            check(tr("Show Selected Outline (beta)"), "show_outline", prepare, close)
        }
        MenuItem(tr("Preferences"), keys = app.keyOf("preferences")) { onDismiss(); app.preferences = true }
        HorizontalDivider()
        MenuItem(tr("Open Speed Dial"), keys = app.keyOf("speed_dial")) { onDismiss(); app.openSpeedDial() }
        HorizontalDivider()
        MenuItem(tr("Preset Bundle")) { onDismiss(); app.webDialogs.add("preset_bundle") }
        // "Pull and apply the latest presets from OrcaCloud"
        MenuItem(tr("Sync Presets"), enabled = app.cloud.optBoolean("logged_in") && !app.cloud.optBoolean("stealth")) {
            onDismiss(); app.call("cloud.sync") { if (it is JSONObject) app.cloud = it }
        }
        HorizontalDivider()
        MenuItem(tr("Plugins")) { onDismiss(); if ("plugins" !in app.webDialogs) app.webDialogs.add("plugins") }
        HorizontalDivider()
        // generate_help_menu
        SubMenu(tr("Help")) { close ->
            MenuItem(tr("Keyboard Shortcuts"), keys = app.keyOf("keyboard_shortcuts")) { close(); onDismiss(); app.shortcutsDialog = "global" }
            MenuItem(tr("Setup Wizard")) { close(); onDismiss(); app.wizard = "models" }
            HorizontalDivider()
            MenuItem(tr("Show Configuration Folder")) { close(); onDismiss(); app.showConfigFolder() }
            MenuItem(tr("Migrate Filament Presets")) { close(); onDismiss(); app.call("presets.migrate_legacy", jobj("by_user" to true)) }
            HorizontalDivider()
            MenuItem(tr("Troubleshoot Center")) { close(); onDismiss(); app.troubleshoot = true }
            MenuItem(tr("Open Network Test")) { close(); onDismiss(); app.networkTest = true }
            MenuItem(tr("Benchmark 3D Scene")) { close(); onDismiss(); app.startBenchmark() }
            HorizontalDivider()
            MenuItem(tr("Show Tip of the Day")) { close(); onDismiss(); app.dailyTips = true }
            MenuItem(tr("Check for Updates")) { close(); onDismiss(); app.checkUpdates(true) }
            MenuItem(tr("About %s").replace("%s", "Orca Slicer")) { close(); onDismiss(); app.about = true }
        }
    }
}

/** a menu entry that opens a submenu next to it (wxMenu::AppendSubMenu) */
@Composable
fun SubMenu(label: String, content: @Composable ColumnScope.(close: () -> Unit) -> Unit) {
    var open by remember { mutableStateOf(false) }
    Box {
        DropdownMenuItem(text = { Text(label, style = deskText(14)) }, onClick = { open = true },
            trailingIcon = { Text("›", style = deskText(16, color = desk().textDim)) })
        SubMenuPopup(open, { open = false }) { content { open = false } }
    }
}

@Composable
fun MenuItem(label: String, icon: androidx.compose.ui.graphics.vector.ImageVector? = null, enabled: Boolean = true, keys: String = "", onClick: () -> Unit) =
    DropdownMenuItem(text = { Text(label, style = deskText(14, color = if (enabled) desk().text else desk().textDisabled)) }, onClick = onClick, enabled = enabled,
        leadingIcon = icon?.let { { Icon(it, null) } },
        // the menu accelerator (shortcut_label): the shortcut's current key
        trailingIcon = if (keys.isNotEmpty()) ({ Text(keys, style = deskText(13, color = desk().textDim)) }) else null)
