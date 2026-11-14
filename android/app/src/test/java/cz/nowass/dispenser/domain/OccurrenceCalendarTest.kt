package cz.nowass.dispenser.domain

import cz.nowass.dispenser.data.MockSeed
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.time.DayOfWeek
import java.time.LocalDate
import java.time.LocalTime
import java.time.ZonedDateTime

class OccurrenceCalendarTest {

    private val seed = MockSeed.default().schedules

    private fun schedule(id: String, time: LocalTime, recurrence: Recurrence, enabled: Boolean = true) =
        Schedule(id, 1, id, enabled, 5.0, LocalDate.of(2026, 11, 2), time, recurrence)

    @Test
    fun seededMonday_hasMorningAndEvening_inTimeOrder() {
        val occ = OccurrenceCalendar.occurrencesOn(seed, LocalDate.of(2026, 11, 16))
        assertEquals(listOf("demo-morning", "demo-evening"), occ.map { it.scheduleId })
        assertEquals(ZonedDateTime.of(2026, 11, 16, 19, 30, 0, 0, DEFAULT_ZONE).toInstant(), occ[1].scheduledAt)
        assertEquals(15.0, occ.sumOf { it.amountMl }, 0.0)
    }

    @Test
    fun disabledSchedule_neverAppears() {
        // demo-weekend is disabled; 21 Nov is one of its on-week Saturdays.
        val occ = OccurrenceCalendar.occurrencesOn(seed, LocalDate.of(2026, 11, 21))
        assertTrue(occ.none { it.scheduleId == "demo-weekend" })
    }

    @Test
    fun beforeStartDate_isEmpty() {
        assertTrue(OccurrenceCalendar.occurrencesOn(seed, LocalDate.of(2026, 11, 1)).isEmpty())
    }

    @Test
    fun sameTimeSchedules_orderedById_noDuplicates() {
        val b = schedule("b", LocalTime.of(8, 0), Recurrence.Daily())
        val a = schedule("a", LocalTime.of(8, 0), Recurrence.Daily())
        val occ = OccurrenceCalendar.occurrencesOn(listOf(b, a, a), LocalDate.of(2026, 11, 20))
        // Duplicated input schedule object still yields deterministic ordering.
        assertEquals(listOf("a", "a", "b"), occ.map { it.scheduleId })
        assertEquals(2, OccurrenceCalendar.occurrencesOn(listOf(b, a), LocalDate.of(2026, 11, 20)).size)
    }

    @Test
    fun range_containsEveryDate_andCountsMatchRules() {
        val map = OccurrenceCalendar.occurrencesBetween(seed, LocalDate.of(2026, 11, 16), LocalDate.of(2026, 11, 22))
        assertEquals(7, map.size)
        assertEquals(listOf(2, 1, 1, 2, 1, 1, 1), map.values.map { it.size })
    }

    @Test
    fun weeklyEveryOtherWeek_inRange() {
        val s = schedule("w", LocalTime.of(10, 0), Recurrence.Weekly(setOf(DayOfWeek.SATURDAY), intervalWeeks = 2))
        val map = OccurrenceCalendar.occurrencesBetween(listOf(s), LocalDate.of(2026, 11, 1), LocalDate.of(2026, 11, 30))
        assertEquals(
            listOf(LocalDate.of(2026, 11, 7), LocalDate.of(2026, 11, 21)),
            map.filterValues { it.isNotEmpty() }.keys.toList(),
        )
    }

    @Test
    fun dstFallBackDay_singleOccurrence() {
        // 25 Oct 2026: 02:30 occurs twice in Europe/Prague; still exactly one occurrence.
        val s = Schedule("d", 1, "d", true, 5.0, LocalDate.of(2026, 10, 1), LocalTime.of(2, 30), Recurrence.Daily())
        assertEquals(1, OccurrenceCalendar.occurrencesOn(listOf(s), LocalDate.of(2026, 10, 25)).size)
    }

    @Test(expected = IllegalArgumentException::class)
    fun invertedRange_rejected() {
        OccurrenceCalendar.occurrencesBetween(seed, LocalDate.of(2026, 11, 2), LocalDate.of(2026, 11, 1))
    }
}
