package org.orcaport.mobile

import android.content.Context
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.setValue
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import org.json.JSONArray
import org.json.JSONObject
import org.orcaport.mobile.engine.Engine
import org.orcaport.mobile.engine.jobj
import org.orcaport.mobile.settings.Layout
import org.orcaport.mobile.ui.runSpeedDial
import org.orcaport.mobile.ui.tr

enum class MainTab(val label: String) { Home("Home"), Prepare("Prepare"), Preview("Preview"), Device("Device"), Project("Project"), Calibration("Calibration"), Plugin("Plugin") }

/** UI-side state shared by the screens; refreshed from engine events. */
/** GLCanvas3D::highlight_gizmo: the hints' gizmo ids and the app's gizmo names */
val TIP_GIZMOS = mapOf("Move" to "Move", "Rotate" to "Rotate", "Scale" to "Scale", "Flatten" to "Lay on face", "Cut" to "Cut",
    "FdmSupports" to "Support painting", "Seam" to "Seam painting", "MmuSegmentation" to "Color painting", "Text" to "Text shape",
    "MeshBoolean" to "Mesh boolean", "Measure" to "Measure", "Assembly" to "Assemble", "BrimEars" to "Brim ears", "FuzzySkin" to "Fuzzy skin painting",
    "TextureDisplacement" to "Texture displacement")

class AppState(ctx: Context) {
    val engine = Engine(ctx.applicationContext)
    val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    init { org.orcaport.mobile.ui.OrcaAssets.resources = engine.resourcesDir }

    var starting = false
    var startup by mutableStateOf("Starting…")
    var startupError by mutableStateOf<String?>(null)
    var ready by mutableStateOf(false)
    var firstRun by mutableStateOf(false)
    var version by mutableStateOf("")

    var tab by mutableStateOf(MainTab.Prepare)
    /** the settings editor being shown: "print", "filament", "printer" or null */
    var editing by mutableStateOf<String?>(null)
    /** the setup wizard's start page, null when closed */
    var wizard by mutableStateOf<String?>(null)
    var presets by mutableStateOf(JSONObject())

    /** print_config_def as JSON (option key -> definition), loaded once */
    var schema: JSONObject = JSONObject(); private set
    var layout: Layout? = null; private set

    var toast by mutableStateOf<String?>(null)

    /** Plater state: objects, plates, selection, undo (Plater.cpp state()) */
    var project by mutableStateOf(JSONObject())
    /** loading progress text (null: idle) */
    var progress by mutableStateOf<String?>(null)
    /** the pure command a progress' Cancel calls (a job run by the engine, e.g. the texture bake), its percentage */
    var progressCancel by mutableStateOf<String?>(null)
    /** where the 3D view's GL surface sits in the window (px): gizmo overlays positioned over it (the projection frame) */
    var glOrigin by mutableStateOf(androidx.compose.ui.geometry.Offset.Zero)
    var glSize by mutableStateOf(androidx.compose.ui.unit.IntSize(1, 1))
    var progressPercent by mutableIntStateOf(-1)
    /** slicing: per plate state ("progress", "done", "error" ...) and the last event */
    var slicing by mutableStateOf(false)
    var sliceEvent by mutableStateOf<JSONObject?>(null)
    val sliceResults = androidx.compose.runtime.mutableStateMapOf<Int, JSONObject>()
    val sliceWarnings = androidx.compose.runtime.mutableStateListOf<String>()
    /** NotificationType::BBLMixedFilamentBroken: a mixed filament's components are invalid or of different types */
    var mixedBroken by mutableStateOf<String?>(null)
    /** the texture import's clustering progress (texture_import_progress), null when not computing */
    var textureProgress by mutableStateOf<JSONObject?>(null)
    /** G-code preview legend / slider state (Preview.cpp info()) */
    var preview by mutableStateOf(JSONObject())
    /** MainFrame slice / print buttons */
    var sliceAll by mutableStateOf(false)
    var exportAction by mutableStateOf("Export G-code file")
    val hasObjects get() = (project.optJSONArray("objects")?.length() ?: 0) > 0
    val canExport get() = sliceResults[project.optInt("current_plate")]?.optString("state") == "done"
    /** the 3D view's overlay anchors (plate labels), refreshed after each frame */
    var overlay by mutableStateOf(JSONObject())

