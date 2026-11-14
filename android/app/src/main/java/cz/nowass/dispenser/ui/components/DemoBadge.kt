package cz.nowass.dispenser.ui.components

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import cz.nowass.dispenser.R
import cz.nowass.dispenser.ui.theme.DispenserTheme

const val DEMO_BADGE_TEST_TAG = "demo_badge"

/** Conspicuous amber marker that all displayed data is simulated. */
@Composable
fun DemoBadge(modifier: Modifier = Modifier) {
    val description = stringResource(R.string.demo_badge_description)
    val warning = DispenserTheme.extendedColors.warning
    Surface(
        modifier = modifier.clearAndSetSemantics { contentDescription = description },
        color = DispenserTheme.extendedColors.warningContainer,
        contentColor = warning,
        border = BorderStroke(1.dp, warning.copy(alpha = 0.7f)),
        shape = MaterialTheme.shapes.small,
    ) {
        Text(
            text = stringResource(R.string.demo_badge),
            style = MaterialTheme.typography.labelLarge,
            fontWeight = FontWeight.Bold,
            modifier = Modifier.padding(horizontal = 10.dp, vertical = 4.dp),
        )
    }
}
