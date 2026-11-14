package cz.nowass.dispenser.domain

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.time.DayOfWeek
import java.time.Instant
import java.time.LocalDate
import java.time.LocalTime
import java.time.ZonedDateTime

class RecurrenceEngineTest {

    private fun at(y: Int, mo: Int, d: Int, h: Int, mi: Int = 0): Instant =
        ZonedDateTime.of(y, mo, d, h, mi, 0, 0, DEFAULT_ZONE).toInstant()

    private fun schedule(
        recurrence: Recurrence,
        start: LocalDate = LocalDate.of(2026, 11, 2), // Monday
        time: LocalTime = LocalTime.of(8, 0),
        end: LocalDate? = null,
        id: String = "s",
        revision: Int = 1,
    ) = Schedule(id, revision, id, true, 10.0, start, time, recurrence, end)

    private fun dates(s: Schedule, after: Instant, n: Int) =
        RecurrenceEngine.nextOccurrences(s, after, n).map { it.localDate }

    // ---- Rules ----

    @Test
    fun daily_nextFive() {
        assertEquals(
            (14..18).map { LocalDate.of(2026, 11, it) },
            dates(schedule(Recurrence.Daily()), at(2026, 11, 14, 7, 30), 5),
        )
    }

    @Test
    fun everyThirdDay_anchoredOnStartDate() {
        assertEquals(
            listOf(14, 17, 20, 23, 26).map { LocalDate.of(2026, 11, it) },
            dates(schedule(Recurrence.Daily(3)), at(2026, 11, 12, 9), 5),
        )
    }

    @Test
    fun weekly_selectedWeekdays() {
        val s = schedule(Recurrence.Weekly(setOf(DayOfWeek.MONDAY, DayOfWeek.THURSDAY)))
        assertEquals(
            listOf(16, 19, 23, 26, 30).map { LocalDate.of(2026, 11, it) },
            dates(s, at(2026, 11, 14, 7), 5),
        )
    }

    @Test
    fun everyOtherWeek_onlyOnWeeks() {
        val s = schedule(Recurrence.Weekly(setOf(DayOfWeek.SATURDAY), intervalWeeks = 2))
        assertEquals(
            listOf(LocalDate.of(2026, 11, 7), LocalDate.of(2026, 11, 21), LocalDate.of(2026, 12, 5)),
            dates(s, at(2026, 11, 1, 0), 3),
        )
    }

    @Test
    fun weekly_neverBeforeStartDate_evenInStartWeek() {
        // Start Wednesday 4 Nov; Monday 2 Nov of the same week must not occur.
        val s = schedule(Recurrence.Weekly(setOf(DayOfWeek.MONDAY, DayOfWeek.WEDNESDAY)), start = LocalDate.of(2026, 11, 4))
        assertEquals(
            listOf(LocalDate.of(2026, 11, 4), LocalDate.of(2026, 11, 9)),
            dates(s, at(2026, 11, 1, 0), 2),
        )
    }

    @Test
    fun endDate_isInclusive() {
        val s = schedule(Recurrence.Daily(), end = LocalDate.of(2026, 11, 16))
        assertEquals(
            listOf(14, 15, 16).map { LocalDate.of(2026, 11, it) },
            dates(s, at(2026, 11, 14, 7), 10),
        )
    }

    @Test
    fun once_futureAndPast() {
        val s = schedule(Recurrence.Once, start = LocalDate.of(2026, 11, 20))
        assertEquals(listOf(LocalDate.of(2026, 11, 20)), dates(s, at(2026, 11, 14, 7), 5))
        assertTrue(dates(s, at(2026, 11, 20, 8), 5).isEmpty())
    }

    @Test
    fun missedOccurrences_areNotCaughtUp() {
        // Device was "away" for 10 days: only strictly-future occurrences are returned.
        val now = at(2026, 11, 24, 12)
        val next = RecurrenceEngine.nextOccurrences(schedule(Recurrence.Daily()), now, 3)
        assertTrue(next.all { it.scheduledAt.isAfter(now) })
        assertEquals(LocalDate.of(2026, 11, 25), next.first().localDate)
    }

    @Test
    fun zeroCount_isEmpty() {
        assertTrue(RecurrenceEngine.nextOccurrences(schedule(Recurrence.Daily()), at(2026, 11, 14, 7), 0).isEmpty())
    }

