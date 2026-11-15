package cz.nowass.dispenser.ui.calendar

import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.createSavedStateHandle
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import cz.nowass.dispenser.data.DispenserRepository
import cz.nowass.dispenser.domain.DeviceStatus
import cz.nowass.dispenser.domain.OccurrenceCalendar
import cz.nowass.dispenser.domain.Schedule
import cz.nowass.dispenser.domain.SyncState
import cz.nowass.dispenser.domain.toDraft
import kotlinx.coroutines.launch
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import java.time.DayOfWeek
import java.time.LocalDate
import java.time.LocalTime
import java.time.YearMonth
import java.time.format.DateTimeParseException
import java.time.temporal.TemporalAdjusters

enum class CalendarMode { MONTH, WEEK }

sealed interface CalendarUiState {
    data object Loading : CalendarUiState

    data class Ready(
        /** Date of the device clock (simulated in DEMO), not the phone's date. */
        val today: LocalDate,
        val mode: CalendarMode,
        val selectedDate: LocalDate,
        /** Month being shown (MONTH mode) or month of the week's Monday (WEEK mode). */
        val visibleMonth: YearMonth,
        /** Grid days in row-major order, Monday first; 42 in MONTH mode, 7 in WEEK mode. */
        val days: List<CalendarDayUi>,
        val selectedDay: SelectedDayUi,
        /** All schedules incl. disabled ones (the only place they are reachable), sorted by time then name. */
        val schedules: List<ScheduleListItemUi> = emptyList(),
        /** Stored schedules that could not be loaded (corrupted); shown as a warning, never deleted. */
        val unreadableScheduleCount: Int = 0,
    ) : CalendarUiState
}

data class CalendarDayUi(
    val date: LocalDate,
    /** False for leading/trailing days of adjacent months in MONTH mode. */
    val inVisibleMonth: Boolean,
    val isToday: Boolean,
    val isSelected: Boolean,
    val doseCount: Int,
    val totalMl: Double,
)

data class SelectedDayUi(
    val date: LocalDate,
    val isToday: Boolean,
    val occurrences: List<OccurrenceUi>,
    val totalMl: Double,
)

data class ScheduleListItemUi(
    val schedule: Schedule,
    val sync: SyncState?,
)

data class OccurrenceUi(
    /** Stable list key: deterministic occurrence id (schedule id + local date). */
    val key: String,
    val scheduleId: String,
    val scheduleName: String,
    val localTime: LocalTime,
    val amountMl: Double,
    /** Planned time already passed on the device clock. Says nothing about the outcome. */
    val isPast: Boolean,
)

