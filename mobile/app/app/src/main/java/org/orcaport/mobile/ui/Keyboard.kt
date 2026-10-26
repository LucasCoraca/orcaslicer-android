package org.orcaport.mobile.ui

import android.view.KeyEvent
import org.orcaport.mobile.AppState
import org.orcaport.mobile.MainTab
import org.orcaport.mobile.engine.Native
import org.orcaport.mobile.engine.jobj
import org.orcaport.mobile.prepare.PAINT_GIZMOS
import org.orcaport.mobile.prepare.runMenu

/**
 * A hardware keyboard: the key presses as upstream's KeyChord (wx key codes and wxMOD_* modifiers), matched in the
 * shortcut contexts the core's ShortcutRegistry lists (shortcuts.table), and run as MainFrame::handle_global_shortcut
 * and GLCanvas3D::handle_shortcut do.
 */
object Wx {
    const val MOD_ALT = 1; const val MOD_CONTROL = 2; const val MOD_SHIFT = 4
    const val BACK = 8; const val TAB = 9; const val RETURN = 13; const val ESCAPE = 27; const val SPACE = 32; const val DELETE = 127
    const val END = 312; const val HOME = 313; const val LEFT = 314; const val UP = 315; const val RIGHT = 316; const val DOWN = 317
    const val INSERT = 322; const val F1 = 340; const val PAGEUP = 366; const val PAGEDOWN = 367
}

/** KeyChord::from_event for an Android key press: letters upper-case, numpad folded, punctuation as the character it types */
fun keyChord(e: KeyEvent): Pair<Int, Int>? {
    var mods = 0
    if (e.isCtrlPressed) mods = mods or Wx.MOD_CONTROL
    if (e.isAltPressed) mods = mods or Wx.MOD_ALT
    if (e.isShiftPressed) mods = mods or Wx.MOD_SHIFT
    val k = e.keyCode
    val key = when (k) {
        in KeyEvent.KEYCODE_A..KeyEvent.KEYCODE_Z -> 'A'.code + (k - KeyEvent.KEYCODE_A)
        in KeyEvent.KEYCODE_0..KeyEvent.KEYCODE_9 -> '0'.code + (k - KeyEvent.KEYCODE_0)
        in KeyEvent.KEYCODE_NUMPAD_0..KeyEvent.KEYCODE_NUMPAD_9 -> '0'.code + (k - KeyEvent.KEYCODE_NUMPAD_0)
        in KeyEvent.KEYCODE_F1..KeyEvent.KEYCODE_F12 -> Wx.F1 + (k - KeyEvent.KEYCODE_F1)
        KeyEvent.KEYCODE_DEL -> Wx.BACK
        KeyEvent.KEYCODE_FORWARD_DEL -> Wx.DELETE
        KeyEvent.KEYCODE_TAB -> Wx.TAB
        KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_NUMPAD_ENTER -> Wx.RETURN
        KeyEvent.KEYCODE_ESCAPE -> Wx.ESCAPE
        KeyEvent.KEYCODE_SPACE -> Wx.SPACE
        KeyEvent.KEYCODE_MOVE_END -> Wx.END
        KeyEvent.KEYCODE_MOVE_HOME -> Wx.HOME
        KeyEvent.KEYCODE_DPAD_LEFT -> Wx.LEFT
        KeyEvent.KEYCODE_DPAD_UP -> Wx.UP
        KeyEvent.KEYCODE_DPAD_RIGHT -> Wx.RIGHT
        KeyEvent.KEYCODE_DPAD_DOWN -> Wx.DOWN
        KeyEvent.KEYCODE_INSERT -> Wx.INSERT
        KeyEvent.KEYCODE_PAGE_UP -> Wx.PAGEUP
        KeyEvent.KEYCODE_PAGE_DOWN -> Wx.PAGEDOWN
        KeyEvent.KEYCODE_NUMPAD_ADD -> '+'.code
        KeyEvent.KEYCODE_NUMPAD_SUBTRACT -> '-'.code
        KeyEvent.KEYCODE_NUMPAD_MULTIPLY -> '*'.code
        KeyEvent.KEYCODE_NUMPAD_DIVIDE -> '/'.code
        KeyEvent.KEYCODE_NUMPAD_DOT -> '.'.code
        else -> {
            // a symbol: the character the layout types (Shift already in it), as the wxEVT_CHAR route does
            val c = e.getUnicodeChar(e.metaState and (KeyEvent.META_SHIFT_MASK or KeyEvent.META_CAPS_LOCK_ON))
            if (c <= 32 || c == 127 || Character.isLetterOrDigit(c)) return null
            mods = mods and Wx.MOD_SHIFT.inv()
            c
        }
    }
    return key to mods
}

/** the contexts a key is looked up in, after Global: the focused view's (a painting gizmo's tools first) */
private fun AppState.keyContexts(): List<String> = when (tab) {
    MainTab.Prepare -> if (gizmo in PAINT_GIZMOS) listOf("painting", "plater") else listOf("plater")
    MainTab.Preview -> listOf("preview")
    else -> emptyList()
}

