package cz.nowass.dispenser.ui.dashboard

import cz.nowass.dispenser.domain.BatteryEstimate
import cz.nowass.dispenser.domain.BatteryReading
import cz.nowass.dispenser.domain.BatteryUnavailable
import cz.nowass.dispenser.domain.Freshness

import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import cz.nowass.dispenser.data.BatteryRefreshResult
import cz.nowass.dispenser.data.DispenserRepository
import cz.nowass.dispenser.domain.DataSource
import cz.nowass.dispenser.domain.DeviceStatus
import cz.nowass.dispenser.domain.DoseOccurrence
import cz.nowass.dispenser.domain.NextDoseCalculator
import cz.nowass.dispenser.domain.Volume
import cz.nowass.dispenser.domain.simulation.LowStockPolicy
import cz.nowass.dispenser.domain.simulation.ReservoirState
import cz.nowass.dispenser.domain.simulation.ReservoirStatus
import cz.nowass.dispenser.domain.Schedule
import cz.nowass.dispenser.ui.components.StatusTone
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
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
        /** Null when the reservoir estimate is unknown (no device, no simulation running). */
        val reservoir: ReservoirUi?,
        val battery: BatteryUi,
        /** Today plus six following days, computed from the repository's schedules. */
        val week: List<DayPreviewUi>,
        /** DEVICE mode: freshness of the reservoir and battery values (one device status record). */
        val freshness: Freshness = Freshness.NotApplicable,
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
    /** DEMO: the simulated value; DEVICE: the estimate from [millivolts] (null = unknown). */
    val percent: Int?,
    val isSimulated: Boolean,
    val tone: StatusTone,
    /** DEVICE: the measured battery voltage; [percent] is then an estimate derived from it. */
    val millivolts: Int? = null,
    /** DEVICE: why no voltage is available (null = never read, or measured). */
    val unavailable: BatteryUnavailable? = null,
    /** Freshness of the battery value (Live/Cached carry the measurement time). */
    val freshness: Freshness = Freshness.NotApplicable,
    /** DEVICE: when the shown reading was physically measured (null = never read). */
    val measuredAt: Instant? = null,
    /** DEVICE: the card requests a new measurement when tapped. */
    val refreshable: Boolean = false,
    /** A requested measurement is in progress (further taps are ignored). */
    val refreshing: Boolean = false,
    /** Outcome of the last requested measurement that did not succeed (null after a success). */
    val refreshMessage: BatteryRefreshMessage? = null,
) {
    val isEstimate: Boolean get() = millivolts != null
}

enum class BatteryRefreshMessage { NOT_CONNECTED, FAILED, UNSUPPORTED }

/** One day of the dashboard week strip. Only real upcoming occurrences are counted. */
data class DayPreviewUi(
    val date: LocalDate,
    val isToday: Boolean,
    val doseCount: Int,
    val totalMl: Double,
)

class DashboardViewModel(private val repository: DispenserRepository) : ViewModel() {

    private data class Refresh(val refreshing: Boolean = false, val message: BatteryRefreshMessage? = null)
    private val refresh = MutableStateFlow(Refresh())

    val uiState: StateFlow<DashboardUiState> = combine(
        repository.observeDevice(),
        repository.observeSchedules(),
        repository.observeReservoir(),
        refresh,
    ) { device, schedules, reservoir, r ->
        val s = toUiState(device, schedules, reservoir)
        s.copy(
            battery = s.battery.copy(
                refreshable = repository.batteryRefreshAvailable && !s.isDemo,
                refreshing = r.refreshing,
                refreshMessage = r.message,
            ),
        )
    }.stateIn(
        scope = viewModelScope,
        started = SharingStarted.WhileSubscribed(5_000),
        initialValue = DashboardUiState.Loading,
    )

