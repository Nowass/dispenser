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
    @JvmStatic external fun connect(mtu: Int): Int
    @JvmStatic external fun write(fragment: ByteArray): Int
    @JvmStatic external fun poll(handle: Int, timeoutMs: Int): ByteArray?
    @JvmStatic external fun disconnect()
    @JvmStatic external fun press3()
    @JvmStatic external fun advance(ms: Long)
    @JvmStatic external fun wakeRunning(): Boolean
    @JvmStatic external fun joinWake(timeoutMs: Int): Boolean
    @JvmStatic external fun inspect(): ByteArray?
    @JvmStatic external fun diagnostics(): IntArray
    @JvmStatic external fun info(): ByteArray

    /** Pump runs of the simulated device (harness pump oracle). */
    fun pumpRuns(): Int = diagnostics()[4]

    /** Pump-oracle violations: a run without a durable decision, or a duplicate run of one manual request. */
    fun pumpViolations(): Int = diagnostics()[5]

    /** Durable ownership state between wakes. */
    data class Ownership(val claimState: Int, val identity: ByteArray, val slots: List<ByteArray?>)

    fun ownership(): Ownership {
        val b = checkNotNull(inspect()) { "inspect only between wakes" }
        return Ownership(b[0].toInt(), b.copyOfRange(1, 33), (0 until 4).map { i ->
            if (b[33 + i * 33].toInt() == 1) b.copyOfRange(34 + i * 33, 66 + i * 33) else null
        })
    }
}
