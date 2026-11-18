package cz.nowass.dispenser.device.transport

import cz.nowass.dispenser.ble.protocol.BleProtocol
import java.util.concurrent.CopyOnWriteArrayList

/**
 * Deterministic fake of the Android GATT client + a dispenser peripheral, for JVM tests of
 * [GattTransport] and [cz.nowass.dispenser.device.link.FrameLink]. Each [create] is one connection
 * attempt; every port keeps its sink, so tests can deliver stale callbacks of old ports. Behavior is
 * scripted per test through the public fields; by default the device answers like the firmware
 * (MTU min(517, 247), valid Info, notifications enabled, every write acknowledged).
 */
class FakeGatt : GattPortFactory {
    // ---- scripted behavior (applies to the next connection attempts) ---------------------------
    var connectStatus = DispenserGatt.GATT_SUCCESS
    var respondToConnect = true
    var servicesComplete = true
    var discoveryStatus = DispenserGatt.GATT_SUCCESS
    var respondToDiscovery = true
    var deviceMtu = 247
    var mtuStatus = DispenserGatt.GATT_SUCCESS
    var respondToMtu = true
    var acceptMtuRequest = true
    /** Reported MTU regardless of the request (a misbehaving stack); null = min(request, deviceMtu). */
    var reportedMtu: Int? = null
    var info: ByteArray = validInfo()
    var infoStatus = DispenserGatt.GATT_SUCCESS
    var subscribeStatus = DispenserGatt.GATT_SUCCESS
    var respondToSubscribe = true
    var writeStatus = DispenserGatt.GATT_SUCCESS
    /** The stack refuses to start a write (API 33+: writeCharacteristic != BluetoothStatusCodes.SUCCESS). */
    var writeStartFails = false
    /** The stack refuses to start the CCCD write (setCharacteristicNotification or writeDescriptor failed). */
    var subscribeStartFails = false
    /** Android 14+: the stack negotiates by itself right after connecting and reports this MTU unsolicited. */
    var unsolicitedMtuAfterConnect: Int? = null
    /** Writes acknowledged automatically; false = the test acks with [Port.ackWrite]. */
    var autoAckWrites = true
    /** Throws SecurityException on this operation name ("connect", "discover", "mtu", "info", "subscribe", "write"). */
    var securityExceptionOn: String? = null
    /** Device behavior for a complete request frame: the frames it sends back (fragmented at the MTU). */
    var device: (ByteArray) -> List<ByteArray> = { listOf(it) } // echo

    val ports = CopyOnWriteArrayList<Port>()
    val last: Port get() = ports.last()

    override fun create(address: String, sink: (GattCallback) -> Unit): GattPort = Port(address, sink).also { ports += it }

    inner class Port(val address: String, val sink: (GattCallback) -> Unit) : GattPort {
        val ops = CopyOnWriteArrayList<String>()
        val written = CopyOnWriteArrayList<ByteArray>()
        var mtu = DispenserGatt.MTU_MIN
        var closed = false
        var disconnected = false
        private val reasm = BleProtocol.Reassembler()

        private fun op(name: String) {
            ops += name
            if (securityExceptionOn == name) throw SecurityException("permission revoked (test)")
        }

        override fun connect() {
            op("connect")
            if (respondToConnect) sink(GattCallback.ConnectionState(connectStatus == DispenserGatt.GATT_SUCCESS, connectStatus))
            unsolicitedMtuAfterConnect?.let { this.mtu = it; sink(GattCallback.MtuChanged(it, DispenserGatt.GATT_SUCCESS)) }
        }

        override fun discoverServices(): Boolean {
            op("discover")
            if (respondToDiscovery) sink(GattCallback.ServicesDiscovered(discoveryStatus, servicesComplete))
            return true
        }

        override fun requestMtu(mtu: Int): Boolean {
            op("mtu")
            if (!acceptMtuRequest) return false
            this.mtu = reportedMtu ?: minOf(mtu, deviceMtu)
            if (respondToMtu) sink(GattCallback.MtuChanged(this.mtu, mtuStatus))
            return true
        }

        override fun requestHighPriority() {
            op("priority")
        }

        override fun readInfo(): Boolean {
            op("info")
            sink(GattCallback.InfoRead(infoStatus, info.copyOf()))
            return true
        }

        override fun enableNotifications(): Boolean {
            op("subscribe")
            if (subscribeStartFails) return false
            if (respondToSubscribe) sink(GattCallback.NotificationsEnabled(subscribeStatus))
            return true
        }

        override fun writeRequest(fragment: ByteArray): Boolean {
            op("write")
            check(fragment.size <= mtu - 3) { "fragment longer than the negotiated MTU allows" }
            if (writeStartFails) return false
            written += fragment.copyOf()
            if (autoAckWrites) ackWrite()
            return true
        }

        /** Completes the oldest write (status [writeStatus]) and lets the device process it. */
        fun ackWrite(status: Int = writeStatus) {
            val f = written.lastOrNull() ?: return
            sink(GattCallback.RequestWritten(status))
            if (status != DispenserGatt.GATT_SUCCESS) return
            val frame = runCatching { reasm.push(f, 0) }.getOrNull() ?: return
            for (reply in device(frame)) notifyFrame(reply)
        }

        fun notifyFrame(frame: ByteArray, atMtu: Int = mtu) {
            for (frag in BleProtocol.fragment(frame, atMtu)) sink(GattCallback.Notification(frag))
        }

        fun notifyRaw(fragment: ByteArray) = sink(GattCallback.Notification(fragment))
        fun drop(status: Int = 8) = sink(GattCallback.ConnectionState(false, status)) // 8 = supervision timeout

        override fun disconnect() {
            ops += "disconnect"
            disconnected = true
        }

        override fun close() {
            ops += "close"
            closed = true
        }
    }

    companion object {
        fun validInfo(claimed: Boolean = true): ByteArray = BleProtocol.encodeInfo(
            BleProtocol.Info(1, 1, if (claimed) 1 else 0, 0, 0, 0, 1, 0, 0x1234),
        )
    }
}
