package cz.nowass.dispenser.data.device

import cz.nowass.dispenser.ble.protocol.BleProtocol
import cz.nowass.dispenser.device.ops.LocalRefusal
import cz.nowass.dispenser.device.ops.MessageType as M
import cz.nowass.dispenser.device.ops.OpResult
import cz.nowass.dispenser.device.ops.Records
import cz.nowass.dispenser.device.ops.StartUpdateRequest
import cz.nowass.dispenser.device.ops.UpdateResult
import cz.nowass.dispenser.device.ops.UpdateVerdict
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
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * GET_UPDATE_STATUS / START_UPDATE in DeviceDispenserRepository against the scripted device: sent only
 * to firmware >= 0.3.0 (older firmware closes the session on an unknown encrypted type), START_UPDATE
 * checked against the wire limits, sent once, single flight; ACCEPTED ends the session (expected).
 */
class DeviceRepositoryUpdateTest {
    private val dev = FakeDevicePort().apply { info = info(0, 3, 0) }
    private val sha = ByteArray(32) { it.toByte() }
    private val request = StartUpdateRequest("0.3.1", "firmware-v0.3.1", 1_430_528, sha)

    private fun TestScope.repo(scope: CoroutineScope = backgroundScope) = DeviceDispenserRepository(
        dev.deviceId, { "A0:B1:C2:D3:E4:F5" }, dev, InMemoryPendingOpsJournal(), InMemoryDeviceCacheStore(), TestSeqAllocator(),
        { UnavailablePhoneTimeSource.now() }, scope, nowMs = { 1_795_000_000_000L },
    )

    private fun unsupported(r: OpResult<*>) =
        r is OpResult.Refused && r.reason == LocalRefusal.Unavailable(DeviceRepository.UPDATE_UNSUPPORTED)

    @Test
    fun gate_is_firmware_0_3_0() {
        assertFalse(DeviceDispenserRepository.firmwareHasUpdate(null))
        assertFalse(DeviceDispenserRepository.firmwareHasUpdate(info(0, 2, 0)))
        assertFalse(DeviceDispenserRepository.firmwareHasUpdate(info(0, 2, 65535)))
        assertTrue(DeviceDispenserRepository.firmwareHasUpdate(info(0, 3, 0)))
        assertTrue(DeviceDispenserRepository.firmwareHasUpdate(info(0, 10, 0)))
        assertTrue(DeviceDispenserRepository.firmwareHasUpdate(info(1, 0, 0)))
    }

    @Test
    fun older_firmware_is_never_asked_and_the_session_stays() = runTest {
        for (old in listOf(info(0, 1, 0), info(0, 2, 0), info(0, 2, 9), null)) {
            dev.close()
            dev.received.clear()
            dev.info = old
            val r = repo()
            assertTrue(r.connect() is ConnectOutcome.Connected)
            assertTrue("$old", unsupported(r.getUpdateStatus()))
            assertTrue("$old", unsupported(r.startUpdate(request)))
            assertEquals(0, dev.count(M.GET_UPDATE_STATUS))
            assertEquals(0, dev.count(M.START_UPDATE))
            assertNotNull("session must stay open for $old", dev.live0())
            assertTrue(r.refreshStatus() is OpResult.Ok)
        }
    }

    @Test
    fun not_connected_sends_nothing() = runTest {
        val r = repo()
        assertTrue(r.getUpdateStatus() is OpResult.NotSent)
        assertTrue(r.startUpdate(request) is OpResult.NotSent)
        assertTrue(dev.received.isEmpty())
    }

    @Test
    fun update_status_is_read_and_decoded() = runTest {
        val r = repo()
        r.connect()
        val ok = r.getUpdateStatus() as OpResult.Ok
        assertEquals("0.3.0", ok.value.runningVersion)
        assertTrue(ok.value.capable)
        assertEquals(UpdateResult.NONE, ok.value.lastResult)
        assertEquals("", ok.value.lastVersion)
        // unknown last_result decodes and counts as a failure
        dev.updateStatus = mapOf("running_version" to "0.3.0-dev".toByteArray(), "capable" to false, "last_result" to 255L,
            "last_version" to "0.3.1".toByteArray())
        val dev2 = (r.getUpdateStatus() as OpResult.Ok).value
        assertEquals(UpdateResult.UNKNOWN, dev2.lastResult)
        assertEquals(255, dev2.lastResultWire)
        assertFalse(dev2.capable)
        assertNotNull(dev.live0())
    }

