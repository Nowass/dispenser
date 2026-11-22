package cz.nowass.dispenser.data.device

import cz.nowass.dispenser.device.ops.BatteryRecord
import cz.nowass.dispenser.device.ops.BatteryState
import cz.nowass.dispenser.device.ops.MessageType as M
import cz.nowass.dispenser.device.ops.OpResult
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.async
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.time.Instant

/**
 * Battery telemetry in DeviceDispenserRepository against the scripted device: GET_BATTERY only to
 * firmware >= 0.2.0 (older firmware would close the session), read with the status at every new
 * session and on refreshStatus (never polled), stamped with the device's measurement time, cached
 * for the offline view, unavailable states kept explicit.
 */
class DeviceRepositoryBatteryTest {
    private val dev = FakeDevicePort()
    private val cache = InMemoryDeviceCacheStore()
    private val now = 1_795_000_000_000L

    private fun TestScope.repo(scope: CoroutineScope = backgroundScope) = DeviceDispenserRepository(
        dev.deviceId, { "A0:B1:C2:D3:E4:F5" }, dev, InMemoryPendingOpsJournal(), cache, TestSeqAllocator(),
        { UnavailablePhoneTimeSource.now() }, scope, nowMs = { now },
    )

    @Test
    fun connect_reads_the_battery_once_with_the_status() = runTest {
        val r = repo()
        assertEquals(Fresh.Unknown, r.battery().first())
        assertTrue(r.connect() is ConnectOutcome.Connected)
        assertEquals(1, dev.count(M.GET_STATUS))
        assertEquals(1, dev.count(M.GET_BATTERY))
        val b = r.battery().first()
        assertTrue("$b", b is Fresh.Live)
        assertEquals(BatteryRecord(BatteryState.VALID, 3970, 5), b.valueOrNull)
        // stamped with the measurement time: 5 s before the read
        assertEquals(Instant.ofEpochMilli(now - 5_000), (b as Fresh.Live).readAt)
        // the same session: a second connect() reads nothing again (no polling)
        r.connect()
        assertEquals(1, dev.count(M.GET_BATTERY))
        assertTrue(dev.live0() != null) // the session is still open
    }

    @Test
    fun older_firmware_is_never_asked_and_reports_unsupported() = runTest {
        for (old in listOf(info(0, 1, 0), info(0, 1, 9), null)) {
            dev.close()
            dev.received.clear()
            dev.info = old
            val r = repo()
            assertTrue(r.connect() is ConnectOutcome.Connected)
            assertEquals(0, dev.count(M.GET_BATTERY))
            assertTrue("session must stay open for $old", dev.live0() != null)
            val b = r.battery().first()
            assertEquals(BatteryState.UNSUPPORTED, b.valueOrNull?.state)
            assertEquals(null, b.valueOrNull?.millivolts)
            // refreshStatus does not ask either
            assertTrue(r.refreshStatus() is OpResult.Ok)
            assertEquals(0, dev.count(M.GET_BATTERY))
        }
    }

    @Test
    fun newer_firmware_versions_are_asked() = runTest {
        assertFalse(DeviceDispenserRepository.firmwareHasBattery(info(0, 1, 65535)))
        assertTrue(DeviceDispenserRepository.firmwareHasBattery(info(0, 2, 0)))
        assertTrue(DeviceDispenserRepository.firmwareHasBattery(info(0, 10, 0)))
        assertTrue(DeviceDispenserRepository.firmwareHasBattery(info(1, 0, 0)))
        assertFalse(DeviceDispenserRepository.firmwareHasBattery(null))
    }

    @Test
    fun refresh_status_rereads_the_battery() = runTest {
        val r = repo()
        r.connect()
        dev.battery = Triple(0L, 3950L, 0L)
        assertTrue(r.refreshStatus() is OpResult.Ok)
        assertEquals(2, dev.count(M.GET_BATTERY))
        assertEquals(3950, r.battery().first().valueOrNull?.millivolts)
        assertEquals(Instant.ofEpochMilli(now), (r.battery().first() as Fresh.Live).readAt)
    }

