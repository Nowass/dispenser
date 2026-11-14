package cz.nowass.dispenser.ui.dashboard

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.IntrinsicSize
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.tooling.preview.PreviewParameter
import androidx.compose.ui.tooling.preview.PreviewParameterProvider
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import cz.nowass.dispenser.R
import cz.nowass.dispenser.data.MockSeed
import cz.nowass.dispenser.ui.Format
import cz.nowass.dispenser.ui.czechPluralStringResource
import java.time.LocalDate
import cz.nowass.dispenser.ui.components.MetricProgress
import cz.nowass.dispenser.ui.components.StatusCard
import cz.nowass.dispenser.ui.components.StatusTone
import cz.nowass.dispenser.ui.components.UpcomingDoseCard
import cz.nowass.dispenser.ui.components.WeekDayItem
import cz.nowass.dispenser.ui.components.WeekPreview
import cz.nowass.dispenser.ui.components.accent
import cz.nowass.dispenser.ui.theme.DispenserTheme
import cz.nowass.dispenser.ui.theme.Spacing
import kotlin.math.roundToInt

/** Below this width the two status cards stack vertically. */
private val TwoColumnMinWidth = 360.dp

@Composable
fun DashboardRoute(
    viewModel: DashboardViewModel,
    /** Opens the calendar; null date means "today / last state". */
    onOpenCalendar: (LocalDate?) -> Unit,
    modifier: Modifier = Modifier,
) {
    val state by viewModel.uiState.collectAsStateWithLifecycle()
    DashboardScreen(state, onOpenCalendar, modifier)
}

@Composable
fun DashboardScreen(
    state: DashboardUiState,
    onOpenCalendar: (LocalDate?) -> Unit,
    modifier: Modifier = Modifier,
) {
    when (state) {
        DashboardUiState.Loading -> Box(modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
            CircularProgressIndicator()
        }
        is DashboardUiState.Ready -> Column(
            modifier = modifier
                .fillMaxSize()
                .verticalScroll(rememberScrollState())
                .padding(horizontal = Spacing.m, vertical = Spacing.s),
            verticalArrangement = Arrangement.spacedBy(Spacing.m),
        ) {
            NextDose(state)
            StatusCards(state)
            Week(state, onOpenCalendar)
            if (state.isDemo) {
                Text(
                    text = stringResource(R.string.dashboard_demo_clock, Format.dateTime(state.now)),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(bottom = Spacing.s),
                )
            }
        }
    }
}

@Composable
private fun NextDose(state: DashboardUiState.Ready) {
    val next = state.nextDose
    val whenText = next?.let {
        val time = Format.time(it.at)
        when (Format.relativeDay(it.at, state.now)) {
            Format.RelativeDay.TODAY -> stringResource(R.string.dashboard_when_today, time)
            Format.RelativeDay.TOMORROW -> stringResource(R.string.dashboard_when_tomorrow, time)
            Format.RelativeDay.OTHER -> stringResource(R.string.dashboard_when_other, Format.dayAndDate(it.at), time)
        }
    }
    UpcomingDoseCard(
        title = stringResource(R.string.dashboard_next_dose),
        whenText = whenText,
        scheduleName = next?.scheduleName,
        amountText = next?.let { stringResource(R.string.unit_ml, Format.ml(it.amountMl)) },
        emptyText = stringResource(R.string.dashboard_no_next_dose),
    )
}

@Composable
private fun StatusCards(state: DashboardUiState.Ready) {
    BoxWithConstraints(Modifier.fillMaxWidth()) {
        if (maxWidth >= TwoColumnMinWidth) {
            Row(
                modifier = Modifier.fillMaxWidth().height(IntrinsicSize.Max),
                horizontalArrangement = Arrangement.spacedBy(Spacing.m),
            ) {
                ReservoirCard(state, Modifier.weight(1f).fillMaxHeight())
                BatteryCard(state.battery, Modifier.weight(1f).fillMaxHeight())
            }
        } else {
            Column(verticalArrangement = Arrangement.spacedBy(Spacing.m)) {
                ReservoirCard(state, Modifier.fillMaxWidth())
                BatteryCard(state.battery, Modifier.fillMaxWidth())
            }
        }
    }
}

