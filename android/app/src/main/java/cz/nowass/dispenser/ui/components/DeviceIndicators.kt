package cz.nowass.dispenser.ui.components

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import cz.nowass.dispenser.R
import cz.nowass.dispenser.domain.ConnectionState
import cz.nowass.dispenser.domain.Freshness
import cz.nowass.dispenser.ui.Format
import cz.nowass.dispenser.ui.theme.DispenserTheme
import java.time.Instant
import java.time.ZoneId

const val CONNECTION_CHIP_TEST_TAG = "connection_chip"

/**
 * DEVICE-mode counterpart of [DemoBadge] (same shape and placement): the connection state of the
 * selected device. "Připojeno" only while an authenticated session is live.
 */
@Composable
fun ConnectionChip(state: ConnectionState, modifier: Modifier = Modifier, testTag: String = CONNECTION_CHIP_TEST_TAG) {
    val (label, color) = when (state) {
        ConnectionState.CONNECTED -> stringResource(R.string.connection_connected) to MaterialTheme.colorScheme.primary
        ConnectionState.CONNECTING -> stringResource(R.string.connection_connecting) to DispenserTheme.extendedColors.warning
        ConnectionState.RECOVERY_REQUIRED -> stringResource(R.string.connection_recovery_required) to DispenserTheme.extendedColors.warning
        ConnectionState.DISCONNECTED, ConnectionState.DEMO -> stringResource(R.string.connection_disconnected) to MaterialTheme.colorScheme.onSurfaceVariant
    }
    val description = stringResource(R.string.connection_chip_description, label)
    Surface(
        modifier = modifier.testTag(testTag).clearAndSetSemantics { contentDescription = description },
        color = Color.Transparent,
        contentColor = color,
        border = BorderStroke(1.dp, color.copy(alpha = 0.7f)),
        shape = MaterialTheme.shapes.small,
    ) {
        Text(
            text = label,
            style = MaterialTheme.typography.labelLarge,
            fontWeight = FontWeight.Bold,
            modifier = Modifier.padding(horizontal = 10.dp, vertical = 4.dp),
        )
    }
}

/**
 * Qualifier for a device value: "živě" only for a value of the current session; a cached value always
 * shows the time the device reported it ("uloženo 14:05", or with the date when not today). Null in
 * DEMO mode (the "simulace" tag and the DEMO badge apply there).
 */
@Composable
fun freshnessLabel(freshness: Freshness, now: Instant, zone: ZoneId): String? = when (freshness) {
    Freshness.NotApplicable -> null
    Freshness.Unknown -> stringResource(R.string.freshness_unknown)
    is Freshness.Live -> stringResource(R.string.freshness_live)
    is Freshness.Cached -> {
        val at = freshness.readAt.atZone(zone)
        if (at.toLocalDate() == now.atZone(zone).toLocalDate()) {
            stringResource(R.string.freshness_cached_today, Format.time(at))
        } else {
            stringResource(R.string.freshness_cached_date, Format.date(at.toLocalDate()), Format.time(at))
        }
    }
}