    @Test
    fun accepted_update_is_sent_once_and_the_device_ends_the_session() = runTest {
        val r = repo()
        r.connect()
        val ok = r.startUpdate(request) as OpResult.Ok
        assertEquals(UpdateVerdict.ACCEPTED, ok.value.verdict)
        assertEquals(1, dev.count(M.START_UPDATE))
        val sent = dev.received.last { it.first == M.START_UPDATE }.second
        assertArrayEquals("0.3.1".toByteArray(), sent["version"] as ByteArray)
        assertArrayEquals("firmware-v0.3.1".toByteArray(), sent["tag"] as ByteArray)
        assertEquals(1_430_528L, sent["size"])
        assertArrayEquals(sha, sent["sha256"] as ByteArray)
        assertNull("ACCEPTED ends the session", dev.live0())
        // afterwards nothing is sent (no live session)
        assertTrue(r.startUpdate(request) is OpResult.NotSent)
        assertEquals(1, dev.count(M.START_UPDATE))
    }

    @Test
    fun refusals_and_unknown_verdicts_keep_the_session() = runTest {
        val r = repo()
        r.connect()
        for ((wire, want) in listOf(1L to UpdateVerdict.NOT_CAPABLE, 4L to UpdateVerdict.DOSE_SOON, 7L to UpdateVerdict.BUSY, 200L to UpdateVerdict.UNKNOWN)) {
            dev.updateVerdict = wire
            val ok = r.startUpdate(request) as OpResult.Ok
            assertEquals(want, ok.value.verdict)
            assertEquals(wire.toInt(), ok.value.wire)
            assertNotNull(dev.live0())
        }
    }

    @Test
    fun a_lost_answer_is_uncertain_and_never_retransmitted() = runTest {
        val r = repo()
        r.connect()
        dev.faults[M.START_UPDATE] = FakeDevicePort.Fault.EXECUTE_THEN_LOSE
        assertTrue(r.startUpdate(request) is OpResult.Uncertain)
        assertEquals(1, dev.count(M.START_UPDATE))
    }

    @Test
    fun requests_outside_the_wire_limits_are_refused_locally() = runTest {
        val r = repo()
        r.connect()
        val bad = listOf(
            StartUpdateRequest("0.31", "t", 1, sha),
            StartUpdateRequest("0.3.01", "t", 1, sha),
            StartUpdateRequest("0.3.65536", "t", 1, sha),
            StartUpdateRequest("0.3.1-beta", "t", 1, sha),
            StartUpdateRequest("0.3.1", "firmware/v0.3.1", 1, sha),
            StartUpdateRequest("0.3.1", "", 1, sha),
            StartUpdateRequest("0.3.1", "x".repeat(41), 1, sha),
            StartUpdateRequest("0.3.1", "t", 0, sha),
            StartUpdateRequest("0.3.1", "t", StartUpdateRequest.MAX_SIZE + 1, sha),
            StartUpdateRequest("0.3.1", "t", 1, ByteArray(31)),
        )
        for (b in bad) {
            val res = r.startUpdate(b)
            assertTrue("${b.version} ${b.tag} ${b.size}", res is OpResult.Refused && res.reason is LocalRefusal.Invalid)
        }
        assertEquals(0, dev.count(M.START_UPDATE))
        assertTrue(r.startUpdate(StartUpdateRequest("65535.65535.65535", "x".repeat(40), StartUpdateRequest.MAX_SIZE, sha)) is OpResult.Ok)
    }

    @Test
    fun start_update_is_single_flight() = runTest {
        val r = repo()
        r.connect()
        dev.updateVerdict = 4
        val gate = CompletableDeferred<Unit>()
        dev.hooks[M.START_UPDATE] = { gate.await() }
        val first = async { r.startUpdate(request) }
        runCurrent()
        val second = r.startUpdate(request)
        assertTrue("$second", second is OpResult.Refused && second.reason == LocalRefusal.InProgress)
        gate.complete(Unit)
        assertTrue(first.await() is OpResult.Ok)
        assertEquals(1, dev.count(M.START_UPDATE))
    }

    @Test
    fun typed_records_from_the_shared_vectors() {
        val st = Records.updateStatus(checkNotNull(BleProtocol.decode(vector("get_update_status_response_rolled_back")).fields))
        assertEquals("0.3.0", st.runningVersion)
        assertEquals(UpdateResult.ROLLED_BACK, st.lastResult)
        assertEquals("0.3.1", st.lastVersion)
        val devFw = Records.updateStatus(checkNotNull(BleProtocol.decode(vector("get_update_status_response_dev")).fields))
        assertEquals("0.3.0-dev", devFw.runningVersion)
        assertEquals(UpdateResult.UNKNOWN, devFw.lastResult)
        assertEquals(UpdateVerdict.DOSE_SOON, Records.updateVerdict(checkNotNull(BleProtocol.decode(vector("start_update_response_dose_soon")).fields)).verdict)
        // our request encodes exactly as the shared vector
        val want = vector("start_update_request")
        val h = BleProtocol.decode(want).header
        val got = BleProtocol.encode(h.type, h.flags, h.txn, Records.startUpdateFields(request))
        assertArrayEquals(want, got)
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
