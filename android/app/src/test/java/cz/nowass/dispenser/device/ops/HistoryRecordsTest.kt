package cz.nowass.dispenser.device.ops

import cz.nowass.dispenser.ble.protocol.BleProtocol
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/** Phase C: typed GET_HISTORY records from the shared vector (decoded by the existing codec). */
class HistoryRecordsTest {
    private fun vector(name: String): ByteArray {
        var dir: File? = File(System.getProperty("user.dir")!!).absoluteFile
        while (dir != null) {
            val f = File(dir, "test-vectors/ble_protocol_v1.txt")
            if (f.isFile) {
                val line = f.readLines().first { it.startsWith("msg name=$name ") }
                val hex = line.substringAfter(" frame=").trim()
                return ByteArray(hex.length / 2) { hex.substring(2 * it, 2 * it + 2).toInt(16).toByte() }
            }
            dir = dir.parentFile
        }
        error("vectors not found")
    }

    @Test
    fun history_page_from_the_shared_vector() {
        val page = Records.historyPage(checkNotNull(BleProtocol.decode(vector("get_history_response")).fields))
        assertEquals(137L, page.newestSeq)
        assertEquals(74L, page.oldestSeq)
        assertFalse(page.faults)
        assertEquals(listOf(137L, 136L, 135L, 4_294_967_295L), page.events.map { it.seq })
        val dose = page.events[0]
        assertEquals(HistoryEventKind.DOSE_SCHEDULED, dose.kind)
        assertEquals(HistoryResult.OK, dose.result)
        assertEquals(1_795_158_000L, dose.atEpochS)
        assertTrue(dose.clockTrusted)
        assertTrue(dose.pumpBenchGpio)
        assertFalse(dose.pumpSimulated)
        assertEquals(100_000L, dose.reservoirBeforeUl)
        assertEquals(95_000L, dose.reservoirAfterUl)
        assertEquals("3f2b8c1e-9d4a-4f6b-8e21-7a5c0d9e1f42", dose.scheduleId)
        val refill = page.events[1]
        assertEquals(HistoryEventKind.RESERVOIR_REFILL, refill.kind)
        assertFalse(refill.clockTrusted)
        assertNull(refill.scheduleId)
        val interrupted = page.events[2]
        assertEquals(HistoryEventKind.DOSE_MANUAL, interrupted.kind)
        assertEquals(HistoryResult.INTERRUPTED, interrupted.result)
        assertNull("flag RESERVOIR_UNKNOWN", interrupted.reservoirBeforeUl)
        assertNull(interrupted.reservoirAfterUl)
        assertTrue(interrupted.pumpSimulated)
        assertEquals(-1L, interrupted.atEpochS) // carried as-is; 0 is the only "unknown" value
        val aborted = page.events[3]
        assertEquals(HistoryResult.ABORTED, aborted.result)
        assertNull(aborted.atEpochS)
        assertEquals("x".repeat(64), aborted.scheduleId)
        assertEquals(4_294_967_295L, interrupted.volumeUl)
        val empty = Records.historyPage(checkNotNull(BleProtocol.decode(vector("get_history_response_empty_faults")).fields))
        assertTrue(empty.faults)
        assertTrue(empty.events.isEmpty())
    }
}
