package cz.nowass.dispenser.ui.devices

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import cz.nowass.dispenser.R
import cz.nowass.dispenser.data.device.PrimeController
import cz.nowass.dispenser.data.device.PrimeOutcome
import cz.nowass.dispenser.data.device.PrimeSupport
import cz.nowass.dispenser.device.ops.PrimeStatusRecord
import cz.nowass.dispenser.device.ops.PrimeStopReason
import cz.nowass.dispenser.device.ops.PrimeVerdict
import cz.nowass.dispenser.ui.theme.Spacing
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

const val PRIME_SECTION_TEST_TAG = "prime_section"
const val PRIME_STATE_TEST_TAG = "prime_state"
const val PRIME_BUTTON_TEST_TAG = "prime_button"

/** The pump state as the screen may claim it; RUNNING / STOPPED only from the device's latest PrimeStatus. */
enum class PrimePhase { UNKNOWN, STARTING, STOPPING, RUNNING, STOPPED }

data class PrimeUiState(
    /** null = not known yet (the section stays hidden). */
    val support: PrimeSupport? = null,
    val connected: Boolean = false,
    val phase: PrimePhase = PrimePhase.UNKNOWN,
    /** The device's latest PrimeStatus (elapsed time, maximum, stop reason). */
    val last: PrimeStatusRecord? = null,
    /** The last PRIME_START refusal (null = none). */
    val refusal: PrimeVerdict? = null,
    /** The last request got no confirmed answer: the state is UNKNOWN (never assumed STOPPED). */
    val failed: Boolean = false,
) {
    val offered: Boolean get() = support == PrimeSupport.Supported
    val canStart: Boolean get() = offered && connected && phase == PrimePhase.STOPPED
    /** STOP also when the state is unknown on a live connection (the safe direction; it re-syncs). */
    val canStop: Boolean get() = offered && connected && (phase == PrimePhase.RUNNING || (phase == PrimePhase.UNKNOWN && failed))
}

/**
 * Pump priming (BLE_PROTOCOL.md 10.9) on the pump screen: one START/STOP button, PRIME_STATUS once per
 * second while running (lease heartbeat), a best-effort PRIME_STOP when the screen goes away ([leave]).
 * Every command bumps [epoch]: an answer that arrives after a newer command is dropped, so a late
 * heartbeat can never overwrite the result of a STOP. START is single flight and never retried.
 */
class PrimeViewModel(private val controller: PrimeController) : ViewModel() {
    private val _state = MutableStateFlow(PrimeUiState())
    val state: StateFlow<PrimeUiState> = _state.asStateFlow()

    @Volatile private var epoch = 0L
    @Volatile private var heartbeat: Job? = null

    init {
        viewModelScope.launch {
            var was = false
            controller.connected.collect { up ->
                if (up) {
                    _state.update { it.copy(connected = true) }
                    if (!was) check() // every new connection: the firmware may have changed
                } else {
                    stopHeartbeat()
                    epoch++
                    _state.update { it.copy(connected = false, phase = PrimePhase.UNKNOWN, failed = true) }
                }
                was = up
            }
        }
    }

    private fun check() {
        val e = ++epoch
        _state.update { it.copy(phase = PrimePhase.UNKNOWN, failed = false) }
        viewModelScope.launch {
            val s = controller.support()
            if (e != epoch) return@launch
            _state.update { it.copy(support = s) }
            if (s == PrimeSupport.Supported) {
                val r = controller.status() // a read, never a START
                if (e == epoch) apply(r, fromStart = false)
            }
        }
    }

    /** The primary button: START when confirmed stopped, otherwise STOP. */
    fun toggle() {
        val s = _state.value
        when {
            s.canStart -> start()
            s.canStop -> stop()
        }
    }

    private fun start() {
        val e = ++epoch
        _state.update { it.copy(phase = PrimePhase.STARTING, refusal = null, failed = false) }
        viewModelScope.launch {
            val r = controller.start()
            if (e == epoch) apply(r, fromStart = true)
        }
    }

    private fun stop() {
        stopHeartbeat() // no heartbeat is queued after this; one already on the link is answered first
        val e = ++epoch
        _state.update { it.copy(phase = PrimePhase.STOPPING) }
        viewModelScope.launch {
            val r = controller.stop()
            if (e == epoch) apply(r, fromStart = false)
        }
    }

    /**
     * The screen was left or the app went to the background: PRIME_STOP when the pump may run (best
     * effort, on the controller's detached scope; the device also stops after its 3 s lease).
     */
    fun leave() {
        stopHeartbeat()
        val s = _state.value
        val mayRun = s.offered && s.connected &&
            (s.phase == PrimePhase.RUNNING || s.phase == PrimePhase.STARTING || (s.phase == PrimePhase.UNKNOWN && s.failed))
        if (!mayRun) return
        val e = ++epoch
        _state.update { it.copy(phase = PrimePhase.STOPPING) }
        controller.detached.launch {
            val r = controller.stop()
            if (e == epoch) apply(r, fromStart = false)
        }
    }

    override fun onCleared() = leave()

    private fun apply(r: PrimeOutcome, fromStart: Boolean) {
        when (r) {
            is PrimeOutcome.Status -> {
                val rec = r.record
                _state.update {
                    it.copy(
                        phase = if (rec.running) PrimePhase.RUNNING else PrimePhase.STOPPED, last = rec, failed = false,
                        refusal = if (fromStart && rec.verdict != PrimeVerdict.OK) rec.verdict else it.refusal,
                    )
                }
                if (rec.running) startHeartbeat() else stopHeartbeat()
            }
            PrimeOutcome.Failed -> {
                stopHeartbeat()
                _state.update { it.copy(phase = PrimePhase.UNKNOWN, failed = true) }
            }
            PrimeOutcome.Unsupported -> {
                stopHeartbeat()
                _state.update { it.copy(support = PrimeSupport.Unsupported, phase = PrimePhase.UNKNOWN) }
            }
            PrimeOutcome.InProgress -> {}
        }
    }

