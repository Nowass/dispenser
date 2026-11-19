package cz.nowass.dispenser.data.onboarding

import cz.nowass.dispenser.crypto.credentials.CredentialStore
import cz.nowass.dispenser.data.mode.AppModeController
import cz.nowass.dispenser.data.mode.DeviceAddressStore
import cz.nowass.dispenser.device.transport.DeviceAssociator
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeout

/** The session side of "Forget device" (implemented over DeviceSessionManager); every call is per device. */
interface ForgetSessions {
    /**
     * Runs [cleanup] exclusively with every session open for [deviceId] (F4): an open in progress is
     * waited for and never adopted, new ones are refused, so no credential can be created meanwhile.
     */
    suspend fun <T> exclusive(deviceId: String, cleanup: suspend () -> T): T
    /** Encrypted END_SESSION of the device's live session (best effort; may throw, time out or be cancelled). */
    suspend fun endGracefully(deviceId: String)
    /** Hard close of the device's live session only; synchronous, never throws, cannot be cancelled. */
    fun closeNow(deviceId: String)
    /** An authenticated session of the device is still exposed. */
    fun hasLiveSession(deviceId: String): Boolean
}

sealed interface ForgetResult {
    /**
     * Removed from this phone. [associationRemoved] false: the CDM association (a locator only, no
     * secret) could not be removed; the user can remove it in the system settings.
     */
    data class Forgotten(val associationRemoved: Boolean) : ForgetResult

    /** Not completed; DEVICE mode stays selected and a retry is safe (every step is idempotent). */
    data class Failed(val step: Step) : ForgetResult

    enum class Step {
        /** The session could not be shown closed. */
        SESSION,
        /** A credential record or its Keystore key could not be removed (or reappeared). */
        CREDENTIALS,
        /** The stored address could not be removed. */
        ADDRESS,
        /** The mode selection could not be cleared. */
        MODE,
    }
}

/**
 * "Forget device" on this phone (docs/ANDROID_BLE_DESIGN.md 11.1 Deletion, review condition 4; security
 * review F1/F2).
 *
 * Local only: it does NOT revoke this phone on the device (the device keeps the slot until another
 * enrolled phone removes it or a recovery replaces all phones). The journal and the device cache are
 * kept (unresolved operations are never dropped silently; nothing is re-sent).
 *
 * The whole cleanup runs NonCancellable and exclusively with session opens for the device (F4: an open
 * already in progress — e.g. an enrollment that has not created its key yet — is waited for and not
 * adopted; new and queued ones are refused), so no credential can be created or restored while it runs
 * and none of its checks can be overtaken by an enrollment. Order:
 * 1. graceful END_SESSION (bounded), then — always — a hard close; the manager must expose no session;
 * 2. both credential records and their Keystore keys removed and verified (key first, then file);
 * 3. the CDM association (best effort) and the stored address;
 * 4. a final check that no record reappeared, then DEMO with no device selected.
 * DEMO is selected only after 1, 2 and 4 succeeded. Any failure returns [ForgetResult.Failed] and keeps
 * DEVICE mode (if the credential is already gone, it shows the recovery-required state); no exception
 * escapes; [afterCredentialChange] (credential-health refresh, F3) runs in every case.
 */
class ForgetDevice(
    private val credentials: CredentialStore,
    private val addresses: DeviceAddressStore,
    private val associator: DeviceAssociator,
    private val modes: AppModeController,
    private val sessions: ForgetSessions,
    private val afterCredentialChange: suspend (deviceId: String) -> Unit = {},
    private val io: CoroutineDispatcher = Dispatchers.IO,
    private val gracefulEndMs: Long = 3_000,
) {
    suspend operator fun invoke(deviceId: String): ForgetResult = withContext(NonCancellable) {
        try {
            sessions.exclusive(deviceId) { forget(deviceId) }
        } catch (e: Throwable) {
            // unexpected: reported, never thrown; the state is consistent for a retry
            ForgetResult.Failed(ForgetResult.Step.CREDENTIALS)
        } finally {
            runCatching { afterCredentialChange(deviceId) }
        }
    }

    private suspend fun forget(deviceId: String): ForgetResult {
        // 1. session: graceful attempt bounded, then always the hard close
        try {
            withTimeout(gracefulEndMs) { sessions.endGracefully(deviceId) }
        } catch (e: Throwable) {
            // thrown, timed out or cancelled: the hard close below still runs
        } finally {
            sessions.closeNow(deviceId)
        }
        if (sessions.hasLiveSession(deviceId)) return ForgetResult.Failed(ForgetResult.Step.SESSION)

        // 2. credentials (confirmed and staged) with their keys, verified
        val problem = try {
            withContext(io) { credentials.removeVerified(deviceId) }
        } catch (e: Exception) {
            return ForgetResult.Failed(ForgetResult.Step.CREDENTIALS)
        }
        if (problem != null) return ForgetResult.Failed(ForgetResult.Step.CREDENTIALS)

        // 3. association (locator only; best effort) and the stored address
        val address = try {
            addresses.addresses.first()[deviceId]
        } catch (e: Exception) {
            return ForgetResult.Failed(ForgetResult.Step.ADDRESS)
        }
        val disassociated = address == null || runCatching { associator.disassociate(address) }.getOrDefault(false)
        try {
            addresses.removeAddress(deviceId)
        } catch (e: Exception) {
            return ForgetResult.Failed(ForgetResult.Step.ADDRESS)
        }

        // 4. defense in depth (no open can run here): nothing came back, then DEMO
        val stillThere = withContext(io) { credentials.hasConfirmedRecord(deviceId) || credentials.stagedDevices().contains(deviceId) }
        if (stillThere || sessions.hasLiveSession(deviceId)) return ForgetResult.Failed(ForgetResult.Step.CREDENTIALS)
        try {
            modes.clearSelection()
        } catch (e: Exception) {
            return ForgetResult.Failed(ForgetResult.Step.MODE)
        }
        return ForgetResult.Forgotten(associationRemoved = disassociated)
    }
}
