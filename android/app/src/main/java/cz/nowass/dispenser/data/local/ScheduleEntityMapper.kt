package cz.nowass.dispenser.data.local

import cz.nowass.dispenser.data.StoredSchedule
import cz.nowass.dispenser.domain.Recurrence
import cz.nowass.dispenser.domain.RecurrenceKind
import cz.nowass.dispenser.domain.ScheduleDraft
import cz.nowass.dispenser.domain.ScheduleValidator
import cz.nowass.dispenser.domain.SyncState
import cz.nowass.dispenser.domain.Volume
import cz.nowass.dispenser.domain.toSchedule
import java.time.DayOfWeek
import java.time.LocalDate
import java.time.LocalTime
import java.time.ZoneId
import java.time.format.DateTimeFormatter

/**
 * Explicit, validated conversion between [ScheduleEntity] rows and domain schedules.
 * Reading never throws: a row that is malformed or violates domain rules is reported
 * as [Decoded.Invalid] so the store can skip it instead of crashing the app. Invalid
 * reasons name the failing check only and never contain stored values (e.g. the name).
 */
object ScheduleEntityMapper {

    private val TIME: DateTimeFormatter = DateTimeFormatter.ofPattern("HH:mm")

    sealed interface Decoded {
        data class Valid(val stored: StoredSchedule) : Decoded
        data class Invalid(val id: String, val reason: String) : Decoded
    }

    /** Requires a valid domain schedule (repository validates before writing). */
    fun toEntity(stored: StoredSchedule, updatedAtEpochMs: Long): ScheduleEntity {
        val s = stored.schedule
        val micro = when (val c = Volume.mlToMicrolitres(s.amountMl)) {
            is Volume.Conversion.Ok -> c.microlitres
            else -> throw IllegalArgumentException("amount not storable: ${s.amountMl} ml ($c)")
        }
        return ScheduleEntity(
            id = s.id,
            revision = s.revision,
            name = s.name,
            enabled = s.enabled,
            amountMicrolitres = micro,
            startDate = s.startDate.toString(),
            localTime = s.localTime.format(TIME),
            zoneId = s.zone.id,
            recurrenceKind = kindOf(s.recurrence).name,
            repeatInterval = when (val r = s.recurrence) {
                Recurrence.Once -> 1
                is Recurrence.Daily -> r.intervalDays
                is Recurrence.Weekly -> r.intervalWeeks
            },
            weekdaysMask = (s.recurrence as? Recurrence.Weekly)?.weekdays?.let(::weekdaysToMask) ?: 0,
            endDate = s.endDate?.toString(),
            syncState = stored.sync.name,
            updatedAtEpochMs = updatedAtEpochMs,
        )
    }

    fun fromEntity(e: ScheduleEntity): Decoded = try {
        decode(e)
    } catch (ex: RuntimeException) { // parse errors, IllegalArgumentException from domain constructors
        // Exception class only: messages may echo stored values, which must not reach the log.
        Decoded.Invalid(e.id, "unparsable field (${ex.javaClass.simpleName})")
    }

    private fun decode(e: ScheduleEntity): Decoded {
        fun invalid(reason: String) = Decoded.Invalid(e.id, reason)
        if (e.id.isBlank()) return invalid("blank id")
        if (e.revision < 1) return invalid("revision < 1")
        if (!Volume.isValidMicrolitres(e.amountMicrolitres)) return invalid("amount_ul out of range")
        val kind = RecurrenceKind.entries.firstOrNull { it.name == e.recurrenceKind }
            ?: return invalid("unknown recurrence_kind")
        val sync = SyncState.entries.firstOrNull { it.name == e.syncState }
            ?: return invalid("unknown sync_state")
        if (e.repeatInterval < 1) return invalid("repeat_interval < 1")
        if (e.weekdaysMask !in 0..ALL_WEEKDAYS_MASK) return invalid("weekdays_mask out of range")
        if (kind != RecurrenceKind.WEEKLY && e.weekdaysMask != 0) return invalid("weekdays_mask set for $kind")
        if (kind == RecurrenceKind.ONCE && e.endDate != null) return invalid("end_date set for ONCE")

        val draft = ScheduleDraft(
            name = e.name,
            amountMl = Volume.microlitresToMl(e.amountMicrolitres),
            startDate = LocalDate.parse(e.startDate),
            localTime = LocalTime.parse(e.localTime, TIME),
            kind = kind,
            interval = e.repeatInterval,
            weekdays = maskToWeekdays(e.weekdaysMask),
            endDate = e.endDate?.let(LocalDate::parse),
            enabled = e.enabled,
            zone = ZoneId.of(e.zoneId),
        )
        val errors = ScheduleValidator.validate(draft)
        if (errors.isNotEmpty()) return invalid("domain validation: $errors")
        // Name is stored as entered (already trimmed on write); keep it byte-identical.
        val schedule = draft.toSchedule(e.id, e.revision).copy(name = e.name)
        return Decoded.Valid(StoredSchedule(schedule, sync))
    }

    private fun kindOf(r: Recurrence): RecurrenceKind = when (r) {
        Recurrence.Once -> RecurrenceKind.ONCE
        is Recurrence.Daily -> RecurrenceKind.DAILY
        is Recurrence.Weekly -> RecurrenceKind.WEEKLY
    }

    const val ALL_WEEKDAYS_MASK = 0b111_1111

    /** Bit 0 = Monday … bit 6 = Sunday. */
    fun weekdaysToMask(days: Set<DayOfWeek>): Int = days.fold(0) { acc, d -> acc or (1 shl (d.value - 1)) }

    fun maskToWeekdays(mask: Int): Set<DayOfWeek> =
        DayOfWeek.entries.filterTo(mutableSetOf()) { mask and (1 shl (it.value - 1)) != 0 }
}
