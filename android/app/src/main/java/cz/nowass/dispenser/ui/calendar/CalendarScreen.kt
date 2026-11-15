package cz.nowass.dispenser.ui.calendar

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.KeyboardArrowLeft
import androidx.compose.material.icons.automirrored.filled.KeyboardArrowRight
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import cz.nowass.dispenser.R
import cz.nowass.dispenser.data.MockSeed
import cz.nowass.dispenser.ui.Format
import cz.nowass.dispenser.ui.components.CalendarDayCell
import cz.nowass.dispenser.ui.components.DoseLegend
import cz.nowass.dispenser.ui.components.OccurrenceRow
import cz.nowass.dispenser.ui.components.StatusTag
import cz.nowass.dispenser.ui.czechPluralStringResource
import cz.nowass.dispenser.ui.schedule.scheduleDisplayName
import cz.nowass.dispenser.ui.schedule.scheduleSummary
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.width
import androidx.compose.material.icons.filled.Add
import androidx.compose.material3.Switch
import androidx.compose.ui.semantics.contentDescription
import cz.nowass.dispenser.ui.theme.DispenserTheme
import cz.nowass.dispenser.ui.theme.Spacing
import java.time.DayOfWeek
import java.time.LocalDate

/** At or above this width the grid and the day details are shown side by side. */
private val TwoPaneMinWidth = 600.dp

@Composable
fun CalendarRoute(
    viewModel: CalendarViewModel,
    onCreate: (LocalDate) -> Unit,
    onEdit: (scheduleId: String) -> Unit,
    modifier: Modifier = Modifier,
) {
    val state by viewModel.uiState.collectAsStateWithLifecycle()
    CalendarScreen(
        state = state,
        onSelectDate = viewModel::selectDate,
        onShift = viewModel::shiftPeriod,
        onToday = viewModel::goToToday,
        onModeChange = viewModel::setMode,
        modifier = modifier,
        onCreate = onCreate,
        onEdit = onEdit,
        onToggleEnabled = viewModel::setEnabled,
    )
}

@Composable
fun CalendarScreen(
    state: CalendarUiState,
    onSelectDate: (LocalDate) -> Unit,
    onShift: (forward: Boolean) -> Unit,
    onToday: () -> Unit,
    onModeChange: (CalendarMode) -> Unit,
    modifier: Modifier = Modifier,
    onCreate: (LocalDate) -> Unit = {},
    onEdit: (scheduleId: String) -> Unit = {},
    onToggleEnabled: (scheduleId: String, enabled: Boolean) -> Unit = { _, _ -> },
) {
    when (state) {
        CalendarUiState.Loading -> Box(modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
            CircularProgressIndicator()
        }
        is CalendarUiState.Ready -> BoxWithConstraints(modifier.fillMaxSize()) {
            val twoPane = maxWidth >= TwoPaneMinWidth
            if (twoPane) {
                Row(
                    Modifier.fillMaxSize().padding(horizontal = Spacing.m, vertical = Spacing.s),
                    horizontalArrangement = Arrangement.spacedBy(Spacing.m),
                ) {
                    Column(
                        Modifier.weight(1f).verticalScroll(rememberScrollState()),
                        verticalArrangement = Arrangement.spacedBy(Spacing.m),
                    ) {
                        PeriodControls(state, onShift, onToday, onModeChange)
                        GridCard(state, onSelectDate)
                    }
                    Column(
                        Modifier.weight(1f).verticalScroll(rememberScrollState()),
                        verticalArrangement = Arrangement.spacedBy(Spacing.m),
                    ) {
                        DayDetails(state.selectedDay, onCreate, onEdit)
                        SchedulesCard(state.schedules, state.unreadableScheduleCount, onCreate = { onCreate(state.selectedDate) }, onEdit, onToggleEnabled)
                    }
                }
            } else {
                Column(
                    Modifier
                        .fillMaxSize()
                        .verticalScroll(rememberScrollState())
                        .padding(horizontal = Spacing.m, vertical = Spacing.s),
                    verticalArrangement = Arrangement.spacedBy(Spacing.m),
                ) {
                    PeriodControls(state, onShift, onToday, onModeChange)
                    GridCard(state, onSelectDate)
                    DayDetails(state.selectedDay, onCreate, onEdit)
                    SchedulesCard(state.schedules, state.unreadableScheduleCount, onCreate = { onCreate(state.selectedDate) }, onEdit, onToggleEnabled)
                }
            }
        }
    }
}

