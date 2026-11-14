package cz.nowass.dispenser.ui.components

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.sizeIn
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.DateRange
import androidx.compose.material.icons.filled.Home
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import cz.nowass.dispenser.ui.theme.DispenserTheme

/** Interactive height of the dock (excluding margins and system insets). */
val NavigationDockHeight = 48.dp

/** Dock never grows wider than this, so it stays compact on large phones. */
private val NavigationDockMaxWidth = 360.dp

private val DockShape = RoundedCornerShape(24.dp)
private val IndicatorShape = RoundedCornerShape(16.dp)

/**
 * Floating, icon-only navigation dock.
 *
 * Layout: a transparent full-width wrapper consumes the system navigation-bar
 * inset (so the gesture handle area stays clear) plus small margins; inside, a
 * centered 48 dp rounded elevated surface holds the items. Scaffold reserves
 * the wrapper's measured height, so scrolling content never sits under the
 * dock and the inset is not applied twice.
 */
@Composable
fun NavigationDock(
    modifier: Modifier = Modifier,
    windowInsets: WindowInsets = WindowInsets.navigationBars.only(WindowInsetsSides.Bottom + WindowInsetsSides.Horizontal),
    content: @Composable RowScope.() -> Unit,
) {
    Box(
        modifier = modifier
            .fillMaxWidth()
            .windowInsetsPadding(windowInsets)
            .padding(start = 16.dp, end = 16.dp, top = 6.dp, bottom = 8.dp),
        contentAlignment = Alignment.Center,
    ) {
        Surface(
            modifier = Modifier.widthIn(max = NavigationDockMaxWidth).fillMaxWidth(),
            shape = DockShape,
            color = MaterialTheme.colorScheme.surfaceContainerHigh,
            border = BorderStroke(1.dp, MaterialTheme.colorScheme.outlineVariant),
            shadowElevation = 8.dp,
        ) {
            Row(
                modifier = Modifier
                    .height(NavigationDockHeight)
                    .padding(horizontal = 4.dp)
                    .selectableGroup(),
                verticalAlignment = Alignment.CenterVertically,
                content = content,
            )
        }
    }
}

/**
 * Icon-only destination. Touch target is the full item (48 dp tall, equal
 * share of the dock width — ≥ 48 dp even at 320 dp screens). [contentDescription]
 * is the Czech accessible name; Tab role and `selected` state are exposed.
 */
@Composable
fun RowScope.NavigationDockItem(
    selected: Boolean,
    onClick: () -> Unit,
    icon: ImageVector,
    contentDescription: String,
    modifier: Modifier = Modifier,
) {
    val colors = MaterialTheme.colorScheme
    Box(
        modifier = modifier
            .weight(1f)
            .fillMaxHeight()
            .sizeIn(minWidth = 48.dp, minHeight = 48.dp)
            .selectable(selected = selected, onClick = onClick, role = Role.Tab)
            .semantics { this.contentDescription = contentDescription },
        contentAlignment = Alignment.Center,
    ) {
        Box(
            modifier = Modifier
                .size(width = 52.dp, height = 36.dp)
                .clip(IndicatorShape)
                .background(if (selected) colors.primary.copy(alpha = 0.16f) else Color.Transparent),
            contentAlignment = Alignment.Center,
        ) {
            Icon(
                imageVector = icon,
                contentDescription = null, // name is on the item itself
                tint = if (selected) colors.primary else colors.onSurfaceVariant,
                modifier = Modifier.size(24.dp),
            )
        }
    }
}

// ---- Previews ----

@Composable
private fun DockPreviewContent(selectedIndex: Int) {
    val icons = listOf(Icons.Filled.Home, Icons.Filled.DateRange, DispenserIcons.History, Icons.Filled.Settings)
    val names = listOf("Přehled", "Kalendář", "Historie", "Nastavení")
    DispenserTheme {
        Box(Modifier.background(MaterialTheme.colorScheme.background)) {
            NavigationDock(windowInsets = WindowInsets(0, 0, 0, 0)) {
                icons.forEachIndexed { i, icon ->
                    NavigationDockItem(selected = i == selectedIndex, onClick = {}, icon = icon, contentDescription = names[i])
                }
            }
        }
    }
}

@Preview(name = "Dock 320 dp", widthDp = 320, showBackground = true, backgroundColor = 0xFF111820)
@Composable
private fun NavigationDock320Preview() = DockPreviewContent(selectedIndex = 0)

@Preview(name = "Dock 412 dp", widthDp = 412, showBackground = true, backgroundColor = 0xFF111820)
@Composable
private fun NavigationDock412Preview() = DockPreviewContent(selectedIndex = 1)