class CalendarViewModel(
    private val repository: DispenserRepository,
    private val savedState: SavedStateHandle,
) : ViewModel() {

    private val selectedIso: StateFlow<String?> =
        savedState.getStateFlow(KEY_SELECTED, parseIsoOrNull(savedState.get<String>(ARG_DATE))?.toString())
    private val modeName: StateFlow<String> = savedState.getStateFlow(KEY_MODE, CalendarMode.MONTH.name)

    private data class StoreView(val schedules: List<Schedule>, val sync: Map<String, SyncState>, val unreadable: Int)

    private val storeView = combine(
        repository.observeSchedules(),
        repository.observeScheduleSync(),
        repository.observeUnreadableScheduleCount(),
        ::StoreView,
    )

    val uiState: StateFlow<CalendarUiState> = combine(
        repository.observeDevice(),
        storeView,
        selectedIso,
        modeName,
    ) { device, store, selected, mode ->
        buildState(
            device = device,
            schedules = store.schedules,
            selected = parseIsoOrNull(selected),
            mode = CalendarMode.entries.firstOrNull { it.name == mode } ?: CalendarMode.MONTH,
            sync = store.sync,
            unreadableScheduleCount = store.unreadable,
        )
    }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), CalendarUiState.Loading)

    fun selectDate(date: LocalDate) {
        savedState[KEY_SELECTED] = date.toString()
    }

    fun setMode(mode: CalendarMode) {
        savedState[KEY_MODE] = mode.name
    }

    /** Moves one month (MONTH) or one week (WEEK) forward/backward, keeping the selection in view. */
    fun shiftPeriod(forward: Boolean) {
        val state = uiState.value as? CalendarUiState.Ready ?: return
        val step = if (forward) 1L else -1L
        selectDate(
            when (state.mode) {
                CalendarMode.MONTH -> state.selectedDate.plusMonths(step)
                CalendarMode.WEEK -> state.selectedDate.plusWeeks(step)
            },
        )
    }

    /** Quick enable/disable from the schedule list; writes a new revision via the repository. */
    fun setEnabled(scheduleId: String, enabled: Boolean) {
        val state = uiState.value as? CalendarUiState.Ready ?: return
        val schedule = state.schedules.firstOrNull { it.schedule.id == scheduleId }?.schedule ?: return
        if (schedule.enabled == enabled) return
        viewModelScope.launch {
            // On Conflict the observed state already reflects the newer stored version.
            repository.updateSchedule(schedule.id, schedule.revision, schedule.toDraft().copy(enabled = enabled))
        }
    }

    fun goToToday() {
        val state = uiState.value as? CalendarUiState.Ready ?: return
        selectDate(state.today)
    }

    companion object {
        /** Navigation argument (ISO yyyy-MM-dd), e.g. from the dashboard week strip. */
        const val ARG_DATE = "date"
        private const val KEY_SELECTED = "calendar_selected"
        private const val KEY_MODE = "calendar_mode"

        fun factory(repository: DispenserRepository): ViewModelProvider.Factory = viewModelFactory {
            initializer { CalendarViewModel(repository, createSavedStateHandle()) }
        }

        internal fun parseIsoOrNull(value: String?): LocalDate? =
            try { value?.let(LocalDate::parse) } catch (_: DateTimeParseException) { null }

        internal fun buildState(
            device: DeviceStatus,
            schedules: List<Schedule>,
            selected: LocalDate?,
            mode: CalendarMode,
            sync: Map<String, SyncState> = emptyMap(),
            unreadableScheduleCount: Int = 0,
        ): CalendarUiState.Ready {
            val today = device.now.atZone(device.zone).toLocalDate()
            val selectedDate = selected ?: today

            val (first, last, visibleMonth) = when (mode) {
                CalendarMode.MONTH -> {
                    val month = YearMonth.from(selectedDate)
                    val start = month.atDay(1).with(TemporalAdjusters.previousOrSame(DayOfWeek.MONDAY))
                    Triple(start, start.plusDays(MONTH_GRID_DAYS - 1L), month)
                }
                CalendarMode.WEEK -> {
                    val start = selectedDate.with(TemporalAdjusters.previousOrSame(DayOfWeek.MONDAY))
                    Triple(start, start.plusDays(6), YearMonth.from(start))
                }
            }

            val byDate = OccurrenceCalendar.occurrencesBetween(schedules, first, last)
            val days = byDate.map { (date, occ) ->
                CalendarDayUi(
                    date = date,
                    inVisibleMonth = mode == CalendarMode.WEEK || YearMonth.from(date) == visibleMonth,
                    isToday = date == today,
                    isSelected = date == selectedDate,
                    doseCount = occ.size,
                    totalMl = occ.sumOf { it.amountMl },
                )
            }

            val selectedOcc = byDate[selectedDate] ?: OccurrenceCalendar.occurrencesOn(schedules, selectedDate)
            return CalendarUiState.Ready(
                today = today,
                mode = mode,
                selectedDate = selectedDate,
                visibleMonth = visibleMonth,
                days = days,
                selectedDay = SelectedDayUi(
                    date = selectedDate,
                    isToday = selectedDate == today,
                    occurrences = selectedOcc.map {
                        OccurrenceUi(
                            key = it.id,
                            scheduleId = it.scheduleId,
                            scheduleName = it.scheduleName,
                            localTime = it.scheduledAt.atZone(device.zone).toLocalTime(),
                            amountMl = it.amountMl,
                            isPast = !it.scheduledAt.isAfter(device.now),
                        )
                    },
                    totalMl = selectedOcc.sumOf { it.amountMl },
                ),
                schedules = schedules
                    .sortedWith(compareBy<Schedule>({ it.localTime }, { it.name.lowercase() }, { it.id }))
                    .map { ScheduleListItemUi(it, sync[it.id]) },
                unreadableScheduleCount = unreadableScheduleCount,
            )
        }

        /** Fixed 6-row month grid so the layout height does not jump between months. */
        internal const val MONTH_GRID_DAYS = 42
    }
}
