package org.orcaport.mobile.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.key.type
import androidx.compose.ui.input.key.onPreviewKeyEvent
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/** The desktop widget palette (StateColor / Label / ComboBox / TextInput / CheckBox, light and dark). */
data class DeskColors(
    val panel: Color, val header: Color, val border: Color, val borderFocus: Color, val text: Color, val textDim: Color,
    val textDisabled: Color, val field: Color, val fieldDisabled: Color, val selection: Color, val accent: Color, val modified: Color,
)

val DeskLight = DeskColors(Color.White, Color(0xFFF1F1F1), Color(0xFFDBDBDB), Color(0xFF009688), Color(0xFF262E30), Color(0xFF6B6A6A),
    Color(0xFFACACAC), Color.White, Color(0xFFF0F0F0), Color(0xFFBFE1DE), Color(0xFF009688), Color(0xFFF1754E))
val DeskDark = DeskColors(Color(0xFF2D2D31), Color(0xFF36363B), Color(0xFF4D4D54), Color(0xFF009688), Color(0xFFEFEFF0), Color(0xFFB2B3B5),
    Color(0xFF6B6B6F), Color(0xFF2D2D31), Color(0xFF36363B), Color(0xFF305C58), Color(0xFF009688), Color(0xFFF1754E))

@Composable
fun desk(): DeskColors = if (isSystemInDarkTheme()) DeskDark else DeskLight

/** Label::Body_14 etc. in Orca's font */
@Composable
fun deskText(size: Int = 14, weight: FontWeight = FontWeight.Normal, color: Color = desk().text) =
    TextStyle(fontFamily = OrcaAssets.font, fontSize = size.sp, fontWeight = weight, color = color)

/** Sidebar title bar: icon, title and the bordered tool buttons on the right (Plater Sidebar m_panel_*_title). */
@Composable
fun SectionTitle(icon: String, title: String, actions: @Composable RowScope.() -> Unit = {}) {
    val c = desk()
    Row(Modifier.fillMaxWidth().background(c.header).height(48.dp).padding(horizontal = 10.dp), verticalAlignment = Alignment.CenterVertically) {
        BoxedIcon(icon, null) {}
        Text(tr(title), Modifier.weight(1f).padding(start = 10.dp), style = deskText(15), maxLines = 1, overflow = TextOverflow.Ellipsis)
        actions()
    }
}

/** ScalableButton drawn in a rounded, bordered box (the sidebar's 32 px tool buttons). */
@Composable
fun BoxedIcon(icon: String, tooltip: String?, size: Dp = 34.dp, enabled: Boolean = true, onClick: () -> Unit) {
    val c = desk()
    Box(Modifier.padding(horizontal = 3.dp).size(size).clip(RoundedCornerShape(4.dp)).background(c.panel)
        .border(1.dp, c.border, RoundedCornerShape(4.dp)).clickable(enabled = enabled, onClick = onClick), Alignment.Center) {
        OrcaIcon(icon, size * 0.62f, alpha = if (enabled) 1f else 0.4f)
    }
}

/** ComboBox: the dropdown arrow on the left, as Orca's wx ComboBox draws it. */
@Composable
fun DeskCombo(
    text: String, modifier: Modifier = Modifier, enabled: Boolean = true, height: Dp = 36.dp, textColor: Color? = null,
    leading: (@Composable () -> Unit)? = null, onClick: () -> Unit,
) {
    val c = desk()
    Row(modifier.height(height).clip(RoundedCornerShape(4.dp)).background(if (enabled) c.field else c.fieldDisabled)
        .border(1.dp, c.border, RoundedCornerShape(4.dp)).clickable(enabled = enabled, onClick = onClick).padding(horizontal = 8.dp),
        verticalAlignment = Alignment.CenterVertically) {
        OrcaIcon("drop_down", 12.dp, alpha = if (enabled) 0.8f else 0.4f)
        Spacer(Modifier.width(8.dp))
        if (leading != null) { leading(); Spacer(Modifier.width(6.dp)) }
        Text(text, Modifier.weight(1f), style = deskText(14, color = textColor ?: if (enabled) c.text else c.textDisabled), maxLines = 1, overflow = TextOverflow.Ellipsis)
    }
}

/** A ComboBox with its choices in a popup list. */
@Composable
fun DeskChoice(labels: List<String>, selected: Int, modifier: Modifier = Modifier, enabled: Boolean = true, display: String? = null, onSelect: (Int) -> Unit) {
    var open by remember { mutableStateOf(false) }
    Box(modifier) {
        DeskCombo(display ?: labels.getOrElse(selected) { "" }, Modifier.fillMaxWidth(), enabled) { open = true }
        DropdownMenu(open, { open = false }) {
            labels.forEachIndexed { i, l ->
                DropdownMenuItem(text = { Text(l, style = deskText(14, color = if (i == selected) desk().accent else desk().text)) }, onClick = { open = false; onSelect(i) })
            }
        }
    }
}

