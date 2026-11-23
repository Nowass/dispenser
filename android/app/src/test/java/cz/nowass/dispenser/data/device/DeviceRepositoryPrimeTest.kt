package cz.nowass.dispenser.data.device

import cz.nowass.dispenser.ble.protocol.BleProtocol
import cz.nowass.dispenser.device.ops.LocalRefusal
import cz.nowass.dispenser.device.ops.MessageType as M
import cz.nowass.dispenser.device.ops.OpResult
import cz.nowass.dispenser.device.ops.PrimeStopReason
import cz.nowass.dispenser.device.ops.PrimeVerdict
import cz.nowass.dispenser.device.ops.Records
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.async
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * PRIME_START / PRIME_STOP / PRIME_STATUS in DeviceDispenserRepository against the scripted device:
 * sent only when this session's GET_UPDATE_STATUS running_version is >= 0.3.3 (BLE_PROTOCOL.md 8.4),
 * sent once (no retry), PRIME_START single flight, PRIME_STOP never reserved.
 */
class DeviceRepositoryPrimeTest {
    private val dev = FakeDevicePort().apply { info = info(0, 3, 3); running("0.3.3") }

    private fun FakeDevicePort.running(v: String) {
        updateStatus = mapOf("running_version" to v.toByteArray(), "capable" to true, "last_result" to 0L, "last_version" to ByteArray(0))
    }

    private fun TestScope.repo(scope: CoroutineScope = backgroundScope) = DeviceDispenserRepository(
        dev.deviceId, { "A0:B1:C2:D3:E4:F5" }, dev, InMemoryPendingOpsJournal(), InMemoryDeviceCacheStore(), TestSeqAllocator(),
        { UnavailablePhoneTimeSource.now() }, scope, nowMs = { 1_795_000_000_000L },
    )

    private fun unsupported(r: OpResult<*>) =
        r is OpResult.Refused && r.reason == LocalRefusal.Unavailable(DeviceRepository.PRIME_UNSUPPORTED)

    private fun primeCount() = dev.count(M.PRIME_START) + dev.count(M.PRIME_STOP) + dev.count(M.PRIME_STATUS)

    @Test
    fun gate_is_running_version_0_3_3() {
        for (v in listOf("0.3.3", "0.3.4", "0.4.0", "1.0.0", "0.3.3+build.5", "0.3.4-dev")) {
            assertTrue(v, DeviceDispenserRepository.firmwareHasPrime(v))
        }
        for (v in listOf(null, "", "0.3.2", "0.3.0", "0.2.9", "0.3.3-dev", "0.3.3-rc.1", "garbage", "0.3")) {
            assertFalse("$v", DeviceDispenserRepository.firmwareHasPrime(v))
        }
    }

    @Test
    fun older_firmware_is_never_sent_a_priming_request() = runTest {
        // < 0.3.0: not even GET_UPDATE_STATUS; 0.3.0 .. 0.3.2 and 0.3.3-dev: the version is read, nothing else
        for ((infoV, running) in listOf(info(0, 2, 0) to "0.2.0", null to "0.3.3", info(0, 3, 0) to "0.3.0",
            info(0, 3, 2) to "0.3.2", info(0, 3, 3) to "0.3.3-dev")) {
            dev.close()
            dev.received.clear()
            dev.info = infoV
            dev.running(running)
            val r = repo()
            assertTrue(r.connect() is ConnectOutcome.Connected)
            assertTrue("$running", unsupported(r.primeStart()))
            assertTrue("$running", unsupported(r.primeStatus()))
            assertTrue("$running", unsupported(r.primeStop()))
            assertEquals("$running", 0, primeCount())
            assertTrue(dev.count(M.GET_UPDATE_STATUS) <= 1) // read once per session
            assertNotNull("session must stay open for $running", dev.live0())
        }
    }

    @Test
    fun not_connected_sends_nothing() = runTest {
        val r = repo()
        assertTrue(r.primeStart() is OpResult.NotSent)
        assertTrue(r.primeStop() is OpResult.NotSent)
        assertTrue(r.primeStatus() is OpResult.NotSent)
        assertTrue(dev.received.isEmpty())
    }

