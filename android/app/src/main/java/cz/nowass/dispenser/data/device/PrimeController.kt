package cz.nowass.dispenser.data.device

import cz.nowass.dispenser.device.ops.LocalRefusal
import cz.nowass.dispenser.device.ops.OpResult
import cz.nowass.dispenser.device.ops.PrimeStatusRecord
import cz.nowass.dispenser.device.session.SessionState
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map

/** Whether the connected firmware provides pump priming (BLE_PROTOCOL.md 8.4). */
sealed interface PrimeSupport {
    data object Supported : PrimeSupport
    /** Firmware < 0.3.3, a pre-release of 0.3.3, or an unknown version: priming is not offered. */
    data object Unsupported : PrimeSupport
    data object NotConnected : PrimeSupport
    /** The version could not be read (refused, lost answer): not offered for now. */
    data object Failed : PrimeSupport
}

/** One PRIME_START / PRIME_STOP / PRIME_STATUS as the pump screen sees it. */
sealed interface PrimeOutcome {
    /** The device's PrimeStatus: the only source of a confirmed RUNNING / STOPPED. */
    data class Status(val record: PrimeStatusRecord) : PrimeOutcome
    /** No confirmed state (lost answer, device error, not connected): the pump state is UNKNOWN. */
    data object Failed : PrimeOutcome
    data object Unsupported : PrimeOutcome
    /** Nothing sent: a PRIME_START is already in flight (single flight). */
    data object InProgress : PrimeOutcome
}

/**
 * Owner-started pump priming (BLE_PROTOCOL.md 10.9): no volume, no history, the reservoir estimate is
 * unchanged. [detached] runs the best-effort PRIME_STOP when the screen goes away (outlives the screen).
 */
class PrimeController(private val repo: DeviceRepository, val detached: CoroutineScope) {
    val connected: Flow<Boolean> = repo.connection.map { st ->
        st is SessionState.Authenticated && st.deviceIdHex == repo.deviceId
    }.distinctUntilChanged()

    /** GET_UPDATE_STATUS `running_version` >= 0.3.3 (never a priming request to older firmware). */
    suspend fun support(): PrimeSupport = when (val r = repo.getUpdateStatus()) {
        is OpResult.Ok -> if (DeviceDispenserRepository.firmwareHasPrime(r.value.runningVersion)) PrimeSupport.Supported else PrimeSupport.Unsupported
        is OpResult.Refused -> if (r.reason.isUnsupported(DeviceRepository.UPDATE_UNSUPPORTED)) PrimeSupport.Unsupported else PrimeSupport.Failed
        is OpResult.NotSent -> PrimeSupport.NotConnected
        is OpResult.Rejected, is OpResult.Uncertain -> PrimeSupport.Failed
    }

    suspend fun start(): PrimeOutcome = map(repo.primeStart())
    suspend fun stop(): PrimeOutcome = map(repo.primeStop())
    suspend fun status(): PrimeOutcome = map(repo.primeStatus())

    private fun map(r: OpResult<PrimeStatusRecord>): PrimeOutcome = when (r) {
        is OpResult.Ok -> PrimeOutcome.Status(r.value)
        is OpResult.Refused -> when {
            r.reason == LocalRefusal.InProgress -> PrimeOutcome.InProgress
            r.reason.isUnsupported(DeviceRepository.PRIME_UNSUPPORTED) || r.reason.isUnsupported(DeviceRepository.UPDATE_UNSUPPORTED) -> PrimeOutcome.Unsupported
            else -> PrimeOutcome.Failed
        }
        is OpResult.NotSent, is OpResult.Rejected, is OpResult.Uncertain -> PrimeOutcome.Failed
    }

    private fun LocalRefusal.isUnsupported(what: String) = this is LocalRefusal.Unavailable && this.what == what
}
