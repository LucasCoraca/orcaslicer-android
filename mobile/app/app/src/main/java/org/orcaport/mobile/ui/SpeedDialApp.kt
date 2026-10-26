package org.orcaport.mobile.ui

import org.json.JSONArray
import org.json.JSONObject
import org.orcaport.mobile.AppState
import org.orcaport.mobile.MainTab
import org.orcaport.mobile.engine.Native
import org.orcaport.mobile.engine.jobj
import org.orcaport.mobile.prepare.runMenu
import org.orcaport.mobile.settings.LayoutLine

/**
 * Speed Dial (GUI_App::open_speed_dial): the settings rows of the app's settings pages go to the engine first (they are
 * upstream's Sidebar settings index, the "setting" actions), then the page opens as a web dialog.
 */
fun AppState.openSpeedDial() {
    call("speeddial.settings", jobj("rows" to speedDialSettingRows())) { if ("speed_dial" !in webDialogs) webDialogs.add("speed_dial") }
}

/** Search::Option of every row of the print / filament / printer pages: key, type, label, page, group, mode, help */
fun AppState.speedDialSettingRows(): JSONArray {
    val out = JSONArray()
    val l = layout ?: return out
    for ((type, pages) in l.tabs) for (p in pages) for (g in p.groups) for (line in g.lines) {
        fun add(key: String, rowLabel: String?, multi: Boolean, wiki: String?) {
            val d = schema.optJSONObject(key) ?: l.customDefs.optJSONObject(key) ?: return
            val defLabel = d.optString("full_label").ifEmpty { d.optString("label") }.ifEmpty { key }
            val optLabel = d.optString("label").ifEmpty { defLabel }
            // resolve_setting_title: a single option's row label, else the label made of the row's and the option's
            val title = if (!multi && !rowLabel.isNullOrEmpty()) rowLabel else if (multi && !rowLabel.isNullOrEmpty()) "$rowLabel: $optLabel" else optLabel
            val r = jobj("key" to key, "type" to type, "title" to tr(title), "title_en" to title,
                "page" to tr(p.title), "page_en" to p.title, "group" to tr(g.title), "group_en" to g.title,
                "mode" to d.optString("mode", "simple"), "icon" to g.icon, "tooltip" to tr(d.optString("tooltip")))
            if (defLabel != title) r.put("full_label", tr(defLabel)).put("full_label_en", defLabel)
            if (!wiki.isNullOrEmpty()) r.put("wiki", wiki)
            out.put(r)
        }
        when (line) {
            is LayoutLine.Single -> add(line.option.key, line.option.overrides.optString("label").takeIf { it.isNotEmpty() }, false, line.wiki)
            is LayoutLine.Multi -> for (o in line.options) add(o.key, line.label, true, line.wiki)
            else -> {}
        }
    }
    return out
}

