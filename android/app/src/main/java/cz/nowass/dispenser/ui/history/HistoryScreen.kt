package cz.nowass.dispenser.ui.history

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.ui.platform.testTag
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import cz.nowass.dispenser.R
import cz.nowass.dispenser.data.MockSeed
import cz.nowass.dispenser.domain.Volume
import cz.nowass.dispenser.domain.simulation.EventOrigin
import cz.nowass.dispenser.domain.simulation.EventReason
import cz.nowass.dispenser.domain.simulation.HistoryEvent
import cz.nowass.dispenser.domain.simulation.HistoryEventType
import cz.nowass.dispenser.ui.Format
import cz.nowass.dispenser.ui.components.StatusTag
import cz.nowass.dispenser.ui.theme.DispenserTheme
import cz.nowass.dispenser.ui.theme.Spacing
import java.time.ZoneId

const val HISTORY_UNAVAILABLE_TEST_TAG = "history_unavailable"

@Composable
fun HistoryRoute(viewModel: HistoryViewModel, modifier: Modifier = Modifier) {
    val state by viewModel.uiState.collectAsStateWithLifecycle()
    HistoryScreen(state, modifier)
}

@Composable
fun HistoryScreen(state: HistoryUiState, modifier: Modifier = Modifier) {
    when (state) {
        HistoryUiState.Loading -> Box(modifier.fillMaxSize(), contentAlignment = Alignment.Center) { CircularProgressIndicator() }
        HistoryUiState.Unavailable -> Box(modifier.fillMaxSize().padding(Spacing.m).testTag(HISTORY_UNAVAILABLE_TEST_TAG)) {
            Text(stringResource(R.string.history_device_unavailable), style = MaterialTheme.typography.bodyLarge, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        is HistoryUiState.Ready -> LazyColumn(
            modifier = modifier.fillMaxSize(),
            contentPadding = androidx.compose.foundation.layout.PaddingValues(horizontal = Spacing.m, vertical = Spacing.s),
            verticalArrangement = Arrangement.spacedBy(Spacing.m),
        ) {
            item {
                Text(
                    stringResource(R.string.history_simulated_note),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            if (state.days.isEmpty()) {
                item {
                    Text(stringResource(R.string.history_empty), style = MaterialTheme.typography.bodyLarge, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
            items(state.days, key = { it.date.toString() }) { day -> DayCard(day, state.zone) }
        }
    }
}

@Composable
private fun DayCard(day: HistoryDayUi, zone: ZoneId) {
    Card(
        shape = MaterialTheme.shapes.large,
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
        modifier = Modifier.fillMaxWidth(),
    ) {
        Column(Modifier.padding(Spacing.m)) {
            Text(Format.fullDateTitle(day.date), style = MaterialTheme.typography.titleMedium, modifier = Modifier.semantics { heading() })
            day.events.forEachIndexed { i, e ->
                if (i > 0) HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                EventRow(e, zone)
            }
        }
    }
}

private fun ml(ul: Long): Double = ul / Volume.MICROLITRES_PER_ML.toDouble()

@Composable
private fun EventRow(e: HistoryEvent, zone: ZoneId) {
    val colors = MaterialTheme.colorScheme
    val warning = DispenserTheme.extendedColors.warning
    val (titleRes, color) = when (e.type) {
        HistoryEventType.DISPENSE_SUCCESS -> R.string.history_type_success to colors.primary
        HistoryEventType.DISPENSE_BLOCKED_INSUFFICIENT_STOCK -> R.string.history_type_blocked to colors.error
        HistoryEventType.DISPENSE_SKIPPED -> R.string.history_type_skipped to colors.onSurfaceVariant
        HistoryEventType.RESERVOIR_REFILLED -> R.string.history_type_refilled to colors.primary
        HistoryEventType.RESERVOIR_LOW_WARNING -> R.string.history_type_low to warning
        HistoryEventType.DISPENSE_INTERRUPTED_UNCERTAIN -> R.string.history_type_interrupted to colors.error
        HistoryEventType.SERVICE_STARTED -> R.string.history_type_service_started to colors.onSurfaceVariant
        HistoryEventType.PRIMING_PUMP_RUN -> R.string.history_type_priming_run to colors.onSurfaceVariant
        HistoryEventType.TUBING_PRIMED -> R.string.history_type_tubing_primed to colors.onSurfaceVariant
        HistoryEventType.SERVICE_COMPLETED -> R.string.history_type_service_completed to colors.primary
    }
    val reasonRes = when (e.reason) {
        EventReason.INSUFFICIENT_STOCK -> R.string.history_reason_insufficient
        EventReason.BLOCKED_UNTIL_REFILL -> R.string.history_reason_blocked_until_refill
        EventReason.DEVICE_OUTAGE -> R.string.history_reason_outage
        EventReason.DEVICE_NOT_READY -> R.string.history_reason_not_ready
        else -> null
    }
    Row(
        Modifier.fillMaxWidth().heightIn(min = 56.dp).padding(vertical = Spacing.s).semantics(mergeDescendants = true) {},
        horizontalArrangement = Arrangement.spacedBy(Spacing.m),
    ) {
        Text(
            Format.time(e.time.atZone(zone)),
            style = MaterialTheme.typography.titleMedium.copy(fontFeatureSettings = "tnum"),
            color = colors.onSurfaceVariant,
            modifier = Modifier.widthIn(min = 52.dp),
        )
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Row(horizontalArrangement = Arrangement.spacedBy(Spacing.s), verticalAlignment = Alignment.CenterVertically) {
                Text(stringResource(titleRes), style = MaterialTheme.typography.bodyLarge, color = color, modifier = Modifier.weight(1f, fill = false))
                StatusTag(stringResource(if (e.origin == EventOrigin.SIMULATION) R.string.history_origin_simulation else R.string.history_origin_device))
            }
            val detail = listOfNotNull(
                e.scheduleNameSnapshot?.ifBlank { stringResource(R.string.schedule_unnamed) },
                e.requestedUl?.let { stringResource(R.string.unit_ml, Format.ml(ml(it))) },
                reasonRes?.let { stringResource(it) },
            ).joinToString(" · ")
            if (detail.isNotEmpty()) Text(detail, style = MaterialTheme.typography.bodyMedium, color = colors.onSurface)
            Text(
                if (e.reservoirBeforeUl != e.reservoirAfterUl) {
                    stringResource(R.string.history_reservoir_change, Format.ml(ml(e.reservoirBeforeUl)), stringResource(R.string.unit_ml, Format.ml(ml(e.reservoirAfterUl))))
                } else {
                    stringResource(R.string.history_reservoir_after, stringResource(R.string.unit_ml, Format.ml(ml(e.reservoirAfterUl))))
                },
                style = MaterialTheme.typography.bodySmall,
                color = colors.onSurfaceVariant,
            )
        }
    }
}

@Preview(name = "History 412", widthDp = 412, heightDp = 800, showBackground = true, backgroundColor = 0xFF111820)
@Composable
private fun HistoryPreview() {
    val seed = MockSeed.default()
    DispenserTheme { HistoryScreen(HistoryUiState.Ready(HistoryViewModel.group(seed.events, seed.device.zone), seed.device.zone)) }
}
