package cz.nowass.dispenser.ui.history

import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import cz.nowass.dispenser.data.DispenserRepository
import cz.nowass.dispenser.data.device.DeviceHistoryPage
import cz.nowass.dispenser.data.device.DeviceHistorySource
import cz.nowass.dispenser.device.ops.HistoryEventRecord
import cz.nowass.dispenser.domain.ConnectionState
import cz.nowass.dispenser.domain.simulation.HistoryEvent
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.filter
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import java.time.Instant
import java.time.LocalDate
import java.time.ZoneId

data class HistoryDayUi(val date: LocalDate, val events: List<HistoryEvent>)

/** DEVICE mode: one device event with the current name of its schedule (null = deleted / manual). */
data class DeviceHistoryRow(val event: HistoryEventRecord, val scheduleName: String?)

/** [date] null: events recorded without a known time (shown in device order). */
data class DeviceHistoryDay(val date: LocalDate?, val rows: List<DeviceHistoryRow>)

enum class DeviceHistoryProblem { NOT_CONNECTED, UNSUPPORTED, FAILED }

data class DeviceHistoryUi(
    val days: List<DeviceHistoryDay>,
    /** At least one page was read from the device in this screen's lifetime. */
    val loaded: Boolean,
    val loading: Boolean,
    val canLoadMore: Boolean,
    /** Why the last request did not load (events already shown stay valid). */
    val problem: DeviceHistoryProblem?,
    /** The device reported unreadable history slots or lost entries since its last boot. */
    val faults: Boolean,
    val connected: Boolean,
)

sealed interface HistoryUiState {
    data object Loading : HistoryUiState
    data class Ready(val days: List<HistoryDayUi>, val zone: ZoneId) : HistoryUiState

    /** No history source at all (neither local events nor a device log). */
    data object Unavailable : HistoryUiState

    /** DEVICE mode (Phase C): the device's own log, newest first, paged over BLE. */
    data class Device(val ui: DeviceHistoryUi, val zone: ZoneId) : HistoryUiState
}

/** Newest-first history grouped by local date (D11). Presentation only. */
class HistoryViewModel(private val repository: DispenserRepository) : ViewModel() {

    private data class DeviceState(
        val events: List<HistoryEventRecord> = emptyList(),
        val loaded: Boolean = false,
        val loading: Boolean = false,
        val canLoadMore: Boolean = false,
        val problem: DeviceHistoryProblem? = null,
        val faults: Boolean = false,
    )

    private val source: DeviceHistorySource? = repository.deviceHistory
    private val dev = MutableStateFlow(DeviceState())

