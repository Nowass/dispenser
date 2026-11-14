package cz.nowass.dispenser.domain

import java.time.Instant
import java.time.LocalDate
import java.time.ZonedDateTime
import java.time.temporal.ChronoUnit

/**
 * Minimal, pure next-occurrence lookup used by the A1.1 dashboard.
 *
 * Scope note: the full recurrence engine (occurrence windows, next-5 preview,
 * documented DST gap/overlap policy) is milestone A1.2. Until then, local times
 * that fall into a DST gap are resolved by [ZonedDateTime.of]'s default
 * (shifted forward) and this is NOT yet an approved product rule.
 */
object NextDoseCalculator {

    /** Upper bound on days scanned per schedule, to keep work bounded. */
    private const val MAX_SCAN_DAYS = 3L * 366

    /** Earliest occurrence strictly after [after] across all enabled schedules. */
    fun nextDose(schedules: List<Schedule>, after: Instant): DoseOccurrence? =
        schedules
            .asSequence()
            .filter { it.enabled }
            .mapNotNull { nextOccurrence(it, after) }
            // Deterministic order for ties: time, then schedule id.
            .minWithOrNull(compareBy<DoseOccurrence>({ it.scheduledAt }, { it.scheduleId }))

    /** Next occurrence of [schedule] strictly after [after], or null if none. */
    fun nextOccurrence(schedule: Schedule, after: Instant): DoseOccurrence? {
        val afterLocalDate = after.atZone(schedule.zone).toLocalDate()
        // The previous day is included so that the scan never misses an occurrence
        // whose local date differs from the UTC instant's date.
        var date = maxOf(schedule.startDate, afterLocalDate.minusDays(1))
        val lastDate = afterLocalDate.plusDays(MAX_SCAN_DAYS)
        while (!date.isAfter(lastDate)) {
            if (schedule.endDate != null && date.isAfter(schedule.endDate)) return null
            if (occursOn(schedule, date)) {
                val at = ZonedDateTime.of(date, schedule.localTime, schedule.zone).toInstant()
                if (at.isAfter(after)) {
                    return DoseOccurrence(
                        scheduleId = schedule.id,
                        scheduleRevision = schedule.revision,
                        scheduleName = schedule.name,
                        localDate = date,
                        scheduledAt = at,
                        amountMl = schedule.amountMl,
                    )
                }
            }
            if (schedule.recurrence is Recurrence.Once && date >= schedule.startDate) return null
            date = date.plusDays(1)
        }
        return null
    }

    internal fun occursOn(schedule: Schedule, date: LocalDate): Boolean {
        if (date.isBefore(schedule.startDate)) return false
        if (schedule.endDate != null && date.isAfter(schedule.endDate)) return false
        return when (val r = schedule.recurrence) {
            Recurrence.Once -> date == schedule.startDate
            is Recurrence.Daily ->
                ChronoUnit.DAYS.between(schedule.startDate, date) % r.intervalDays == 0L
            is Recurrence.Weekly -> {
                // Week index counted from the Monday of the start week.
                val startWeekMonday = schedule.startDate.minusDays(schedule.startDate.dayOfWeek.value - 1L)
                val weeks = ChronoUnit.WEEKS.between(startWeekMonday, date)
                date.dayOfWeek in r.weekdays && weeks % r.intervalWeeks == 0L
            }
        }
    }
}
