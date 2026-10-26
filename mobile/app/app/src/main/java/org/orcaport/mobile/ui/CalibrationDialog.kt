package org.orcaport.mobile.ui

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import org.json.JSONArray
import org.json.JSONObject
import org.orcaport.mobile.AppState
import org.orcaport.mobile.MainTab
import org.orcaport.mobile.engine.jobj
import org.orcaport.mobile.prepare.DeskDialog

/** BBLTopbar's calibration menu (MainFrame::init_menubar: the SoftFever calibrations) */
@Composable
fun CalibrationMenu(app: AppState, open: Boolean, onDismiss: () -> Unit) {
    // is_view3D_shown: the tests are created in the Prepare view
    val enabled = app.tab == MainTab.Prepare
    fun run(kind: String) { onDismiss(); app.calibration = kind }
    DropdownMenu(open, onDismiss) {
        MenuItem(tr("Temperature"), enabled = enabled) { run("temp") }
        MenuItem(tr("Max flowrate"), enabled = enabled) { run("max_flowrate") }
        MenuItem(tr("Pressure advance"), enabled = enabled) { run("pa") }
        MenuItem(tr("Flow ratio"), enabled = enabled) { run("flowrate") }
        MenuItem(tr("Retraction"), enabled = enabled) { run("retraction") }
        MenuItem(tr("Cornering"), enabled = enabled) { run("cornering") }
        SubMenu(tr("Input Shaping")) { close ->
            MenuItem(tr("Input Shaping Frequency"), enabled = enabled) { close(); run("is_freq") }
            MenuItem(tr("Input Shaping Damping/zeta factor"), enabled = enabled) { close(); run("is_damp") }
        }
        MenuItem(tr("VFA"), enabled = enabled) { run("vfa") }
        MenuItem(tr("Calibration Guide"), enabled = enabled) {
            onDismiss()
            runCatching {
                app.appContext?.startActivity(android.content.Intent(android.content.Intent.ACTION_VIEW,
                    android.net.Uri.parse("https://www.orcaslicer.com/wiki/calibration_guide")).addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK))
            }
        }
    }
}

/**
 * The calibration dialogs (PA_Calibration_Dlg, Temp_Calibration_Dlg, ...): the fields the engine describes, the
 * desktop's reactions to a choice (calib.update), OK runs the dialog's on_start (calib.run); the Wiki Guide button.
 */
@Composable
fun CalibrationDialog(app: AppState, kind: String, onClose: () -> Unit) {
    var form by remember(kind) { mutableStateOf<JSONObject?>(null) }
    val values = remember(kind) { mutableStateMapOf<String, Any>() }
    val enabled = remember(kind) { mutableStateMapOf<String, Boolean>() }
    fun update() = app.call("calib.update", jobj("kind" to kind, "values" to JSONObject(values.toMap()))) { r ->
        if (r !is JSONObject) return@call
        r.optJSONObject("values")?.let { v -> for (k in v.keys()) values[k] = v.get(k) }
        r.optJSONObject("enabled")?.let { e -> for (k in e.keys()) enabled[k] = e.getBoolean(k) }
    }
    LaunchedEffect(kind) {
        app.call("calib.dialog", jobj("kind" to kind)) { r ->
            if (r !is JSONObject) return@call
            val fields = r.optJSONArray("fields") ?: JSONArray()
            for (i in 0 until fields.length()) {
                val f = fields.getJSONObject(i)
                if (f.has("key")) values[f.getString("key")] = f.get("value")
            }
            form = r
            if (kind == "pa") update()
        }
    }
    val f = form ?: return
    val c = desk()
    DeskDialog(f.optString("title"), onDismiss = onClose, okLabel = f.optString("ok", "OK"), cancelLabel = null, ok = {
        app.call("calib.run", jobj("kind" to kind, "values" to JSONObject(values.toMap()))) { r ->
            if (r is JSONObject && r.optBoolean("closed")) {
                onClose()
                if (r.has("objects")) app.project = r
                app.call("presets.state") { p -> if (p is JSONObject) app.presets = p }
                app.redraw()
            }
        }
    }) {
        Column(Modifier.heightIn(max = 520.dp).verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            val fields = f.optJSONArray("fields") ?: JSONArray()
            for (i in 0 until fields.length()) {
                val fd = fields.getJSONObject(i)
                val key = fd.optString("key")
                when (fd.optString("kind")) {
                    "radio" -> {
                        Text(fd.optString("label"), style = deskText(14, androidx.compose.ui.text.font.FontWeight.Bold))
                        val opts = fd.optJSONArray("options") ?: JSONArray()
                        FlowRowCompat {
                            for (k in 0 until opts.length()) Row(Modifier.clickable {
                                values[key] = k
                                if ((kind == "pa" && (key == "method" || key == "extruder_type")) || (kind == "temp" && key == "filament_type")) update()
                            }, verticalAlignment = Alignment.CenterVertically) {
                                RadioButton((values[key] as? Int ?: 0) == k, null, colors = RadioButtonDefaults.colors(selectedColor = OrcaColors.accent))
                                Text(opts.optString(k), Modifier.padding(end = 12.dp), style = deskText(13))
                            }
                        }
                    }
                    "text" -> Row(verticalAlignment = Alignment.CenterVertically) {
                        Text(fd.optString("label"), Modifier.width(200.dp), style = deskText(13))
                        DeskInput(values[key]?.toString() ?: "", enabled[key] ?: true, true, Modifier.width(160.dp), sidetext = fd.optString("unit")) { s -> values[key] = s }
                    }
                    "check" -> Row(verticalAlignment = Alignment.CenterVertically) {
                        DeskCheck(values[key] as? Boolean ?: false, enabled[key] ?: true) { values[key] = it }
                        Text(fd.optString("label"), style = deskText(13))
                    }
                    "note" -> Text(fd.optString("label"), style = deskText(12, color = c.textDim))
                }
            }
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(f.optString("wiki_label"), Modifier.clickable {
                    runCatching {
                        app.appContext?.startActivity(android.content.Intent(android.content.Intent.ACTION_VIEW, android.net.Uri.parse(f.optString("wiki")))
                            .addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK))
                    }
                }, style = deskText(13, color = OrcaColors.accent))
            }
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun FlowRowCompat(content: @Composable () -> Unit) = FlowRow { content() }