    /** the sidebar's Process section shows the object list ("Objects") instead of the global settings */
    var objectsMode by mutableStateOf(false)
    /** the per-object / per-part settings of the selection are shown (ObjectSettings, "Edit in Parameter Table") */
    var editingObject by mutableStateOf(false)
    /** the object list's "Layers" item ({"obj", "root": true}) or one height range ({"obj", "range": [min, max]}) */
    var layerItem by mutableStateOf<JSONObject?>(null)
    /** the parameter table dialog (Edit in Parameter Table) */
    var objectTable by mutableStateOf(false)
    /** the Preferences dialog */
    var preferences by mutableStateOf(false)
    /** the Export Preset Bundle dialog */
    var exportBundle by mutableStateOf(false)
    /** the calibration dialog that is open ("pa", "temp", ...) */
    var calibration by mutableStateOf<String?>(null)
    /** the Assembly view (Plater assemble_view) and its selection mode (Object / Part) */
    var assembleView by mutableStateOf(false)
    var assemblePartMode by mutableStateOf(false)
    fun setAssembleView(on: Boolean) = call("assemble.set", jobj("view" to on)) {
        gizmo = null; assembleView = on; redraw()
    }
    /** Help > Show Tip of the Day (DailyTipsWindow) */
    var dailyTips by mutableStateOf(false)
    /** the web dialogs open (webdialog.open: "preset_bundle", "export_preset_bundle"), the last one on top */
    val webDialogs = mutableStateListOf<String>()
    // the OrcaCloud account (cloud.state) and the home page, which the core sends scripts to (GUI_App::run_script)
    var cloud by mutableStateOf(org.json.JSONObject())
    var authorizing by mutableStateOf<org.json.JSONObject?>(null)
    var homeHandle: org.orcaport.mobile.ui.WebPageHandle? = null
    // the home page's OrcaCloud pane: open, and what the core asks it to load next (seed / load / signout)
    var cloudPane by mutableStateOf(false)
    /** GUI_App::open_orca_cloud(next): the page the OrcaCloud pane opens on */
    var cloudNext by mutableStateOf("")
    fun openOrcaCloud(next: String) { gizmo = null; cloudNext = next; tab = MainTab.Home; cloudPane = true }
    var cloudPaneEvent by mutableStateOf<org.json.JSONObject?>(null)
    /** Help > About */
    var about by mutableStateOf(false)
    /** a newer release of this app (UpdateVersionDialog; "update_available" from update.check) */
    var updateAvailable by mutableStateOf<JSONObject?>(null)
    /** Help > Check for Updates (GUI_App::check_new_version_sf), at startup without the "newest version" message */
    fun checkUpdates(byUser: Boolean) = call("update.check", jobj("version" to BuildConfig.VERSION_NAME, "by_user" to if (byUser) 1 else 0))

    /** the settings mode changed outside the settings pages (Speed Dial's "Mode: ..."): the pages reload */
    var modeVersion by mutableIntStateOf(0)
    /** Sidebar::jump_to_option: the option a Speed Dial setting action jumps to ({key, type, category}) */
    var jumpOption by mutableStateOf<JSONObject?>(null)

    /** Help > Benchmark 3D Scene: the running benchmark's status / its report ("benchmark" events) */
    var benchmark by mutableStateOf<JSONObject?>(null)
    fun startBenchmark() = call("benchmark.start") { redraw() }

    /** Help > Troubleshoot Center */
    var troubleshoot by mutableStateOf(false)

