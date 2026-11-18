package cz.nowass.dispenser.device.transport

import java.util.UUID

/**
 * One Android GATT client connection attempt, as thin as possible (implemented by [AndroidGattPort]
 * over `BluetoothGatt`, and by fakes in tests). Calls never block; results arrive as [GattCallback]s
 * through the sink given to [GattPortFactory.create], possibly on any thread. A `false` return
 * means the operation could not be started. Any call may throw SecurityException (permission
 * revoked), which the transport handles.
 */
interface GattPort {
    fun connect()
    fun discoverServices(): Boolean
    fun requestMtu(mtu: Int): Boolean
    fun requestHighPriority()
    fun readInfo(): Boolean
    /** Local notification enable + CCCD write (0x0001) on the Response characteristic. */
    fun enableNotifications(): Boolean
    /** Write-with-response of one fragment to the Request characteristic. */
    fun writeRequest(fragment: ByteArray): Boolean
    fun disconnect()
    fun close()
}

/** Callbacks of one port, already reduced to what the transport needs. */
sealed interface GattCallback {
    data class ConnectionState(val connected: Boolean, val status: Int) : GattCallback
    /** [complete]: the Dispenser service with Info, Request (write), Response (notify) and its CCCD. */
    data class ServicesDiscovered(val status: Int, val complete: Boolean) : GattCallback
    data class MtuChanged(val mtu: Int, val status: Int) : GattCallback
    class InfoRead(val status: Int, val value: ByteArray) : GattCallback
    data class NotificationsEnabled(val status: Int) : GattCallback
    data class RequestWritten(val status: Int) : GattCallback
    class Notification(val value: ByteArray) : GattCallback
}

fun interface GattPortFactory {
    /** A new port for [address]; [sink] receives its callbacks (and only its own). */
    fun create(address: String, sink: (GattCallback) -> Unit): GattPort
}

/** Normative GATT layout (docs/BLE_PROTOCOL.md section 2). */
object DispenserGatt {
    val SERVICE: UUID = UUID.fromString("36b60001-3457-4e8e-9cba-8126de3f52e4")
    val INFO: UUID = UUID.fromString("36b60002-3457-4e8e-9cba-8126de3f52e4")
    val REQUEST: UUID = UUID.fromString("36b60003-3457-4e8e-9cba-8126de3f52e4")
    val RESPONSE: UUID = UUID.fromString("36b60004-3457-4e8e-9cba-8126de3f52e4")
    val CCCD: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

    const val GATT_SUCCESS = 0
    /** The app asks for 517; the effective value is what the stack reports (the device prefers 247). */
    const val MTU_REQUEST = 517
    const val MTU_MIN = 23
}
