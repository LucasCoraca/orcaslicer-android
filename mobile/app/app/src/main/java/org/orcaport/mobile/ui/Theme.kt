package org.orcaport.mobile.ui

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.unit.sp

/** OrcaSlicer's palette (GUI_App::update_label_colours, StateColor): teal accent, orange "modified" labels. */
object OrcaColors {
    val accent = Color(0xFF009688)
    val accentDark = Color(0xFF00675B)
    val modified = Color(0xFFF1754E)
    val warning = Color(0xFFFF6F00)
    val error = Color(0xFFE14747)
}

@Immutable
data class OrcaExtraColors(
    val labelSys: Color,
    val labelModified: Color,
    val panel: Color,
    val panelAlt: Color,
    val divider: Color,
    val canvas: Color,
)

val LocalOrcaColors = staticCompositionLocalOf {
    OrcaExtraColors(Color(0xFF363636), OrcaColors.modified, Color.White, Color(0xFFF1F1F1), Color(0xFFDBDBDB), Color(0xFFEDEDED))
}

@Composable
fun OrcaTheme(content: @Composable () -> Unit) {
    val dark = isSystemInDarkTheme()
    val scheme = if (dark) darkColorScheme(
        primary = OrcaColors.accent, onPrimary = Color.White, secondary = OrcaColors.accent,
        background = Color(0xFF2D2D31), surface = Color(0xFF2B2B2B), surfaceVariant = Color(0xFF36363B),
        onBackground = Color(0xFFEFEFF0), onSurface = Color(0xFFEFEFF0), onSurfaceVariant = Color(0xFFB2B3B5),
        outline = Color(0xFF4D4D54), primaryContainer = Color(0xFF00544C), onPrimaryContainer = Color(0xFFE0F2F1),
        error = OrcaColors.error,
    ) else lightColorScheme(
        primary = OrcaColors.accent, onPrimary = Color.White, secondary = OrcaColors.accent,
        background = Color(0xFFF8F8F8), surface = Color.White, surfaceVariant = Color(0xFFF1F1F1),
        onBackground = Color(0xFF262E30), onSurface = Color(0xFF262E30), onSurfaceVariant = Color(0xFF6B6A6A),
        outline = Color(0xFFDBDBDB), primaryContainer = Color(0xFFD9F0EE), onPrimaryContainer = Color(0xFF00403A),
        error = OrcaColors.error,
    )
    val extra = if (dark) OrcaExtraColors(Color(0xFFB2B3B5), OrcaColors.modified, Color(0xFF2B2B2B), Color(0xFF36363B), Color(0xFF4D4D54), Color(0xFF2D2D31))
    else OrcaExtraColors(Color(0xFF363636), OrcaColors.modified, Color.White, Color(0xFFF1F1F1), Color(0xFFDBDBDB), Color(0xFFEDEDED))
    val base = Typography()
    val typo = base.copy(
        bodyMedium = base.bodyMedium.copy(fontSize = 14.sp),
        bodySmall = base.bodySmall.copy(fontSize = 12.sp),
        labelMedium = base.labelMedium.copy(fontSize = 13.sp),
    )
    androidx.compose.runtime.CompositionLocalProvider(LocalOrcaColors provides extra) {
        MaterialTheme(colorScheme = scheme, typography = typo, content = content)
    }
}

val monoStyle = TextStyle(fontFamily = androidx.compose.ui.text.font.FontFamily.Monospace, fontSize = 13.sp)
