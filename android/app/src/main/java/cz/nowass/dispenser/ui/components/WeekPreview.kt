package cz.nowass.dispenser.ui.components

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import cz.nowass.dispenser.ui.theme.Spacing

/** Presentation model for one day of [WeekPreview]; texts pre-formatted by caller. */
data class WeekDayItem(
    val weekdayLabel: String,
    val dayNumber: String,
    val isToday: Boolean,
    val doseCount: Int,
    val accessibilityLabel: String,
)

/** Maximum dots drawn per day; the a11y label carries the exact count. */
private const val MAX_DOTS = 3

/**
 * Seven-day strip with a dot per planned dose. Whole card is one click target
 * (>= 48 dp) that opens the calendar. Presentation only — no editing.
 */
@Composable
fun WeekPreview(
    title: String,
    actionLabel: String,
    summary: String,
    days: List<WeekDayItem>,
    onOpen: () -> Unit,
    modifier: Modifier = Modifier,
) {
    Card(
        onClick = onOpen,
        modifier = modifier.fillMaxWidth(),
        shape = MaterialTheme.shapes.large,
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
    ) {
        Column(Modifier.padding(Spacing.m), verticalArrangement = Arrangement.spacedBy(Spacing.m)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text(title, style = MaterialTheme.typography.titleMedium)
                    Text(
                        summary,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                Text(
                    actionLabel,
                    style = MaterialTheme.typography.labelLarge,
                    color = MaterialTheme.colorScheme.primary,
                )
            }
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(Spacing.xs)) {
                days.forEach { DayCell(it, Modifier.weight(1f)) }
            }
        }
    }
}

@Composable
private fun DayCell(day: WeekDayItem, modifier: Modifier) {
    val primary = MaterialTheme.colorScheme.primary
    val shape = MaterialTheme.shapes.medium
    Column(
        modifier = modifier
            .heightIn(min = 72.dp)
            .clip(shape)
            .then(
                if (day.isToday) Modifier
                    .background(MaterialTheme.colorScheme.primaryContainer.copy(alpha = 0.45f))
                    .border(BorderStroke(1.dp, primary.copy(alpha = 0.7f)), shape)
                else Modifier.background(MaterialTheme.colorScheme.surfaceContainerHigh)
            )
            .padding(vertical = Spacing.s)
            .clearAndSetSemantics { contentDescription = day.accessibilityLabel },
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(2.dp),
    ) {
        Text(
            day.weekdayLabel,
            style = MaterialTheme.typography.labelMedium,
            color = if (day.isToday) primary else MaterialTheme.colorScheme.onSurfaceVariant,
            textAlign = TextAlign.Center,
            maxLines = 1,
        )
        Text(
            day.dayNumber,
            style = MaterialTheme.typography.titleMedium,
            color = MaterialTheme.colorScheme.onSurface,
            maxLines = 1,
        )
        Spacer(Modifier.height(2.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(3.dp), modifier = Modifier.height(6.dp)) {
            repeat(day.doseCount.coerceAtMost(MAX_DOTS)) {
                Box(Modifier.size(6.dp).clip(CircleShape).background(primary))
            }
        }
    }
}