@Composable
private fun ReservoirCard(state: DashboardUiState.Ready, modifier: Modifier) {
    val r = state.reservoir
    val accent = r.tone.accent()
    val percent = (r.fraction * 100).roundToInt()
    StatusCard(
        title = stringResource(R.string.dashboard_reservoir),
        icon = painterResource(R.drawable.ic_reservoir),
        tone = r.tone,
        accent = accent,
        tags = buildList {
            add(stringResource(R.string.dashboard_estimate_label))
            if (state.isDemo) add(stringResource(R.string.dashboard_simulated_label))
        },
        modifier = modifier,
    ) {
        Text(
            stringResource(R.string.unit_ml, Format.ml(r.estimatedRemainingMl)),
            style = MaterialTheme.typography.headlineMedium,
            color = MaterialTheme.colorScheme.onSurface,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
        Text(
            stringResource(R.string.dashboard_reservoir_capacity, stringResource(R.string.unit_ml, Format.ml(r.capacityMl))),
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        MetricProgress(
            fraction = r.fraction,
            accessibilityLabel = stringResource(R.string.dashboard_reservoir_a11y, percent),
            color = accent,
        )
        val message = when (r.tone) {
            StatusTone.Blocked -> R.string.dashboard_reservoir_insufficient
            StatusTone.Low -> R.string.dashboard_reservoir_low
            else -> R.string.dashboard_reservoir_estimate_note
        }
        Text(
            stringResource(message),
            style = MaterialTheme.typography.bodySmall,
            color = if (r.tone == StatusTone.Normal) MaterialTheme.colorScheme.onSurfaceVariant else accent,
        )
    }
}

@Composable
private fun BatteryCard(battery: BatteryUi, modifier: Modifier) {
    val accent = battery.tone.accent(normal = DispenserTheme.extendedColors.battery)
    StatusCard(
        title = stringResource(R.string.dashboard_battery),
        icon = painterResource(R.drawable.ic_battery),
        tone = battery.tone,
        accent = accent,
        tags = if (battery.isSimulated) listOf(stringResource(R.string.dashboard_simulated_label)) else emptyList(),
        modifier = modifier,
    ) {
        Text(
            battery.percent?.let { stringResource(R.string.unit_percent, it) }
                ?: stringResource(R.string.dashboard_battery_unknown),
            style = MaterialTheme.typography.headlineMedium,
            color = MaterialTheme.colorScheme.onSurface,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
        MetricProgress(
            fraction = battery.percent?.let { it / 100f },
            accessibilityLabel = battery.percent?.let { stringResource(R.string.dashboard_battery_a11y, it) }
                ?: stringResource(R.string.dashboard_battery_a11y_unknown),
            color = accent,
        )
        if (battery.isSimulated) {
            Text(
                stringResource(R.string.dashboard_battery_simulated_note),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
private fun doseCountText(n: Int): String = czechPluralStringResource(R.plurals.dose_count, n, n)

@Composable
private fun Week(state: DashboardUiState.Ready, onOpenCalendar: (LocalDate?) -> Unit) {
    val totalCount = state.week.sumOf { it.doseCount }
    val totalMl = state.week.sumOf { it.totalMl }
    val todayLabel = stringResource(R.string.today)
    WeekPreview(
        title = stringResource(R.string.dashboard_week_title),
        actionLabel = stringResource(R.string.dashboard_week_action),
        summary = if (totalCount == 0) stringResource(R.string.dashboard_week_empty)
        else stringResource(
            R.string.dashboard_week_summary,
            doseCountText(totalCount),
            stringResource(R.string.unit_ml, Format.ml(totalMl)),
        ),
        days = state.week.map { day ->
            val dateLabel = Format.fullDate(day.date).let { if (day.isToday) "$todayLabel, $it" else it }
            WeekDayItem(
                weekdayLabel = Format.shortWeekday(day.date),
                dayNumber = Format.dayOfMonth(day.date),
                isToday = day.isToday,
                doseCount = day.doseCount,
                accessibilityLabel = stringResource(R.string.dashboard_week_day_a11y, dateLabel, doseCountText(day.doseCount)) +
                    ". " + stringResource(R.string.dashboard_week_day_open_a11y),
            )
        },
        onOpen = { onOpenCalendar(null) },
        onDayClick = { index -> onOpenCalendar(state.week[index].date) },
    )
}

// ---- Previews: edge cases from TASK_A1_1_1_UI.md (long labels, low/blocked, unknown, no dose) ----

private class DashboardPreviewStates : PreviewParameterProvider<DashboardUiState.Ready> {
    private val seed = MockSeed.default()
    private val base = DashboardViewModel.toUiState(seed.device, seed.schedules, seed.reservoir)
    override val values = sequenceOf(
        base,
        DashboardViewModel.toUiState(seed.device, seed.schedules, seed.reservoir.copy(estimatedRemainingMl = 60.0)),
        DashboardViewModel.toUiState(
            seed.device.copy(batteryPercent = null),
            seed.schedules,
            seed.reservoir.copy(estimatedRemainingMl = 4.5),
        ),
        DashboardViewModel.toUiState(seed.device, seed.schedules.map { it.copy(enabled = false) }, seed.reservoir),
    )
}

@Preview(name = "Phone", widthDp = 412, heightDp = 915, showBackground = true, backgroundColor = 0xFF111820)
@Composable
private fun DashboardPreview(@PreviewParameter(DashboardPreviewStates::class) state: DashboardUiState.Ready) {
    DispenserTheme { DashboardScreen(state, onOpenCalendar = {}) }
}

@Preview(name = "Compact, large font", widthDp = 320, heightDp = 640, fontScale = 1.3f, showBackground = true, backgroundColor = 0xFF111820)
@Composable
private fun DashboardCompactPreview() {
    val seed = MockSeed.default()
    DispenserTheme {
        DashboardScreen(DashboardViewModel.toUiState(seed.device, seed.schedules, seed.reservoir), onOpenCalendar = {})
    }
}
