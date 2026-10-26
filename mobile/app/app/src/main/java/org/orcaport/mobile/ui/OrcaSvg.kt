package org.orcaport.mobile.ui

import android.graphics.Bitmap
import android.graphics.Canvas
import android.util.LruCache
import androidx.compose.foundation.Image
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.size
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.font.Font
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.caverock.androidsvg.SVG
import java.io.File

/** Orca's own artwork (resources/images, unpacked by the engine), as the desktop draws it. */
object OrcaAssets {
    lateinit var resources: File
    private val cache = LruCache<String, ImageBitmap>(400)

    fun file(name: String): File = File(resources, "images/$name")

    /** the dark-theme variant ("<name>_dark.svg") when the desktop has one, like ScalableBitmap does */
    fun svgName(name: String, dark: Boolean): String {
        val base = name.removeSuffix(".svg")
        if (dark && !base.endsWith("_dark") && file("${base}_dark.svg").exists()) return "${base}_dark.svg"
        return "$base.svg"
    }

    fun bitmap(name: String, widthPx: Int, heightPx: Int = widthPx): ImageBitmap? {
        val key = "$name@$widthPx x $heightPx"
        cache.get(key)?.let { return it }
        val f = file(name)
        if (!f.exists()) return null
        val bmp = runCatching {
            if (name.endsWith(".svg")) {
                val svg = f.inputStream().use { SVG.getFromInputStream(it) }
                svg.setDocumentWidth(widthPx.toFloat()); svg.setDocumentHeight(heightPx.toFloat())
                val b = Bitmap.createBitmap(widthPx.coerceAtLeast(1), heightPx.coerceAtLeast(1), Bitmap.Config.ARGB_8888)
                svg.renderToCanvas(Canvas(b))
                b
            } else android.graphics.BitmapFactory.decodeFile(f.absolutePath)
        }.getOrNull() ?: return null
        val ib = bmp.asImageBitmap()
        cache.put(key, ib)
        return ib
    }

    val font: FontFamily by lazy {
        runCatching {
            FontFamily(
                Font(File(resources, "fonts/HarmonyOS_Sans_SC_Regular.ttf"), FontWeight.Normal),
                Font(File(resources, "fonts/HarmonyOS_Sans_SC_Bold.ttf"), FontWeight.Bold),
                Font(File(resources, "fonts/HarmonyOS_Sans_SC_Bold.ttf"), FontWeight.SemiBold),
            )
        }.getOrDefault(FontFamily.Default)
    }
}

/** An Orca icon by file name ("toolbar_move", "param_layer_height" ...), dark variant picked automatically. */
@Composable
fun OrcaIcon(name: String, size: Dp, modifier: Modifier = Modifier, height: Dp = size, darkVariant: Boolean = isSystemInDarkTheme(), alpha: Float = 1f,
             tint: androidx.compose.ui.graphics.Color? = null) {
    val d = LocalDensity.current
    val w = with(d) { size.roundToPx() }; val h = with(d) { height.roundToPx() }
    val file = remember(name, darkVariant) { OrcaAssets.svgName(name, darkVariant) }
    val bmp = remember(file, w, h) { OrcaAssets.bitmap(file, w, h) }
    if (bmp != null) Image(bmp, null, modifier.size(size, height), alpha = alpha,
        colorFilter = tint?.let { androidx.compose.ui.graphics.ColorFilter.tint(it) })
    else androidx.compose.foundation.layout.Spacer(modifier.size(size, height))
}

val Int.px: Dp get() = this.dp
