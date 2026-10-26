package org.orcaport.mobile.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import org.orcaport.mobile.AppState
import org.orcaport.mobile.MainTab

/** The desktop's frame colours (BBLTopbar, MainFrame tab bar, SideButton). */
object FrameColors {
    val topbar = Color(0xFF262E30)          // BBLTopbar
    val tabbar = Color(0xFF3B4446)          // MainFrame tab panel / slice panel
    val tabSelected = Color(0xFF009688)     // selected tab
    val text = Color(0xFFFFFFFF)
    val textDim = Color(0xFF9DA6A8)
    val sliceNormal = Color(0xFF009688)
    val slicePressed = Color(0xFF00897B)
    val sliceDisabled = Color(0xFF5B6A6C)
}

/** BBLTopbar: File menu, the dropdown menu, save / undo / redo, calibration, and the project title. */
@Composable
fun TitleBar(app: AppState, compact: Boolean, onFile: () -> Unit, onMenu: () -> Unit) {
    val p = app.project
    Row(Modifier.fillMaxWidth().background(FrameColors.topbar).statusBarsPadding().height(40.dp).padding(horizontal = 6.dp),
        verticalAlignment = Alignment.CenterVertically) {
        TopbarTool("topbar_file", if (compact) null else tr("File"), onClick = onFile)
        TopbarTool("topbar_dropdown", null, onClick = onMenu)
        Box(Modifier.padding(horizontal = 6.dp).width(1.dp).height(20.dp).background(Color(0xFF5B6A6C)))
        TopbarTool("topbar_save", null) { app.saveProject() }
        TopbarTool(if (p.optBoolean("can_undo")) "topbar_undo" else "topbar_undo_inactive", null, enabled = p.optBoolean("can_undo")) { app.projectCall("project.undo") }
        TopbarTool(if (p.optBoolean("can_redo")) "topbar_redo" else "topbar_redo_inactive", null, enabled = p.optBoolean("can_redo")) { app.projectCall("project.redo") }
        Box {
            var calibMenu by remember { mutableStateOf(false) }
            TopbarTool("calib_sf", if (compact) null else tr("Calibration")) { calibMenu = true }
            CalibrationMenu(app, calibMenu) { calibMenu = false }
        }
        Box(Modifier.weight(1f), Alignment.Center) {
            val name = p.optString("project").ifEmpty { tr("Untitled") } + if (p.optBoolean("dirty")) "*" else ""
            Text(name, color = FrameColors.text, fontSize = 13.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
    }
}

@Composable
private fun TopbarTool(icon: String, label: String?, enabled: Boolean = true, onClick: () -> Unit) {
    Row(Modifier.heightIn(min = 40.dp).clip(RoundedCornerShape(4.dp)).clickable(enabled = enabled, onClick = onClick).padding(horizontal = 8.dp),
        verticalAlignment = Alignment.CenterVertically) {
        OrcaIcon(icon, 22.dp, darkVariant = false)
        if (label != null) Text(label, Modifier.padding(start = 6.dp), color = FrameColors.text, fontSize = 13.sp)
    }
}

private val TAB_ICONS = mapOf(MainTab.Home to "tab_home_active", MainTab.Prepare to "tab_3d_active", MainTab.Preview to "tab_preview_active",
    MainTab.Device to "tab_monitor_active", MainTab.Project to "tab_auxiliary_active", MainTab.Calibration to "tab_calibration_active")

/** MainFrame's tab bar: Home (icon only), Prepare, Preview, Device, Project, then the slice and print / export buttons. */
@Composable
fun TabBar(app: AppState, compact: Boolean, onSlice: (all: Boolean) -> Unit, onExport: (String) -> Unit) {
    Row(Modifier.fillMaxWidth().background(FrameColors.tabbar).height(48.dp), verticalAlignment = Alignment.CenterVertically) {
        for (tab in listOf(MainTab.Home, MainTab.Prepare, MainTab.Preview, MainTab.Device, MainTab.Project)) {
            val sel = app.tab == tab
            Row(Modifier.fillMaxHeight().background(if (sel) FrameColors.tabSelected else Color.Transparent)
                .clickable { app.tab = tab }.padding(horizontal = if (tab == MainTab.Home || compact) 14.dp else 28.dp),
                verticalAlignment = Alignment.CenterVertically) {
                OrcaIcon(TAB_ICONS[tab]!!, 22.dp, darkVariant = false)
                if (tab != MainTab.Home && !compact)
                    Text(tr(tab.label), Modifier.padding(start = 10.dp), color = FrameColors.text, fontSize = 14.sp,
                        fontWeight = if (sel) FontWeight.Bold else FontWeight.Normal)
            }
        }
        PluginPageTabs(app, compact)
        Spacer(Modifier.weight(1f))
        SplitButton(tr(if (app.sliceAll) "Slice all" else "Slice plate"), enabled = !app.slicing && app.hasObjects,
            menu = listOf(tr("Slice all") to { app.sliceAll = true; onSlice(true) }, tr("Slice plate") to { app.sliceAll = false; onSlice(false) })) { onSlice(app.sliceAll) }
        Spacer(Modifier.width(10.dp))
        // MainFrame::available_print_actions (device.state): Print (send to the print host), the sliced file, the G-code
        val actions = app.device.optJSONArray("actions")
        val menu = if (actions != null && actions.length() > 0) List(actions.length()) { i ->
            val a = actions.getJSONObject(i)
            a.optString("label") to when (a.optString("key")) { "send_gcode" -> "send"; "export_sliced_file" -> "plate_3mf"; else -> "gcode" }
        } else listOf(tr("Export G-code file") to "gcode")
        val current = menu.firstOrNull { it.first == app.exportAction } ?: menu.first()
        SplitButton(current.first, enabled = app.canExport,
            menu = menu.map { (label, what) -> label to { app.exportAction = label; onExport(what) } }) { onExport(current.second) }
        Spacer(Modifier.width(12.dp))
    }
}

/** SideButton + its dropdown half (MainFrame m_slice_btn / m_slice_option_btn). */
@Composable
fun SplitButton(label: String, enabled: Boolean, menu: List<Pair<String, () -> Unit>>, onClick: () -> Unit) {
    var open by remember { mutableStateOf(false) }
    val bg = if (enabled) FrameColors.sliceNormal else FrameColors.sliceDisabled
    Box {
        Row(Modifier.height(32.dp).clip(RoundedCornerShape(16.dp)).background(bg), verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.fillMaxHeight().clickable { open = true }.padding(horizontal = 8.dp), Alignment.Center) {
                OrcaIcon("sidebutton_dropdown", 14.dp, darkVariant = false)
            }
            Box(Modifier.fillMaxHeight().width(1.dp).background(Color(0x55FFFFFF)))
            Box(Modifier.fillMaxHeight().clickable(enabled = enabled, onClick = onClick).padding(horizontal = 18.dp), Alignment.Center) {
                Text(label, color = if (enabled) Color.White else Color(0xFFB8C2C4), fontSize = 14.sp)
            }
        }
        DropdownMenu(open, { open = false }) {
            for ((l, f) in menu) DropdownMenuItem(text = { Text(l) }, onClick = { open = false; f() })
        }
    }
}
