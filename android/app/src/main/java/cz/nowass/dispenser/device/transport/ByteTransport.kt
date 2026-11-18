package cz.nowass.dispenser.device.transport

import cz.nowass.dispenser.ble.protocol.BleProtocol
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.StateFlow

/**
 * One GATT connection to a dispenser as a byte pipe of protocol fragments (docs/ANDROID_BLE_DESIGN.md
 * 2.2, 4.2–4.3). Created ready (service found, MTU negotiated, Info read, notifications enabled) by
 * [GattTransportFactory]; closed exactly once. A new connection always is a new transport with a
 * new [generation]; nothing of an old one can reach a new one.
 */
interface ByteTransport {
    /** Strictly increasing per connection attempt within the process. */
    val generation: Long

    /** ATT MTU in effect (negotiated; 23 if none was reported). */
    val mtu: Int

    /** The Info characteristic read during setup (BLE_PROTOCOL.md 7.1). */
    val info: BleProtocol.Info

    /** Notifications (one protocol fragment each), then exactly one [TransportEvent.Closed]. */
    val events: Flow<TransportEvent>

    /** Null while open; the reason once closed. */
    val closeReason: StateFlow<CloseReason?>

    /**
     * Writes one fragment with write-with-response and returns after the write response
     * (protocol flow control: the next fragment only after the response). At most one write in
     * flight; throws [TransportClosedException] if the transport is or becomes closed, or the write
     * fails or times out (the transport is then closed).
     */
    suspend fun writeFragment(fragment: ByteArray)

    /** Idempotent: disconnects and releases the GATT client; later callbacks are ignored. */
    fun close(reason: CloseReason = CloseReason.Local)
}

sealed interface TransportEvent {
    class Notification(val fragment: ByteArray) : TransportEvent
    data class Closed(val reason: CloseReason) : TransportEvent
}

/** Why a connection ended or could not be set up. Never contains payload bytes. */
sealed interface CloseReason {
    /** Closed by this app ([ByteTransport.close]). */
    data object Local : CloseReason
    /** The link dropped (device, supervision timeout, out of range); [status] is the GATT status. */
    data class Disconnected(val status: Int) : CloseReason
    /** Bluetooth was switched off. */
    data object BluetoothOff : CloseReason
    /** A Bluetooth permission is missing or was revoked (SecurityException). */
    data object PermissionRevoked : CloseReason
    /** A setup step or a write did not complete in time. */
    data class Timeout(val step: Step) : CloseReason
    /** A GATT operation failed or could not be started. */
    data class GattFailure(val step: Step, val status: Int) : CloseReason
    /** The peer does not expose the Dispenser service, its characteristics or a valid Info record. */
    data object NotADispenser : CloseReason
    /** The app did not consume notifications fast enough (bounded buffer exceeded). */
    data object NotificationOverflow : CloseReason

    enum class Step { CONNECT, DISCOVERY, MTU, INFO, SUBSCRIBE, WRITE }
}

class TransportClosedException(val reason: CloseReason) : Exception("transport closed: $reason")

/** Result of [GattTransportFactory.connect]. */
sealed interface ConnectResult {
    class Connected(val transport: ByteTransport) : ConnectResult
    data class Failed(val reason: CloseReason) : ConnectResult
    /** Bluetooth cannot be used now; nothing was attempted. */
    data class NotReady(val readiness: BluetoothReadiness) : ConnectResult
}

/** Setup and operation timeouts (docs/ANDROID_BLE_DESIGN.md 4.4; tuned in M1.10e.5). */
data class TransportTimeouts(
    val connectMs: Long = 15_000,
    val discoveryMs: Long = 10_000,
    val mtuMs: Long = 5_000,
    val infoMs: Long = 5_000,
    val subscribeMs: Long = 5_000,
    val writeMs: Long = 5_000,
)