    /**
     * Battery card tap (DEVICE): one GET_BATTERY over the live session. Ignored while one is in
     * progress (no overlapping BLE requests) and in DEMO. The reading and its measurement time
     * change only through the device's answer; a failure keeps the previous value (shown stale).
     */
    fun refreshBattery() {
        if (!repository.batteryRefreshAvailable) return
        val current = refresh.value
        if (current.refreshing || !refresh.compareAndSet(current, current.copy(refreshing = true))) return
        viewModelScope.launch {
            val message = when (repository.refreshBattery()) {
                BatteryRefreshResult.Refreshed -> null
                BatteryRefreshResult.NotConnected -> BatteryRefreshMessage.NOT_CONNECTED
                BatteryRefreshResult.Failed -> BatteryRefreshMessage.FAILED
                BatteryRefreshResult.Unsupported -> BatteryRefreshMessage.UNSUPPORTED
            }
            refresh.value = Refresh(refreshing = false, message = message)
        }
    }

    companion object {
        internal const val WEEK_DAYS = 7

        /** Bound on occurrences enumerated per schedule for the week strip. */
        private const val MAX_OCCURRENCES_PER_SCHEDULE = 64

        fun factory(repository: DispenserRepository): ViewModelProvider.Factory = viewModelFactory {
            initializer { DashboardViewModel(repository) }
        }

        internal fun toUiState(
            device: DeviceStatus,
            schedules: List<Schedule>,
            reservoir: ReservoirState?,
            lowStock: LowStockPolicy = LowStockPolicy(),
        ): DashboardUiState.Ready {
            val next = NextDoseCalculator.nextDose(schedules, device.now)
            val nextUl = next?.let { (Volume.mlToMicrolitres(it.amountMl) as? Volume.Conversion.Ok)?.microlitres }
            val covers = reservoir == null || nextUl == null || reservoir.canDispense(nextUl)
            val isDemo = device.source == DataSource.DEMO
            return DashboardUiState.Ready(
                isDemo = isDemo,
                now = device.now.atZone(device.zone),
                nextDose = next?.let {
                    NextDoseUi(it.scheduleName, it.amountMl, it.scheduledAt.atZone(device.zone))
                },
                reservoir = reservoir?.let { r ->
                    ReservoirUi(
                        capacityMl = Volume.microlitresToMl(r.capacityUl),
                        estimatedRemainingMl = r.remainingUl / Volume.MICROLITRES_PER_ML.toDouble(),
                        fraction = (r.remainingUl.toDouble() / r.capacityUl).toFloat(),
                        coversNextDose = covers,
                        // Single shared rule (domain LowStockPolicy / blocked state), no UI-only threshold.
                        tone = when {
                            r.status(lowStock) == ReservoirStatus.BLOCKED || !covers -> StatusTone.Blocked
                            r.status(lowStock) == ReservoirStatus.LOW -> StatusTone.Low
                            else -> StatusTone.Normal
                        },
                    )
                },
                battery = batteryUi(device, isDemo),
                week = weekPreview(schedules, device),
                freshness = device.freshness,
            )
        }

        /**
         * DEMO keeps its simulated percentage. DEVICE: the measured voltage with an estimated
         * percentage, or the reason it is unavailable. No battery thresholds are approved yet (no
         * low-battery tone), and no charging state is shown (no charger telemetry exists).
         */
        internal fun batteryUi(device: DeviceStatus, isDemo: Boolean): BatteryUi {
            val freshness = if (device.battery != null) device.batteryFreshness else device.freshness
            val measuredAt = when (val f = device.batteryFreshness) {
                is Freshness.Live -> f.readAt
                is Freshness.Cached -> f.readAt
                else -> null
            }.takeIf { device.battery != null }
            return when (val b = device.battery) {
                is BatteryReading.Measured -> BatteryUi(
                    percent = BatteryEstimate.percent(b.millivolts),
                    isSimulated = false,
                    tone = StatusTone.Normal,
                    millivolts = b.millivolts,
                    freshness = freshness,
                    measuredAt = measuredAt,
                )
                is BatteryReading.Unavailable -> BatteryUi(null, false, StatusTone.Unknown, unavailable = b.reason, freshness = freshness)
                null -> BatteryUi(
                    percent = device.batteryPercent,
                    isSimulated = isDemo,
                    tone = if (device.batteryPercent == null) StatusTone.Unknown else StatusTone.Normal,
                    freshness = freshness,
                )
            }
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
