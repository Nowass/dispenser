package cz.nowass.dispenser.data.device

import cz.nowass.dispenser.data.AppSettingsRepository
import cz.nowass.dispenser.data.update.FirmwareUpdateChecker
import cz.nowass.dispenser.device.ops.LocalRefusal
import cz.nowass.dispenser.device.ops.OpResult
import cz.nowass.dispenser.device.ops.StartUpdateRequest
import cz.nowass.dispenser.device.ops.UpdateStatusRecord
import cz.nowass.dispenser.device.ops.UpdateVerdictRecord
import cz.nowass.dispenser.device.session.SessionState
import cz.nowass.dispenser.domain.update.FirmwareCheckResult
import cz.nowass.dispenser.domain.update.FirmwareOffer
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map

/** GET_UPDATE_STATUS as the firmware screen sees it. */
sealed interface FirmwareStatusOutcome {
    data class Status(val record: UpdateStatusRecord) : FirmwareStatusOutcome
    /** Firmware < 0.3.0 (or unknown): no OTA update; one USB installation is needed. Nothing was sent. */
    data object Unsupported : FirmwareStatusOutcome
    data object NotConnected : FirmwareStatusOutcome
    /** The device refused or the answer was lost. */
    data object Failed : FirmwareStatusOutcome
}

/** START_UPDATE as the firmware screen sees it. */
sealed interface StartUpdateOutcome {
    /** The device answered; ACCEPTED ends the session (expected), any other verdict is a refusal. */
    data class Answered(val verdict: UpdateVerdictRecord) : StartUpdateOutcome
    /** No answer: the device may have started; the status after the next connection tells. Not repeated. */
    data object Uncertain : StartUpdateOutcome
    data object NotConnected : StartUpdateOutcome
    data object Unsupported : StartUpdateOutcome
    /** Nothing sent: the offer does not fit the device's limits, or a request is already running. */
    data object NotSent : StartUpdateOutcome
    /** An error frame from the device (nothing installed). */
    data class Rejected(val code: Int) : StartUpdateOutcome
}

/**
 * Owner-initiated firmware update of the connected device (docs/FIRMWARE_UPDATE.md 1). The release
 * check runs only on request; START_UPDATE only after the owner's confirmation in the UI.
 */
class FirmwareUpdateController(
    private val repo: DeviceRepository,
    private val checker: FirmwareUpdateChecker,
    private val settings: AppSettingsRepository,
) {
    val connected: Flow<Boolean> = repo.connection.map { st ->
        st is SessionState.Authenticated && st.deviceIdHex == repo.deviceId
    }.distinctUntilChanged()

    val betaChannel: Flow<Boolean> get() = settings.betaFirmwareChannel

    suspend fun setBetaChannel(enabled: Boolean) = settings.setBetaFirmwareChannel(enabled)

    /** Connects (an existing session is reused), then GET_UPDATE_STATUS when the firmware provides it. */
    suspend fun readStatus(): FirmwareStatusOutcome {
        if (repo.connect() !is ConnectOutcome.Connected) return FirmwareStatusOutcome.NotConnected
        return when (val r = repo.getUpdateStatus()) {
            is OpResult.Ok -> FirmwareStatusOutcome.Status(r.value)
            is OpResult.Refused -> if (r.reason.isUnsupported()) FirmwareStatusOutcome.Unsupported else FirmwareStatusOutcome.Failed
            is OpResult.NotSent -> FirmwareStatusOutcome.NotConnected
            is OpResult.Rejected, is OpResult.Uncertain -> FirmwareStatusOutcome.Failed
        }
    }

    suspend fun check(runningVersion: String, beta: Boolean): FirmwareCheckResult = checker.check(runningVersion, beta)

    suspend fun start(offer: FirmwareOffer): StartUpdateOutcome {
        val sha = hex(offer.sha256Hex) ?: return StartUpdateOutcome.NotSent
        return when (val r = repo.startUpdate(StartUpdateRequest(offer.version, offer.tag, offer.size, sha))) {
            is OpResult.Ok -> StartUpdateOutcome.Answered(r.value)
            is OpResult.Uncertain -> StartUpdateOutcome.Uncertain
            is OpResult.NotSent -> StartUpdateOutcome.NotConnected
            is OpResult.Rejected -> StartUpdateOutcome.Rejected(r.error.code.wire)
            is OpResult.Refused -> if (r.reason.isUnsupported()) StartUpdateOutcome.Unsupported else StartUpdateOutcome.NotSent
        }
    }

    private fun LocalRefusal.isUnsupported() = this is LocalRefusal.Unavailable && what == DeviceRepository.UPDATE_UNSUPPORTED

    private fun hex(s: String): ByteArray? =
        if (s.length != 64 || s.any { it !in '0'..'9' && it !in 'a'..'f' }) null
        else ByteArray(32) { s.substring(2 * it, 2 * it + 2).toInt(16).toByte() }
}
