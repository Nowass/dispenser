package cz.nowass.dispenser.domain

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.time.DayOfWeek
import java.time.Instant
import java.time.LocalDate
import java.time.LocalTime
import java.time.ZonedDateTime
import kotlin.random.Random

/** A1.2c QA: long intervals, leap years, month/year boundaries, DST combinations. */
class RecurrenceEdgeCaseTest {

    private fun at(y: Int, mo: Int, d: Int, h: Int = 12) = ZonedDateTime.of(y, mo, d, h, 0, 0, 0, DEFAULT_ZONE).toInstant()

    private fun s(r: Recurrence, start: LocalDate, time: LocalTime = LocalTime.of(8, 0), end: LocalDate? = null) =
        Schedule("s", 1, "s", true, 1.0, start, time, r, end)

    private fun dates(sch: Schedule, after: Instant, n: Int) = RecurrenceEngine.nextOccurrences(sch, after, n).map { it.localDate }

    // ---- Long intervals (previously limited by a 3-year scan window) ----

    @Test
    fun veryLongIntervals_stillFindFutureOccurrences() {
        val daily = s(Recurrence.Daily(2000), LocalDate.of(2026, 11, 14))
        assertEquals(LocalDate.of(2026, 11, 14).plusDays(2000), RecurrenceEngine.nextOccurrence(daily, at(2026, 11, 15))?.localDate)
        val weekly = s(Recurrence.Weekly(setOf(DayOfWeek.MONDAY), 200), LocalDate.of(2026, 11, 2))
        assertEquals(LocalDate.of(2026, 11, 2).plusWeeks(200), RecurrenceEngine.nextOccurrence(weekly, at(2026, 11, 15))?.localDate)
    }

    @Test
    fun startFarInPast_isEfficientAndAligned() {
        val daily = s(Recurrence.Daily(7), LocalDate.of(1990, 1, 1)) // a Monday
        val next = RecurrenceEngine.nextOccurrence(daily, at(2026, 11, 15))!!.localDate
        assertEquals(DayOfWeek.MONDAY, next.dayOfWeek)
        assertEquals(LocalDate.of(2026, 11, 16), next)
    }

    // ---- Leap years / month and year boundaries ----

    @Test
    fun daily_crossesLeapDay() {
        val sch = s(Recurrence.Daily(1), LocalDate.of(2028, 2, 27))
        assertEquals(
            listOf(LocalDate.of(2028, 2, 27), LocalDate.of(2028, 2, 28), LocalDate.of(2028, 2, 29), LocalDate.of(2028, 3, 1)),
            dates(sch, at(2028, 2, 26), 4),
        )
    }

    @Test
    fun everySecondDay_overFebruaryInLeapAndCommonYear() {
        assertEquals(LocalDate.of(2028, 3, 1), dates(s(Recurrence.Daily(2), LocalDate.of(2028, 2, 28)), at(2028, 2, 28, 9), 1).single())
        assertEquals(LocalDate.of(2027, 3, 2), dates(s(Recurrence.Daily(2), LocalDate.of(2027, 2, 28)), at(2027, 2, 28, 9), 1).single())
    }

    @Test
    fun once_onLeapDay() {
        val sch = s(Recurrence.Once, LocalDate.of(2028, 2, 29))
        assertEquals(listOf(LocalDate.of(2028, 2, 29)), dates(sch, at(2027, 1, 1), 3))
        assertTrue(dates(sch, at(2028, 3, 1), 3).isEmpty())
    }

    @Test
    fun weekly_acrossYearBoundary_and53WeekIsoYear() {
        // 2026 has ISO week 53 (Mon 28 Dec 2026); every-2-weeks Monday anchored on 14 Dec 2026.
        val sch = s(Recurrence.Weekly(setOf(DayOfWeek.MONDAY), 2), LocalDate.of(2026, 12, 14))
        assertEquals(
            listOf(LocalDate.of(2026, 12, 14), LocalDate.of(2026, 12, 28), LocalDate.of(2027, 1, 11)),
            dates(sch, at(2026, 12, 1), 3),
        )
    }

    @Test
    fun endDate_onMonthEnd_isInclusive() {
        val sch = s(Recurrence.Daily(1), LocalDate.of(2027, 1, 30), end = LocalDate.of(2027, 1, 31))
        assertEquals(listOf(LocalDate.of(2027, 1, 30), LocalDate.of(2027, 1, 31)), dates(sch, at(2027, 1, 1), 5))
    }

