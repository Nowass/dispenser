package cz.nowass.dispenser.ui.components

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import cz.nowass.dispenser.ui.theme.Spacing

/**
 * One planned dose in a day list: time, schedule name (wraps for long Czech
 * names), amount. [pastLabel] marks a planned time that has passed — it does
 * not claim whether the dose was dispensed.
 */
@Composable
fun OccurrenceRow(
    timeText: String,
    name: String,
    amountText: String,
    pastLabel: String?,
    modifier: Modifier = Modifier,
    /** Opens the schedule definition; null = not interactive. */
    onClick: (() -> Unit)? = null,
    onClickLabel: String? = null,
) {
    val muted = pastLabel != null
    val colors = MaterialTheme.colorScheme
    Row(
        modifier = modifier
            .fillMaxWidth()
            .heightIn(min = 56.dp)
            .then(if (onClick != null) Modifier.clickable(onClickLabel = onClickLabel, onClick = onClick) else Modifier)
            .semantics(mergeDescendants = true) {}
            .padding(vertical = Spacing.s),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(Spacing.m),
    ) {
        Text(
            timeText,
            style = MaterialTheme.typography.titleMedium.copy(fontFeatureSettings = "tnum"),
            color = if (muted) colors.onSurfaceVariant else colors.primary,
            modifier = Modifier.widthIn(min = 52.dp),
        )
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(
                name,
                style = MaterialTheme.typography.bodyLarge,
                color = if (muted) colors.onSurfaceVariant else colors.onSurface,
            )
            if (pastLabel != null) StatusTag(pastLabel)
        }
        Text(
            amountText,
            style = MaterialTheme.typography.titleMedium,
            color = if (muted) colors.onSurfaceVariant else colors.onSurface,
        )
    }
}