    val uiState: StateFlow<HistoryUiState> = when {
        source != null -> combine(dev, repository.observeSchedules(), repository.observeDevice()) { d, schedules, device ->
            val names = schedules.associate { it.id to it.name }
            HistoryUiState.Device(
                DeviceHistoryUi(
                    days = groupDevice(d.events, names, device.zone),
                    loaded = d.loaded, loading = d.loading, canLoadMore = d.canLoadMore, problem = d.problem,
                    faults = d.faults, connected = device.connection == ConnectionState.CONNECTED,
                ),
                device.zone,
            )
        }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), HistoryUiState.Loading)
        !repository.historyAvailable -> MutableStateFlow(HistoryUiState.Unavailable)
        else -> combine(repository.observeHistory(), repository.observeDevice()) { events, device ->
            HistoryUiState.Ready(group(events, device.zone), device.zone)
        }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), HistoryUiState.Loading)
    }

    init {
        if (source != null) {
            // Loads by itself only once a session is up (opening the screen never starts a connection);
            // while disconnected the user loads explicitly ([refresh] connects through the shared session).
            viewModelScope.launch {
                repository.observeDevice().map { it.connection == ConnectionState.CONNECTED }.distinctUntilChanged()
                    .filter { it }.first()
                if (!dev.value.loaded && !dev.value.loading) refresh()
            }
        }
    }

    /** Reloads from the newest event (the shown list is replaced only by a successful read). */
    fun refresh() = load(fromNewest = true)

    /** Loads older events below the oldest shown one. */
    fun loadMore() {
        if (dev.value.canLoadMore) load(fromNewest = false)
    }

    private fun load(fromNewest: Boolean) {
        val src = source ?: return
        if (dev.value.loading) return
        dev.update { it.copy(loading = true) }
        viewModelScope.launch {
            var cursor = if (fromNewest) 0L else dev.value.events.minOfOrNull { it.seq } ?: 0L
            val fresh = mutableListOf<HistoryEventRecord>()
            var end = false
            var problem: DeviceHistoryProblem? = null
            var faults = dev.value.faults
            for (i in 0 until BATCH_PAGES) {
                when (val p = src.page(cursor)) {
                    is DeviceHistoryPage.Loaded -> {
                        fresh += p.events
                        faults = p.faults
                        end = p.events.isEmpty() || p.events.last().seq <= p.oldestSeq
                        if (end) break
                        cursor = p.events.last().seq
                    }
                    DeviceHistoryPage.Unsupported -> problem = DeviceHistoryProblem.UNSUPPORTED
                    DeviceHistoryPage.NotConnected -> problem = DeviceHistoryProblem.NOT_CONNECTED
                    is DeviceHistoryPage.Failed -> problem = DeviceHistoryProblem.FAILED
                }
                if (problem != null) break
            }
            val failed = problem
            dev.update { s ->
                // a refresh replaces the list only when the whole batch read succeeded
                val base = if (fromNewest && failed == null) emptyList() else s.events
                s.copy(
                    events = merge(base, fresh),
                    loaded = s.loaded || failed == null || fresh.isNotEmpty(),
                    loading = false,
                    canLoadMore = when (failed) {
                        null -> !end
                        DeviceHistoryProblem.UNSUPPORTED -> false
                        else -> s.canLoadMore || fresh.isNotEmpty()
                    },
                    problem = failed,
                    faults = faults,
                )
            }
        }
    }

    companion object {
        /** Pages (4 events each) per user action: up to 12 events. */
        const val BATCH_PAGES = 3

        fun factory(repository: DispenserRepository): ViewModelProvider.Factory = viewModelFactory {
            initializer { HistoryViewModel(repository) }
        }

        /** Newest first: by time, then by seq for events at the same instant. */
        internal fun group(events: List<HistoryEvent>, zone: ZoneId): List<HistoryDayUi> =
            events
                .sortedWith(compareByDescending<HistoryEvent> { it.time }.thenByDescending { it.seq })
                .groupBy { it.time.atZone(zone).toLocalDate() }
                .map { (date, list) -> HistoryDayUi(date, list) }

        /** One row per seq (repeated or overlapping reads never duplicate), newest (highest seq) first. */
        internal fun merge(a: List<HistoryEventRecord>, b: List<HistoryEventRecord>): List<HistoryEventRecord> =
            (a + b).associateBy { it.seq }.values.sortedByDescending { it.seq }

        /**
         * The device's order (seq) is its chronology; consecutive events of the same local date form a
         * day section (a clock correction can make a date reappear: it is never reordered by time).
         */
        internal fun groupDevice(events: List<HistoryEventRecord>, names: Map<String, String>, zone: ZoneId): List<DeviceHistoryDay> {
            val days = mutableListOf<DeviceHistoryDay>()
            for (e in events.sortedByDescending { it.seq }) {
                val row = DeviceHistoryRow(e, e.scheduleId?.let(names::get))
                val date = e.atEpochS?.let { Instant.ofEpochSecond(it).atZone(zone).toLocalDate() }
                val last = days.lastOrNull()
                if (last != null && last.date == date) {
                    days[days.size - 1] = last.copy(rows = last.rows + row)
                } else {
                    days += DeviceHistoryDay(date, listOf(row))
                }
            }
            return days
        }
    }
}
