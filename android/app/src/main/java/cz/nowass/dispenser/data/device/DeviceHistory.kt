package cz.nowass.dispenser.data.device

import cz.nowass.dispenser.device.ops.DeviceErrorCode
import cz.nowass.dispenser.device.ops.HistoryEventRecord
import cz.nowass.dispenser.device.ops.OpResult

/**
 * DEVICE-mode event history (Phase C, docs/ANDROID_BLE_DESIGN.md 16c): pages of the device's own log
 * (GET_HISTORY). Nothing is invented, merged with DEMO data or kept beyond the screen's lifetime; the
 * device is the only source.
 */
sealed interface DeviceHistoryPage {
    /** Newest first; [newestSeq]/[oldestSeq]: the range the device holds (0 = empty). */
    data class Loaded(
        val events: List<HistoryEventRecord>,
        val newestSeq: Long,
        val oldestSeq: Long,
        val faults: Boolean,
    ) : DeviceHistoryPage

    /** The firmware has no GET_HISTORY (UNKNOWN_TYPE / UNAVAILABLE): permanent for that firmware. */
    data object Unsupported : DeviceHistoryPage

    /** No connection to the device could be made; nothing was read. */
    data object NotConnected : DeviceHistoryPage

    /** The request failed (refused, answer lost); nothing is shown as history for it. */
    data class Failed(val code: Int?) : DeviceHistoryPage
}

fun interface DeviceHistorySource {
    /** One page below [beforeSeq] (0 = newest). Connects first (the application-wide session is reused). */
    suspend fun page(beforeSeq: Long): DeviceHistoryPage
}

/** [DeviceHistorySource] over a [DeviceRepository]. */
class DeviceHistoryLoader(private val device: DeviceRepository) : DeviceHistorySource {
    override suspend fun page(beforeSeq: Long): DeviceHistoryPage {
        if (device.connect() !is ConnectOutcome.Connected) return DeviceHistoryPage.NotConnected
        return when (val r = device.history(beforeSeq)) {
            is OpResult.Ok -> DeviceHistoryPage.Loaded(r.value.events, r.value.newestSeq, r.value.oldestSeq, r.value.faults)
            is OpResult.Rejected -> when (r.error.code) {
                DeviceErrorCode.UNKNOWN_TYPE, DeviceErrorCode.UNAVAILABLE -> DeviceHistoryPage.Unsupported
                else -> DeviceHistoryPage.Failed(r.error.code.wire)
            }
            is OpResult.NotSent -> DeviceHistoryPage.NotConnected
            is OpResult.Uncertain, is OpResult.Refused -> DeviceHistoryPage.Failed(null)
        }
    }
}