@Composable
private fun PeriodControls(
    state: CalendarUiState.Ready,
    onShift: (Boolean) -> Unit,
    onToday: () -> Unit,
    onModeChange: (CalendarMode) -> Unit,
) {
    val isMonth = state.mode == CalendarMode.MONTH
    val label = if (isMonth) {
        Format.monthYear(state.visibleMonth)
    } else {
        val start = state.days.first().date
        val end = state.days.last().date
        val sameMonthTemplate = stringResource(R.string.calendar_week_range_same_month)
        val rangeTemplate = stringResource(R.string.calendar_week_range)
        Format.weekRange(
            start, end,
            sameMonth = { a, b, m, y -> String.format(Format.CZECH, sameMonthTemplate, a, b, m, y) },
            range = { a, b -> String.format(Format.CZECH, rangeTemplate, a, b) },
        )
    }

    Column(verticalArrangement = Arrangement.spacedBy(Spacing.s)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            IconButton(onClick = { onShift(false) }) {
                Icon(
                    Icons.AutoMirrored.Filled.KeyboardArrowLeft,
                    contentDescription = stringResource(if (isMonth) R.string.calendar_previous_month else R.string.calendar_previous_week),
                )
            }
            Text(
                label,
                style = MaterialTheme.typography.titleLarge,
                textAlign = TextAlign.Center,
                modifier = Modifier
                    .weight(1f)
                    .semantics {
                        heading()
                        liveRegion = LiveRegionMode.Polite
                    },
            )
            IconButton(onClick = { onShift(true) }) {
                Icon(
                    Icons.AutoMirrored.Filled.KeyboardArrowRight,
                    contentDescription = stringResource(if (isMonth) R.string.calendar_next_month else R.string.calendar_next_week),
                )
            }
        }
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(Spacing.s)) {
            SingleChoiceSegmentedButtonRow(Modifier.weight(1f)) {
                CalendarMode.entries.forEachIndexed { index, mode ->
                    SegmentedButton(
                        selected = state.mode == mode,
                        onClick = { onModeChange(mode) },
                        shape = SegmentedButtonDefaults.itemShape(index, CalendarMode.entries.size),
                        icon = {}, // no checkmark: keeps labels untruncated at large font sizes
                        colors = SegmentedButtonDefaults.colors(
                            activeContainerColor = MaterialTheme.colorScheme.primaryContainer,
                            activeContentColor = MaterialTheme.colorScheme.onPrimaryContainer,
                            inactiveContainerColor = MaterialTheme.colorScheme.surface,
                            inactiveContentColor = MaterialTheme.colorScheme.onSurfaceVariant,
                        ),
                    ) {
                        Text(
                            stringResource(if (mode == CalendarMode.MONTH) R.string.calendar_mode_month else R.string.calendar_mode_week),
                            maxLines = 1,
                        )
                    }
                }
            }
            TextButton(onClick = onToday, enabled = state.selectedDate != state.today) {
                Text(stringResource(R.string.calendar_today))
            }
        }
    }
}

@Composable
private fun GridCard(state: CalendarUiState.Ready, onSelectDate: (LocalDate) -> Unit) {
    val isWeek = state.mode == CalendarMode.WEEK
    Card(
        shape = MaterialTheme.shapes.large,
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
    ) {
        Column(
            Modifier.padding(horizontal = Spacing.s, vertical = Spacing.m),
            verticalArrangement = Arrangement.spacedBy(Spacing.xs),
        ) {
            if (!isWeek) WeekdayHeader()
            state.days.chunked(7).forEach { week ->
                Row(Modifier.fillMaxWidth()) {
                    week.forEach { day -> DayCell(day, isWeek, onSelectDate, Modifier.weight(1f).padding(1.dp)) }
                }
            }
            DoseLegend(
                singleLabel = stringResource(R.string.calendar_legend_single),
                multipleLabel = stringResource(R.string.calendar_legend_multiple),
                modifier = Modifier.padding(start = Spacing.s, top = Spacing.s),
            )
        }
    }
}