private fun AppState.lookup(context: String, chord: Pair<Int, Int>) =
    shortcutTable.optJSONObject(context)?.optJSONObject("${chord.first},${chord.second}")

/** Ctrl / Alt chords of the Global context run before a focused field sees the key (menu accelerators) */
fun AppState.onKeyBeforeFocus(e: KeyEvent): Boolean {
    if (e.action != KeyEvent.ACTION_DOWN || !ready) return false
    val chord = keyChord(e) ?: return false
    if (chord.second and (Wx.MOD_CONTROL or Wx.MOD_ALT) == 0) return false
    val m = lookup("global", chord) ?: return false
    if (e.repeatCount > 0 && !m.optBoolean("repeat")) return true
    return runShortcut(m.getString("key"), m.optInt("step"))
}

/** Global shortcuts, then the view's and its fixed keys (Esc, 1-9) */
fun AppState.onShortcutKey(e: KeyEvent): Boolean {
    if (e.action != KeyEvent.ACTION_DOWN || !ready) return false
    val chord = keyChord(e) ?: return false
    for (ctx in listOf("global") + keyContexts()) {
        val m = lookup(ctx, chord) ?: continue
        if (e.repeatCount > 0 && !m.optBoolean("repeat")) return true
        if (runShortcut(m.getString("key"), m.optInt("step"))) return true
    }
    if (tab != MainTab.Prepare || chord.second != 0) return false
    val hasSel = (project.optJSONArray("selection")?.length() ?: 0) > 0
    when (chord.first) {
        // GLCanvas3D::on_char: Esc closes the arrange / orient menus, else deselects (a gizmo handles it first)
        Wx.ESCAPE -> if (gizmo != null) gizmo = null else projectCall("selection.set", jobj("items" to org.json.JSONArray()))
        // 1-9, 0: the filament of the selected objects / parts (not while color painting)
        in '0'.code..'9'.code -> {
            if (!hasSel || gizmo == "Color painting") return false
            val n = if (chord.first == '0'.code) 10 else chord.first - '0'.code
            runMenu(this, "filament:$n")
        }
        else -> return false
    }
    return true
}

