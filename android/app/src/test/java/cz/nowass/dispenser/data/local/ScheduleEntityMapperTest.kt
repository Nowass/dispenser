package cz.nowass.dispenser.data.local

import cz.nowass.dispenser.data.StoredSchedule
import cz.nowass.dispenser.domain.Recurrence
import cz.nowass.dispenser.domain.Schedule
import cz.nowass.dispenser.domain.SyncState
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.time.DayOfWeek
import java.time.LocalDate
import java.time.LocalTime

class ScheduleEntityMapperTest {

    private fun schedule(recurrence: Recurrence, end: LocalDate? = null, amount: Double = 12.345) = Schedule(
        id = "id-1", revision = 3, name = "Ranní dávka", enabled = false, amountMl = amount,
        startDate = LocalDate.of(2026, 11, 16), localTime = LocalTime.of(7, 5), recurrence = recurrence, endDate = end,
    )

    private fun roundTrip(s: Schedule, sync: SyncState = SyncState.PENDING): StoredSchedule {
        val entity = ScheduleEntityMapper.toEntity(StoredSchedule(s, sync), updatedAtEpochMs = 42)
        val decoded = ScheduleEntityMapper.fromEntity(entity)
        return (decoded as ScheduleEntityMapper.Decoded.Valid).stored
    }

    @Test
    fun roundTrip_allRecurrenceKinds() {
        val cases = listOf(
            schedule(Recurrence.Once),
            schedule(Recurrence.Daily(3), end = LocalDate.of(2026, 12, 31)),
            schedule(Recurrence.Weekly(setOf(DayOfWeek.MONDAY, DayOfWeek.SUNDAY), 2), end = LocalDate.of(2027, 1, 1)),
        )
        for (s in cases) assertEquals(StoredSchedule(s, SyncState.PENDING), roundTrip(s))
        assertEquals(SyncState.DEVICE_APPLIED, roundTrip(cases[1], SyncState.DEVICE_APPLIED).sync)
    }

    @Test
    fun entityColumns_useStorageUnitsAndFormats() {
        val e = ScheduleEntityMapper.toEntity(
            StoredSchedule(schedule(Recurrence.Weekly(setOf(DayOfWeek.MONDAY, DayOfWeek.THURSDAY))), SyncState.PENDING), 42,
        )
        assertEquals(12_345L, e.amountMicrolitres)
        assertEquals("2026-11-16", e.startDate)
        assertEquals("07:05", e.localTime)
        assertEquals("Europe/Prague", e.zoneId)
        assertEquals("WEEKLY", e.recurrenceKind)
        assertEquals(0b0001001, e.weekdaysMask) // Monday bit0 + Thursday bit3
        assertEquals("PENDING", e.syncState)
    }

    @Test
    fun weekdayMask_roundTrip() {
        assertEquals(0, ScheduleEntityMapper.weekdaysToMask(emptySet()))
        assertEquals(ScheduleEntityMapper.ALL_WEEKDAYS_MASK, ScheduleEntityMapper.weekdaysToMask(DayOfWeek.entries.toSet()))
        for (mask in 0..ScheduleEntityMapper.ALL_WEEKDAYS_MASK) {
            assertEquals(mask, ScheduleEntityMapper.weekdaysToMask(ScheduleEntityMapper.maskToWeekdays(mask)))
        }
    }

    @Test
    fun corruptedRows_areReportedInvalid_neverThrow() {
        val good = ScheduleEntityMapper.toEntity(StoredSchedule(schedule(Recurrence.Daily()), SyncState.PENDING), 0)
        val corrupted = mapOf(
            "bad date" to good.copy(startDate = "2026-13-40"),
            "bad time" to good.copy(localTime = "25:99"),
            "bad zone" to good.copy(zoneId = "Mars/Base"),
            "unknown kind" to good.copy(recurrenceKind = "MONTHLY"),
            "unknown sync" to good.copy(syncState = "SIMULATED_ACK"),
            "zero interval" to good.copy(repeatInterval = 0),
            "zero amount" to good.copy(amountMicrolitres = 0),
            "negative amount" to good.copy(amountMicrolitres = -5),
            "huge amount" to good.copy(amountMicrolitres = Long.MAX_VALUE),
            "revision 0" to good.copy(revision = 0),
            "mask out of range" to good.copy(weekdaysMask = 128),
            "mask on daily" to good.copy(weekdaysMask = 1),
            "weekly without days" to good.copy(recurrenceKind = "WEEKLY", weekdaysMask = 0),
            "end before start" to good.copy(endDate = "2026-01-01"),
            "end on once" to good.copy(recurrenceKind = "ONCE", endDate = "2026-12-01"),
            "blank id" to good.copy(id = " "),
        )
        for ((label, row) in corrupted) {
            assertTrue(label, ScheduleEntityMapper.fromEntity(row) is ScheduleEntityMapper.Decoded.Invalid)
        }
        assertTrue(ScheduleEntityMapper.fromEntity(good) is ScheduleEntityMapper.Decoded.Valid)
    }

    @Test
    fun storedOnceInDstGap_isInvalid() {
        val good = ScheduleEntityMapper.toEntity(StoredSchedule(schedule(Recurrence.Once), SyncState.PENDING), 0)
        val gap = good.copy(startDate = "2027-03-28", localTime = "02:15")
        assertTrue(ScheduleEntityMapper.fromEntity(gap) is ScheduleEntityMapper.Decoded.Invalid)
    }

    @Test
    fun invalidReasons_neverContainStoredValues() {
        val good = ScheduleEntityMapper.toEntity(StoredSchedule(schedule(Recurrence.Daily()), SyncState.PENDING), 0)
        val rows = listOf(
            good.copy(name = "Tajná dávka", startDate = "secret-date-value"),
            good.copy(name = "Tajná dávka", recurrenceKind = "SECRET_KIND"),
            good.copy(name = "Tajná dávka", syncState = "SECRET_SYNC"),
            good.copy(name = "Tajná dávka", zoneId = "Secret/Zone"),
        )
        for (row in rows) {
            val reason = (ScheduleEntityMapper.fromEntity(row) as ScheduleEntityMapper.Decoded.Invalid).reason
            for (secret in listOf("Tajná", "secret", "SECRET", "Secret")) {
                assertTrue("'$reason' leaks a stored value", !reason.contains(secret))
            }
        }
    }
}
