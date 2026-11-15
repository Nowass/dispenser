package cz.nowass.dispenser.domain

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.time.DayOfWeek
import java.time.LocalDate
import java.time.LocalTime

class ScheduleValidatorTest {

    private val base = ScheduleDraft(
        name = "  Ranní  ",
        amountMl = 10.0,
        startDate = LocalDate.of(2026, 11, 16),
        localTime = LocalTime.of(8, 0),
        kind = RecurrenceKind.DAILY,
    )

    private fun errors(d: ScheduleDraft) = ScheduleValidator.validate(d)

    @Test
    fun valid_hasNoErrors() = assertTrue(errors(base).isEmpty())

    @Test
    fun amount_mustBePositiveAndFinite() {
        assertEquals(setOf(ValidationError.AMOUNT_NOT_POSITIVE), errors(base.copy(amountMl = 0.0)))
        assertEquals(setOf(ValidationError.AMOUNT_NOT_POSITIVE), errors(base.copy(amountMl = -1.0)))
        assertEquals(setOf(ValidationError.AMOUNT_NOT_FINITE), errors(base.copy(amountMl = Double.NaN)))
        assertEquals(setOf(ValidationError.AMOUNT_NOT_FINITE), errors(base.copy(amountMl = Double.POSITIVE_INFINITY)))
        assertTrue(errors(base.copy(amountMl = 0.1)).isEmpty())
    }

    @Test
    fun noFixedUpperLimit_isEnforced() {
        // FR-016 limits are TBD; validator must not invent them.
        assertTrue(errors(base.copy(amountMl = 5000.0)).isEmpty())
    }

    @Test
    fun interval_atLeastOne_exceptOnce() {
        assertEquals(setOf(ValidationError.INTERVAL_TOO_SMALL), errors(base.copy(interval = 0)))
        assertTrue(errors(base.copy(kind = RecurrenceKind.ONCE, interval = 0)).isEmpty())
    }

    @Test
    fun weekly_requiresWeekday() {
        assertEquals(setOf(ValidationError.NO_WEEKDAYS), errors(base.copy(kind = RecurrenceKind.WEEKLY)))
        assertTrue(errors(base.copy(kind = RecurrenceKind.WEEKLY, weekdays = setOf(DayOfWeek.FRIDAY))).isEmpty())
    }

    @Test
    fun endDate_notBeforeStart_sameDayAllowed_ignoredForOnce() {
        assertEquals(setOf(ValidationError.END_BEFORE_START), errors(base.copy(endDate = base.startDate.minusDays(1))))
        assertTrue(errors(base.copy(endDate = base.startDate)).isEmpty())
        assertTrue(errors(base.copy(kind = RecurrenceKind.ONCE, endDate = base.startDate.minusDays(1))).isEmpty())
    }

    @Test
    fun multipleErrors_reportedTogether() {
        val e = errors(base.copy(amountMl = 0.0, kind = RecurrenceKind.WEEKLY, interval = 0, endDate = base.startDate.minusDays(3)))
        assertEquals(
            setOf(ValidationError.AMOUNT_NOT_POSITIVE, ValidationError.INTERVAL_TOO_SMALL, ValidationError.NO_WEEKDAYS, ValidationError.END_BEFORE_START),
            e,
        )
    }

    @Test
    fun toSchedule_trimsName_dropsEndDateForOnce_roundTripsViaDraft() {
        val weekly = base.copy(kind = RecurrenceKind.WEEKLY, interval = 2, weekdays = setOf(DayOfWeek.MONDAY), endDate = LocalDate.of(2026, 12, 31))
        val s = weekly.toSchedule("id1", 3)
        assertEquals("Ranní", s.name)
        assertEquals(Recurrence.Weekly(setOf(DayOfWeek.MONDAY), 2), s.recurrence)
        assertEquals(weekly.copy(name = "Ranní"), s.toDraft())

        val once = base.copy(kind = RecurrenceKind.ONCE, endDate = LocalDate.of(2026, 12, 31)).toSchedule("id2", 1)
        assertNull(once.endDate)
        assertEquals(Recurrence.Once, once.recurrence)
    }

    // ---- I3: one-off dose at a nonexistent DST local time ----

    @Test
    fun once_inSpringGap_isRejected_recurringIsAllowed() {
        val gap = base.copy(kind = RecurrenceKind.ONCE, startDate = LocalDate.of(2027, 3, 28), localTime = LocalTime.of(2, 15))
        assertEquals(setOf(ValidationError.TIME_NONEXISTENT), errors(gap))
        // Recurring schedules keep the approved behaviour: the gap day is skipped, not an error.
        assertTrue(errors(gap.copy(kind = RecurrenceKind.DAILY)).isEmpty())
        assertTrue(errors(gap.copy(kind = RecurrenceKind.WEEKLY, weekdays = setOf(DayOfWeek.SUNDAY))).isEmpty())
        // Boundaries: 01:59 and 03:00 exist on that day.
        assertTrue(errors(gap.copy(localTime = LocalTime.of(1, 59))).isEmpty())
        assertTrue(errors(gap.copy(localTime = LocalTime.of(3, 0))).isEmpty())
    }

    @Test
    fun once_inAutumnOverlap_isAllowed() {
        // 02:30 on 25 Oct 2026 occurs twice; it exists, so a one-off is valid (runs once, first instant).
        assertTrue(errors(base.copy(kind = RecurrenceKind.ONCE, startDate = LocalDate.of(2026, 10, 25), localTime = LocalTime.of(2, 30))).isEmpty())
    }
}
