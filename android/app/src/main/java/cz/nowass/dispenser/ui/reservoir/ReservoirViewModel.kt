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
        /** DEVICE mode: capacity and current amount can be set (initialization / capacity change). */
        val configurable: Boolean = false,
        /** DEVICE mode: [lowThresholdPercent] is the device's own value (false: the app default, not read yet). */
        val thresholdFromDevice: Boolean = false,
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

/** Outcome of a LOW-threshold change (DEVICE mode). */
enum class ThresholdOutcome { DONE, UNCERTAIN, NOT_SENT, AWAITING_VERIFICATION, REJECTED, NOT_ACCEPTED }

enum class RefillError { INVALID, NOT_POSITIVE, TOO_PRECISE, ABOVE_CAPACITY, NOT_ACTIVE, OUT_OF_RANGE }

/** Field errors of the reservoir configuration (capacity + current amount); both null = valid. */
data class ReservoirConfigErrors(val capacity: RefillError?, val remaining: RefillError?) {
    val valid: Boolean get() = capacity == null && remaining == null
}

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
        repository.observeLowStockPercent(),
    ) { device, schedules, reservoir, deviceThreshold ->
        // DEVICE mode: the device's own threshold decides LOW (as the firmware's notification does)
        val policy = deviceThreshold?.takeIf { it in 0..100 }?.let { LowStockPolicy(it) } ?: lowStock
        ReservoirUiState.Ready(
            reservoir = reservoir,
            status = reservoir?.status(policy),
            lowThresholdPercent = policy.thresholdPercent,
            thresholdFromDevice = deviceThreshold != null,
            prediction = reservoir?.let { predictor.predict(it, device.now, schedules) },
            zone = device.zone,
            isDemo = device.source == DataSource.DEMO,
            freshness = device.freshness,
            now = device.now,
            refillNeedsConnection = repository.writesRequireConnection && device.connection != ConnectionState.CONNECTED,
            configurable = device.source == DataSource.DEVICE,
        )
    }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), ReservoirUiState.Loading)

    init {
        viewModelScope.launch { repository.refreshReservoirSettings() } // DEVICE: the live threshold
    }

    private val _thresholdOutcome = MutableStateFlow<ThresholdOutcome?>(null)
    val thresholdOutcome: StateFlow<ThresholdOutcome?> = _thresholdOutcome.asStateFlow()
    private val _thresholdBusy = MutableStateFlow(false)
    val thresholdBusy: StateFlow<Boolean> = _thresholdBusy.asStateFlow()

    /** A LOW threshold (whole percent 1–99, as the firmware accepts); null = invalid. */
    fun parseThreshold(text: String): Int? = text.trim().toIntOrNull()?.takeIf { it in 1..99 }

    /** DEVICE mode: one SET_CONFIG of the LOW threshold; ignored while one is in flight. */
    fun setLowStock(text: String) {
        val p = parseThreshold(text) ?: return
        if (!_thresholdBusy.compareAndSet(false, true)) return
        _thresholdOutcome.value = null
        viewModelScope.launch {
            _thresholdOutcome.value = when (val r = repository.setLowStockPercent(p)) {
                is CommandResult.Done -> ThresholdOutcome.DONE
                CommandResult.Uncertain -> ThresholdOutcome.UNCERTAIN
                CommandResult.AwaitingVerification -> ThresholdOutcome.AWAITING_VERIFICATION
                is CommandResult.DeviceRejected -> ThresholdOutcome.REJECTED
                CommandResult.NotAccepted -> ThresholdOutcome.NOT_ACCEPTED
                CommandResult.NotSent, CommandResult.NotActive, CommandResult.AlreadyActive, is CommandResult.Rejected -> ThresholdOutcome.NOT_SENT
            }
            _thresholdBusy.value = false
        }
    }

    private val _refillDone = MutableStateFlow(false)
    val refillDone: StateFlow<Boolean> = _refillDone.asStateFlow()

    private val _refillOutcome = MutableStateFlow<RefillOutcome?>(null)
    val refillOutcome: StateFlow<RefillOutcome?> = _refillOutcome.asStateFlow()

    private val _configured = MutableStateFlow(false)
    /** The device confirmed a capacity/amount configuration. */
    val configured: StateFlow<Boolean> = _configured.asStateFlow()

    fun consumeRefillDone() {
        _refillDone.value = false
        _configured.value = false
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
            if (outcome(repository.refillReservoir(ul))) _refillDone.value = true
        }
    }

    /**
     * Validates a capacity and a current amount (ml texts). The firmware requires
     * 0 < amount <= capacity <= u32 µl: an empty reservoir (0 ml) is refused here and never replaced by
     * another value (a later firmware may accept it).
     */
    fun validateConfigure(capacityText: String, remainingText: String): ReservoirConfigErrors {
        val capacity = parse(capacityText)
        val capError = when {
            capacity == null -> RefillError.INVALID
            capacity is Volume.Conversion.Ok && capacity.microlitres > CAPACITY_MAX_UL -> RefillError.OUT_OF_RANGE
            else -> errorOf(capacity)
        }
        val remaining = parse(remainingText)
        val remError = when {
            remaining == null -> RefillError.INVALID
            remaining !is Volume.Conversion.Ok -> errorOf(remaining)
            capacity is Volume.Conversion.Ok && capError == null && remaining.microlitres > capacity.microlitres -> RefillError.ABOVE_CAPACITY
            else -> null
        }
        return ReservoirConfigErrors(capError, remError)
    }

    /** Sends the capacity and current amount (DEVICE mode); only valid input is sent. */
    fun configure(capacityText: String, remainingText: String) {
        if (!validateConfigure(capacityText, remainingText).valid) return
        val cap = (parse(capacityText) as Volume.Conversion.Ok).microlitres
        val rem = (parse(remainingText) as Volume.Conversion.Ok).microlitres
        viewModelScope.launch {
            if (outcome(repository.configureReservoir(cap, rem))) _configured.value = true
        }
    }

    private fun parse(text: String): Volume.Conversion? = AmountInput.parseMl(text)?.let { Volume.mlToMicrolitres(it) }

    private fun errorOf(c: Volume.Conversion): RefillError? = when (c) {
        is Volume.Conversion.Ok -> null
        Volume.Conversion.NotPositive -> RefillError.NOT_POSITIVE
        Volume.Conversion.TooPrecise -> RefillError.TOO_PRECISE
        Volume.Conversion.NotFinite, Volume.Conversion.OutOfRange -> RefillError.INVALID
    }

    /** Maps a refill/configure result to the shared outcome; true = confirmed by the device (or DEMO done). */
    private fun outcome(r: CommandResult): Boolean {
        when (r) {
            is CommandResult.Done -> return true
            CommandResult.Uncertain -> _refillOutcome.value = RefillOutcome.UNCERTAIN
            CommandResult.NotSent -> _refillOutcome.value = RefillOutcome.NOT_SENT
            CommandResult.AwaitingVerification -> _refillOutcome.value = RefillOutcome.AWAITING_VERIFICATION
            is CommandResult.DeviceRejected -> _refillOutcome.value = RefillOutcome.REJECTED
            CommandResult.NotAccepted -> _refillOutcome.value = RefillOutcome.NOT_ACCEPTED
            // DEMO results other than Done: unchanged behavior (nothing shown).
            is CommandResult.Rejected, CommandResult.NotActive, CommandResult.AlreadyActive -> Unit
        }
        return false
    }

    companion object {
        /** REFILL carries capacity_ul as a u32 (BLE_PROTOCOL 0x42). */
        const val CAPACITY_MAX_UL = 0xFFFF_FFFFL

        fun factory(repository: DispenserRepository, lowStock: LowStockPolicy): ViewModelProvider.Factory = viewModelFactory {
            initializer { ReservoirViewModel(repository, lowStock) }
        }
    }
}
