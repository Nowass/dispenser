package cz.nowass.dispenser.domain

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test
import java.time.DayOfWeek
import java.time.LocalDate
import java.time.LocalTime
import java.time.ZonedDateTime

class NextDoseCalculatorTest {

    private fun at(y: Int, mo: Int, d: Int, h: Int, mi: Int = 0) =
        ZonedDateTime.of(y, mo, d, h, mi, 0, 0, DEFAULT_ZONE).toInstant()

    private fun schedule(
        id: String = "s",
        recurrence: Recurrence,
        start: LocalDate = LocalDate.of(2026, 11, 2), // Monday
        time: LocalTime = LocalTime.of(8, 0),
        enabled: Boolean = true,
        endDate: LocalDate? = null,
        amountMl: Double = 10.0,
    ) = Schedule(id, 1, id, enabled, amountMl, start, time, recurrence, endDate)

    @Test
    fun daily_returnsSameDayWhenTimeNotYetPassed() {
        val next = NextDoseCalculator.nextOccurrence(schedule(recurrence = Recurrence.Daily()), at(2026, 11, 14, 7, 30))
        assertEquals(at(2026, 11, 14, 8), next?.scheduledAt)
    }

    @Test
    fun daily_isStrictlyAfter_soExactTimeMovesToNextDay() {
        val next = NextDoseCalculator.nextOccurrence(schedule(recurrence = Recurrence.Daily()), at(2026, 11, 14, 8))
        assertEquals(at(2026, 11, 15, 8), next?.scheduledAt)
    }

    @Test
    fun everyThirdDay_countsFromStartDate() {
        // Start Mon 2 Nov -> 2, 5, 8, 11, 14, 17 ...
        val s = schedule(recurrence = Recurrence.Daily(intervalDays = 3))
        assertEquals(at(2026, 11, 14, 8), NextDoseCalculator.nextOccurrence(s, at(2026, 11, 12, 9))?.scheduledAt)
        assertEquals(at(2026, 11, 17, 8), NextDoseCalculator.nextOccurrence(s, at(2026, 11, 14, 9))?.scheduledAt)
    }

    @Test
    fun weekly_selectedWeekdays() {
        val s = schedule(recurrence = Recurrence.Weekly(setOf(DayOfWeek.MONDAY, DayOfWeek.THURSDAY)))
        // From Sat 14 Nov -> Mon 16 Nov
        assertEquals(at(2026, 11, 16, 8), NextDoseCalculator.nextOccurrence(s, at(2026, 11, 14, 7))?.scheduledAt)
        // From Mon 16 Nov 09:00 -> Thu 19 Nov
        assertEquals(at(2026, 11, 19, 8), NextDoseCalculator.nextOccurrence(s, at(2026, 11, 16, 9))?.scheduledAt)
    }

    @Test
    fun everyOtherWeek_skipsOffWeeks() {
        // Start week of Mon 2 Nov; Saturdays in on-weeks: 7 Nov, 21 Nov.
        val s = schedule(recurrence = Recurrence.Weekly(setOf(DayOfWeek.SATURDAY), intervalWeeks = 2))
        assertEquals(at(2026, 11, 21, 8), NextDoseCalculator.nextOccurrence(s, at(2026, 11, 8, 0))?.scheduledAt)
    }

    @Test
    fun once_inPast_returnsNull_andInFuture_returnsIt() {
        val s = schedule(recurrence = Recurrence.Once, start = LocalDate.of(2026, 11, 20))
        assertEquals(at(2026, 11, 20, 8), NextDoseCalculator.nextOccurrence(s, at(2026, 11, 14, 7))?.scheduledAt)
        assertNull(NextDoseCalculator.nextOccurrence(s, at(2026, 11, 20, 8)))
        assertNull(NextDoseCalculator.nextOccurrence(s, at(2026, 12, 1, 0)))
    }

    @Test
    fun endDate_stopsRecurrence() {
        val s = schedule(recurrence = Recurrence.Daily(), endDate = LocalDate.of(2026, 11, 14))
        assertNull(NextDoseCalculator.nextOccurrence(s, at(2026, 11, 14, 9)))
    }

    @Test
    fun startDateInFuture_returnsFirstOccurrence() {
        val s = schedule(recurrence = Recurrence.Daily(), start = LocalDate.of(2027, 1, 1))
        assertEquals(at(2027, 1, 1, 8), NextDoseCalculator.nextOccurrence(s, at(2026, 11, 14, 9))?.scheduledAt)
    }

    @Test
    fun nextDose_ignoresDisabled_andPicksEarliest() {
        val disabledEarly = schedule(id = "a", recurrence = Recurrence.Daily(), time = LocalTime.of(7, 45), enabled = false)
        val later = schedule(id = "b", recurrence = Recurrence.Daily(), time = LocalTime.of(9, 0))
        val earlier = schedule(id = "c", recurrence = Recurrence.Daily(), time = LocalTime.of(8, 0))
        val next = NextDoseCalculator.nextDose(listOf(disabledEarly, later, earlier), at(2026, 11, 14, 7))
        assertEquals("c", next?.scheduleId)
    }

    @Test
    fun nextDose_tieIsBrokenDeterministicallyByScheduleId() {
        val z = schedule(id = "z", recurrence = Recurrence.Daily())
        val a = schedule(id = "a", recurrence = Recurrence.Daily())
        assertEquals("a", NextDoseCalculator.nextDose(listOf(z, a), at(2026, 11, 14, 7))?.scheduleId)
    }

    @Test
    fun nextDose_noEnabledSchedules_returnsNull() {
        assertNull(NextDoseCalculator.nextDose(emptyList(), at(2026, 11, 14, 7)))
    }
}
