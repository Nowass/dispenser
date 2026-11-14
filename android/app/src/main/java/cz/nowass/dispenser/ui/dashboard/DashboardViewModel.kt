package cz.nowass.dispenser.ui.dashboard

import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import cz.nowass.dispenser.data.DispenserRepository
import cz.nowass.dispenser.domain.DataSource
import cz.nowass.dispenser.domain.DeviceStatus
import cz.nowass.dispenser.domain.DoseOccurrence
import cz.nowass.dispenser.domain.NextDoseCalculator
import cz.nowass.dispenser.domain.ReservoirEstimate
import cz.nowass.dispenser.domain.Schedule
import cz.nowass.dispenser.ui.components.StatusTone
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import java.time.Instant
import java.time.LocalDate
import java.time.ZonedDateTime

/** Immutable dashboard UI state. */
sealed interface DashboardUiState {
    data object Loading : DashboardUiState

    data class Ready(
        val isDemo: Boolean,
        /** Device clock (simulated in DEMO), in the schedule zone. */
        val now: ZonedDateTime,
        val nextDose: NextDoseUi?,
        val reservoir: ReservoirUi,
        val battery: BatteryUi,
        /** Today plus six following days, computed from the repository's schedules. */
        val week: List<DayPreviewUi>,
    ) : DashboardUiState
}

data class NextDoseUi(
    val scheduleName: String,
    val amountMl: Double,
    val at: ZonedDateTime,
)

data class ReservoirUi(
    val capacityMl: Double,
    val estimatedRemainingMl: Double,
    val fraction: Float,
    /** False when the estimate cannot cover the next planned dose (FR-009). */
    val coversNextDose: Boolean,
    val tone: StatusTone,
)

data class BatteryUi(
    val percent: Int?,
    val isSimulated: Boolean,
    val tone: StatusTone,
)

/** One day of the dashboard week strip. Only real upcoming occurrences are counted. */
data class DayPreviewUi(
    val date: LocalDate,
    val isToday: Boolean,
    val doseCount: Int,
    val totalMl: Double,
)

class DashboardViewModel(repository: DispenserRepository) : ViewModel() {

    val uiState: StateFlow<DashboardUiState> = combine(
        repository.observeDevice(),
        repository.observeSchedules(),
        repository.observeReservoir(),
        ::toUiState,
    ).stateIn(
        scope = viewModelScope,
        started = SharingStarted.WhileSubscribed(5_000),
        initialValue = DashboardUiState.Loading,
    )

    companion object {
        /**
         * UI styling threshold only. Mirrors the PROPOSED 20 % low-stock default
         * (REQUIREMENTS_DRAFT FR-021); not an approved business rule and it does
         * not affect dosing/blocking logic.
         */
        internal const val LOW_STOCK_UI_FRACTION = 0.20

        internal const val WEEK_DAYS = 7

        /** Bound on occurrences enumerated per schedule for the week strip. */
        private const val MAX_OCCURRENCES_PER_SCHEDULE = 64

        fun factory(repository: DispenserRepository): ViewModelProvider.Factory = viewModelFactory {
            initializer { DashboardViewModel(repository) }
        }

        internal fun toUiState(
            device: DeviceStatus,
            schedules: List<Schedule>,
            reservoir: ReservoirEstimate,
        ): DashboardUiState.Ready {
            val next = NextDoseCalculator.nextDose(schedules, device.now)
            val covers = next == null || reservoir.canCover(next.amountMl)
            val isDemo = device.source == DataSource.DEMO
            return DashboardUiState.Ready(
                isDemo = isDemo,
                now = device.now.atZone(device.zone),
                nextDose = next?.let {
                    NextDoseUi(it.scheduleName, it.amountMl, it.scheduledAt.atZone(device.zone))
                },
                reservoir = ReservoirUi(
                    capacityMl = reservoir.capacityMl,
                    estimatedRemainingMl = reservoir.estimatedRemainingMl,
                    fraction = reservoir.fraction.toFloat(),
                    coversNextDose = covers,
                    tone = when {
                        !covers -> StatusTone.Blocked
                        reservoir.fraction < LOW_STOCK_UI_FRACTION -> StatusTone.Low
                        else -> StatusTone.Normal
                    },
                ),
                battery = BatteryUi(
                    percent = device.batteryPercent,
                    isSimulated = isDemo,
                    // No battery thresholds are approved yet; only distinguish unknown.
                    tone = if (device.batteryPercent == null) StatusTone.Unknown else StatusTone.Normal,
                ),
                week = weekPreview(schedules, device),
            )
        }

        /**
         * Upcoming occurrences (strictly after the device clock) for today and the
         * next six days, by chaining the existing domain [NextDoseCalculator].
         */
        internal fun weekPreview(schedules: List<Schedule>, device: DeviceStatus): List<DayPreviewUi> {
            val today = device.now.atZone(device.zone).toLocalDate()
            val endExclusive: Instant = today.plusDays(WEEK_DAYS.toLong()).atStartOfDay(device.zone).toInstant()

            val occurrences = schedules.filter { it.enabled }.flatMap { schedule ->
                buildList<DoseOccurrence> {
                    var cursor = device.now
                    while (size < MAX_OCCURRENCES_PER_SCHEDULE) {
                        val occ = NextDoseCalculator.nextOccurrence(schedule, cursor) ?: break
                        if (!occ.scheduledAt.isBefore(endExclusive)) break
                        add(occ)
                        cursor = occ.scheduledAt
                    }
                }
            }
            val byDate = occurrences.groupBy { it.localDate }
            return (0 until WEEK_DAYS).map { offset ->
                val date = today.plusDays(offset.toLong())
                val day = byDate[date].orEmpty()
                DayPreviewUi(
                    date = date,
                    isToday = offset == 0,
                    doseCount = day.size,
                    totalMl = day.sumOf { it.amountMl },
                )
            }
        }
    }
}