/** ::CheckBox (check_on / check_off artwork) */
@Composable
fun DeskCheck(checked: Boolean, enabled: Boolean = true, onChange: (Boolean) -> Unit) {
    val icon = when { !enabled && checked -> "check_on_disabled"; !enabled -> "check_off_disabled"; checked -> "check_on"; else -> "check_off" }
    Box(Modifier.size(36.dp).clickable(enabled = enabled) { onChange(!checked) }, Alignment.Center) { OrcaIcon(icon, 20.dp, darkVariant = false) }
}

/** TextInput: a bordered field with the unit (sidetext) inside on the right; commits on Done / focus loss. */
@Composable
fun DeskInput(
    value: String, enabled: Boolean, numeric: Boolean, modifier: Modifier = Modifier, sidetext: String? = null,
    singleLine: Boolean = true, mono: Boolean = false, onCommit: (String) -> Unit,
) {
    val c = desk()
    var text by remember(value) { mutableStateOf(value) }
    var focused by remember { mutableStateOf(false) }
    val focus = LocalFocusManager.current
    Row(modifier.heightIn(min = 36.dp).clip(RoundedCornerShape(4.dp)).background(if (enabled) c.field else c.fieldDisabled)
        .border(1.dp, if (focused) c.borderFocus else c.border, RoundedCornerShape(4.dp)).padding(horizontal = 8.dp, vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically) {
        BasicTextField(text, { text = it }, Modifier.weight(1f).onFocusChanged {
            if (focused && !it.isFocused && text != value) onCommit(text)
            focused = it.isFocused
        }.onPreviewKeyEvent { e ->
            // a hardware Enter commits here and stops: otherwise its key up activates whatever takes the focus next
            if (singleLine && (e.key == Key.Enter || e.key == Key.NumPadEnter)) {
                if (e.type == KeyEventType.KeyUp) focus.clearFocus()   // on the key up: the up would otherwise reach the next focus
                true
            } else false
        }, enabled = enabled, singleLine = singleLine,
            textStyle = if (mono) monoStyle.copy(color = if (enabled) c.text else c.textDisabled) else deskText(14, color = if (enabled) c.text else c.textDisabled),
            cursorBrush = SolidColor(c.accent),
            keyboardOptions = KeyboardOptions(keyboardType = if (numeric) KeyboardType.Decimal else KeyboardType.Text, imeAction = if (singleLine) ImeAction.Done else ImeAction.Default),
            keyboardActions = KeyboardActions(onDone = { focus.clearFocus() }))
        if (!sidetext.isNullOrEmpty()) Text(sidetext, Modifier.padding(start = 4.dp), style = deskText(12, color = c.textDim), maxLines = 1)
    }
}

/** SwitchButton (Global | Objects in the process title bar) */
@Composable
fun DeskSwitch(left: String, right: String, rightSelected: Boolean, onChange: (Boolean) -> Unit) {
    val c = desk()
    Row(Modifier.height(30.dp).clip(RoundedCornerShape(15.dp)).background(c.selection.copy(alpha = 0.5f)).border(1.dp, c.border, RoundedCornerShape(15.dp)).padding(2.dp)) {
        for ((i, l) in listOf(left, right).withIndex()) {
            val sel = (i == 1) == rightSelected
            Box(Modifier.fillMaxHeight().clip(RoundedCornerShape(13.dp)).background(if (sel) c.accent else Color.Transparent)
                .clickable { onChange(i == 1) }.padding(horizontal = 12.dp), Alignment.Center) {
                Text(tr(l), style = deskText(12, color = if (sel) Color.White else c.textDim))
            }
        }
    }
}

/** the settings group header: icon, bold title and a rule to the right (OptionsGroup / StaticLine) */
@Composable
fun GroupTitle(icon: String, title: String) {
    val c = desk()
    Row(Modifier.fillMaxWidth().padding(start = 8.dp, end = 8.dp, top = 12.dp, bottom = 4.dp), verticalAlignment = Alignment.CenterVertically) {
        if (icon.isNotEmpty()) { OrcaIcon(icon, 22.dp); Spacer(Modifier.width(8.dp)) }
        Text(tr(title), style = deskText(14, FontWeight.Bold))
        Box(Modifier.padding(start = 10.dp).weight(1f).height(1.dp).background(c.border))
    }
}