@Composable
private fun WeekdayHeader() {
    // Monday-first (Czech convention); labels derived from java.time, not hardcoded.
    val monday = LocalDate.of(2024, 1, 1).with(DayOfWeek.MONDAY)
    Row(Modifier.fillMaxWidth()) {
        (0 until 7).forEach { i ->
            Text(
                Format.shortWeekday(monday.plusDays(i.toLong())),
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                textAlign = TextAlign.Center,
                modifier = Modifier.weight(1f),
            )
        }
    }
}

@Composable
private fun DayCell(day: CalendarDayUi, isWeek: Boolean, onSelectDate: (LocalDate) -> Unit, modifier: Modifier) {
    val count = czechPluralStringResource(R.plurals.dose_count, day.doseCount, day.doseCount)
    val dateText = Format.fullDate(day.date).let { if (day.isToday) stringResource(R.string.today) + ", " + it else it }
    val a11y = if (day.doseCount > 0) {
        stringResource(R.string.calendar_day_a11y_total, dateText, count, stringResource(R.string.unit_ml, Format.ml(day.totalMl)))
    } else {
        stringResource(R.string.calendar_day_a11y, dateText, count)
    }
    CalendarDayCell(
        dayNumber = Format.dayOfMonth(day.date),
        doseCount = day.doseCount,
        isToday = day.isToday,
        isSelected = day.isSelected,
        inVisiblePeriod = day.inVisibleMonth,
        accessibilityLabel = a11y,
        onClick = { onSelectDate(day.date) },
        weekdayLabel = if (isWeek) Format.shortWeekday(day.date) else null,
        secondaryText = if (isWeek && day.doseCount > 0) stringResource(R.string.unit_ml, Format.ml(day.totalMl)) else null,
        modifier = modifier,
    )
}

@Composable
private fun DayDetails(day: SelectedDayUi, onCreate: (LocalDate) -> Unit, onEdit: (String) -> Unit) {
    Card(
        shape = MaterialTheme.shapes.large,
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
        modifier = Modifier.fillMaxWidth(),
    ) {
        Column(Modifier.padding(Spacing.m), verticalArrangement = Arrangement.spacedBy(Spacing.xs)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(Spacing.s)) {
                Text(
                    Format.fullDateTitle(day.date),
                    style = MaterialTheme.typography.titleLarge,
                    modifier = Modifier.weight(1f, fill = false).semantics { heading() },
                )
                if (day.isToday) StatusTag(stringResource(R.string.calendar_today))
            }
            if (day.occurrences.isEmpty()) {
                Text(
                    stringResource(R.string.calendar_day_empty),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(vertical = Spacing.s),
                )
            } else {
                Text(
                    stringResource(
                        R.string.calendar_day_summary,
                        czechPluralStringResource(R.plurals.dose_count, day.occurrences.size, day.occurrences.size),
                        stringResource(R.string.unit_ml, Format.ml(day.totalMl)),
                    ),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Column(Modifier.padding(top = Spacing.s)) {
                    day.occurrences.forEachIndexed { i, occ ->
                        if (i > 0) HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                        OccurrenceRow(
                            timeText = Format.time(occ.localTime),
                            name = occ.scheduleName,
                            amountText = stringResource(R.string.unit_ml, Format.ml(occ.amountMl)),
                            pastLabel = if (occ.isPast) stringResource(R.string.calendar_past_occurrence) else null,
                            onClick = { onEdit(occ.scheduleId) },
                            onClickLabel = stringResource(R.string.occurrence_edit_a11y, occ.scheduleName),
                        )
                    }
                }
            }
            TextButton(
                onClick = { onCreate(day.date) },
                modifier = Modifier.heightIn(min = 48.dp),
            ) {
                Icon(Icons.Filled.Add, contentDescription = null)
                Spacer(Modifier.width(Spacing.s))
                Text(stringResource(R.string.calendar_add_for_day))
            }
        }
    }
}

