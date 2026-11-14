package cz.nowass.dispenser.domain

import java.time.Instant

/** Next-dose lookup for the dashboard; delegates rules and DST policy to [RecurrenceEngine]. */
object NextDoseCalculator {

    /** Earliest occurrence strictly after [after] across all enabled schedules. */
    fun nextDose(schedules: List<Schedule>, after: Instant): DoseOccurrence? =
        schedules
            .asSequence()
            .filter { it.enabled }
            .mapNotNull { nextOccurrence(it, after) }
            // Deterministic order for ties: time, then schedule id.
            .minWithOrNull(compareBy<DoseOccurrence>({ it.scheduledAt }, { it.scheduleId }))

    /** Next occurrence of [schedule] strictly after [after], or null if none. */
    fun nextOccurrence(schedule: Schedule, after: Instant): DoseOccurrence? =
        RecurrenceEngine.nextOccurrence(schedule, after)
}
