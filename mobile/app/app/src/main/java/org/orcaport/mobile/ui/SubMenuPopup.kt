package org.orcaport.mobile.ui

import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.IntrinsicSize
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.IntRect
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.LayoutDirection
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Popup
import androidx.compose.ui.window.PopupPositionProvider
import androidx.compose.ui.window.PopupProperties

/**
 * A submenu (wxMenu::AppendSubMenu) beside the entry that opened it, like a desktop menu: to its right when it fits,
 * else to the left of the menu, its first entry level with the parent entry and moved up when it would leave the screen.
 */
@Composable
fun SubMenuPopup(expanded: Boolean, onDismiss: () -> Unit, content: @Composable ColumnScope.() -> Unit) {
    if (!expanded) return
    val density = LocalDensity.current
    val view = LocalView.current
    val provider = remember(density, view) {
        object : PopupPositionProvider {
            override fun calculatePosition(bounds: IntRect, windowSize: IntSize, layoutDirection: LayoutDirection, popupContentSize: IntSize): IntOffset {
                // the entry's bounds come in its menu window's coordinates, the popup is placed on the screen
                val origin = IntArray(2).also { view.getLocationOnScreen(it) }
                val anchorBounds = bounds.translate(origin[0], origin[1])
                val pad = with(density) { 8.dp.roundToPx() }   // the menu's own top padding
                val x = if (anchorBounds.right + popupContentSize.width <= windowSize.width) anchorBounds.right
                        else (anchorBounds.left - popupContentSize.width).coerceAtLeast(0)
                val y = (anchorBounds.top - pad).coerceIn(0, (windowSize.height - popupContentSize.height).coerceAtLeast(0))
                return IntOffset(x, y)
            }
        }
    }
    Popup(popupPositionProvider = provider, onDismissRequest = onDismiss, properties = PopupProperties(focusable = true)) {
        Surface(shape = MaterialTheme.shapes.extraSmall, color = MaterialTheme.colorScheme.surfaceContainer, tonalElevation = 3.dp, shadowElevation = 3.dp) {
            Column(Modifier.width(IntrinsicSize.Max).verticalScroll(rememberScrollState()).padding(vertical = 8.dp), content = content)
        }
    }
}