    // ---- DST Europe/Prague ----

    @Test
    fun springForwardGap_localTimeIsSkippedThatDay() {
        // 28 Mar 2027: 02:00 -> 03:00 in Europe/Prague; 02:30 does not exist.
        val s = schedule(Recurrence.Daily(), start = LocalDate.of(2027, 3, 1), time = LocalTime.of(2, 30))
        assertNull(RecurrenceEngine.occurrenceOn(s, LocalDate.of(2027, 3, 28)))
        assertEquals(
            listOf(LocalDate.of(2027, 3, 27), LocalDate.of(2027, 3, 29), LocalDate.of(2027, 3, 30)),
            dates(s, at(2027, 3, 27, 0), 3),
        )
    }

    @Test
    fun springForward_validTimeThatDayStillOccurs() {
        val s = schedule(Recurrence.Daily(), start = LocalDate.of(2027, 3, 1), time = LocalTime.of(3, 30))
        val occ = RecurrenceEngine.occurrenceOn(s, LocalDate.of(2027, 3, 28))
        assertNotNull(occ)
        assertEquals(Instant.parse("2027-03-28T01:30:00Z"), occ!!.scheduledAt)
    }

    @Test
    fun fallBackOverlap_singleOccurrenceAtEarlierInstant() {
        // 25 Oct 2026: 03:00 -> 02:00; 02:30 happens at 00:30Z (+02) and 01:30Z (+01).
        val s = schedule(Recurrence.Daily(), start = LocalDate.of(2026, 10, 1), time = LocalTime.of(2, 30))
        val occ = RecurrenceEngine.occurrenceOn(s, LocalDate.of(2026, 10, 25))
        assertEquals(Instant.parse("2026-10-25T00:30:00Z"), occ!!.scheduledAt)

        // Asking again just after the first instant (during the repeated hour) must not yield 25 Oct again.
        val next = RecurrenceEngine.nextOccurrence(s, Instant.parse("2026-10-25T00:30:00Z"))
        assertEquals(LocalDate.of(2026, 10, 26), next!!.localDate)
        val duringRepeat = RecurrenceEngine.nextOccurrence(s, Instant.parse("2026-10-25T01:15:00Z"))
        assertEquals(LocalDate.of(2026, 10, 26), duringRepeat!!.localDate)
    }

    @Test
    fun fallBack_rangeHasNoDuplicateIds() {
        val s = schedule(Recurrence.Daily(), start = LocalDate.of(2026, 10, 1), time = LocalTime.of(2, 30))
        val occ = RecurrenceEngine.nextOccurrences(s, at(2026, 10, 24, 0), 4)
        assertEquals(occ.size, occ.map { it.id }.toSet().size)
        assertEquals(LocalDate.of(2026, 10, 25), occ[1].localDate)
    }

    // ---- Identity ----

    @Test
    fun occurrenceId_isDeterministic_andRevisionIndependent() {
        val r1 = schedule(Recurrence.Daily(), revision = 1)
        val r2 = schedule(Recurrence.Daily(), revision = 2, time = LocalTime.of(20, 0))
        val d = LocalDate.of(2026, 11, 20)
        assertEquals("s@2026-11-20", RecurrenceEngine.occurrenceOn(r1, d)!!.id)
        assertEquals(RecurrenceEngine.occurrenceOn(r1, d)!!.id, RecurrenceEngine.occurrenceOn(r2, d)!!.id)
    }

    @Test
    fun manySchedules_sameDay_idsUnique() {
        val list = listOf(
            schedule(Recurrence.Daily(), id = "a", time = LocalTime.of(8, 0)),
            schedule(Recurrence.Daily(), id = "b", time = LocalTime.of(8, 0)),
            schedule(Recurrence.Weekly(DayOfWeek.entries.toSet()), id = "c", time = LocalTime.of(20, 0)),
        )
        val all = list.flatMap { RecurrenceEngine.nextOccurrences(it, at(2026, 11, 14, 0), 60) }
        assertEquals(all.size, all.map { it.id }.toSet().size)
        assertEquals(3, all.count { it.localDate == LocalDate.of(2026, 11, 20) })
    }
}
