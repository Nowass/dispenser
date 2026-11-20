package cz.nowass.dispenser.device.fwsim

import java.io.File

/**
 * JNI facade of `libdsp_fwsim` (firmware/test/fwsim/fwsim.c): the REAL firmware application,
 * security and BLE transport logic compiled for the host, with a simulated radio (decision E10).
 * One simulated device per process. Host interoperability only — not radio, target RNG or timing.
 */
object FirmwareSim {
    const val EVT_NOTIFY = 1
    const val EVT_DISCONNECTED = 2
    /** [poll]: the caller's link is over and nothing of it is queued (harness-only; the poller stops). */
    const val EVT_GONE = 3
    /** Header of a [poll] event: type u8, BLE handle u16 LE, link u32 LE; the payload follows. */
    const val EVT_HEADER = 7

    @Volatile private var loaded = false

    fun load() {
        if (loaded) return
        synchronized(this) {
            if (loaded) return
            val path = checkNotNull(System.getProperty("dsp.fwsim.lib")) { "dsp.fwsim.lib not set: run through Gradle (buildFirmwareSim)" }
            check(File(path).isFile) { "firmware simulator library missing: $path" }
            System.load(path)
            loaded = true
        }
    }

    @JvmStatic external fun reset(deviceId: ByteArray, setupCode: ByteArray): Boolean
    @JvmStatic external fun buttonWake(recoveryHold: Boolean): Boolean
    @JvmStatic external fun waitAdvertising(timeoutMs: Int): Boolean
    /**
     * Connects; returns `(link shl 16) or handle`, 0 when refused. The BLE handle is reused across
     * connections (as a controller does); the harness link id is never reused, and [write], [poll]
     * and [disconnect] are bound to it — see [linkOf] / [handleOf].
     */
    @JvmStatic external fun connect(mtu: Int): Long
    /** One ATT write on [link]: the ATT result, -2 when [link] is not the live link. */
    @JvmStatic external fun write(link: Long, fragment: ByteArray): Int
    /** Next event of [link] ({type, handle, link, payload}, see [EVT_HEADER]); null on timeout. */
    @JvmStatic external fun poll(link: Long, timeoutMs: Int): ByteArray?
    /** The phone drops [link]; a no-op for an ended link. */
    @JvmStatic external fun disconnect(link: Long)
    /** The BLE handle the next connection gets (1..0x0EFF; default 0x0001); only while not connected. */
    @JvmStatic external fun setConnectionHandle(handle: Int): Boolean
    /** Harness-only: queue a notification on the live link as if the radio delivered it. */
    @JvmStatic external fun injectNotify(payload: ByteArray): Boolean
    @JvmStatic external fun press3()
    @JvmStatic external fun advance(ms: Long)
    @JvmStatic external fun wakeRunning(): Boolean
    @JvmStatic external fun joinWake(timeoutMs: Int): Boolean
    @JvmStatic external fun inspect(): ByteArray?
    @JvmStatic external fun diagnostics(): IntArray
    @JvmStatic external fun info(): ByteArray
    /** The next wake loses its power during the first pump run (harness cut point), then reboots. */
    @JvmStatic external fun armPumpPowerCut(): Boolean
    @JvmStatic external fun powerCutHappened(): Boolean

    fun linkOf(token: Long): Long = token ushr 16
    fun handleOf(token: Long): Int = (token and 0xFFFF).toInt()
    fun eventHandle(e: ByteArray): Int = (e[1].toInt() and 0xFF) or ((e[2].toInt() and 0xFF) shl 8)
    fun eventLink(e: ByteArray): Long =
        (0 until 4).fold(0L) { acc, i -> acc or ((e[3 + i].toLong() and 0xFF) shl (8 * i)) }

    /** Events of an ended link discarded unread (never delivered to a later link). */
    fun staleEvents(): Int = diagnostics()[6]

    /** Pump runs of the simulated device (harness pump oracle). */
    fun pumpRuns(): Int = diagnostics()[4]

    /** Pump-oracle violations: a run without a durable decision, or a duplicate run of one manual request. */
    fun pumpViolations(): Int = diagnostics()[5]

    /** Bench GPIO pump driver (the real dsp_pump_gpio on a fake pad): output active outside a run, or not off/held when a wake ended. */
    fun gpioViolations(): Int = diagnostics()[7]

    /** Activations of the GPIO pump control signal (each one is a pump run of the oracle). */
    fun gpioActivations(): Int = diagnostics()[8]

    /** Durable ownership state between wakes. */
    data class Ownership(val claimState: Int, val identity: ByteArray, val slots: List<ByteArray?>)

    fun ownership(): Ownership {
        val b = checkNotNull(inspect()) { "inspect only between wakes" }
        return Ownership(b[0].toInt(), b.copyOfRange(1, 33), (0 until 4).map { i ->
            if (b[33 + i * 33].toInt() == 1) b.copyOfRange(34 + i * 33, 66 + i * 33) else null
        })
    }
}
