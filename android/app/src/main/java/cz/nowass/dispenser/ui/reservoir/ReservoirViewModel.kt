package cz.nowass.dispenser.ui.reservoir

import cz.nowass.dispenser.domain.ConnectionState
import cz.nowass.dispenser.domain.DataSource
import cz.nowass.dispenser.domain.Freshness
import java.time.Instant

import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import cz.nowass.dispenser.data.CommandResult
import cz.nowass.dispenser.data.DispenserRepository
import cz.nowass.dispenser.domain.Volume
import cz.nowass.dispenser.domain.simulation.LowStockPolicy
import cz.nowass.dispenser.domain.simulation.ReservoirPredictor
import cz.nowass.dispenser.domain.simulation.ReservoirPrediction
import cz.nowass.dispenser.domain.simulation.ReservoirState
import cz.nowass.dispenser.domain.simulation.ReservoirStatus
import cz.nowass.dispenser.ui.AmountInput
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import java.time.ZoneId

sealed interface ReservoirUiState {
    data object Loading : ReservoirUiState

    data class Ready(
        /** Null when unknown (no device, no simulation running). */
        val reservoir: ReservoirState?,
        val status: ReservoirStatus?,
        val lowThresholdPercent: Int,
        val prediction: ReservoirPrediction?,
        val zone: ZoneId,
        val isDemo: Boolean = true,
        /** DEVICE mode: freshness of [reservoir] (never shown as live when cached). */
        val freshness: Freshness = Freshness.NotApplicable,
        val now: Instant = Instant.EPOCH,
        /** DEVICE mode while not connected: refill needs a live session (E12), so it is not offered. */
        val refillNeedsConnection: Boolean = false,
    ) : ReservoirUiState
}

/** DEVICE-mode refill outcomes other than success (DEMO never produces them). */
enum class RefillOutcome {
    /** May have been applied: verified at the next connection; never re-sent automatically, no retry offered. */
    UNCERTAIN,
    /** Provably nothing reached the device. */
    NOT_SENT,
    /** An earlier refill awaits verification; nothing was sent. */
    AWAITING_VERIFICATION,
    /** The device refused; nothing changed. */
    REJECTED,
    /** Refused by the phone (device limits, capacity unknown); nothing was sent. */
    NOT_ACCEPTED,
}

enum class RefillError { INVALID, NOT_POSITIVE, TOO_PRECISE, ABOVE_CAPACITY, NOT_ACTIVE }

/** Reservoir detail: presentation mapping only; rules live in the domain (policy, predictor, engine). */
class ReservoirViewModel(
    private val repository: DispenserRepository,
    private val lowStock: LowStockPolicy = LowStockPolicy(),
    private val predictor: ReservoirPredictor = ReservoirPredictor(),
) : ViewModel() {

    val uiState: StateFlow<ReservoirUiState> = combine(
        repository.observeDevice(),
        repository.observeSchedules(),
        repository.observeReservoir(),
    ) { device, schedules, reservoir ->
        ReservoirUiState.Ready(
            reservoir = reservoir,
            status = reservoir?.status(lowStock),
            lowThresholdPercent = lowStock.thresholdPercent,
            prediction = reservoir?.let { predictor.predict(it, device.now, schedules) },
            zone = device.zone,
            isDemo = device.source == DataSource.DEMO,
            freshness = device.freshness,
            now = device.now,
            refillNeedsConnection = repository.writesRequireConnection && device.connection != ConnectionState.CONNECTED,
        )
    }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), ReservoirUiState.Loading)

    private val _refillDone = MutableStateFlow(false)
    val refillDone: StateFlow<Boolean> = _refillDone.asStateFlow()

    private val _refillOutcome = MutableStateFlow<RefillOutcome?>(null)
    val refillOutcome: StateFlow<RefillOutcome?> = _refillOutcome.asStateFlow()

    fun consumeRefillDone() {
        _refillDone.value = false
        _refillOutcome.value = null
    }

    /** Validates a refill amount (ml text) against the current capacity; null = valid. */
    fun validateRefill(text: String): RefillError? {
        val r = (uiState.value as? ReservoirUiState.Ready)?.reservoir ?: return RefillError.NOT_ACTIVE
        val ml = AmountInput.parseMl(text) ?: return RefillError.INVALID
        return when (val c = Volume.mlToMicrolitres(ml)) {
            is Volume.Conversion.Ok -> if (c.microlitres > r.capacityUl) RefillError.ABOVE_CAPACITY else null
            Volume.Conversion.NotPositive -> RefillError.NOT_POSITIVE
            Volume.Conversion.TooPrecise -> RefillError.TOO_PRECISE
            Volume.Conversion.NotFinite, Volume.Conversion.OutOfRange -> RefillError.INVALID
        }
    }

    fun refill(text: String) {
        if (validateRefill(text) != null) return
        val ul = (Volume.mlToMicrolitres(AmountInput.parseMl(text)!!) as Volume.Conversion.Ok).microlitres
        viewModelScope.launch {
            when (repository.refillReservoir(ul)) {
                is CommandResult.Done -> _refillDone.value = true
                CommandResult.Uncertain -> _refillOutcome.value = RefillOutcome.UNCERTAIN
                CommandResult.NotSent -> _refillOutcome.value = RefillOutcome.NOT_SENT
                CommandResult.AwaitingVerification -> _refillOutcome.value = RefillOutcome.AWAITING_VERIFICATION
                is CommandResult.DeviceRejected -> _refillOutcome.value = RefillOutcome.REJECTED
                CommandResult.NotAccepted -> _refillOutcome.value = RefillOutcome.NOT_ACCEPTED
                // DEMO results other than Done: unchanged behavior (nothing shown).
                is CommandResult.Rejected, CommandResult.NotActive, CommandResult.AlreadyActive -> Unit
            }
        }
    }

    companion object {
        fun factory(repository: DispenserRepository, lowStock: LowStockPolicy): ViewModelProvider.Factory = viewModelFactory {
            initializer { ReservoirViewModel(repository, lowStock) }
        }
    }
}