    @Test
    fun start_status_stop_are_decoded_and_sent_without_retries() = runTest {
        val r = repo()
        r.connect()
        val start = (r.primeStart() as OpResult.Ok).value
        assertEquals(PrimeVerdict.OK, start.verdict)
        assertTrue(start.running)
        assertEquals(30_000L, start.maxMs)
        assertEquals(3_000L, start.leaseMs)
        assertTrue((r.primeStatus() as OpResult.Ok).value.running)
        val stop = (r.primeStop() as OpResult.Ok).value
        assertFalse(stop.running)
        assertEquals(PrimeStopReason.USER, stop.stopReason)
        assertEquals(1, dev.count(M.GET_UPDATE_STATUS)) // the version is read once per session
        for (t in listOf(M.PRIME_START, M.PRIME_STATUS, M.PRIME_STOP)) {
            assertEquals(0, dev.retriesOf[t])
            assertTrue(dev.received.first { it.first == t }.second.isEmpty()) // empty request body
        }
        // a reconnect reads the version again (the firmware may have changed)
        dev.close()
        r.connect()
        dev.running("0.3.2")
        assertTrue(unsupported(r.primeStatus()))
        assertEquals(2, dev.count(M.GET_UPDATE_STATUS))
    }

    @Test
    fun refusal_verdicts_decode_and_unknown_values_are_refusals() = runTest {
        val r = repo()
        r.connect()
        for ((wire, want) in listOf(2L to PrimeVerdict.BATTERY, 3L to PrimeVerdict.DOSE_SOON, 4L to PrimeVerdict.RESERVOIR,
            6L to PrimeVerdict.BUSY, 99L to PrimeVerdict.UNKNOWN)) {
            dev.primeVerdict = wire
            val ok = (r.primeStart() as OpResult.Ok).value
            assertEquals(want, ok.verdict)
            assertFalse(ok.running)
        }
        assertEquals(PrimeStopReason.UNKNOWN, PrimeStopReason.of(42))
    }

    @Test
    fun a_lost_answer_is_uncertain_and_never_retransmitted() = runTest {
        val r = repo()
        r.connect()
        dev.faults[M.PRIME_START] = FakeDevicePort.Fault.EXECUTE_THEN_LOSE
        assertTrue(r.primeStart() is OpResult.Uncertain)
        assertEquals(1, dev.count(M.PRIME_START))
    }

    @Test
    fun start_is_single_flight_but_stop_is_never_blocked_by_it() = runTest {
        val r = repo()
        r.connect()
        r.primeStatus() // the version is known
        val gate = CompletableDeferred<Unit>()
        dev.hooks[M.PRIME_START] = { gate.await() }
        val first = async { r.primeStart() }
        runCurrent()
        val second = r.primeStart()
        assertTrue("$second", second is OpResult.Refused && second.reason == LocalRefusal.InProgress)
        val stop = async { r.primeStop() } // not refused by the START reservation
        gate.complete(Unit)
        assertTrue(first.await() is OpResult.Ok)
        assertTrue(stop.await() is OpResult.Ok)
        assertEquals(1, dev.count(M.PRIME_START))
        assertEquals(1, dev.count(M.PRIME_STOP))
    }

    @Test
    fun typed_records_from_the_shared_vectors() {
        val run = Records.primeStatus(checkNotNull(BleProtocol.decode(vector("prime_status_response_running")).fields))
        assertTrue(run.running)
        assertEquals(12_345L, run.elapsedMs)
        assertEquals(PrimeStopReason.NONE, run.stopReason)
        val max = Records.primeStatus(checkNotNull(BleProtocol.decode(vector("prime_status_response_max_time")).fields))
        assertFalse(max.running)
        assertEquals(PrimeStopReason.MAX_TIME, max.stopReason)
        assertEquals(30_000L, max.elapsedMs)
        assertEquals(PrimeVerdict.BATTERY, Records.primeStatus(checkNotNull(BleProtocol.decode(vector("prime_start_response_battery")).fields)).verdict)
        val stop = Records.primeStatus(checkNotNull(BleProtocol.decode(vector("prime_stop_response")).fields))
        assertEquals(PrimeStopReason.USER, stop.stopReason)
        assertEquals(7_000L, stop.elapsedMs)
        // the empty requests encode exactly as the shared vectors
        for ((name, type) in listOf("prime_start_request" to M.PRIME_START, "prime_status_request" to M.PRIME_STATUS, "prime_stop_request" to M.PRIME_STOP)) {
            val want = vector(name)
            val h = BleProtocol.decode(want).header
            assertEquals(type, h.type)
            assertArrayEquals(want, BleProtocol.encode(h.type, h.flags, h.txn, emptyMap()))
        }
    }

    private fun vector(name: String): ByteArray {
        var dir: File? = File(System.getProperty("user.dir")!!).absoluteFile
        while (dir != null) {
            val f = File(dir, "test-vectors/ble_protocol_v1.txt")
            if (f.isFile) {
                val hex = f.readLines().first { it.startsWith("msg name=$name ") }.substringAfter(" frame=").trim()
                return ByteArray(hex.length / 2) { hex.substring(2 * it, 2 * it + 2).toInt(16).toByte() }
            }
            dir = dir.parentFile
        }
        error("vectors not found")
    }
}
