package cz.nowass.dispenser.domain

import java.time.DayOfWeek
import java.time.LocalDate
import java.time.LocalTime
import java.time.ZoneId

enum class RecurrenceKind { ONCE, DAILY, WEEKLY }

/**
 * User-entered schedule definition before it becomes a [Schedule]. Holds raw
 * values (e.g. interval 0, empty weekdays) so they can be validated instead of
 * throwing from [Recurrence] constructors.
 */
data class ScheduleDraft(
    val name: String,
    val amountMl: Double,
    val startDate: LocalDate,
    val localTime: LocalTime,
    val kind: RecurrenceKind,
    val interval: Int = 1,
    val weekdays: Set<DayOfWeek> = emptySet(),
    /** Inclusive; ignored for ONCE. */
    val endDate: LocalDate? = null,
    val enabled: Boolean = true,
    val zone: ZoneId = DEFAULT_ZONE,
)

enum class ValidationError {
    AMOUNT_NOT_FINITE,
    AMOUNT_NOT_POSITIVE,
    INTERVAL_TOO_SMALL,
    NO_WEEKDAYS,
    END_BEFORE_START,
}

/**
 * Validation of business-level constraints only. No physical pump limits are
 * enforced here: FR-016 maximums are still TBD after pump tests.
 */
object ScheduleValidator {
    fun validate(draft: ScheduleDraft): Set<ValidationError> = buildSet {
        when {
            !draft.amountMl.isFinite() -> add(ValidationError.AMOUNT_NOT_FINITE)
            draft.amountMl <= 0.0 -> add(ValidationError.AMOUNT_NOT_POSITIVE)
        }
        if (draft.kind != RecurrenceKind.ONCE && draft.interval < 1) add(ValidationError.INTERVAL_TOO_SMALL)
        if (draft.kind == RecurrenceKind.WEEKLY && draft.weekdays.isEmpty()) add(ValidationError.NO_WEEKDAYS)
        if (draft.kind != RecurrenceKind.ONCE && draft.endDate != null && draft.endDate.isBefore(draft.startDate)) {
            add(ValidationError.END_BEFORE_START)
        }
    }
}

/** Requires a valid draft (see [ScheduleValidator]). */
fun ScheduleDraft.toRecurrence(): Recurrence = when (kind) {
    RecurrenceKind.ONCE -> Recurrence.Once
    RecurrenceKind.DAILY -> Recurrence.Daily(interval)
    RecurrenceKind.WEEKLY -> Recurrence.Weekly(weekdays, interval)
}

/** Requires a valid draft. Name is trimmed; ONCE drops any end date. */
fun ScheduleDraft.toSchedule(id: String, revision: Int): Schedule = Schedule(
    id = id,
    revision = revision,
    name = name.trim(),
    enabled = enabled,
    amountMl = amountMl,
    startDate = startDate,
    localTime = localTime,
    recurrence = toRecurrence(),
    endDate = if (kind == RecurrenceKind.ONCE) null else endDate,
    zone = zone,
)

fun Schedule.toDraft(): ScheduleDraft = ScheduleDraft(
    name = name,
    amountMl = amountMl,
    startDate = startDate,
    localTime = localTime,
    kind = when (recurrence) {
        Recurrence.Once -> RecurrenceKind.ONCE
        is Recurrence.Daily -> RecurrenceKind.DAILY
        is Recurrence.Weekly -> RecurrenceKind.WEEKLY
    },
    interval = when (val r = recurrence) {
        Recurrence.Once -> 1
        is Recurrence.Daily -> r.intervalDays
        is Recurrence.Weekly -> r.intervalWeeks
    },
    weekdays = (recurrence as? Recurrence.Weekly)?.weekdays ?: emptySet(),
    endDate = endDate,
    enabled = enabled,
    zone = zone,
)
