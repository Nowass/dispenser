package cz.nowass.dispenser.device.transport

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.companion.AssociationInfo
import android.companion.AssociationRequest
import android.companion.BluetoothLeDeviceFilter
import android.companion.CompanionDeviceManager
import android.content.Context
import android.content.Intent
import android.content.IntentSender
import android.content.pm.PackageManager
import android.os.Build
import android.os.ParcelUuid
import kotlinx.coroutines.suspendCancellableCoroutine
import java.util.concurrent.Executor
import kotlin.coroutines.resume

/** First step of a Companion Device Manager association (docs/ANDROID_BLE_DESIGN.md 4.1, E8). */
sealed interface AssociationStart {
    /** The system dialog to show the user (launched by the UI; its result goes to [DeviceAssociator.addressFrom]). */
    class NeedsUser(val intentSender: IntentSender) : AssociationStart
    /** CDM is not available on this phone (no companion_device_setup feature). */
    data object Unsupported : AssociationStart
    data class Failed(val reason: String) : AssociationStart
}

/**
 * Finds the dispenser through the Companion Device Manager. The association (and its address) is only
 * a locator, never an identity: the device is authenticated by the Noise handshake alone (4.1).
 */
interface DeviceAssociator {
    suspend fun start(): AssociationStart
    /** The selected device's public address (upper case, `XX:XX:…`) from the dialog result; null if cancelled. */
    fun addressFrom(result: Intent?): String?
    /** Removes the association of [address] (best effort; "Forget device"). */
    fun disassociate(address: String)
}

/** [DeviceAssociator] over [CompanionDeviceManager]; single device, filtered by the Dispenser service UUID. */
class AndroidDeviceAssociator(private val context: Context) : DeviceAssociator {
    private val cdm: CompanionDeviceManager? get() =
        if (context.packageManager.hasSystemFeature(PackageManager.FEATURE_COMPANION_DEVICE_SETUP)) {
            context.getSystemService(CompanionDeviceManager::class.java)
        } else {
            null
        }

    private fun request(): AssociationRequest = AssociationRequest.Builder()
        .addDeviceFilter(
            BluetoothLeDeviceFilter.Builder()
                .setScanFilter(ScanFilter.Builder().setServiceUuid(ParcelUuid(DispenserGatt.SERVICE)).build())
                .build(),
        )
        .setSingleDevice(true)
        .build()

    override suspend fun start(): AssociationStart {
        val manager = cdm ?: return AssociationStart.Unsupported
        return suspendCancellableCoroutine { cont ->
            fun done(r: AssociationStart) { if (cont.isActive) cont.resume(r) }
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                manager.associate(request(), Executor { it.run() }, object : CompanionDeviceManager.Callback() {
                    override fun onAssociationPending(intentSender: IntentSender) = done(AssociationStart.NeedsUser(intentSender))
                    override fun onAssociationCreated(associationInfo: AssociationInfo) = Unit // the result arrives with the dialog's Intent
                    override fun onFailure(error: CharSequence?) = done(AssociationStart.Failed(error?.toString() ?: "association failed"))
                })
            } else {
                @Suppress("DEPRECATION")
                manager.associate(request(), object : CompanionDeviceManager.Callback() {
                    @Deprecated("API < 33 path")
                    override fun onDeviceFound(intentSender: IntentSender) = done(AssociationStart.NeedsUser(intentSender))
                    override fun onFailure(error: CharSequence?) = done(AssociationStart.Failed(error?.toString() ?: "association failed"))
                }, null)
            }
        }
    }

    @SuppressLint("MissingPermission") // reading the address of the selected device needs no permission
    override fun addressFrom(result: Intent?): String? {
        result ?: return null
        val address = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            result.getParcelableExtra(CompanionDeviceManager.EXTRA_ASSOCIATION, AssociationInfo::class.java)?.deviceMacAddress?.toString()
        } else {
            @Suppress("DEPRECATION")
            when (val extra = result.getParcelableExtra<android.os.Parcelable>(CompanionDeviceManager.EXTRA_DEVICE)) {
                is ScanResult -> extra.device?.address
                is BluetoothDevice -> extra.address
                else -> null
            }
        }
        return address?.uppercase()?.takeIf { ADDRESS.matches(it) }
    }

    override fun disassociate(address: String) {
        val manager = cdm ?: return
        runCatching {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                manager.myAssociations.filter { it.deviceMacAddress?.toString()?.equals(address, ignoreCase = true) == true }
                    .forEach { manager.disassociate(it.id) }
            } else {
                @Suppress("DEPRECATION")
                manager.disassociate(address)
            }
        }
    }

    private companion object {
        val ADDRESS = Regex("[0-9A-F]{2}(:[0-9A-F]{2}){5}")
    }
}
