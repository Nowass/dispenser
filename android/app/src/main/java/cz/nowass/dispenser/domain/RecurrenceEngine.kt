package cz.nowass.dispenser.domain

import java.time.Instant
import java.time.LocalDate
import java.time.LocalDateTime
import java.time.ZonedDateTime
import java.time.temporal.ChronoUnit

/**
 * Pure, deterministic recurrence engine — the single source of truth for which
 * local dates a schedule occurs on and at which instant.
 *
 * Rules (docs/ARCHITECTURE.md "Calendar-style recurrence model"):
 * - ONCE: exactly on [Schedule.startDate].
 * - DAILY every N days: calendar-day distance from start date divisible by N.
 * - WEEKLY every N weeks: selected weekdays, in weeks (Monday-based) whose
 *   distance from the start date's week is divisible by N; never before start.
 * - Optional end date is inclusive.
 *
 * DST policy in the schedule's zone (approved A1.2b):
 * - A local time that does not exist (spring-forward gap) is SKIPPED that day.
 * - A local time that occurs twice (autumn fall-back) yields ONE occurrence,
 *   at the earlier instant (pre-transition offset).
 *
 * Identity: at most one occurrence per schedule per local date; see
 * [DoseOccurrence.id]. The engine never "catches up": callers ask only for
 * occurrences strictly after a given instant.
 */
object RecurrenceEngine {

    /** Bound on days scanned when searching forward, keeps work bounded. */
    const val MAX_SCAN_DAYS = 3L * 366

    /** Whether the recurrence rule matches [date] (ignores DST and enabled flag). */
    fun occursOn(schedule: Schedule, date: LocalDate): Boolean {
        if (date.isBefore(schedule.startDate)) return false
        if (schedule.endDate != null && date.isAfter(schedule.endDate)) return false
        return when (val r = schedule.recurrence) {
            Recurrence.Once -> date == schedule.startDate
            is Recurrence.Daily ->
                ChronoUnit.DAYS.between(schedule.startDate, date) % r.intervalDays == 0L
            is Recurrence.Weekly -> {
                val startWeekMonday = schedule.startDate.minusDays(schedule.startDate.dayOfWeek.value - 1L)
                val weeks = ChronoUnit.WEEKS.between(startWeekMonday, date)
                date.dayOfWeek in r.weekdays && weeks % r.intervalWeeks == 0L
            }
        }
    }

    /**
     * The occurrence of [schedule] on [date], or null if the rule does not match
     * or the local time does not exist that day (DST gap → skipped).
     */
    fun occurrenceOn(schedule: Schedule, date: LocalDate): DoseOccurrence? {
        if (!occursOn(schedule, date)) return null
        val local = LocalDateTime.of(date, schedule.localTime)
        val offsets = schedule.zone.rules.getValidOffsets(local)
        if (offsets.isEmpty()) return null // spring-forward gap: skip, do not shift
        // Overlap: ofLocal with null preference picks the earlier offset (first instant).
        val at = ZonedDateTime.ofLocal(local, schedule.zone, null).toInstant()
        return DoseOccurrence(
            scheduleId = schedule.id,
            scheduleRevision = schedule.revision,
            scheduleName = schedule.name,
            localDate = date,
            scheduledAt = at,
            amountMl = schedule.amountMl,
        )
    }

    /** Next occurrence strictly after [after], or null if none within the scan bound. */
    fun nextOccurrence(schedule: Schedule, after: Instant): DoseOccurrence? =
        nextOccurrences(schedule, after, 1).firstOrNull()

    /** Up to [count] occurrences strictly after [after], in time order. */
    fun nextOccurrences(schedule: Schedule, after: Instant, count: Int): List<DoseOccurrence> {
        require(count >= 0) { "count must be >= 0" }
        if (count == 0) return emptyList()
        val afterLocalDate = after.atZone(schedule.zone).toLocalDate()
        // Start one day early so a local date differing from the UTC date is never missed.
        var date = maxOf(schedule.startDate, afterLocalDate.minusDays(1))
        val lastDate = afterLocalDate.plusDays(MAX_SCAN_DAYS)
        val result = ArrayList<DoseOccurrence>(count)
        while (!date.isAfter(lastDate) && result.size < count) {
            if (schedule.endDate != null && date.isAfter(schedule.endDate)) break
            if (schedule.recurrence is Recurrence.Once && date.isAfter(schedule.startDate)) break
            val occ = occurrenceOn(schedule, date)
            if (occ != null && occ.scheduledAt.isAfter(after)) result += occ
            date = date.plusDays(1)
        }
        return result
    }
}
