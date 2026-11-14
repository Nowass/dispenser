package cz.nowass.dispenser.ui.components

import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.semantics.ProgressBarRangeInfo
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.progressBarRangeInfo
import androidx.compose.ui.unit.dp

/**
 * Horizontal metric bar with explicit accessibility text, e.g.
 * "Zásobník: 62 %, odhad". Null [fraction] renders an empty track (unknown).
 */
@Composable
fun MetricProgress(
    fraction: Float?,
    accessibilityLabel: String,
    color: Color,
    modifier: Modifier = Modifier,
) {
    val value = (fraction ?: 0f).coerceIn(0f, 1f)
    LinearProgressIndicator(
        progress = { value },
        modifier = modifier
            .fillMaxWidth()
            .height(8.dp)
            .clearAndSetSemantics {
                contentDescription = accessibilityLabel
                if (fraction != null) progressBarRangeInfo = ProgressBarRangeInfo(value, 0f..1f)
            },
        color = color,
        trackColor = MaterialTheme.colorScheme.surfaceContainerHighest,
        strokeCap = StrokeCap.Round,
        gapSize = 0.dp,
        drawStopIndicator = {},
    )
}
