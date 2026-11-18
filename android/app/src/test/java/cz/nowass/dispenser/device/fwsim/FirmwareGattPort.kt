package cz.nowass.dispenser.device.fwsim

import cz.nowass.dispenser.device.transport.DispenserGatt
import cz.nowass.dispenser.device.transport.GattCallback
import cz.nowass.dispenser.device.transport.GattPort
import cz.nowass.dispenser.device.transport.GattPortFactory
import kotlin.concurrent.thread

/**
 * [GattPort] onto the firmware simulator: the app's real [cz.nowass.dispenser.device.transport.GattTransport],
 * FrameLink and SecureSession talk to the real firmware logic. Fault hooks act on whole frames the
 * device sends (by the frame type in the first fragment), so faults hit the real firmware.
 */
class FirmwareGattPortFactory(private val mtuCap: Int = 247) : GattPortFactory {
    /** Drop the next device frame of this type (all its fragments); then optionally drop the link. */
    @Volatile var dropFrameType: Int? = null
    @Volatile var disconnectAfterDrop = false
    @Volatile var dropped = 0
    /** Delay the next device frame of this type by [delayMs] (later frames wait behind it, order kept). */
    @Volatile var delayFrameType: Int? = null
    @Volatile var delayMs = 0L
    /**
     * Discard the phone's next frame of this type (all its fragments) before the firmware sees it,
     * with the GATT write still acknowledged: the device never decrypts it, exactly like a frame its
     * full queue drops (BLE_PROTOCOL.md 3, T3b) — the device's receive nonce does not advance.
     */
    @Volatile var dropWriteType: Int? = null
    @Volatile var droppedWrites = 0
    /**
     * Deliver the phone's next frame of this type to the firmware, but report its last fragment's
     * GATT write as failed — only after the device's reply to it arrived (that reply is dropped), so
     * the device has certainly processed the frame: the phone sees a failed write and the link
     * closes — the "transmission may have happened" case, with the device side deterministic.
     */
    @Volatile var failAckWriteType: Int? = null
    /** Matching frames let through normally before [failAckWriteType] applies. */
    @Volatile var failAckSkip = 0
    /** Complete frames the phone wrote, by frame type (dropped ones included). */
    val sentFrames = java.util.concurrent.ConcurrentHashMap<Int, Int>()
    fun sent(type: Int): Int = sentFrames[type] ?: 0

    override fun create(address: String, sink: (GattCallback) -> Unit): GattPort = Port(sink)

    inner class Port(private val sink: (GattCallback) -> Unit) : GattPort {
        @Volatile private var running = false
        private var dropping = false
        private var handle = 0
        /** Events of other (earlier) connections seen and ignored. */
        @Volatile var staleEvents = 0

        override fun connect() {
            val adv = FirmwareSim.waitAdvertising(3_000)
            handle = if (adv) FirmwareSim.connect(mtuCap) else 0
            if (handle == 0) {
                System.err.println("fwsim: connect refused: adv=$adv wake=${FirmwareSim.wakeRunning()} diag=${FirmwareSim.diagnostics().toList()}")
                sink(GattCallback.ConnectionState(false, 133))
                return
            }
            running = true
            thread(name = "fwsim-poll", isDaemon = true) { poll() }
            sink(GattCallback.ConnectionState(true, DispenserGatt.GATT_SUCCESS))
        }

        private fun poll() {
            while (running) {
                val e = FirmwareSim.poll(handle, 20) ?: continue
                if ((e[1].toInt() and 0xFF) != handle) {
                    staleEvents++ // an earlier connection's event: never delivered to this one
                    continue
                }
                when (e[0].toInt()) {
                    FirmwareSim.EVT_NOTIFY -> deliver(e.copyOfRange(2, e.size))
                    FirmwareSim.EVT_DISCONNECTED -> {
                        running = false
                        sink(GattCallback.ConnectionState(false, 19)) // remote user terminated
                    }
                }
            }
        }

        private fun deliver(frag: ByteArray) {
            val first = frag[0].toInt() and 0x80 != 0
            val last = frag[0].toInt() and 0x40 != 0
            if (first && failAckAfterReply) {
                failAckAfterReply = false
                droppingReply = true
            }
            if (droppingReply) {
                if (last) {
                    droppingReply = false
                    dropped++
                    sink(GattCallback.RequestWritten(133)) // the write the device answered "failed"
                }
                return
            }
            if (first && frag.size > 3 && delayFrameType == (frag[3].toInt() and 0xFF)) {
                delayFrameType = null
                Thread.sleep(delayMs) // a slow answer: everything behind it waits too
            }
            if (first && frag.size > 3 && dropFrameType == (frag[3].toInt() and 0xFF)) {
                dropFrameType = null
                dropping = true
            }
            if (dropping) {
                if (last) {
                    dropping = false
                    dropped++
                    if (disconnectAfterDrop) {
                        disconnectAfterDrop = false
                        FirmwareSim.disconnect() // the phone drops the link (out of range)
                        running = false
                        sink(GattCallback.ConnectionState(false, 8))
                    }
                }
                return
            }
            sink(GattCallback.Notification(frag))
        }

        override fun discoverServices(): Boolean = true.also { sink(GattCallback.ServicesDiscovered(DispenserGatt.GATT_SUCCESS, true)) }
        override fun requestMtu(mtu: Int): Boolean = true.also { sink(GattCallback.MtuChanged(minOf(mtu, mtuCap), DispenserGatt.GATT_SUCCESS)) }
        override fun requestHighPriority() = Unit
        override fun readInfo(): Boolean = true.also { sink(GattCallback.InfoRead(DispenserGatt.GATT_SUCCESS, FirmwareSim.info())) }
        override fun enableNotifications(): Boolean = true.also { sink(GattCallback.NotificationsEnabled(DispenserGatt.GATT_SUCCESS)) }

        private var droppingWrite = false
        private var failingAck = false
        @Volatile private var failAckAfterReply = false
        private var droppingReply = false

        override fun writeRequest(fragment: ByteArray): Boolean {
            val first = fragment[0].toInt() and 0x80 != 0
            val last = fragment[0].toInt() and 0x40 != 0
            if (first && fragment.size > 3) {
                val type = fragment[3].toInt() and 0xFF
                sentFrames.merge(type, 1, Int::plus)
                if (dropWriteType == type) {
                    dropWriteType = null
                    droppingWrite = true
                }
                if (failAckWriteType == type) {
                    if (failAckSkip > 0) {
                        failAckSkip--
                    } else {
                        failAckWriteType = null
                        failingAck = true
                    }
                }
            }
            if (droppingWrite) {
                if (last) {
                    droppingWrite = false
                    droppedWrites++
                }
                sink(GattCallback.RequestWritten(DispenserGatt.GATT_SUCCESS)) // acknowledged, never delivered
                return true
            }
            val rc = FirmwareSim.write(fragment)
            if (rc == -2) return false // not connected
            if (failingAck && last) {
                failingAck = false
                failAckAfterReply = true // reported by deliver() once the device's reply arrived
                return true
            }
            sink(GattCallback.RequestWritten(if (rc == 0) DispenserGatt.GATT_SUCCESS else rc))
            return true
        }

        override fun disconnect() {
            if (running) {
                running = false
                FirmwareSim.disconnect()
            }
        }

        override fun close() {
            running = false
        }
    }
}