@Composable
private fun SchedulesCard(
    items: List<ScheduleListItemUi>,
    unreadableCount: Int,
    onCreate: () -> Unit,
    onEdit: (String) -> Unit,
    onToggleEnabled: (String, Boolean) -> Unit,
) {
    Card(
        shape = MaterialTheme.shapes.large,
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
        modifier = Modifier.fillMaxWidth(),
    ) {
        Column(Modifier.padding(Spacing.m), verticalArrangement = Arrangement.spacedBy(Spacing.xs)) {
            Text(
                stringResource(R.string.schedules_title),
                style = MaterialTheme.typography.titleLarge,
                modifier = Modifier.semantics { heading() },
            )
            if (unreadableCount > 0) UnreadableSchedulesWarning(unreadableCount)
            if (items.isEmpty()) {
                Text(
                    stringResource(R.string.schedules_empty),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(vertical = Spacing.s),
                )
            }
            items.forEachIndexed { i, item ->
                if (i > 0) HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                ScheduleListRow(item, onEdit, onToggleEnabled)
            }
            TextButton(onClick = onCreate, modifier = Modifier.heightIn(min = 48.dp)) {
                Icon(Icons.Filled.Add, contentDescription = null)
                Spacer(Modifier.width(Spacing.s))
                Text(stringResource(R.string.schedules_add))
            }
        }
    }
}

/** Non-blocking warning: some stored schedules could not be loaded; nothing was deleted. */
@Composable
private fun UnreadableSchedulesWarning(count: Int) {
    val warning = cz.nowass.dispenser.ui.theme.DispenserTheme.extendedColors.warning
    androidx.compose.material3.Surface(
        color = cz.nowass.dispenser.ui.theme.DispenserTheme.extendedColors.warningContainer,
        contentColor = warning,
        shape = MaterialTheme.shapes.medium,
        modifier = Modifier.fillMaxWidth().padding(vertical = Spacing.xs),
    ) {
        Column(
            Modifier.padding(Spacing.m).semantics(mergeDescendants = true) {},
            verticalArrangement = Arrangement.spacedBy(2.dp),
        ) {
            Text(
                czechPluralStringResource(R.plurals.schedules_unreadable, count, count),
                style = MaterialTheme.typography.bodyMedium,
            )
            Text(
                stringResource(R.string.schedules_unreadable_detail),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
private fun ScheduleListRow(
    item: ScheduleListItemUi,
    onEdit: (String) -> Unit,
    onToggleEnabled: (String, Boolean) -> Unit,
) {
    val s = item.schedule
    val name = scheduleDisplayName(s.name)
    val muted = !s.enabled
    Row(
        Modifier.fillMaxWidth().heightIn(min = 64.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(Spacing.s),
    ) {
        Column(
            Modifier
                .weight(1f)
                .clickable(onClickLabel = stringResource(R.string.occurrence_edit_a11y, name)) { onEdit(s.id) }
                .padding(vertical = Spacing.s),
            verticalArrangement = Arrangement.spacedBy(2.dp),
        ) {
            Text(
                name,
                style = MaterialTheme.typography.bodyLarge,
                color = if (muted) MaterialTheme.colorScheme.onSurfaceVariant else MaterialTheme.colorScheme.onSurface,
            )
            Text(
                scheduleSummary(s),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            if (muted) StatusTag(stringResource(R.string.schedule_disabled_tag))
        }
        val switchLabel = stringResource(R.string.schedule_enabled_switch_a11y, name)
        Switch(
            checked = s.enabled,
            onCheckedChange = { onToggleEnabled(s.id, it) },
            modifier = Modifier.semantics { contentDescription = switchLabel },
        )
    }
}

// ---- Previews ----

private fun previewState(mode: CalendarMode, selected: LocalDate? = null): CalendarUiState.Ready {
    val seed = MockSeed.default()
    return CalendarViewModel.buildState(seed.device, seed.schedules, selected, mode)
}

@Preview(name = "Month", widthDp = 412, heightDp = 1100, showBackground = true, backgroundColor = 0xFF111820)
@Composable
private fun CalendarMonthPreview() {
    DispenserTheme { CalendarScreen(previewState(CalendarMode.MONTH, LocalDate.of(2026, 11, 16)), {}, {}, {}, {}) }
}

@Preview(name = "Week, compact, large font", widthDp = 320, heightDp = 900, fontScale = 1.3f, showBackground = true, backgroundColor = 0xFF111820)
@Composable
private fun CalendarWeekPreview() {
    DispenserTheme { CalendarScreen(previewState(CalendarMode.WEEK), {}, {}, {}, {}) }
}

@Preview(name = "Landscape two-pane", widthDp = 915, heightDp = 412, showBackground = true, backgroundColor = 0xFF111820)
@Composable
private fun CalendarLandscapePreview() {
    DispenserTheme { CalendarScreen(previewState(CalendarMode.MONTH, LocalDate.of(2026, 11, 1)), {}, {}, {}, {}) }
}
