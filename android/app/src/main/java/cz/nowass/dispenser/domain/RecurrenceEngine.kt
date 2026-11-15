package cz.nowass.dispenser.domain

import java.time.Instant
import java.time.LocalDate
import java.time.LocalDateTime
import java.time.ZonedDateTime
import java.time.temporal.ChronoUnit
import java.time.temporal.TemporalAdjusters
import java.time.DayOfWeek

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

    /** Safety cap on candidate dates examined per query (each candidate is a rule match). */
    private const val MAX_CANDIDATES = 10_000

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
        return occurrencesAfter(schedule, after).take(count).toList()
    }

    /**
     * All occurrences in (afterExclusive, untilInclusive], in time order, or null when there
     * are more than [limit] (callers must reject such ranges explicitly instead of truncating).
     */
    fun occurrencesBetween(schedule: Schedule, afterExclusive: Instant, untilInclusive: Instant, limit: Int): List<DoseOccurrence>? {
        require(limit >= 0) { "limit must be >= 0" }
        val result = ArrayList<DoseOccurrence>()
        for (occ in occurrencesAfter(schedule, afterExclusive)) {
            if (occ.scheduledAt.isAfter(untilInclusive)) break
            if (result.size == limit) return null
            result += occ
        }
        return result
    }

    /**
     * Lazy, strictly chronological occurrences after [after]. Jumps directly from one
     * rule-matching date to the next (no day-by-day scan), so any interval length works.
     * A matching date whose local time falls into a DST gap is skipped.
     */
    private fun occurrencesAfter(schedule: Schedule, after: Instant): Sequence<DoseOccurrence> = sequence {
        val afterLocalDate = after.atZone(schedule.zone).toLocalDate()
        // Start one day early so a local date differing from the UTC date is never missed.
        var date = firstMatchOnOrAfter(schedule, afterLocalDate.minusDays(1))
        var examined = 0
        while (date != null && examined++ < MAX_CANDIDATES) {
            if (schedule.endDate != null && date.isAfter(schedule.endDate)) break
            val occ = occurrenceOn(schedule, date)
            if (occ != null && occ.scheduledAt.isAfter(after)) yield(occ)
            date = firstMatchOnOrAfter(schedule, date.plusDays(1))
        }
    }

    /**
     * First date >= [from] (and >= start date) matching the rule, ignoring end date and DST.
     * Null when there is none (ONCE already passed).
     */
    internal fun firstMatchOnOrAfter(schedule: Schedule, from: LocalDate): LocalDate? {
        val start = schedule.startDate
        val d0 = maxOf(from, start)
        return when (val r = schedule.recurrence) {
            Recurrence.Once -> if (d0 == start) start else null
            is Recurrence.Daily -> {
                val rem = Math.floorMod(ChronoUnit.DAYS.between(start, d0), r.intervalDays.toLong())
                if (rem == 0L) d0 else d0.plusDays(r.intervalDays - rem)
            }
            is Recurrence.Weekly -> {
                val startMonday = start.with(TemporalAdjusters.previousOrSame(DayOfWeek.MONDAY))
                var monday = d0.with(TemporalAdjusters.previousOrSame(DayOfWeek.MONDAY))
                val offWeeks = Math.floorMod(ChronoUnit.WEEKS.between(startMonday, monday), r.intervalWeeks.toLong())
                if (offWeeks != 0L) monday = monday.plusWeeks(r.intervalWeeks - offWeeks)
                // This on-week from d0 onwards, otherwise the following on-week (weekdays is non-empty).
                firstSelectedDay(monday, d0, r.weekdays)
                    ?: firstSelectedDay(monday.plusWeeks(r.intervalWeeks.toLong()), d0, r.weekdays)
            }
        }
    }

    private fun firstSelectedDay(monday: LocalDate, notBefore: LocalDate, weekdays: Set<DayOfWeek>): LocalDate? =
        (0L..6L).asSequence().map(monday::plusDays).firstOrNull { !it.isBefore(notBefore) && it.dayOfWeek in weekdays }
}