    /** Help > Show Configuration Folder (desktop_open_datadir_folder): the data folder in the system's file manager,
     *  served by DataFolderProvider */
    fun showConfigFolder() {
        val ctx = appContext ?: return
        val auth = ctx.packageName + ".data"
        val tries = listOf(
            android.content.Intent(android.content.Intent.ACTION_VIEW)
                .setDataAndType(android.provider.DocumentsContract.buildRootUri(auth, "orca"), android.provider.DocumentsContract.Root.MIME_TYPE_ITEM),
            android.content.Intent(android.content.Intent.ACTION_VIEW)
                .setDataAndType(android.provider.DocumentsContract.buildDocumentUri(auth, "root"), android.provider.DocumentsContract.Document.MIME_TYPE_DIR))
        for (i in tries) {
            // not this app (it opens content:// documents itself)
            val target = ctx.packageManager.queryIntentActivities(i, 0).firstOrNull { it.activityInfo.packageName != ctx.packageName } ?: continue
            i.setClassName(target.activityInfo.packageName, target.activityInfo.name)
                .addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK or android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION)
            if (runCatching { ctx.startActivity(i) }.isSuccess) return
        }
        toast = engine.dataDir.absolutePath
    }

    /** the Troubleshoot Center cleaned the system profiles: the app starts again (GUI_App restarts the executable) */
    private fun restartApp() {
        val ctx = appContext ?: return
        val i = ctx.packageManager.getLaunchIntentForPackage(ctx.packageName) ?: return
        ctx.startActivity(i.addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK or android.content.Intent.FLAG_ACTIVITY_CLEAR_TASK))
        Runtime.getRuntime().exit(0)
    }

    /** the engine asks for the project to be saved (Plater::save_project inside another command): the document is
     *  picked here and handed to the engine as an open file descriptor, which it writes the 3MF into */
    fun answerSaveProject(ask: org.orcaport.mobile.engine.Ask) {
        val ctx = appContext ?: return ask.reply(JSONObject())
        fun reply(uri: android.net.Uri?) {
            saveCanceled = null
            val pfd = uri?.let { runCatching { ctx.contentResolver.openFileDescriptor(it, "wt") }.getOrNull() }
            if (pfd == null) return ask.reply(JSONObject())
            projectUri = uri
            ask.reply(JSONObject().put("fd", pfd.detachFd()).put("name", displayName(ctx, uri)))
        }
        val target = projectUri
        if (target != null) reply(target)
        else {
            saveCanceled = { reply(null) }
            saveDocument(project.optString("project").ifEmpty { "Untitled" }.removeSuffix(".3mf") + ".3mf") { reply(it) }
        }
    }

    /** Help > Open Network Test: the result label of each test job and the log ("network_test" events) */
    var networkTest by mutableStateOf(false)
    /** keyboard shortcuts: KBShortcutsDialog open on a context's page; what each key matches per context; the menus' key labels */
    var shortcutsDialog by mutableStateOf<String?>(null)
    /** the plugin system's UI (PluginsUi.cpp): HTML windows, docked panels, progress dialogs and notifications by id;
     *  the scripts the engine runs in a window, a panel or a web dialog go out through [pageScripts] */
    val pluginWindows = mutableStateMapOf<Int, JSONObject>()
    val pluginDocks = mutableStateMapOf<Int, JSONObject>()
    val pluginProgress = mutableStateMapOf<Int, JSONObject>()
    val pluginNotifications = mutableStateListOf<JSONObject>()
    /** PluginPages: the plugins' pages as main tabs (plugins.pages), how many show before the overflow menu, the one shown */
    var pluginPages by mutableStateOf(JSONArray())
    var pluginPagesVisible by mutableIntStateOf(3)
    var pluginPage by mutableStateOf<String?>(null)
    fun loadPluginPages() = call("plugins.pages") { r ->
        if (r !is JSONObject) return@call
        pluginPages = r.optJSONArray("pages") ?: JSONArray()
        pluginPagesVisible = r.optInt("visible", 3)
        // a page that went away: back to Prepare
        if (tab == MainTab.Plugin && (0 until pluginPages.length()).none { pluginPages.getJSONObject(it).optString("id") == pluginPage }) tab = MainTab.Prepare
    }
    val pageScripts = kotlinx.coroutines.flow.MutableSharedFlow<JSONObject>(extraBufferCapacity = 256)
    /** a call the engine queues and does not answer, sent outside its command queue (a modal plugin window runs its own loop) */
    fun post(cmd: String, args: JSONObject) = scope.launch(Dispatchers.IO) { runCatching { org.orcaport.mobile.engine.Native.call(cmd, args.toString()) } }
    /** PublishSettingsDialog's state (publish.open / publish.action) while it is open */
    var publishDialog by mutableStateOf<JSONObject?>(null)
    var shortcutTable = JSONObject()
    var shortcutLabels by mutableStateOf(JSONObject())
    fun loadShortcuts() {
        call("shortcuts.table") { if (it is JSONObject) shortcutTable = it }
        call("shortcuts.labels") { if (it is JSONObject) shortcutLabels = it }
    }
    fun keyOf(shortcut: String): String = shortcutLabels.optString(shortcut)
    /** the Plater sidebar is folded away (the "<>" button, Shift+Tab) */
    var sidebarCollapsed by mutableStateOf<Boolean?>(null)
    /** a key for the preview's sliders (layer / moves steps, one layer mode, go to layer), handled by the layer slider */
    var sliderKey by mutableStateOf<JSONObject?>(null)
    /** bumped by the Search key (Sidebar::can_search) and the device page reload key (F5) */
    var settingsSearch by mutableIntStateOf(0)
    var deviceReload by mutableIntStateOf(0)
    val networkTestResults = androidx.compose.runtime.mutableStateMapOf<Int, String>()
    var networkTestLog by mutableStateOf("")
    /** the view toggles (view.options: canvas menu / View menu) */
    var viewOptions by mutableStateOf(JSONObject())
    fun setViewOption(key: String, on: Boolean) = call("view.set", jobj("key" to key, "on" to on)) { if (it is JSONObject) { viewOptions = it; redraw() } }
    /** Device tab: the print host's web UI, the print actions and the upload queue (device.state) */
    var device by mutableStateOf(JSONObject())
    /** the printer connection dialog (PhysicalPrinterDialog) and its network lookup */
    var printerHost by mutableStateOf<JSONObject?>(null)
    var printHostBrowse by mutableStateOf<JSONObject?>(null)
    /** PrintHostSendDialog's data (printhost.send_prepare) */
    var sendDialog by mutableStateOf<JSONObject?>(null)
    /** PrintHostQueueDialog's jobs; notifications closed by the user */
    var uploadJobs by mutableStateOf(org.json.JSONArray())
    val dismissedUploads = mutableStateListOf<Int>()
    /** a dialog asked for by a menu action (rename, number of copies, clone, confirmation, tip) */
    var menuDialog by mutableStateOf<org.json.JSONObject?>(null)

    /** the active gizmo (GLGizmosManager), by its toolbar name */
    var gizmo by mutableStateOf<String?>(null)
    /** bumped when a gizmo changed something only the engine knows (the cut plane): the panel reloads */
    var gizmoRevision by mutableIntStateOf(0)
    /** extra arguments of the next gizmo.set (the volume type of "Add part > Text"), and a counter that reopens the same gizmo */
    var gizmoArgs: org.json.JSONObject? = null
    var gizmoRequest by mutableIntStateOf(0)
    /** the cut gizmo is in connector editing: taps on the 3D view add / select connectors */
    var cutEditing by mutableStateOf(false)
    /** opens the system file picker for import (set by the main screen) */
    var openImporter: () -> Unit = {}

    /** opens the system "create document" picker (MainScreen) and hands back the chosen document */
    var saveDocument: (name: String, then: (android.net.Uri) -> Unit) -> Unit = { _, _ -> }
    /** called when that picker is closed without a document */
    var saveCanceled: (() -> Unit)? = null
    /** opens the system document picker for one file of these MIME types (MainScreen) */
    var openDocument: (mimes: Array<String>, then: (android.net.Uri) -> Unit) -> Unit = { _, _ -> }

    /** the documents behind the cached copies the engine loaded (Reload from disk copies them again) */
    val sourceUris = mutableMapOf<String, android.net.Uri>()
    /** MainFrame::open_recent_project: the document is copied again when the app still may read it (it may have changed) */
    fun openRecent(path: String) = scope.launch {
        val ctx = appContext ?: return@launch
        progress = "Loading…"
        try {
            val prefs = ctx.getSharedPreferences("recent_documents", Context.MODE_PRIVATE)
            val uri = prefs.getString(path, null)?.let { android.net.Uri.parse(it) }
            if (uri != null) kotlinx.coroutines.withContext(Dispatchers.IO) {
                runCatching { ctx.contentResolver.openInputStream(uri)?.use { i -> java.io.File(path).outputStream().use { o -> i.copyTo(o) } } }
            }
            project = engine.obj("project.load", jobj("paths" to JSONArray().put(path), "names" to JSONArray().put(java.io.File(path).name), "new_project" to true))
            if (project.optBoolean("canceled")) { progress = null; return@launch }
            projectUri = uri
            if (uri != null) sourceUris[path] = uri
            presets = engine.obj("presets.state")
            tab = MainTab.Prepare
        } catch (e: Exception) { toast = e.message }
        progress = null
        redraw()
    }

    /** MainFrame::load_config_file: presets from .json / .zip / .orca_printer / .orca_bundle / .orca_filament files */
    fun importConfigs(ctx: Context): Unit = openDocuments { uris ->
        scope.launch {
            try {
                val paths = JSONArray()
                for (u in uris) paths.put(copyToCache(ctx, u))
                val r = engine.obj("configs.import", jobj("paths" to paths))
                presets = engine.obj("presets.state")
                menuDialog = JSONObject().put("ui", "message").put("title", r.optString("caption")).put("text", r.optString("message"))
            } catch (e: Exception) { toast = e.message }
        }
    }

    /** opens the system picker for several documents (MainScreen) */
    var openDocuments: (then: (List<android.net.Uri>) -> Unit) -> Unit = { }

    /** ObjectList::load_subobject: parts / modifiers of the selected object from model files */
    fun loadPart(ctx: Context, type: String): Unit = openDocuments { uris ->
        scope.launch {
            try {
                val paths = JSONArray(); val names = JSONArray()
                for (u in uris) { val pth = copyToCache(ctx, u); sourceUris[pth] = u; paths.put(pth); names.put(displayName(ctx, u)) }
                project = engine.obj("object.load_part", jobj("paths" to paths, "names" to names, "type" to type)); redraw()
            } catch (e: Exception) { toast = e.message }
        }
    }

    /** opens the system folder picker (MainScreen) */
    var openFolder: (then: (android.net.Uri) -> Unit) -> Unit = { }

    /** Plater::reload_from_disk: fresh copies of the remembered documents, then the engine reloads; a missing source
     *  is asked for with the document picker (the desktop's "Please select a file" loop) */
    fun reloadFromDisk(ctx: Context, all: Boolean = false, located: JSONObject = JSONObject()): kotlinx.coroutines.Job = scope.launch {
        try {
            kotlinx.coroutines.withContext(Dispatchers.IO) {
                for ((path, u) in sourceUris) runCatching { ctx.contentResolver.openInputStream(u)?.use { i -> java.io.File(path).outputStream().use { o -> i.copyTo(o) } } }
            }
            val r = engine.obj(if (all) "object.reload_all" else "object.reload", jobj("located" to located))
            if (r.has("objects")) { project = r; redraw() }
            r.optString("notice").takeIf { it.isNotEmpty() }?.let { toast = it }
            val missing = r.optJSONArray("missing")?.optJSONObject(0) ?: return@launch
            toast = r.optString("title") + " " + missing.optString("name")
            openDocument(arrayOf("*/*")) { u ->
                scope.launch {
                    val path = copyToCache(ctx, u)
                    sourceUris[path] = u
                    reloadFromDisk(ctx, all, JSONObject(located.toString()).put(missing.optString("source"), path))
                }
            }
        } catch (e: Exception) { toast = e.message }
    }

    /** Plater::replace_with_stl */
    fun replaceVolume(ctx: Context): Unit = openDocument(arrayOf("*/*")) { u ->
        scope.launch {
            try {
                val path = copyToCache(ctx, u)
                sourceUris[path] = u
                project = engine.obj("object.replace", jobj("path" to path)); redraw()
            } catch (e: Exception) { toast = e.message }
        }
    }

    /** Plater::replace_all_with_stl: the files of the picked folder copied into the cache, then replaced by name */
    fun replaceAll(ctx: Context): Unit = openFolder { tree ->
        scope.launch {
            try {
                val dir = java.io.File(ctx.cacheDir, "replace").apply { deleteRecursively(); mkdirs() }
                var dirName = ""
                kotlinx.coroutines.withContext(Dispatchers.IO) {
                    val docId = android.provider.DocumentsContract.getTreeDocumentId(tree)
                    dirName = docId.substringAfterLast(':')
                    val children = android.provider.DocumentsContract.buildChildDocumentsUriUsingTree(tree, docId)
                    ctx.contentResolver.query(children, arrayOf(android.provider.DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                        android.provider.DocumentsContract.Document.COLUMN_DISPLAY_NAME, android.provider.DocumentsContract.Document.COLUMN_MIME_TYPE), null, null, null)?.use { c ->
                        while (c.moveToNext()) {
                            if (c.getString(2) == android.provider.DocumentsContract.Document.MIME_TYPE_DIR) continue
                            val doc = android.provider.DocumentsContract.buildDocumentUriUsingTree(tree, c.getString(0))
                            ctx.contentResolver.openInputStream(doc)?.use { i -> java.io.File(dir, c.getString(1).replace('/', '_')).outputStream().use { o -> i.copyTo(o) } }
                        }
                    }
                }
                val r = engine.obj("object.replace_all", jobj("dir" to dir.absolutePath, "dir_name" to dirName))
                project = r; redraw()
                menuDialog = JSONObject().put("ui", "message").put("title", r.optString("caption")).put("text", r.optString("message"))
            } catch (e: Exception) { toast = e.message }
        }
    }

    /** a content:// document copied into the cache, for the engine to read */
    suspend fun copyToCache(ctx: Context, u: android.net.Uri): String = kotlinx.coroutines.withContext(Dispatchers.IO) {
        val dir = java.io.File(ctx.cacheDir, "import").apply { mkdirs() }
        val f = java.io.File(dir, displayName(ctx, u))
        ctx.contentResolver.openInputStream(u)?.use { i -> f.outputStream().use { o -> i.copyTo(o) } }
        f.absolutePath
    }

    /** GLGizmoSVG::create_volume for a picked .svg (the part menus pass the volume type; a drop / import makes a part) */
    fun openSvg(ctx: Context, u: android.net.Uri, type: String = "part") = scope.launch {
        val path = copyToCache(ctx, u)
        gizmoArgs = jobj("path" to path, "type" to type)
        gizmoRequest++
        gizmo = "SVG"
    }
    /** the 3MF document this project was opened from or last saved to (Save Project writes back to it) */
    var projectUri: android.net.Uri? = null
    val appContext: Context? = ctx.applicationContext

    /** run an engine export into a temporary file, then copy it to the document */
    private fun exportInto(uri: android.net.Uri, cmd: String, args: JSONObject, name: String, done: String, then: () -> Unit = {}) = scope.launch {
        val ctx = appContext ?: return@launch
        progress = tr("Exporting") + "…"
        try {
            val tmp = java.io.File(java.io.File(ctx.cacheDir, "export").apply { mkdirs() }, name)
            engine.call(cmd, JSONObject(args.toString()).put("path", tmp.absolutePath).put("name", name))
            kotlinx.coroutines.withContext(Dispatchers.IO) {
                ctx.contentResolver.openOutputStream(uri, "wt")?.use { o -> tmp.inputStream().use { it.copyTo(o) } } ?: error("cannot write $name")
            }
            if (done.isNotEmpty()) toast = tr("Exported successfully") + "\n" + name
            then()
        } catch (e: Exception) { toast = e.message }
        progress = null
    }

    fun export(cmd: String, args: JSONObject, name: String, done: String) = saveDocument(name) { uri -> exportInto(uri, cmd, args, name, done) }

    /** an export that writes several files (one per object, an OBJ with its MTL): into a folder the user picks; the
     *  engine writes them into the cache ("dir", or "path" for a named main file) and they are copied over */
    fun exportToFolder(cmd: String, args: JSONObject, done: String, mainName: String? = null): Unit = openFolder { tree ->
        scope.launch {
            val ctx = appContext ?: return@launch
            progress = tr("Exporting") + "…"
            try {
                val dir = java.io.File(ctx.cacheDir, "export_multi").apply { deleteRecursively(); mkdirs() }
                val a = JSONObject(args.toString())
                if (mainName != null) a.put("path", java.io.File(dir, mainName).absolutePath) else a.put("dir", dir.absolutePath)
                engine.call(cmd, a)
                kotlinx.coroutines.withContext(Dispatchers.IO) {
                    val parent = android.provider.DocumentsContract.buildDocumentUriUsingTree(tree, android.provider.DocumentsContract.getTreeDocumentId(tree))
                    for (f in dir.listFiles() ?: emptyArray()) {
                        val mime = when (f.extension.lowercase()) { "stl" -> "model/stl"; "obj" -> "model/obj"; else -> "application/octet-stream" }
                        val doc = android.provider.DocumentsContract.createDocument(ctx.contentResolver, parent, mime, f.name) ?: continue
                        ctx.contentResolver.openOutputStream(doc, "wt")?.use { o -> f.inputStream().use { it.copyTo(o) } }
                    }
                }
                toast = tr("Exported successfully") + "\n" + android.provider.DocumentsContract.getTreeDocumentId(tree).substringAfter(':')
            } catch (e: Exception) { toast = e.message }
            progress = null
        }
    }

    /** Plater::export_gcode: the current plate's G-code, named like the desktop names it */
    fun exportGcode() = scope.launch {
        val plate = project.optInt("current_plate")
        val name = runCatching { engine.call("slice.gcode_name", jobj("plate" to plate)) as String }.getOrDefault("plate.gcode")
        export("slice.export_gcode", jobj("plate" to plate), name, "G-code exported")
    }

    /** Plater::save_project: to the opened 3MF, or Save Project as */
    fun saveProject(saveAs: Boolean = false) {
        val name = project.optString("project").ifEmpty { "Untitled" }.removeSuffix(".3mf") + ".3mf"
        val target = projectUri
        if (!saveAs && target != null) exportInto(target, "project.save", JSONObject(), name, "")
        else saveDocument(name) { uri -> exportInto(uri, "project.save", JSONObject(), name, "") { projectUri = uri } }
    }

    /** asks the 3D view for a new frame */
    var redraw: () -> Unit = {}

    fun start(ctx: Context) = scope.launch {
        try {
            val r = engine.start { startup = it }
            org.orcaport.mobile.ui.Tr.ready = true   // the catalog of the language is loaded
            version = r.optString("version")
            firstRun = r.optBoolean("first_run")
            if (firstRun) wizard = "welcome"
            r.optString("error").takeIf { it.isNotEmpty() }?.let { toast = it }

            schema = engine.obj("config.schema")
            val lists = engine.obj("layout.lists")
            layout = Layout.load(ctx, lists)
            presets = engine.obj("presets.state")
            project = engine.obj("project.state")
            viewOptions = engine.obj("view.options")
            ready = true
            loadShortcuts()
            loadPluginPages()
            launch { engine.host.events.collect { onEvent(it.kind, it.data) } }
            // what the engine queued while starting (the plugin system's start, GUI_App::CallAfter) runs now
            call("core.call_after")
            // EVT_RESTORE_PROJECT: an unsaved project of an earlier session (Android may have ended it in the background)
            if (!firstRun) runCatching { engine.obj("project.restore_check") }.getOrNull()?.takeIf { it.optBoolean("available") }?.let {
                menuDialog = JSONObject().put("ui", "restore").put("title", it.optString("title")).put("text", it.optString("message"))
            }
            // GUI_App::on_init_inner: filament presets a profile update moved away from their printers (not after the wizard)
            if (!firstRun) call("presets.migrate_legacy", jobj("by_user" to false))
            checkUpdates(false)
            call("cloud.state") { if (it is JSONObject) cloud = it }
        } catch (e: Exception) {
            startupError = e.message ?: e.toString()
        }
    }

    private fun onEvent(kind: String, data: JSONObject) {
        when (kind) {
            "presets" -> { presets = data; redraw() }
            "toast" -> toast = data.optString("text")
            "redraw" -> redraw()
            "gizmo_changed" -> gizmoRevision++   // a click in the 3D view changed the open gizmo's state   // a result computed off the engine thread is ready to draw (the texture displacement preview)
            "project" -> { project = data; redraw() }
            "progress" -> {
                progress = data.optString("text").takeIf { it.isNotEmpty() && data.optInt("percent") < 100 }
                progressCancel = data.optString("cancel").takeIf { it.isNotEmpty() && progress != null }
                progressPercent = if (data.has("cancel")) data.optInt("percent") else -1
            }
            "slice" -> {
                sliceEvent = data
                val plate = data.optInt("plate")
                when (data.optString("state")) {
                    "done", "error", "empty", "canceled" -> sliceResults[plate] = data
                    "warning" -> data.optString("text").takeIf { it.isNotEmpty() && it !in sliceWarnings }?.let { sliceWarnings += it }
                }
                if (data.has("warnings")) { val w = data.getJSONArray("warnings"); for (i in 0 until w.length()) w.optString(i).takeIf { it !in sliceWarnings }?.let { sliceWarnings += it } }
            }
            "slice_finished" -> { slicing = false; redraw() }
            "calib_prepare" -> tab = MainTab.Prepare
            "restart_app" -> restartApp()
            "call_after" -> call("core.call_after")
            "shortcuts" -> loadShortcuts()
            "plugin_window", "plugin_dock" -> {
                val map = if (kind == "plugin_window") pluginWindows else pluginDocks
                val id = data.optInt("id")
                when {
                    data.has("script") && !data.has("html") -> pageScripts.tryEmit(JSONObject().put("target", kind).put("id", id).put("script", data.getString("script")))
                    data.has("open") && !data.optBoolean("open") -> map.remove(id)
                    data.optBoolean("open") -> map[id] = data
                    data.has("shown") -> map[id]?.let { map[id] = JSONObject(it.toString()).put("hidden", !data.optBoolean("shown")) }
                }
            }
            "plugin_progress" -> { val id = data.optInt("id"); if (data.optBoolean("open")) pluginProgress[id] = data else pluginProgress.remove(id) }
            "plugin_notification" -> pluginNotifications.add(data)
            "texture_import_progress" -> textureProgress = if (data.optBoolean("done")) null else data
            "mixed_filament_broken" -> mixedBroken = if (data.optBoolean("show")) data.optString("text") else null
            "plugin_pages" -> loadPluginPages()
            "plugin_page_select" -> { pluginPage = data.optString("id"); tab = MainTab.Plugin }
            "plugin_page" -> pageScripts.tryEmit(JSONObject(data.toString()).put("target", "plugin_page"))
            "webdialog_run" -> pageScripts.tryEmit(JSONObject(data.toString()).put("target", "webdialog"))
            "home_script" -> homeHandle?.run(data.optString("script"))
            // ZUserLogin: the app opens the login page, the loopback server closes it after a browser sign-in
            // a web dialog the engine opens (GUI_App::open_terminal_dialog); an open one stays
            "open_web_dialog" -> data.optString("name").takeIf { it.isNotEmpty() && it !in webDialogs }?.let { webDialogs.add(it) }
            "cloud_login" -> if (data.optBoolean("closed")) webDialogs.remove("login") else if ("login" !in webDialogs) webDialogs.add("login")
            "cloud" -> cloud = data
            "cloud_pane" -> cloudPaneEvent = data
            "cloud_conflict" -> menuDialog = JSONObject(data.toString()).put("ui", "cloud_conflict")
            "speed_dial_run" -> runSpeedDial(data.optString("key"), data.optString("param"))
            "jump_to_option" -> {
                jumpOption = data
                when (data.optString("type")) { "print" -> tab = MainTab.Prepare; else -> editing = data.optString("type") }
            }
            "mode_changed" -> modeVersion++
            "benchmark" -> when {
                data.optBoolean("closed") -> benchmark = null
                // SceneBenchmarkDialog::start_slicing: reslice, the Preview tab
                data.optBoolean("slice") -> { tab = MainTab.Preview; slice() }
                else -> { val b = JSONObject((benchmark ?: JSONObject()).toString()); for (k in data.keys()) b.put(k, data.get(k)); benchmark = b }
            }
            "update_available" -> updateAvailable = data
            "info_dialog" -> menuDialog = JSONObject().put("ui", "message").put("title", data.optString("title")).put("text", data.optString("text"))
            "network_test" -> {
                if (data.optInt("job", -1) >= 0) networkTestResults[data.optInt("job")] = data.optString("text")
                networkTestLog += data.optString("log")
            }
            "physical_printer" -> if (printerHost != null) printerHost = data
            // OAuthDialog / TokenAuthDialog: "Authorizing..." with Cancel while the sign-in runs
            "authorizing" -> authorizing = if (data.optBoolean("open")) data else null
            "printhost_test" -> menuDialog = JSONObject().put("ui", "message").put("title", data.optString("title")).put("text", data.optString("message"))
            "printhost_browse" -> printHostBrowse?.let { b -> printHostBrowse = JSONObject(b.toString()).put("printers", data.optJSONArray("printers")).put("done", true) }
            "printhost_send" -> if (data.has("error")) menuDialog = JSONObject().put("ui", "message").put("title", "Error").put("text", data.optString("error")) else sendDialog = data
            "upload_queue" -> {
                uploadJobs = data.optJSONArray("jobs") ?: org.json.JSONArray()
                // PrintHostQueueDialog::on_error shows the error
                uploadJobs.optJSONObject(data.optInt("id"))?.takeIf { it.optString("state") == "error" }?.let {
                    menuDialog = JSONObject().put("ui", "message").put("title", "Error").put("text", it.optString("error"))
                }
            }
            "assemble" -> assemblePartMode = data.optBoolean("part_mode")
            "select_tab" -> when (data.optString("tab")) { "monitor" -> tab = MainTab.Device; "prepare" -> tab = MainTab.Prepare; "preview" -> tab = MainTab.Preview }
            // a hint's hypertext (HintDatabase callbacks): the page, the option, the dialog or the tool it names
            "tip_action" -> when {
                data.has("url") -> runCatching {
                    appContext?.startActivity(android.content.Intent(android.content.Intent.ACTION_VIEW, android.net.Uri.parse(data.optString("url")))
                        .addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK))
                }
                data.has("preferences") -> preferences = true
                data.has("settings") -> editing = when (data.optInt("type")) { 1 -> "filament"; 2, 3, 4 -> "printer"; else -> "print" }
                data.has("gizmo") -> { tab = MainTab.Prepare; gizmo = TIP_GIZMOS[data.optString("gizmo")] ?: gizmo }
                data.has("menubar") -> toast = data.optString("menubar").trimStart('&') + " > " + data.optString("item")
                else -> {}
            }
            "printer_url" -> call("device.state") { if (it is JSONObject) device = it }
            "open_url" -> runCatching {
                appContext?.startActivity(android.content.Intent(android.content.Intent.ACTION_VIEW, android.net.Uri.parse(data.optString("url")))
                    .addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK))
            }
            "slice_started" -> { sliceWarnings.clear(); sliceResults.remove(data.optInt("plate")); slicing = true }   // an automatic re-slice
            "preview" -> { preview = data; redraw() }
        }
    }

    /** files handed over by other apps (VIEW / SEND), imported once the engine is ready */
    val pendingOpen = mutableListOf<android.net.Uri>()

    fun openIntent(ctx: Context, intent: android.content.Intent) {
        val uris = mutableListOf<android.net.Uri>()
        intent.data?.let { uris += it }
        if (intent.action == android.content.Intent.ACTION_SEND)
            (intent.getParcelableExtra<android.net.Uri>(android.content.Intent.EXTRA_STREAM))?.let { uris += it }
        if (uris.isEmpty()) return
        pendingOpen += uris
    }

    fun call(cmd: String, args: JSONObject = JSONObject(), then: (Any?) -> Unit = {}) = scope.launch {
        try { then(engine.call(cmd, args)) } catch (e: Exception) { toast = e.message }
    }

    /** copy content:// documents to the cache (the engine reads files) and import them */
    /** Plater::new_project: kept when the user cancels saving the open one */
    fun newProject() = call("project.new") {
        if (it !is JSONObject || it.optBoolean("canceled")) return@call
        project = it; projectUri = null; tab = MainTab.Prepare; redraw()
    }

    /** MainFrame's Open Project (Plater::load_project): one project file, opened as a new project */
    fun openProject() { val ctx = appContext ?: return; openDocument(arrayOf("*/*")) { importUris(ctx, listOf(it), newProject = true) } }

    fun importUris(ctx: Context, uris: List<android.net.Uri>, newProject: Boolean = false) = scope.launch {
        // a ZIP: its model files are offered (Plater::preview_zip_archive / FileArchiveDialog)
        if (uris.size == 1 && displayName(ctx, uris[0]).endsWith(".zip", true)) {
            val path = copyToCache(ctx, uris[0])
            try { menuDialog = engine.obj("zip.list", jobj("path" to path)).put("ui", "zip").put("path", path) } catch (e: Exception) { toast = e.message }
            return@launch
        }
        // one .svg is embossed (Plater::emboss_svg), not loaded as a model
        if (uris.size == 1 && displayName(ctx, uris[0]).endsWith(".svg", true)) { openSvg(ctx, uris[0]); return@launch }
        progress = "Loading…"
        try {
            val paths = org.json.JSONArray(); val names = org.json.JSONArray()
            kotlinx.coroutines.withContext(Dispatchers.IO) {
                val dir = java.io.File(ctx.cacheDir, "import").apply { mkdirs() }
                for (u in uris) {
                    val name = displayName(ctx, u)
                    val f = java.io.File(dir, name)
                    ctx.contentResolver.openInputStream(u)?.use { i -> f.outputStream().use { o -> i.copyTo(o) } }
                    paths.put(f.absolutePath); names.put(name)
                    sourceUris[f.absolutePath] = u
                }
            }
            project = engine.obj("project.load", jobj("paths" to paths, "names" to names, "new_project" to newProject))
            if (project.optBoolean("canceled")) { progress = null; return@launch }
            if (project.optBoolean("show_preview")) { tab = MainTab.Preview; sliceResults.clear() }   // a G-code: the viewer mode
            // a single opened 3MF becomes the project document (Save Project writes back to it)
            if (uris.size == 1 && names.getString(0).endsWith(".3mf", true)) {
                projectUri = uris[0]
                // the recent projects list reopens it later: keep the permission and remember the document
                runCatching { ctx.contentResolver.takePersistableUriPermission(uris[0], android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION or android.content.Intent.FLAG_GRANT_WRITE_URI_PERMISSION) }
                ctx.getSharedPreferences("recent_documents", Context.MODE_PRIVATE).edit().putString(paths.getString(0), uris[0].toString()).apply()
            }
            presets = engine.obj("presets.state")
        } catch (e: Exception) { toast = e.message }
        progress = null
        redraw()
    }

    /** Sidebar's Connection button: PhysicalPrinterDialog */
    fun openPrinterHost() = call("printhost.dialog") { if (it is JSONObject) printerHost = it }

    /** BonjourDialog: Wi-Fi drops multicast unless a lock is held during the lookup */
    fun browsePrintHosts() {
        val wifi = appContext?.applicationContext?.getSystemService(Context.WIFI_SERVICE) as? android.net.wifi.WifiManager
        val lock = runCatching { wifi?.createMulticastLock("orca-bonjour")?.apply { setReferenceCounted(false); acquire() } }.getOrNull()
        call("printhost.browse") { r ->
            if (r is JSONObject) printHostBrowse = r
            scope.launch { kotlinx.coroutines.delay(15000); runCatching { lock?.release() } }
        }
    }

    /** the CA file's Browse: the certificate copied where the engine reads it */
    fun pickCaFile() = openDocument(arrayOf("application/x-x509-ca-cert", "application/x-pem-file", "*/*")) { u ->
        val ctx = appContext ?: return@openDocument
        scope.launch {
            val path = copyToCache(ctx, u)
            call("printhost.set", jobj("key" to "printhost_cafile", "value" to path)) { if (it is JSONObject) printerHost = it }
        }
    }

    /** MainFrame's "Print" for third-party printers (Plater::send_gcode_legacy) */
    fun sendGcode() = call("printhost.send_prepare", jobj("plate" to project.optInt("current_plate")))

    fun displayName(ctx: Context, u: android.net.Uri): String {
        var name: String? = null
        runCatching {
            ctx.contentResolver.query(u, arrayOf(android.provider.OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
                if (c.moveToFirst()) name = c.getString(0)
            }
        }
        return (name ?: u.lastPathSegment ?: "model.stl").replace('/', '_')
    }

    fun slice(plate: Int = project.optInt("current_plate")) = scope.launch {
        sliceWarnings.clear()
        sliceResults.remove(plate)
        slicing = true
        try {
            val r = engine.call("slice.start", jobj("plate" to plate))
            if (r != true) slicing = false
            if (r is JSONObject && r.optBoolean("canceled")) tab = MainTab.Prepare   // the user stopped at a pre-slice warning
        } catch (e: Exception) { slicing = false; toast = e.message }
    }

    fun projectCall(cmd: String, args: JSONObject = JSONObject()) = call(cmd, args) { if (it is JSONObject && it.has("objects")) { project = it; redraw() } }

    fun selectPreset(type: String, name: String, slot: Int? = null) {
        val a = jobj("type" to type, "name" to name)
        if (slot != null) a.put("slot", slot)
        call("presets.select", a) { if (it is JSONObject) presets = it }
    }

    fun presetList(type: String, all: Boolean = false, then: (JSONArray) -> Unit) =
        call("presets.list", jobj("type" to type, "all" to all)) { then(it as JSONArray) }
}
