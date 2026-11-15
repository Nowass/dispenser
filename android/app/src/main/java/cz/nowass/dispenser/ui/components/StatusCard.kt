package cz.nowass.dispenser.ui.components

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.ReadOnlyComposable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.painter.Painter
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.ui.unit.dp
import cz.nowass.dispenser.ui.theme.DispenserTheme
import cz.nowass.dispenser.ui.theme.Spacing

/** Presentation-level condition of a status metric. */
enum class StatusTone { Normal, Low, Blocked, Unknown }

/**
 * Accent color for a tone. [normal] lets each metric keep its identity color
 * (aqua for reservoir, lime for battery) while warnings stay consistent.
 */
@Composable
@ReadOnlyComposable
fun StatusTone.accent(normal: Color = MaterialTheme.colorScheme.primary): Color = when (this) {
    StatusTone.Normal -> normal
    StatusTone.Low -> DispenserTheme.extendedColors.warning
    StatusTone.Blocked -> MaterialTheme.colorScheme.error
    StatusTone.Unknown -> MaterialTheme.colorScheme.onSurfaceVariant
}

/**
 * Card for a single status metric: icon + title, qualifier tags (e.g. "odhad"),
 * and arbitrary content. Border tints amber/red for Low/Blocked tones.
 */
@OptIn(ExperimentalLayoutApi::class)
@Composable
fun StatusCard(
    title: String,
    icon: Painter,
    tone: StatusTone,
    modifier: Modifier = Modifier,
    accent: Color = tone.accent(),
    tags: List<String> = emptyList(),
    /** Optional: makes the whole card a single touch target (e.g. opens a detail screen). */
    onClick: (() -> Unit)? = null,
    onClickLabel: String? = null,
    content: @Composable ColumnScope.() -> Unit,
) {
    val border = when (tone) {
        StatusTone.Low, StatusTone.Blocked -> BorderStroke(1.dp, accent.copy(alpha = 0.6f))
        else -> null
    }
    Card(
        modifier = if (onClick != null) {
            modifier.clip(MaterialTheme.shapes.large).clickable(onClickLabel = onClickLabel, onClick = onClick)
        } else {
            modifier
        },
        shape = MaterialTheme.shapes.large,
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
        border = border,
    ) {
        Column(Modifier.padding(Spacing.m), verticalArrangement = Arrangement.spacedBy(Spacing.s)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(Spacing.s)) {
                IconBubble(icon, accent)
                Text(
                    title,
                    style = MaterialTheme.typography.titleMedium,
                    color = MaterialTheme.colorScheme.onSurface,
                )
            }
            if (tags.isNotEmpty()) {
                FlowRow(
                    horizontalArrangement = Arrangement.spacedBy(Spacing.xs),
                    verticalArrangement = Arrangement.spacedBy(Spacing.xs),
                ) { tags.forEach { StatusTag(it) } }
            }
            content()
        }
    }
}

/** Decorative tinted circular icon. The adjacent text carries the meaning. */
@Composable
fun IconBubble(icon: Painter, tint: Color, modifier: Modifier = Modifier) {
    Box(
        modifier = modifier
            .size(32.dp)
            .clip(CircleShape)
            .background(tint.copy(alpha = 0.16f)),
        contentAlignment = Alignment.Center,
    ) {
        Icon(icon, contentDescription = null, tint = tint, modifier = Modifier.size(18.dp))
    }
}
