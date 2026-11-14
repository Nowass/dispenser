package cz.nowass.dispenser.ui.schedule

import androidx.compose.runtime.Composable
import androidx.compose.ui.res.stringResource
import cz.nowass.dispenser.R
import cz.nowass.dispenser.domain.Recurrence
import cz.nowass.dispenser.domain.Schedule
import cz.nowass.dispenser.domain.SyncState
import cz.nowass.dispenser.ui.Format
import cz.nowass.dispenser.ui.czechPluralStringResource
import java.time.DayOfWeek
import java.time.LocalDate

/** Monday-first weekday order (Czech convention). */
val WEEKDAYS_MONDAY_FIRST: List<DayOfWeek> = DayOfWeek.entries

/** Short Czech weekday label, e.g. "po". Uses a fixed reference week only for formatting. */
fun weekdayShort(day: DayOfWeek): String =
    Format.shortWeekday(LocalDate.of(2024, 1, 1).plusDays(day.value - 1L))

/** e.g. "Každý den", "Každé 2 týdny · po, čt", "Jednou, 16. 11. 2026". */
@Composable
fun recurrenceText(recurrence: Recurrence, startDate: LocalDate): String = when (recurrence) {
    Recurrence.Once -> stringResource(R.string.recurrence_once, Format.date(startDate))
    is Recurrence.Daily -> czechPluralStringResource(R.plurals.every_n_days, recurrence.intervalDays, recurrence.intervalDays)
    is Recurrence.Weekly -> stringResource(
        R.string.recurrence_with_days,
        czechPluralStringResource(R.plurals.every_n_weeks, recurrence.intervalWeeks, recurrence.intervalWeeks),
        WEEKDAYS_MONDAY_FIRST.filter { it in recurrence.weekdays }.joinToString(", ") { weekdayShort(it) },
    )
}

/** "Každý den · 08:00 · 10 ml" */
@Composable
fun scheduleSummary(schedule: Schedule): String = stringResource(
    R.string.schedule_summary,
    recurrenceText(schedule.recurrence, schedule.startDate),
    Format.time(schedule.localTime),
    stringResource(R.string.unit_ml, Format.ml(schedule.amountMl)),
)

@Composable
fun scheduleDisplayName(name: String): String = name.ifBlank { stringResource(R.string.schedule_unnamed) }

@Composable
fun syncStateText(state: SyncState): String = stringResource(
    when (state) {
        SyncState.PENDING -> R.string.sync_pending
        SyncState.SIMULATED_ACK -> R.string.sync_simulated
        SyncState.DEVICE_APPLIED -> R.string.sync_device
    },
)