    private fun startHeartbeat() {
        if (heartbeat?.isActive == true) return
        val e = epoch
        heartbeat = viewModelScope.launch {
            while (true) {
                delay(HEARTBEAT_MS)
                if (e != epoch || _state.value.phase != PrimePhase.RUNNING) break
                val r = controller.status()
                if (e != epoch) break
                apply(r, fromStart = false)
                if (_state.value.phase != PrimePhase.RUNNING) break
            }
        }
    }

    private fun stopHeartbeat() {
        heartbeat?.cancel()
        heartbeat = null
    }

    companion object {
        /** PRIME_STATUS interval while running (the device's lease is 3 s). */
        const val HEARTBEAT_MS = 1_000L
    }
}

/** The "Zavodnění pumpy" section: hidden until the firmware's support is known. */
@Composable
fun PrimeSection(state: PrimeUiState, onToggle: () -> Unit) {
    if (state.support != PrimeSupport.Supported && state.support != PrimeSupport.Unsupported) return
    Card(Modifier.fillMaxWidth().testTag(PRIME_SECTION_TEST_TAG)) {
        Column(Modifier.padding(Spacing.m), verticalArrangement = Arrangement.spacedBy(Spacing.s)) {
            Text(stringResource(R.string.prime_title), style = MaterialTheme.typography.titleMedium, modifier = Modifier.semantics { heading() })
            if (state.support == PrimeSupport.Unsupported) {
                Text(stringResource(R.string.prime_unsupported))
                return@Column
            }
            Text(stringResource(R.string.prime_intro), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            Text(stateText(state), style = MaterialTheme.typography.titleMedium, modifier = Modifier.testTag(PRIME_STATE_TEST_TAG))
            val last = state.last
            if (last != null && last.maxMs > 0 && (state.phase == PrimePhase.RUNNING || state.phase == PrimePhase.STOPPED)) {
                val sec = (last.elapsedMs / 1000).toInt()
                val max = (last.maxMs / 1000).toInt()
                if (state.phase == PrimePhase.RUNNING) Text(stringResource(R.string.prime_elapsed_running, sec, max))
                else if (last.elapsedMs > 0) Text(stringResource(R.string.prime_elapsed_last, sec, max))
            }
            val stopping = state.canStop || state.phase == PrimePhase.STOPPING
            Button(
                onClick = onToggle,
                enabled = state.canStart || state.canStop,
                colors = if (stopping) ButtonDefaults.buttonColors(containerColor = MaterialTheme.colorScheme.error, contentColor = MaterialTheme.colorScheme.onError)
                else ButtonDefaults.buttonColors(),
                modifier = Modifier.fillMaxWidth().testTag(PRIME_BUTTON_TEST_TAG),
            ) { Text(stringResource(if (stopping) R.string.prime_stop else R.string.prime_start)) }
            state.refusal?.let { Text(refusalText(it), color = MaterialTheme.colorScheme.error) }
            if (state.phase == PrimePhase.STOPPED && last != null && state.refusal == null &&
                (last.stopReason != PrimeStopReason.NONE || last.elapsedMs > 0)
            ) {
                Text(stopReasonText(last))
                Text(stringResource(R.string.prime_reminder), style = MaterialTheme.typography.bodyMedium)
            }
        }
    }
}

@Composable
private fun stateText(s: PrimeUiState): String = stringResource(
    when (s.phase) {
        PrimePhase.RUNNING -> R.string.prime_state_running
        PrimePhase.STOPPED -> R.string.prime_state_stopped
        PrimePhase.STARTING -> R.string.prime_state_starting
        PrimePhase.STOPPING -> R.string.prime_state_stopping
        PrimePhase.UNKNOWN -> when {
            !s.connected -> R.string.prime_state_unknown_disconnected
            s.failed -> R.string.prime_state_unknown
            else -> R.string.prime_state_reading
        }
    },
)

@Composable
private fun stopReasonText(r: PrimeStatusRecord): String = when (r.stopReason) {
    PrimeStopReason.MAX_TIME -> stringResource(R.string.prime_stop_max_time, (r.maxMs / 1000).toInt())
    PrimeStopReason.NONE, PrimeStopReason.USER -> stringResource(R.string.prime_stop_user)
    PrimeStopReason.HEARTBEAT -> stringResource(R.string.prime_stop_heartbeat)
    PrimeStopReason.DISCONNECT -> stringResource(R.string.prime_stop_disconnect)
    PrimeStopReason.SESSION -> stringResource(R.string.prime_stop_session)
    PrimeStopReason.FAULT -> stringResource(R.string.prime_stop_fault)
    PrimeStopReason.DOSE -> stringResource(R.string.prime_stop_dose)
    PrimeStopReason.UNKNOWN -> stringResource(R.string.prime_stop_unknown)
}

@Composable
private fun refusalText(v: PrimeVerdict): String = stringResource(
    when (v) {
        PrimeVerdict.NOT_CAPABLE -> R.string.prime_refused_not_capable
        PrimeVerdict.BATTERY -> R.string.prime_refused_battery
        PrimeVerdict.DOSE_SOON -> R.string.prime_refused_dose_soon
        PrimeVerdict.RESERVOIR -> R.string.prime_refused_reservoir
        PrimeVerdict.FAULT -> R.string.prime_refused_fault
        PrimeVerdict.BUSY -> R.string.prime_refused_busy
        PrimeVerdict.OK, PrimeVerdict.UNKNOWN -> R.string.prime_refused_unknown
    },
)
