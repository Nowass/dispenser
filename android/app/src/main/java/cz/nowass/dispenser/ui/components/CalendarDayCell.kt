package cz.nowass.dispenser.ui.components

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.sizeIn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.onClick
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/** Minimum cell height; together with the 7-column width it keeps touch targets near 48 dp. */
val CalendarCellMinHeight = 48.dp

/**
 * One selectable calendar day.
 *
 * Visual language (shared with the legend):
 * - one dose: a single dot;
 * - multiple doses: tinted cell + numeric count badge;
 * - today: primary outline; selected: filled primary.
 */
@Composable
fun CalendarDayCell(
    dayNumber: String,
    doseCount: Int,
    isToday: Boolean,
    isSelected: Boolean,
    inVisiblePeriod: Boolean,
    accessibilityLabel: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    weekdayLabel: String? = null,
    secondaryText: String? = null,
) {
    val colors = MaterialTheme.colorScheme
    val shape = MaterialTheme.shapes.medium
    val multiple = doseCount > 1
    val background = when {
        isSelected -> colors.primary
        multiple -> colors.primaryContainer.copy(alpha = 0.55f)
        else -> Color.Transparent
    }
    val contentColor = when {
        isSelected -> colors.onPrimary
        !inVisiblePeriod -> colors.onSurfaceVariant.copy(alpha = 0.5f)
        else -> colors.onSurface
    }
    val accent = if (isSelected) colors.onPrimary else colors.primary

    Column(
        modifier = modifier
            .heightIn(min = CalendarCellMinHeight)
            .clip(shape)
            .background(background)
            .then(if (isToday && !isSelected) Modifier.border(1.5.dp, colors.primary, shape) else Modifier)
            .clickable(onClick = onClick)
            .clearAndSetSemantics {
                contentDescription = accessibilityLabel
                role = Role.Button
                selected = isSelected
                onClick { onClick(); true }
            }
            .padding(vertical = 6.dp, horizontal = 2.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(2.dp, Alignment.CenterVertically),
    ) {
        if (weekdayLabel != null) {
            Text(
                weekdayLabel,
                style = MaterialTheme.typography.labelMedium,
                color = if (isSelected) colors.onPrimary else colors.onSurfaceVariant,
                maxLines = 1,
            )
        }
        Text(
            dayNumber,
            style = MaterialTheme.typography.titleSmall,
            color = contentColor,
            textAlign = TextAlign.Center,
            maxLines = 1,
        )
        DoseIndicator(doseCount, accent, if (isSelected) colors.primary else colors.onPrimary)
        if (secondaryText != null) {
            Text(
                secondaryText,
                style = MaterialTheme.typography.labelSmall,
                color = if (isSelected) colors.onPrimary else colors.onSurfaceVariant,
                maxLines = 1,
            )
        }
    }
}

/** Fixed-height indicator: nothing, a dot, or a count badge for multiple doses. */
@Composable
fun DoseIndicator(count: Int, color: Color, onColor: Color, modifier: Modifier = Modifier) {
    Box(modifier.height(16.dp), contentAlignment = Alignment.Center) {
        when {
            count == 1 -> Box(Modifier.size(6.dp).clip(CircleShape).background(color))
            count > 1 -> Box(
                Modifier
                    .sizeIn(minWidth = 16.dp, minHeight = 16.dp)
                    .clip(CircleShape)
                    .background(color)
                    .padding(horizontal = 4.dp),
                contentAlignment = Alignment.Center,
            ) {
                Text(
                    count.toString(),
                    style = MaterialTheme.typography.labelSmall.copy(letterSpacing = 0.sp),
                    color = onColor,
                    maxLines = 1,
                )
            }
        }
    }
}

/** Legend explaining [DoseIndicator]; decorative indicators, text carries meaning. */
@Composable
fun DoseLegend(singleLabel: String, multipleLabel: String, modifier: Modifier = Modifier) {
    val colors = MaterialTheme.colorScheme
    Row(
        modifier = modifier,
        horizontalArrangement = Arrangement.spacedBy(16.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Row(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalAlignment = Alignment.CenterVertically) {
            DoseIndicator(1, colors.primary, colors.onPrimary)
            Text(singleLabel, style = MaterialTheme.typography.bodySmall, color = colors.onSurfaceVariant)
        }
        Row(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalAlignment = Alignment.CenterVertically) {
            DoseIndicator(2, colors.primary, colors.onPrimary)
            Text(multipleLabel, style = MaterialTheme.typography.bodySmall, color = colors.onSurfaceVariant)
        }
    }
}