/** what each shortcut does (MainFrame::handle_global_shortcut, GLCanvas3D::handle_shortcut, the gizmos' and sliders' keys) */
fun AppState.runShortcut(key: String, step: Int): Boolean {
    val prepare = tab == MainTab.Prepare
    val hasSel = (project.optJSONArray("selection")?.length() ?: 0) > 0
    val painting = gizmo in PAINT_GIZMOS
    val plate = project.optInt("current_plate")
    fun view(name: String) { if (tab == MainTab.Prepare || tab == MainTab.Preview) { Native.vpView(name); redraw() } }
    fun openGizmo(name: String) {
        if (!prepare) return
        // GLGizmosManager::open_gizmo_by_shortcut: a gizmo that needs a selection stays closed without one
        if (!hasSel && name != "Measure" && name != "Text shape") return
        gizmo = if (gizmo == name) null else name
    }
    fun paintTool(tool: String) { call("paint.set", jobj("tool" to tool)) { gizmoRevision++; redraw() } }
    fun slider(what: String) { if (tab == MainTab.Preview) sliderKey = jobj("what" to what, "step" to step) }
    val base = project.optString("project").ifEmpty { "Untitled" }
    when (key) {
        // Global
        "new_project" -> newProject()
        "open_project" -> openProject()
        "save_project" -> saveProject()
        "save_project_as" -> saveProject(saveAs = true)
        "import_model" -> openImporter()
        "publish_3mf" -> publishProject()
        "slice_plate" -> { slice(); tab = MainTab.Preview }
        "export_sliced_file" -> if (canExport) export("project.export_sliced", jobj("plate" to plate), "$base.gcode.3mf", "Sliced file exported")
        "print_plate" -> if (canExport) sendGcode()
        "print_host_queue" -> tab = MainTab.Device
        "speed_dial" -> call("shortcuts.app_bool", jobj("key" to "enable_speed_dial")) { if (it == true) openSpeedDial() }
        "search" -> if (prepare || tab == MainTab.Preview) settingsSearch++
        "preferences" -> preferences = true
        "show_labels" -> if (prepare) setViewOption("labels", !viewOptions.optBoolean("labels"))
        "view_default" -> { view("iso"); Native.vpZoomTo("plate") }
        "view_top" -> view("top")
        "view_bottom" -> view("bottom")
        "view_front" -> view("front")
        "view_rear" -> view("rear")
        "view_left" -> view("left")
        "view_right" -> view("right")
        "view_plate" -> { Native.vpZoomTo("plate"); redraw() }
        // Plater / Preview canvases
        "select_all" -> if (prepare && !painting) projectCall("selection.all")
        "select_all_plates" -> if (prepare && !painting) projectCall("selection.all", jobj("all_plates" to true))
        "copy" -> if (prepare && !painting) call("clipboard.copy")
        "paste" -> if (prepare && !painting) projectCall("clipboard.paste")
        "cut" -> if (prepare && !painting) projectCall("clipboard.cut")
        "undo" -> if (prepare) projectCall("project.undo")
        "redo" -> if (prepare) projectCall("project.redo")
        "delete_selected" -> if (prepare && hasSel) projectCall("selection.delete")
        "delete_all" -> if (prepare) projectCall("objects.delete_all")
        "clone_selected" -> if (prepare && hasSel) runMenu(this, "clone")
        "add_instance" -> if (prepare && hasSel) projectCall("objects.instances", jobj("delta" to 1))
        "remove_instance" -> if (prepare && hasSel) projectCall("objects.instances", jobj("delta" to -1))
        "toggle_printable" -> if (prepare && hasSel) runMenu(this, "printable")
        "arrange" -> if (prepare) projectCall("arrange")
        "arrange_plate" -> if (prepare) projectCall("arrange", jobj("plate" to plate))
        "orient" -> if (prepare) projectCall("orient")
        "orient_plate" -> if (prepare) projectCall("orient", jobj("plate" to plate))
        "move_selection_left", "move_selection_right", "move_selection_up", "move_selection_down" -> if (prepare && hasSel && !assembleView) {
            val dir = when (key) { "move_selection_left" -> listOf(-1, 0); "move_selection_right" -> listOf(1, 0); "move_selection_up" -> listOf(0, 1); else -> listOf(0, -1) }
            projectCall("selection.key_move", jobj("dir" to org.json.JSONArray(dir), "slow" to (step and Wx.MOD_SHIFT != 0), "camera" to (step and Wx.MOD_CONTROL != 0)))
        }
        "rotate_selection_left" -> if (prepare && hasSel && !assembleView) projectCall("selection.key_rotate", jobj("angle" to Math.PI / 4))
        "rotate_selection_right" -> if (prepare && hasSel && !assembleView) projectCall("selection.key_rotate", jobj("angle" to -Math.PI / 4))
        "gizmo_move" -> openGizmo("Move")
        "gizmo_rotate" -> openGizmo("Rotate")
        "gizmo_scale" -> openGizmo("Scale")
        "gizmo_flatten" -> openGizmo("Lay on face")
        "gizmo_cut" -> openGizmo("Cut")
        "gizmo_mesh_boolean" -> openGizmo("Mesh boolean")
        "gizmo_fdm_supports" -> openGizmo("Support painting")
        "gizmo_seam" -> openGizmo("Seam painting")
        "gizmo_fuzzy_skin" -> openGizmo("Fuzzy skin painting")
        "gizmo_mmu_segmentation" -> openGizmo("Color painting")
        "gizmo_emboss" -> openGizmo("Text shape")
        "gizmo_measure" -> openGizmo("Measure")
        "gizmo_assembly" -> openGizmo("Assemble")
        "gizmo_brim_ears" -> openGizmo("Brim ears")
        "gizmo_displacement" -> return false
        "paint_tool_circle" -> paintTool("circle")
        "paint_tool_sphere" -> paintTool("sphere")
        "paint_tool_triangle" -> paintTool("pointer")
        "paint_tool_height_range" -> paintTool("height_range")
        "paint_tool_gap_fill" -> paintTool("gap_fill")
        "paint_tool_fill" -> paintTool(if (gizmo == "Support painting" || gizmo == "Color painting") "fill" else "smart_fill")
        "zoom_in" -> { Native.vpZoom(1 / 0.9, -1.0, -1.0); redraw() }   // Camera::update_zoom(1)
        "zoom_out" -> { Native.vpZoom(1 / 1.1, -1.0, -1.0); redraw() }
        "switch_view" -> tab = if (tab == MainTab.Preview) MainTab.Prepare else MainTab.Preview
        "collapse_sidebar" -> if (prepare) sidebarCollapsed = !(sidebarCollapsed ?: false)
        "show_wireframe" -> if (prepare) setViewOption("wireframe", !viewOptions.optBoolean("wireframe"))
        "reload_device_page" -> if (tab == MainTab.Device) deviceReload++
        "keyboard_shortcuts" -> shortcutsDialog = if (tab == MainTab.Preview) "preview" else if (painting) "painting" else "plater"
        "toggle_gcode_window" -> setViewOption("show_gcode_window", !viewOptions.optBoolean("show_gcode_window"))
        "toggle_one_layer_mode" -> slider("one_layer")
        "go_to_layer" -> slider("go_to_layer")
        "layer_slider_up" -> slider("up")
        "layer_slider_down" -> slider("down")
        "moves_slider_left" -> slider("left")
        "moves_slider_right" -> slider("right")
        "moves_slider_start" -> slider("start")
        "moves_slider_end" -> slider("end")
        else -> return false
    }
    return true
}