    @Test
    fun unavailable_states_never_carry_a_voltage() = runTest {
        val cases = mapOf(
            Triple(1L, 0L, 0L) to BatteryState.NOT_MEASURED,
            Triple(2L, 0L, 0L) to BatteryState.ADC_FAILED,
            Triple(3L, 0L, 0L) to BatteryState.NO_CALIBRATION,
            Triple(4L, 0L, 0L) to BatteryState.OUT_OF_RANGE,
            Triple(5L, 0L, 0L) to BatteryState.UNSTABLE,
            Triple(2L, 3970L, 9L) to BatteryState.ADC_FAILED, // a stray voltage is ignored
            Triple(0L, 0L, 0L) to BatteryState.OUT_OF_RANGE,  // "VALID 0 mV" is not a measurement
        )
        for ((wire, want) in cases) {
            dev.close()
            dev.battery = wire
            val r = repo()
            r.connect()
            val v = r.battery().first().valueOrNull
            assertEquals("$wire", want, v?.state)
            assertEquals("$wire", null, v?.millivolts)
            assertEquals("$wire", 0L, v?.ageSeconds)
        }
    }

    @Test
    fun cached_value_survives_process_death_and_is_never_live() = runTest {
        repo().connect()
        dev.close()
        val r2 = repo()
        runCurrent()
        val c = r2.battery().first()
        assertTrue("$c", c is Fresh.Cached)
        assertEquals(3970, c.valueOrNull?.millivolts)
        assertEquals(Instant.ofEpochMilli(now - 5_000), (c as Fresh.Cached).readAt)
    }

    @Test
    fun a_rejected_battery_read_keeps_the_session_and_the_status() = runTest {
        dev.rejects[M.GET_BATTERY] = Triple(14, 0L, 0L) // DEGRADED
        val r = repo()
        assertTrue(r.connect() is ConnectOutcome.Connected)
        assertTrue(r.status().first() is Fresh.Live)
        assertEquals(Fresh.Unknown, r.battery().first())
        assertTrue(dev.live0() != null)
    }

    // ---- manual refresh (battery card tap) ------------------------------------------------------

    @Test
    fun manual_refresh_sends_one_get_battery_and_takes_the_devices_sample_time() = runTest {
        val r = repo()
        r.connect()
        assertEquals(1, dev.count(M.GET_BATTERY))
        dev.battery = Triple(0L, 4160L, 0L) // the device took a new sample
        val res = r.refreshBattery()
        assertTrue("$res", res is OpResult.Ok)
        assertEquals(2, dev.count(M.GET_BATTERY))
        assertEquals(1, dev.count(M.GET_STATUS)) // only GET_BATTERY
        val b = r.battery().first() as Fresh.Live
        assertEquals(4160, b.value.millivolts)
        assertEquals(Instant.ofEpochMilli(now), b.readAt) // age 0: measured now
        // a cached device answer (age 40 s) is never stamped as a new measurement
        dev.battery = Triple(0L, 4160L, 40L)
        r.refreshBattery()
        assertEquals(Instant.ofEpochMilli(now - 40_000), (r.battery().first() as Fresh.Live).readAt)
    }

    @Test
    fun manual_refresh_while_disconnected_sends_nothing_and_keeps_the_value() = runTest {
        val r = repo()
        r.connect()
        val before = r.battery().first()
        dev.close()
        val res = r.refreshBattery()
        assertTrue("$res", res is OpResult.NotSent)
        assertEquals(1, dev.count(M.GET_BATTERY))
        assertEquals(before.valueOrNull, r.battery().first().valueOrNull)
        assertTrue(r.battery().first() is Fresh.Cached) // shown as stored, with its old time
    }

    @Test
    fun failed_refresh_keeps_the_previous_reading_and_time() = runTest {
        val r = repo()
        r.connect()
        val before = r.battery().first() as Fresh.Live
        dev.rejects[M.GET_BATTERY] = Triple(14, 0L, 0L)
        val res = r.refreshBattery()
        assertTrue("$res", res is OpResult.Rejected)
        val after = r.battery().first() as Fresh.Live
        assertEquals(before.value, after.value)
        assertEquals(before.readAt, after.readAt) // no fabricated timestamp
    }

    @Test
    fun overlapping_refreshes_never_send_two_requests() = runTest {
        val r = repo()
        r.connect()
        val gate = kotlinx.coroutines.CompletableDeferred<Unit>()
        dev.hooks[M.GET_BATTERY] = { gate.await() }
        val first = backgroundScope.async { r.refreshBattery() }
        runCurrent()
        val second = r.refreshBattery() // while the first is in flight
        assertTrue("$second", second is OpResult.Refused)
        gate.complete(Unit)
        assertTrue(first.await() is OpResult.Ok)
        assertEquals(2, dev.count(M.GET_BATTERY)) // connect + one refresh
    }
}
