package cz.nowass.dispenser.data.device.local

import android.content.Context
import androidx.test.core.app.ApplicationProvider
import cz.nowass.dispenser.data.device.CacheEntry
import cz.nowass.dispenser.data.device.DeviceDispenserRepository
import cz.nowass.dispenser.data.device.FakeDevicePort
import cz.nowass.dispenser.data.device.Fresh
import cz.nowass.dispenser.data.device.PendingKind
import cz.nowass.dispenser.data.device.PendingOp
import cz.nowass.dispenser.data.device.PendingState
import cz.nowass.dispenser.data.device.Resolution
import cz.nowass.dispenser.data.device.TestSeqAllocator
import cz.nowass.dispenser.data.device.UnavailablePhoneTimeSource
import cz.nowass.dispenser.device.ops.MessageType
import cz.nowass.dispenser.device.ops.OpResult
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.After
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

/**
 * `device.db` (Room) journal and cache, across a process death (the database file is closed and
 * reopened by new objects), including the repository's manual-dose resolution over the reopened file.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35])
class DeviceDatabaseTest {
    private val ctx = ApplicationProvider.getApplicationContext<Context>()
    private val name = "device-test.db"
    private val open = mutableListOf<DeviceDatabase>()
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)

    private fun db() = DeviceDatabase.build(ctx, name).also { open += it }

    @After
    fun tearDown() {
        scope.cancel()
        open.forEach { it.close() }
        ctx.deleteDatabase(name)
    }

    @Test
    fun journal_rows_survive_reopening_with_every_field() = runBlocking {
        val db1 = db()
        val j1 = RoomPendingOpsJournal(db1.dao())
        val payload = byteArrayOf(1, 2, 3)
        val id = j1.insert(PendingOp(deviceId = "a0b1c2d3e4f5", kind = PendingKind.MANUAL_DOSE, createdAtMs = 5, seq = Long.MAX_VALUE,
            volumeUl = 4_294_967_295L, payload = payload))
        j1.insert(PendingOp(deviceId = "ffffffffffff", kind = PendingKind.REFILL, createdAtMs = 6))
        db1.close()
        val j2 = RoomPendingOpsJournal(db().dao()) // a new process
        val row = j2.list("a0b1c2d3e4f5").single()
        assertEquals(id, row.id)
        assertEquals(Long.MAX_VALUE, row.seq)
        assertEquals(4_294_967_295L, row.volumeUl)
        assertArrayEquals(payload, row.payload)
        assertEquals(PendingState.PREPARED, row.state)
        j2.update(row.copy(state = PendingState.RESOLVED, resolution = Resolution.INTERRUPTED, resolutionDetail = 7, resolvedAtMs = 9))
        assertEquals(Resolution.INTERRUPTED, j2.observe("a0b1c2d3e4f5").first().single().resolution)
        j2.delete(id)
        assertTrue(j2.list("a0b1c2d3e4f5").isEmpty())
        // an update of a missing row is an error, never silently lost
        val failed = runCatching { j2.update(row) }
        assertTrue(failed.isFailure)
    }

    @Test
    fun cache_entries_survive_reopening() = runBlocking {
        val c1 = RoomDeviceCacheStore(db().dao())
        c1.put("a0b1c2d3e4f5", "status", CacheEntry(byteArrayOf(9, 8), 42))
        c1.put("a0b1c2d3e4f5", "status", CacheEntry(byteArrayOf(7), 43)) // replaced
        open.forEach { it.close() }
        val c2 = RoomDeviceCacheStore(db().dao())
        val e = c2.get("a0b1c2d3e4f5", "status")!!
        assertArrayEquals(byteArrayOf(7), e.body)
        assertEquals(43L, e.readAtMs)
        assertNull(c2.get("a0b1c2d3e4f5", "config"))
    }

    @Test
    fun interrupted_dose_resolves_after_process_death_over_the_room_journal() = runBlocking {
        val dev = FakeDevicePort()
        val seq = TestSeqAllocator()
        fun repo(d: DeviceDatabase) = DeviceDispenserRepository(dev.deviceId, { "A0:B1:C2:D3:E4:F5" }, dev,
            RoomPendingOpsJournal(d.dao()), RoomDeviceCacheStore(d.dao()), seq, UnavailablePhoneTimeSource, scope)
        val db1 = db()
        val r1 = repo(db1)
        r1.connect()
        assertTrue(r1.manualDose(800) is OpResult.Uncertain)
        db1.close() // process death
        val db2 = db()
        val r2 = repo(db2)
        withTimeout(5_000) { r2.status().first { it is Fresh.Cached } } // the cached status is back, as cached
        r2.connect()
        val row = RoomPendingOpsJournal(db2.dao()).list(dev.deviceId).single()
        assertEquals(Resolution.DONE, row.resolution)
        assertEquals(1, dev.count(MessageType.MANUAL_DOSE))
        assertEquals(1, dev.pumpRuns)
    }
}
