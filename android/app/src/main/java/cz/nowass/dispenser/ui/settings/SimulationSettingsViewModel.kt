package cz.nowass.dispenser.ui.settings

import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import cz.nowass.dispenser.data.CommandResult
import cz.nowass.dispenser.data.DispenserRepository
import cz.nowass.dispenser.data.SimulationController
import cz.nowass.dispenser.domain.Volume
import cz.nowass.dispenser.domain.simulation.RejectReason
import cz.nowass.dispenser.ui.AmountInput
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import java.time.LocalDateTime
import java.time.ZoneId
import java.time.ZonedDateTime
import java.time.temporal.ChronoUnit

sealed interface SimulationUiState {
    data object Loading : SimulationUiState
    /** No simulation: app uses the phone clock; [phoneNow] is the default start. */
    data class Inactive(val phoneNow: ZonedDateTime) : SimulationUiState
    data class Active(val clock: ZonedDateTime, val startedAt: ZonedDateTime) : SimulationUiState
}

/** Feedback for the last command (mapped to Czech text in the UI). */
sealed interface SimulationMessage {
    data class Processed(val events: Int) : SimulationMessage
    data class Failed(val reason: SimulationFailure) : SimulationMessage
}

enum class SimulationFailure { TARGET_BEFORE_CLOCK, RANGE_TOO_LONG, TOO_MANY_OCCURRENCES, INVALID_CAPACITY, INVALID_VOLUME, NOT_ACTIVE, ALREADY_ACTIVE }

/** Settings "Simulace" card. Only issues commands; all rules live in SimulationEngine. */
class SimulationSettingsViewModel(
    private val simulation: SimulationController,
    repository: DispenserRepository,
) : ViewModel() {

    val uiState: StateFlow<SimulationUiState> = combine(simulation.observeSimulation(), repository.observeDevice()) { sim, device ->
        if (sim == null) {
            SimulationUiState.Inactive(device.now.atZone(device.zone).truncatedTo(ChronoUnit.MINUTES))
        } else {
            SimulationUiState.Active(sim.clock.atZone(device.zone), sim.startedAt.atZone(device.zone))
        }
    }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), SimulationUiState.Loading)

    private val _message = MutableStateFlow<SimulationMessage?>(null)
    val message: StateFlow<SimulationMessage?> = _message.asStateFlow()

    fun consumeMessage() { _message.value = null }

    /** Starts a run at [start] (schedule zone) with capacity/remaining given as ml text. */
    fun start(start: LocalDateTime, zone: ZoneId, capacityText: String, remainingText: String) {
        val capacity = microlitres(capacityText, allowZero = false)
        if (capacity == null) return fail(SimulationFailure.INVALID_CAPACITY)
        val remaining = microlitres(remainingText, allowZero = true)
        if (remaining == null || remaining > capacity) return fail(SimulationFailure.INVALID_VOLUME)
        run { simulation.start(ZonedDateTime.of(start, zone).toInstant(), capacity, remaining) }
    }

    fun advanceHours(hours: Long) = run { simulation.advanceHours(hours) }
    fun advanceDays(days: Long) = run { simulation.advanceDays(days) }
    fun advanceTo(target: LocalDateTime, zone: ZoneId) = run { simulation.advanceTo(ZonedDateTime.of(target, zone).toInstant()) }
    fun outageUntil(target: LocalDateTime, zone: ZoneId) = run { simulation.simulateOutageUntil(ZonedDateTime.of(target, zone).toInstant()) }

    fun reset() {
        viewModelScope.launch { simulation.reset(); _message.value = null }
    }

    private fun run(command: suspend () -> CommandResult) {
        viewModelScope.launch {
            _message.value = when (val r = command()) {
                is CommandResult.Done -> SimulationMessage.Processed(r.newEvents)
                is CommandResult.Rejected -> SimulationMessage.Failed(
                    when (r.reason) {
                        RejectReason.TARGET_BEFORE_CLOCK -> SimulationFailure.TARGET_BEFORE_CLOCK
                        RejectReason.RANGE_TOO_LONG -> SimulationFailure.RANGE_TOO_LONG
                        RejectReason.TOO_MANY_OCCURRENCES -> SimulationFailure.TOO_MANY_OCCURRENCES
                        RejectReason.INVALID_CAPACITY -> SimulationFailure.INVALID_CAPACITY
                        RejectReason.INVALID_VOLUME -> SimulationFailure.INVALID_VOLUME
                    },
                )
                CommandResult.NotActive -> SimulationMessage.Failed(SimulationFailure.NOT_ACTIVE)
                CommandResult.AlreadyActive -> SimulationMessage.Failed(SimulationFailure.ALREADY_ACTIVE)
                // Device outcomes are never produced by the simulation (DEMO and DEVICE are never mixed).
                is CommandResult.Device -> SimulationMessage.Failed(SimulationFailure.NOT_ACTIVE)
            }
        }
    }

    private fun fail(reason: SimulationFailure) { _message.value = SimulationMessage.Failed(reason) }

    private fun microlitres(text: String, allowZero: Boolean): Long? {
        val ml = AmountInput.parseMl(text) ?: return null
        if (ml == 0.0) return if (allowZero) 0 else null
        return (Volume.mlToMicrolitres(ml) as? Volume.Conversion.Ok)?.microlitres
    }

    companion object {
        fun factory(simulation: SimulationController, repository: DispenserRepository): ViewModelProvider.Factory = viewModelFactory {
            initializer { SimulationSettingsViewModel(simulation, repository) }
        }
    }
}
