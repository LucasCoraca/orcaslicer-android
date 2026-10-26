package org.orcaport.mobile.ui

import android.graphics.BitmapFactory
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.launch
import org.json.JSONArray
import org.json.JSONObject
import org.orcaport.mobile.AppState
import org.orcaport.mobile.engine.jobj
import org.orcaport.mobile.prepare.DeskDialog
import org.orcaport.mobile.prepare.PanelButton

/**
 * AuxiliaryPanel ("Edit Project Info"): Return, the tabs Basic Info (author, model name, license, description),
 * Pictures, Bill of Materials, Assembly Guide and Others; each folder lists its files with Add, and per file set as
 * cover (pictures), rename and delete.
 */
@OptIn(ExperimentalLayoutApi::class)
@Composable
fun ProjectInfoEditor(app: AppState, onClose: () -> Unit) {
    var st by remember { mutableStateOf(JSONObject()) }
    var tab by remember { mutableIntStateOf(0) }
    var renaming by remember { mutableStateOf<JSONObject?>(null) }
    LaunchedEffect(Unit) { app.call("projectinfo.aux") { if (it is JSONObject) st = it } }
    fun set(vararg kv: Pair<String, Any?>) = app.call("projectinfo.aux_set", jobj(*kv)) { r ->
        if (r is JSONObject) {
            st = r
            r.optString("notice").takeIf { n -> n.isNotEmpty() }?.let { n -> app.toast = n }
            r.optJSONArray("thumbnails")?.let { t -> writeCoverThumbnails(r.optString("source"), t) }
        }
    }
    val c = desk()
    val l = st.optJSONObject("labels") ?: JSONObject()
    val folders = st.optJSONArray("folders") ?: JSONArray()
    Column(Modifier.fillMaxSize().background(c.panel)) {
        Row(Modifier.fillMaxWidth().padding(8.dp), verticalAlignment = Alignment.CenterVertically) {
            PanelButton("‹ " + l.optString("return")) { onClose() }
            Spacer(Modifier.width(16.dp))
            for ((i, key) in listOf("basic", "pictures", "bom", "assembly", "others").withIndex()) {
                val sel = i == tab
                Text(l.optString(key), Modifier.clickable { tab = i }.padding(horizontal = 14.dp, vertical = 8.dp),
                    style = deskText(15, if (sel) FontWeight.Bold else FontWeight.Normal, if (sel) c.accent else c.text))
            }
        }
        HorizontalDivider(color = c.border)
        Column(Modifier.weight(1f).verticalScroll(rememberScrollState()).padding(horizontal = 50.dp, vertical = 30.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
            if (tab == 0) {
                @Composable fun row(label: String, content: @Composable () -> Unit) = Row(verticalAlignment = Alignment.Top) {
                    Text(label, Modifier.width(180.dp).padding(top = 8.dp), style = deskText(14)); content()
                }
                row(l.optString("author")) { DeskInput(st.optString("designer"), true, false, Modifier.width(450.dp)) { set("what" to "designer", "value" to it) } }
                row(l.optString("model_name")) { DeskInput(st.optString("model_name"), true, false, Modifier.width(450.dp)) { set("what" to "model_name", "value" to it) } }
                row(l.optString("license")) {
                    val licenses = st.optJSONArray("licenses")?.let { a -> List(a.length()) { a.optString(it) } } ?: emptyList()
                    DeskChoice(licenses, licenses.indexOf(st.optString("license")).coerceAtLeast(0), Modifier.width(450.dp)) { i -> set("what" to "license", "value" to licenses[i]) }
                }
                row(l.optString("description")) {
                    // the field owns the text while typing; it takes the engine's value only once it arrives
                    var text by remember(st.has("description")) { mutableStateOf(st.optString("description")) }
                    BasicTextField(text, { text = it; set("what" to "description", "value" to it) }, Modifier.width(450.dp).height(300.dp)
                        .clip(RoundedCornerShape(4.dp)).background(Color.White).border(1.dp, c.border, RoundedCornerShape(4.dp)).padding(8.dp),
                        textStyle = deskText(14, color = Color.Black), cursorBrush = SolidColor(c.accent))
                }
            } else {
                val f = folders.optJSONObject(tab - 1) ?: JSONObject()
                val files = f.optJSONArray("files") ?: JSONArray()
                FlowRow(horizontalArrangement = Arrangement.spacedBy(16.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
                    for (k in 0 until files.length()) {
                        val file = files.getJSONObject(k)
                        Column(Modifier.width(180.dp)) {
                            Box(Modifier.size(180.dp).clip(RoundedCornerShape(6.dp)).background(c.field)
                                .border(if (file.optBoolean("cover")) 2.dp else 1.dp, if (file.optBoolean("cover")) c.accent else c.border, RoundedCornerShape(6.dp)), Alignment.Center) {
                                val bmp = remember(file.optString("path")) { if (file.optBoolean("image")) runCatching { BitmapFactory.decodeFile(file.optString("path"))?.asImageBitmap() }.getOrNull() else null }
                                if (bmp != null) Image(bmp, null, Modifier.fillMaxSize(), contentScale = ContentScale.Crop)
                                else OrcaIcon("file", 64.dp)
                            }
                            Text(file.optString("name"), style = deskText(13), maxLines = 1)
                            Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                                if (tab == 1) Text(l.optString("cover"), Modifier.clickable { set("what" to "cover", "path" to file.optString("path")) }, style = deskText(12, color = c.accent))
                                Text(l.optString("rename"), Modifier.clickable { renaming = file }, style = deskText(12, color = c.accent))
                                Text(l.optString("delete"), Modifier.clickable { set("what" to "delete", "path" to file.optString("path")) }, style = deskText(12, color = OrcaColors.error))
                            }
                        }
                    }
                    // AuFile AddFileButton: the document picker, the files copied into the folder
                    Box(Modifier.size(180.dp).clip(RoundedCornerShape(6.dp)).border(1.dp, c.border, RoundedCornerShape(6.dp)).clickable {
                        val ctx = app.appContext ?: return@clickable
                        app.openDocuments { uris ->
                            app.scope.launch {
                                val paths = JSONArray(); val names = JSONArray()
                                for (u in uris) { paths.put(app.copyToCache(ctx, u)); names.put(app.displayName(ctx, u)) }
                                set("what" to "add", "folder" to f.optString("folder"), "paths" to paths, "names" to names)
                            }
                        }
                    }, Alignment.Center) { Text("+  " + l.optString("add"), style = deskText(14, color = c.accent)) }
                }
            }
        }
    }
    renaming?.let { file ->
        var name by remember { mutableStateOf(file.optString("stem")) }
        DeskDialog(l.optString("rename"), onDismiss = { renaming = null }, ok = { renaming = null; set("what" to "rename", "path" to file.optString("path"), "name" to name) }) {
            DeskInput(name, true, false, Modifier.fillMaxWidth()) { name = it }
        }
    }
}

/** generate_image (GERNERATE_IMAGE_RESIZE) of AuFile::on_set_cover: the cover scaled to fill each size, centred (cropped) */
private fun writeCoverThumbnails(source: String, sizes: JSONArray) {
    val img = android.graphics.BitmapFactory.decodeFile(source) ?: return
    for (i in 0 until sizes.length()) {
        val t = sizes.getJSONObject(i)
        val w = t.optInt("width"); val h = t.optInt("height")
        val factor = minOf(img.height / h.toFloat(), img.width / w.toFloat())
        val tw = (img.width / factor).toInt(); val th = (img.height / factor).toInt()
        val out = android.graphics.Bitmap.createBitmap(w, h, android.graphics.Bitmap.Config.ARGB_8888)
        val scaled = android.graphics.Bitmap.createScaledBitmap(img, tw, th, true)
        android.graphics.Canvas(out).drawBitmap(scaled, ((w - tw) / 2).toFloat(), ((h - th) / 2).toFloat(), null)
        runCatching { java.io.File(t.optString("path")).outputStream().use { out.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, it) } }
    }
}
