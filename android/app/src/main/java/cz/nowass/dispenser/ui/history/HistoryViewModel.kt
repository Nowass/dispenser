package cz.nowass.dispenser.ui.history

import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import cz.nowass.dispenser.data.DispenserRepository
import cz.nowass.dispenser.domain.simulation.HistoryEvent
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import java.time.LocalDate
import java.time.ZoneId

data class HistoryDayUi(val date: LocalDate, val events: List<HistoryEvent>)

sealed interface HistoryUiState {
    data object Loading : HistoryUiState
    data class Ready(val days: List<HistoryDayUi>, val zone: ZoneId) : HistoryUiState
}

/** Newest-first history grouped by local date (D11). Presentation only. */
class HistoryViewModel(repository: DispenserRepository) : ViewModel() {

    val uiState: StateFlow<HistoryUiState> = combine(repository.observeHistory(), repository.observeDevice()) { events, device ->
        HistoryUiState.Ready(group(events, device.zone), device.zone)
    }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), HistoryUiState.Loading)

    companion object {
        fun factory(repository: DispenserRepository): ViewModelProvider.Factory = viewModelFactory {
            initializer { HistoryViewModel(repository) }
        }

        /** Newest first: by time, then by seq for events at the same instant. */
        internal fun group(events: List<HistoryEvent>, zone: ZoneId): List<HistoryDayUi> =
            events
                .sortedWith(compareByDescending<HistoryEvent> { it.time }.thenByDescending { it.seq })
                .groupBy { it.time.atZone(zone).toLocalDate() }
                .map { (date, list) -> HistoryDayUi(date, list) }
    }
}
