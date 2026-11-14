package cz.nowass.dispenser.domain

import java.time.LocalDate

/**
 * Pure calendar projection of schedules onto local dates.
 *
 * - At most one occurrence per schedule per local date (no duplicates).
 * - Disabled schedules produce no occurrences.
 * - Ordered by scheduled instant, then schedule id (deterministic).
 *
 * Rules and DST policy come from [RecurrenceEngine] (gap times skipped,
 * repeated autumn times produce one occurrence).
 */
object OccurrenceCalendar {

    /** Maximum span accepted by [occurrencesBetween], keeps work bounded. */
    const val MAX_RANGE_DAYS = 366L

    fun occurrencesOn(schedules: List<Schedule>, date: LocalDate): List<DoseOccurrence> =
        schedules
            .asSequence()
            .filter { it.enabled }
            .mapNotNull { RecurrenceEngine.occurrenceOn(it, date) }
            .sortedWith(ORDER)
            .toList()

    /** Occurrences for every date in [from]..[toInclusive]; dates without doses map to empty lists. */
    fun occurrencesBetween(
        schedules: List<Schedule>,
        from: LocalDate,
        toInclusive: LocalDate,
    ): Map<LocalDate, List<DoseOccurrence>> {
        require(!toInclusive.isBefore(from)) { "toInclusive before from" }
        require(java.time.temporal.ChronoUnit.DAYS.between(from, toInclusive) < MAX_RANGE_DAYS) { "range too large" }
        val result = LinkedHashMap<LocalDate, List<DoseOccurrence>>()
        var date = from
        while (!date.isAfter(toInclusive)) {
            result[date] = occurrencesOn(schedules, date)
            date = date.plusDays(1)
        }
        return result
    }

    private val ORDER = compareBy<DoseOccurrence>({ it.scheduledAt }, { it.scheduleId })
}
