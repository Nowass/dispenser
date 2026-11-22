package cz.nowass.dispenser.device.transport

import android.annotation.SuppressLint
import android.bluetooth.BluetoothManager
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.ParcelUuid
import androidx.core.content.ContextCompat
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withTimeoutOrNull
import kotlin.coroutines.resume

/** Outcome of one bounded wait for the owned device's advertising. */
sealed interface ScanOutcome {
    /** The device advertises now (service UUID and associated address matched). Only a locator: nothing is trusted before the IK handshake. */
    data object Found : ScanOutcome
    /** Nothing seen within the time limit (the device sleeps or is out of range). */
    data object Timeout : ScanOutcome
    /** Scanning is not possible here (API < 31, BLUETOOTH_SCAN not granted, no scanner, scan start failed): the caller falls back to direct connects. */
    data class Unavailable(val reason: String) : ScanOutcome
}

/**
 * Foreground-only, bounded, filtered discovery of the already-owned device (docs/ANDROID_BLE_DESIGN.md
 * 4.1 "fallback discovery", used by the automatic connection): never a background or unfiltered scan.
 */
fun interface DeviceScanner {
    suspend fun awaitAdvertising(address: String, timeoutMs: Long): ScanOutcome
}

/**
 * [DeviceScanner] over `BluetoothLeScanner`: one scan filtered by the Dispenser service UUID AND the
 * associated address, low-latency mode, stopped on the first match, on the timeout, on a failure and
 * on cancellation. Needs BLUETOOTH_SCAN (neverForLocation) on API 31+; on API 26–30 a scan would need
 * location access, so it reports [ScanOutcome.Unavailable] there (direct connects still work).
 */
class AndroidDeviceScanner(private val context: Context) : DeviceScanner {
    @SuppressLint("MissingPermission") // checked below; SecurityException is handled
    override suspend fun awaitAdvertising(address: String, timeoutMs: Long): ScanOutcome {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) return ScanOutcome.Unavailable("api")
        if (missingScanPermission(context).isNotEmpty()) return ScanOutcome.Unavailable("permission")
        val scanner = context.getSystemService(BluetoothManager::class.java)?.adapter?.bluetoothLeScanner
            ?: return ScanOutcome.Unavailable("no scanner")
        val filter = try {
            ScanFilter.Builder().setServiceUuid(ParcelUuid(DispenserGatt.SERVICE)).setDeviceAddress(address).build()
        } catch (e: IllegalArgumentException) {
            return ScanOutcome.Unavailable("address")
        }
        val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
        var callback: ScanCallback? = null
        try {
            return withTimeoutOrNull(timeoutMs) {
                suspendCancellableCoroutine { cont ->
                    val cb = object : ScanCallback() {
                        override fun onScanResult(callbackType: Int, result: ScanResult) {
                            if (result.device?.address.equals(address, ignoreCase = true) && cont.isActive) cont.resume(ScanOutcome.Found)
                        }
                        override fun onScanFailed(errorCode: Int) {
                            if (cont.isActive) cont.resume(ScanOutcome.Unavailable("scan failed $errorCode"))
                        }
                    }
                    callback = cb
                    try {
                        scanner.startScan(listOf(filter), settings, cb)
                    } catch (e: SecurityException) {
                        if (cont.isActive) cont.resume(ScanOutcome.Unavailable("permission"))
                    } catch (e: IllegalStateException) { // Bluetooth turned off meanwhile
                        if (cont.isActive) cont.resume(ScanOutcome.Unavailable("adapter off"))
                    }
                }
            } ?: ScanOutcome.Timeout
        } finally {
            callback?.let { cb -> runCatching { scanner.stopScan(cb) } } // every path: found, timeout, failure, cancellation
        }
    }
}

/** android.Manifest.permission.BLUETOOTH_SCAN (API 31+), as a literal for the API 26-30 build. */
const val BLUETOOTH_SCAN = "android.permission.BLUETOOTH_SCAN"

/** The scan permission still to request (empty below API 31, where scanning is not used). */
fun missingScanPermission(context: Context): List<String> =
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S &&
        ContextCompat.checkSelfPermission(context, BLUETOOTH_SCAN) != PackageManager.PERMISSION_GRANTED
    ) listOf(BLUETOOTH_SCAN) else emptyList()
