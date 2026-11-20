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
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.TextButton
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
import cz.nowass.dispenser.device.ops.HistoryEventKind
import cz.nowass.dispenser.device.ops.HistoryEventRecord
import cz.nowass.dispenser.device.ops.HistoryResult
import java.time.Instant
import cz.nowass.dispenser.ui.Format
import cz.nowass.dispenser.ui.components.StatusTag
import cz.nowass.dispenser.ui.theme.DispenserTheme
import cz.nowass.dispenser.ui.theme.Spacing
import java.time.ZoneId

const val HISTORY_UNAVAILABLE_TEST_TAG = "history_unavailable"

const val HISTORY_DEVICE_LIST_TEST_TAG = "history_device_list"
const val HISTORY_DEVICE_MORE_TEST_TAG = "history_device_more"
const val HISTORY_DEVICE_LOAD_TEST_TAG = "history_device_load"

@Composable
fun HistoryRoute(viewModel: HistoryViewModel, modifier: Modifier = Modifier) {
    val state by viewModel.uiState.collectAsStateWithLifecycle()
    HistoryScreen(state, modifier, onRefresh = viewModel::refresh, onLoadMore = viewModel::loadMore)
}

@Composable
fun HistoryScreen(state: HistoryUiState, modifier: Modifier = Modifier, onRefresh: () -> Unit = {}, onLoadMore: () -> Unit = {}) {
    when (state) {
        HistoryUiState.Loading -> Box(modifier.fillMaxSize(), contentAlignment = Alignment.Center) { CircularProgressIndicator() }
        is HistoryUiState.Device -> DeviceHistory(state.ui, state.zone, modifier, onRefresh, onLoadMore)
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

// ---- DEVICE mode (Phase C) -----------------------------------------------------------------

@Composable
private fun DeviceHistory(ui: DeviceHistoryUi, zone: ZoneId, modifier: Modifier, onRefresh: () -> Unit, onLoadMore: () -> Unit) {
    val colors = MaterialTheme.colorScheme
    LazyColumn(
        modifier = modifier.fillMaxSize().testTag(HISTORY_DEVICE_LIST_TEST_TAG),
        contentPadding = androidx.compose.foundation.layout.PaddingValues(horizontal = Spacing.m, vertical = Spacing.s),
        verticalArrangement = Arrangement.spacedBy(Spacing.m),
    ) {
        item {
            Text(stringResource(R.string.hist_dev_note), style = MaterialTheme.typography.bodySmall, color = colors.onSurfaceVariant)
        }
        val problem = when (ui.problem) {
            DeviceHistoryProblem.UNSUPPORTED -> R.string.hist_dev_unsupported
            DeviceHistoryProblem.NOT_CONNECTED -> R.string.hist_dev_not_connected
            DeviceHistoryProblem.FAILED -> R.string.hist_dev_failed
            null -> if (!ui.loaded && !ui.connected && !ui.loading) R.string.hist_dev_not_connected else null
        }
        if (problem != null) {
            item { Text(stringResource(problem), style = MaterialTheme.typography.bodyLarge, color = colors.onSurfaceVariant) }
        }
        if (ui.faults) {
            item { Text(stringResource(R.string.hist_dev_faults), style = MaterialTheme.typography.bodyMedium, color = colors.error) }
        }
        if (ui.problem != DeviceHistoryProblem.UNSUPPORTED) {
            item {
                Row(horizontalArrangement = Arrangement.spacedBy(Spacing.s), verticalAlignment = Alignment.CenterVertically) {
                    OutlinedButton(onClick = onRefresh, enabled = !ui.loading, modifier = Modifier.testTag(HISTORY_DEVICE_LOAD_TEST_TAG)) {
                        Text(stringResource(if (ui.loaded) R.string.hist_dev_refresh else R.string.hist_dev_load))
                    }
                    if (ui.loading) Text(stringResource(R.string.hist_dev_loading), style = MaterialTheme.typography.bodyMedium)
                }
            }
        }
        if (ui.loaded && ui.days.isEmpty() && ui.problem == null) {
            item { Text(stringResource(R.string.hist_dev_empty), style = MaterialTheme.typography.bodyLarge, color = colors.onSurfaceVariant) }
        }
        items(ui.days.size, key = { i -> "d$i:" + (ui.days[i].rows.first().event.seq) }) { i -> DeviceDayCard(ui.days[i], zone) }
        if (ui.loaded && ui.days.isNotEmpty()) {
            item {
                if (ui.canLoadMore) {
                    TextButton(onClick = onLoadMore, enabled = !ui.loading, modifier = Modifier.testTag(HISTORY_DEVICE_MORE_TEST_TAG)) {
                        Text(stringResource(R.string.hist_dev_more))
                    }
                } else if (ui.problem == null) {
                    Text(stringResource(R.string.hist_dev_end), style = MaterialTheme.typography.bodySmall, color = colors.onSurfaceVariant)
                }
            }
        }
    }
}

@Composable
private fun DeviceDayCard(day: DeviceHistoryDay, zone: ZoneId) {
    Card(
        shape = MaterialTheme.shapes.large,
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
        modifier = Modifier.fillMaxWidth(),
    ) {
        Column(Modifier.padding(Spacing.m)) {
            Text(
                day.date?.let { Format.fullDateTitle(it) } ?: stringResource(R.string.hist_dev_unknown_date),
                style = MaterialTheme.typography.titleMedium, modifier = Modifier.semantics { heading() },
            )
            day.rows.forEachIndexed { i, r ->
                if (i > 0) HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                DeviceEventRow(r, zone)
            }
        }
    }
}

/** Title and colour of a device event; bench/simulated doses are never shown as delivered liquid. */
@Composable
private fun deviceTitle(e: HistoryEventRecord): Pair<Int, androidx.compose.ui.graphics.Color> {
    val colors = MaterialTheme.colorScheme
    val warning = DispenserTheme.extendedColors.warning
    return when (e.kind) {
        HistoryEventKind.RESERVOIR_INIT -> R.string.hist_dev_res_init to colors.primary
        HistoryEventKind.RESERVOIR_REFILL -> R.string.hist_dev_res_refill to colors.primary
        HistoryEventKind.RESERVOIR_ADJUST -> R.string.hist_dev_res_adjust to colors.onSurface
        HistoryEventKind.DOSE_SCHEDULED, HistoryEventKind.DOSE_MANUAL -> when (e.result) {
            HistoryResult.OK -> (if (e.kind == HistoryEventKind.DOSE_MANUAL) R.string.hist_dev_manual_ok else R.string.hist_dev_scheduled_ok) to colors.primary
            HistoryResult.PUMP_FAILED -> R.string.hist_dev_pump_failed to colors.error
            HistoryResult.START_FAILED -> R.string.hist_dev_start_failed to colors.error
            HistoryResult.INTERRUPTED -> R.string.hist_dev_interrupted to warning
            HistoryResult.ABORTED -> R.string.hist_dev_aborted to colors.error
        }
    }
}

@Composable
private fun DeviceEventRow(r: DeviceHistoryRow, zone: ZoneId) {
    val e = r.event
    val colors = MaterialTheme.colorScheme
    val (titleRes, color) = deviceTitle(e)
    Row(
        Modifier.fillMaxWidth().heightIn(min = 56.dp).padding(vertical = Spacing.s).semantics(mergeDescendants = true) {},
        horizontalArrangement = Arrangement.spacedBy(Spacing.m),
    ) {
        Text(
            e.atEpochS?.let { Format.time(Instant.ofEpochSecond(it).atZone(zone)) } ?: "--:--",
            style = MaterialTheme.typography.titleMedium.copy(fontFeatureSettings = "tnum"),
            color = colors.onSurfaceVariant,
            modifier = Modifier.widthIn(min = 52.dp),
        )
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(stringResource(titleRes), style = MaterialTheme.typography.bodyLarge, color = color)
            val tags = listOfNotNull(
                if (e.kind.isDose && e.pumpBenchGpio) stringResource(R.string.hist_dev_tag_bench) else null,
                if (e.kind.isDose && e.pumpSimulated) stringResource(R.string.hist_dev_tag_sim) else null,
                if (e.atEpochS != null && !e.clockTrusted) stringResource(R.string.hist_dev_tag_untrusted) else null,
            )
            if (tags.isNotEmpty()) {
                Row(horizontalArrangement = Arrangement.spacedBy(Spacing.s)) { tags.forEach { StatusTag(it) } }
            }
            val detail = listOfNotNull(
                when (e.kind) {
                    HistoryEventKind.DOSE_SCHEDULED -> r.scheduleName?.ifBlank { stringResource(R.string.schedule_unnamed) }
                        ?: stringResource(R.string.hist_dev_schedule_deleted)
                    HistoryEventKind.DOSE_MANUAL -> stringResource(R.string.hist_dev_manual)
                    else -> null
                },
                if (e.kind.isDose) stringResource(R.string.unit_ml, Format.ml(ml(e.volumeUl))) else null,
            ).joinToString(" · ")
            if (detail.isNotEmpty()) Text(detail, style = MaterialTheme.typography.bodyMedium, color = colors.onSurface)
            val before = e.reservoirBeforeUl
            val after = e.reservoirAfterUl
            Text(
                when {
                    before == null || after == null -> stringResource(R.string.hist_dev_reservoir_unknown)
                    before != after -> stringResource(R.string.history_reservoir_change, Format.ml(ml(before)), stringResource(R.string.unit_ml, Format.ml(ml(after))))
                    else -> stringResource(R.string.history_reservoir_after, stringResource(R.string.unit_ml, Format.ml(ml(after))))
                } + if (!e.kind.isDose && e.capacityUl > 0) " · " + stringResource(R.string.hist_dev_capacity, stringResource(R.string.unit_ml, Format.ml(ml(e.capacityUl)))) else "",
                style = MaterialTheme.typography.bodySmall,
                color = colors.onSurfaceVariant,
            )
            if (e.kind.isDose && e.result == HistoryResult.OK && e.pumpBenchGpio) {
                Text(stringResource(R.string.hist_dev_bench_note), style = MaterialTheme.typography.bodySmall, color = colors.onSurfaceVariant)
            }
        }
    }
}

@Preview(name = "History device 412", widthDp = 412, heightDp = 800, showBackground = true, backgroundColor = 0xFF111820)
@Composable
private fun DeviceHistoryPreview() {
    val zone = ZoneId.of("Europe/Prague")
    val events = listOf(
        HistoryEventRecord(5, HistoryEventKind.DOSE_SCHEDULED, HistoryResult.OK, 1_795_158_000, true, 5_000, 95_000, 90_000, 100_000, "a", false, true),
        HistoryEventRecord(4, HistoryEventKind.DOSE_MANUAL, HistoryResult.INTERRUPTED, 1_795_150_000, true, 1_000, null, null, 0, null, false, true),
        HistoryEventRecord(3, HistoryEventKind.RESERVOIR_REFILL, HistoryResult.OK, 1_795_100_000, false, 0, 20_000, 100_000, 100_000, null, false, false),
    )
    val ui = DeviceHistoryUi(HistoryViewModel.groupDevice(events, mapOf("a" to "Ráno"), zone), loaded = true, loading = false,
        canLoadMore = true, problem = null, faults = false, connected = true)
    DispenserTheme { HistoryScreen(HistoryUiState.Device(ui, zone)) }
}

@Preview(name = "History 412", widthDp = 412, heightDp = 800, showBackground = true, backgroundColor = 0xFF111820)
@Composable
private fun HistoryPreview() {
    val seed = MockSeed.default()
    DispenserTheme { HistoryScreen(HistoryUiState.Ready(HistoryViewModel.group(seed.events, seed.device.zone), seed.device.zone)) }
}