    // ---- DST combined with intervals / end dates ----

    @Test
    fun gapDay_skipped_evenWhenItIsTheOnlyMatchBeforeEndDate() {
        val sch = s(Recurrence.Daily(1), LocalDate.of(2027, 3, 28), LocalTime.of(2, 30), end = LocalDate.of(2027, 3, 28))
        assertTrue(dates(sch, at(2027, 3, 1), 3).isEmpty())
    }

    @Test
    fun weekly_onDstSundays_gapSkipped_overlapOnce() {
        val sch = s(Recurrence.Weekly(setOf(DayOfWeek.SUNDAY)), LocalDate.of(2027, 3, 21), LocalTime.of(2, 30))
        assertEquals(listOf(LocalDate.of(2027, 3, 21), LocalDate.of(2027, 4, 4)), dates(sch, at(2027, 3, 20), 2))
        val autumn = s(Recurrence.Weekly(setOf(DayOfWeek.SUNDAY)), LocalDate.of(2026, 10, 18), LocalTime.of(2, 30))
        val occ = RecurrenceEngine.nextOccurrences(autumn, at(2026, 10, 19), 2)
        assertEquals(listOf(LocalDate.of(2026, 10, 25), LocalDate.of(2026, 11, 1)), occ.map { it.localDate })
        assertEquals(Instant.parse("2026-10-25T00:30:00Z"), occ.first().scheduledAt)
    }

    @Test
    fun once_inDstGap_neverOccurs() {
        val sch = s(Recurrence.Once, LocalDate.of(2027, 3, 28), LocalTime.of(2, 15))
        assertNull(RecurrenceEngine.nextOccurrence(sch, at(2027, 3, 1)))
    }

    // ---- Jump search must equal the brute-force day-by-day definition ----

    @Test
    fun jumpSearch_matchesBruteForce_forRandomSchedules() {
        val rnd = Random(20261115)
        repeat(400) {
            val start = LocalDate.of(2026, 1, 1).plusDays(rnd.nextLong(0, 900))
            val recurrence = when (rnd.nextInt(3)) {
                0 -> Recurrence.Once
                1 -> Recurrence.Daily(rnd.nextInt(1, 40))
                else -> Recurrence.Weekly(DayOfWeek.entries.filter { rnd.nextBoolean() }.toSet().ifEmpty { setOf(DayOfWeek.FRIDAY) }, rnd.nextInt(1, 6))
            }
            val end = if (recurrence != Recurrence.Once && rnd.nextBoolean()) start.plusDays(rnd.nextLong(0, 400)) else null
            val time = LocalTime.of(rnd.nextInt(0, 24), listOf(0, 15, 30, 45)[rnd.nextInt(4)])
            val sch = s(recurrence, start, time, end)
            val after = ZonedDateTime.of(LocalDate.of(2026, 1, 1).plusDays(rnd.nextLong(0, 1000)).atTime(rnd.nextInt(0, 24), 0), DEFAULT_ZONE).toInstant()

            val expected = generateSequence(after.atZone(DEFAULT_ZONE).toLocalDate().minusDays(1)) { it.plusDays(1) }
                .take(1500)
                .mapNotNull { RecurrenceEngine.occurrenceOn(sch, it) }
                .filter { it.scheduledAt.isAfter(after) }
                .take(5)
                .toList()
            assertEquals("schedule=$sch after=$after", expected, RecurrenceEngine.nextOccurrences(sch, after, 5))
        }
    }

    // ---- occurrencesBetween (used by the simulation) ----

    @Test
    fun occurrencesBetween_isExclusiveStartInclusiveEnd_andBounded() {
        val sch = s(Recurrence.Daily(1), LocalDate.of(2026, 11, 1))
        val list = RecurrenceEngine.occurrencesBetween(sch, at(2026, 11, 14, 8), at(2026, 11, 17, 8), 10)!!
        assertEquals(listOf(15, 16, 17).map { LocalDate.of(2026, 11, it) }, list.map { it.localDate })
        assertNull(RecurrenceEngine.occurrencesBetween(sch, at(2026, 11, 1, 0), at(2026, 11, 30, 0), 5))
        assertTrue(RecurrenceEngine.occurrencesBetween(sch, at(2026, 11, 14, 9), at(2026, 11, 14, 23), 0)!!.isEmpty())
    }
}
