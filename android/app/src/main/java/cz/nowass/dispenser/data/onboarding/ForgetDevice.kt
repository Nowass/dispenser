package cz.nowass.dispenser.data.onboarding

import cz.nowass.dispenser.crypto.credentials.CredentialStore
import cz.nowass.dispenser.data.mode.AppModeController
import cz.nowass.dispenser.data.mode.DeviceAddressStore
import cz.nowass.dispenser.device.transport.DeviceAssociator
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.withContext

/**
 * "Forget device" on this phone (docs/ANDROID_BLE_DESIGN.md 11.1 Deletion, review condition 4).
 *
 * Local only: ends a live session, deletes this phone's credential for the device (confirmed and staged
 * records with their Keystore keys), removes the stored address and the CDM association, and returns to
 * DEMO with no device selected. It does NOT revoke this phone on the device: the device keeps the slot
 * until another enrolled phone removes it (PHONE_REMOVE) or a recovery replaces all phones.
 *
 * Kept on purpose: the pending-operations journal and the device cache. Unresolved operations are never
 * dropped silently; they become resolvable again if this phone is enrolled with the device again
 * (invitation or recovery), and nothing is ever re-sent automatically.
 */
class ForgetDevice(
    private val credentials: CredentialStore,
    private val addresses: DeviceAddressStore,
    private val associator: DeviceAssociator,
    private val modes: AppModeController,
    private val endSession: suspend () -> Unit,
    private val io: CoroutineDispatcher = Dispatchers.IO,
) {
    suspend operator fun invoke(deviceId: String) {
        runCatching { endSession() }
        withContext(io) { credentials.delete(deviceId) }
        addresses.addresses.first()[deviceId]?.let { runCatching { associator.disassociate(it) } }
        addresses.removeAddress(deviceId)
        modes.clearSelection()
    }
}
