package cz.nowass.dispenser.ui.components

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import cz.nowass.dispenser.ui.theme.Spacing

/**
 * Compact top header: small brand line, large screen title and the persistent
 * DEMO badge. Handles the status-bar inset itself (edge-to-edge).
 */
@Composable
fun AppHeader(
    brand: String,
    title: String,
    showDemoBadge: Boolean,
    modifier: Modifier = Modifier,
    /** Visible simulation-clock indicator (D1); null outside simulation mode. */
    simulationClock: String? = null,
) {
    Row(
        modifier = modifier
            .fillMaxWidth()
            .windowInsetsPadding(WindowInsets.statusBars)
            .padding(start = Spacing.m, end = Spacing.m, top = Spacing.m, bottom = Spacing.s),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(Spacing.s),
    ) {
        Column(Modifier.weight(1f)) {
            Text(
                brand,
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.primary,
            )
            Text(
                title,
                style = MaterialTheme.typography.headlineMedium,
                color = MaterialTheme.colorScheme.onBackground,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.semantics { heading() },
            )
            if (simulationClock != null) {
                Text(
                    simulationClock,
                    style = MaterialTheme.typography.labelMedium,
                    color = cz.nowass.dispenser.ui.theme.DispenserTheme.extendedColors.warning,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
        if (showDemoBadge) DemoBadge(Modifier.testTag(DEMO_BADGE_TEST_TAG))
    }
}