/** NativeCommands' runners: what the same entry of the app's menus, toolbars and dialogs does */
fun AppState.runSpeedDial(key: String, param: String) {
    val plate = project.optInt("current_plate")
    val hasSel = (project.optJSONArray("selection")?.length() ?: 0) > 0
    fun toPrepare() { if (tab != MainTab.Prepare) tab = MainTab.Prepare }
    fun objectOp(run: () -> Unit) { toPrepare(); if (!hasSel) toast = tr("Select an object first.") else run() }
    fun view(name: String) { Native.vpView(name); Native.vpZoomTo("plate"); redraw() }
    val base = project.optString("project").ifEmpty { "Untitled" }
    when {
        key == "slice_and_preview" -> { slice(); tab = MainTab.Preview }
        key == "go_to_layer" -> {
            tab = MainTab.Preview
            // go_to_layer: the top of the layer range at a percentage
            val n = preview.optJSONArray("layers")?.length() ?: 0
            if (n > 0) {
                val pct = (param.toDoubleOrNull() ?: 50.0).coerceIn(0.0, 100.0)
                val top = Math.round(pct / 100.0 * (n - 1)).toInt()
                Native.previewSet(jobj("layer_range" to JSONArray().put(0).put(top)).toString()); redraw()
            }
        }
        key == "go_to_tab" -> when (param) {
            "home" -> tab = MainTab.Home; "prepare" -> tab = MainTab.Prepare; "preview" -> tab = MainTab.Preview
            "monitor" -> tab = MainTab.Device; "project" -> tab = MainTab.Project
        }
        key == "load_project" -> openProject()
        key == "save_project" -> saveProject()
        key == "save_project_as" -> saveProject(saveAs = true)
        key == "open_preferences" -> preferences = true
        key.startsWith("mode_") -> call("config.set_mode", jobj("mode" to key.removePrefix("mode_"))) { modeVersion++ }
        key == "toggle_developer_mode" -> call("speeddial.toggle_developer_mode") { r -> modeVersion++; (r as? String)?.let { toast = it } }
        key == "export_gcode" -> exportGcode()
        key == "export_stl" -> export("export.stl", JSONObject(), "$base.stl", "STL exported")
        key == "export_3mf" -> export("export.generic_3mf", JSONObject(), "$base.3mf", "3MF exported")
        key == "export_sliced_file" -> export("project.export_sliced", jobj("plate" to plate), "$base.gcode.3mf", "Sliced file exported")
        key == "export_all_sliced_file" -> export("project.export_sliced", jobj("all" to true), "$base.gcode.3mf", "Sliced file exported")
        key.startsWith("calib_") -> {
            toPrepare()
            calibration = when (key) {
                "calib_temperature" -> "temp"; "calib_max_volumetric" -> "max_flowrate"; "calib_pressure_advance" -> "pa"
                "calib_flow_ratio" -> "flowrate"; "calib_retraction" -> "retraction"; "calib_cornering" -> "cornering"
                "calib_input_shaping_freq" -> "is_freq"; "calib_input_shaping_damp" -> "is_damp"; else -> "vfa"
            }
        }
        key == "view_default" -> view("iso")
        key.startsWith("view_") && key != "view_fit_bed" && key != "view_toggle_perspective" -> view(key.removePrefix("view_"))
        key == "view_fit_bed" -> { Native.vpZoomTo("plate"); redraw() }
        key == "view_toggle_perspective" -> setViewOption("use_perspective_camera", !viewOptions.optBoolean("use_perspective_camera"))
        key == "obj_delete" -> objectOp { projectCall("selection.delete") }
        key == "obj_delete_all" -> { toPrepare(); projectCall("objects.delete_all") }
        key.startsWith("obj_mirror_") -> objectOp { runMenu(this, "mirror:" + key.removePrefix("obj_mirror_")) }
        key == "obj_split_objects" || key == "obj_split_parts" -> objectOp {
            project.optJSONArray("selection")?.optJSONObject(0)?.let {
                projectCall("object.split", jobj("obj" to it.getInt("obj"), "to_parts" to (key == "obj_split_parts")))
            }
        }
        key == "obj_center" -> objectOp { runMenu(this, "center") }
        key == "obj_drop" -> objectOp { runMenu(this, "drop") }
        key == "obj_fit_volume" -> objectOp { runMenu(this, "scale_to_fit") }
        key == "obj_instances_up" -> objectOp { projectCall("objects.instances", jobj("delta" to 1)) }
        key == "obj_instances_down" -> objectOp { projectCall("objects.instances", jobj("delta" to -1)) }
        key == "obj_arrange" -> { toPrepare(); projectCall("arrange") }
        key == "obj_orient" -> { toPrepare(); projectCall("orient") }
        key == "add_primitive_text" -> { toPrepare(); runMenu(this, "add:invalid:text") }
        key == "add_primitive_svg" -> { toPrepare(); runMenu(this, "add:invalid:svg") }
        key.startsWith("add_primitive_") -> { toPrepare(); runMenu(this, "add:invalid:" + key.removePrefix("add_primitive_").replaceFirstChar { it.uppercase() }) }
        key.startsWith("add_handy_") -> { toPrepare(); runMenu(this, "handy:" + key.removePrefix("add_handy_")) }
        key == "plate_add" -> { toPrepare(); projectCall("plate.add") }
        key == "plate_duplicate" -> { toPrepare(); projectCall("plate.duplicate") }
        key == "plate_delete" -> { toPrepare(); projectCall("plate.delete", jobj("plate" to plate)) }
        key == "plate_rename" -> { toPrepare(); menuDialog = jobj("ui" to "plate_rename", "plate" to plate) }
        key == "plate_toggle_lock" -> {
            val locked = project.optJSONArray("plates")?.optJSONObject(plate)?.optBoolean("locked") ?: false
            projectCall("plate.set", jobj("plate" to plate, "locked" to !locked))
        }
        key == "plate_goto" -> { toPrepare(); projectCall("plate.select", jobj("plate" to (param.toIntOrNull() ?: 0))); Native.vpZoomTo("plate"); redraw() }
        key == "preset_bundle" -> webDialogs.add("preset_bundle")
        key == "import_file" || key == "import_zip_archive" -> openImporter()
        key == "import_configs" -> appContext?.let { importConfigs(it) }
        key == "export_stl_multi" -> exportToFolder("export.stl", JSONObject(), "STLs exported")
        key == "export_drc_single" -> export("export.stl", jobj("format" to "drc"), "$base.drc", "DRC exported")
        key == "export_drc_multi" -> exportToFolder("export.stl", jobj("format" to "drc"), "DRCs exported")
        key == "export_toolpaths_obj" -> exportToFolder("preview.export_obj", JSONObject(), "Toolpaths exported", "$base.obj")
        key == "export_config" -> exportBundle = true
        key == "help_setup_wizard" -> wizard = "models"
        key == "help_open_config_folder" -> showConfigFolder()
        key == "help_migrate_filament_presets" -> call("presets.migrate_legacy", jobj("by_user" to true))
        key == "help_troubleshoot" -> troubleshoot = true
        key == "help_network_test" -> networkTest = true
        key == "help_benchmark_3d_scene" -> startBenchmark()
        key == "help_tip_of_the_day" -> dailyTips = true
        key == "help_check_updates" -> checkUpdates(true)
        key == "help_about" -> about = true
        key == "open_wiki" -> openUrl("https://www.orcaslicer.com/wiki/")
        key == "open_youtube" -> openUrl("https://www.youtube.com/@OfficialOrcaSlicer/videos")
        key == "open_recent_project" -> openRecent(param)
        key == "open_plugins" -> if ("plugins" !in webDialogs) webDialogs.add("plugins")
        key == "refresh_plugins" -> call("plugins.refresh")
        key == "install_plugin" -> call("plugins.open_hub")
        key == "install_local_plugin" -> call("plugins.install_local")
        key == "sync_presets" -> call("cloud.sync") { if (it is JSONObject) cloud = it }
        key == "help_keyboard_shortcuts" -> shortcutsDialog = "global"
    }
}

fun AppState.openUrl(url: String) {
    runCatching {
        appContext?.startActivity(android.content.Intent(android.content.Intent.ACTION_VIEW, android.net.Uri.parse(url))
            .addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK))
    }
}
