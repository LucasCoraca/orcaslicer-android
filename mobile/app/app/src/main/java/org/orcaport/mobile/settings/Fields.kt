package org.orcaport.mobile.settings

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ArrowDropDown
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/** A compact text field that commits on Done / focus loss (the desktop Field commits on kill focus / Enter). */
@Composable
fun CommitField(
    value: String,
    enabled: Boolean,
    numeric: Boolean,
    modifier: Modifier = Modifier,
    suffix: String? = null,
    singleLine: Boolean = true,
    textStyle: TextStyle = MaterialTheme.typography.bodyMedium,
    onCommit: (String) -> Unit,
) {
    var text by remember(value) { mutableStateOf(value) }
    var focused by remember { mutableStateOf(false) }
    val focus = LocalFocusManager.current
    val color = if (enabled) MaterialTheme.colorScheme.onSurface else MaterialTheme.colorScheme.onSurface.copy(alpha = 0.38f)
    val border = if (focused) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.outline
    Row(
        modifier
            .heightIn(min = 36.dp)
            .border(1.dp, border, RoundedCornerShape(4.dp))
            .background(MaterialTheme.colorScheme.surface, RoundedCornerShape(4.dp))
            .padding(horizontal = 8.dp, vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        BasicTextField(
            value = text,
            onValueChange = { text = it },
            enabled = enabled,
            singleLine = singleLine,
            textStyle = textStyle.copy(color = color),
            cursorBrush = SolidColor(MaterialTheme.colorScheme.primary),
            keyboardOptions = KeyboardOptions(
                keyboardType = if (numeric) KeyboardType.Decimal else KeyboardType.Text,
                imeAction = if (singleLine) ImeAction.Done else ImeAction.Default,
            ),
            keyboardActions = KeyboardActions(onDone = { focus.clearFocus() }),
            modifier = Modifier
                .weight(1f)
                .onFocusChanged {
                    if (focused && !it.isFocused && text != value) onCommit(text)
                    focused = it.isFocused
                },
        )
        if (!suffix.isNullOrEmpty())
            Text(suffix, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(start = 4.dp), maxLines = 1)
    }
}

/** A drop-down selector (wx ComboBox / Choice). */
@Composable
fun Dropdown(
    labels: List<String>,
    selected: Int,
    enabled: Boolean,
    modifier: Modifier = Modifier,
    display: String? = null,
    onSelect: (Int) -> Unit,
) {
    var open by remember { mutableStateOf(false) }
    Box(modifier) {
        Row(
            Modifier
                .fillMaxWidth()
                .heightIn(min = 36.dp)
                .border(1.dp, MaterialTheme.colorScheme.outline, RoundedCornerShape(4.dp))
                .background(MaterialTheme.colorScheme.surface, RoundedCornerShape(4.dp))
                .clickable(enabled = enabled) { open = true }
                .padding(start = 8.dp, end = 2.dp, top = 6.dp, bottom = 6.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(display ?: labels.getOrElse(selected) { "" }, Modifier.weight(1f), maxLines = 1, overflow = TextOverflow.Ellipsis,
                style = MaterialTheme.typography.bodyMedium,
                color = if (enabled) MaterialTheme.colorScheme.onSurface else MaterialTheme.colorScheme.onSurface.copy(alpha = 0.38f))
            Icon(Icons.Default.ArrowDropDown, null, tint = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        DropdownMenu(open, onDismissRequest = { open = false }) {
            labels.forEachIndexed { i, l ->
                DropdownMenuItem(
                    text = { Text(l, fontSize = 14.sp, color = if (i == selected) MaterialTheme.colorScheme.primary else Color.Unspecified) },
                    onClick = { open = false; onSelect(i) })
            }
        }
    }
}

fun parseColor(s: String): Color? {
    val h = s.trim().removePrefix("#")
    return when (h.length) {
        6 -> h.toLongOrNull(16)?.let { Color(0xFF000000 or it) }
        8 -> h.toLongOrNull(16)?.let { Color(((it and 0xFF) shl 24) or (it shr 8)) }   // #RRGGBBAA
        else -> null
    }
}

fun colorHex(c: Color): String {
    val r = (c.red * 255).toInt(); val g = (c.green * 255).toInt(); val b = (c.blue * 255).toInt()
    return "#%02X%02X%02X".format(r, g, b)
}
