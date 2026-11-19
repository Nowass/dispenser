package cz.nowass.dispenser.data.device

import cz.nowass.dispenser.ble.protocol.BleDate
import cz.nowass.dispenser.ble.protocol.BleProtocol
import cz.nowass.dispenser.device.ops.DeviceValidation
import cz.nowass.dispenser.device.ops.ScheduleKind
import cz.nowass.dispenser.device.ops.ScheduleRecord
import cz.nowass.dispenser.domain.DEFAULT_ZONE
import cz.nowass.dispenser.domain.Recurrence
import cz.nowass.dispenser.domain.RecurrenceKind
import cz.nowass.dispenser.domain.Schedule
import cz.nowass.dispenser.domain.ScheduleDraft
import cz.nowass.dispenser.domain.ScheduleValidator
import cz.nowass.dispenser.domain.ValidationError
import cz.nowass.dispenser.domain.Volume
import java.time.DayOfWeek
import java.time.LocalDate
import java.time.LocalTime

/**
 * Domain [ScheduleDraft]/[Schedule] ⇄ device [ScheduleRecord] (BLE_PROTOCOL.md 7.5, 10.3; decision
 * D2): the existing domain rules plus the device's wire limits, checked before anything is sent.
 * The device timezone is Europe/Prague (E12): other zones are refused in DEVICE mode.
 */
object DeviceScheduleMapping {
    const val NAME_MAX_BYTES = 32
    const val U32_MAX = 0xFFFF_FFFFL
    private const val INTERVAL_MAX = 2_147_483_647L // DSP_REC_INTERVAL_MAX

    sealed interface ToRecord {
        data class Ok(val record: ScheduleRecord) : ToRecord
        data class Invalid(val problems: Set<DeviceValidation>) : ToRecord
    }

    fun toRecord(draft: ScheduleDraft, id: String, revision: Long): ToRecord {
        val problems = mutableSetOf<DeviceValidation>()
        ScheduleValidator.validate(draft).mapTo(problems) { it.toDevice() }
        val ul = (Volume.mlToMicrolitres(draft.amountMl) as? Volume.Conversion.Ok)?.microlitres
        if (ul != null && ul > U32_MAX) problems += DeviceValidation.AMOUNT_ABOVE_U32
        val name = draft.name.trim()
        val nameBytes = name.toByteArray(Charsets.UTF_8)
        if (nameBytes.size > NAME_MAX_BYTES) problems += DeviceValidation.NAME_TOO_LONG
        if (!BleProtocol.utf8Ok(nameBytes, noControls = true)) problems += DeviceValidation.NAME_CONTROL_CHARACTER
        if (draft.zone != DEFAULT_ZONE) problems += DeviceValidation.ZONE_NOT_SUPPORTED
        val end = if (draft.kind == RecurrenceKind.ONCE) null else draft.endDate
        if (!dateOk(draft.startDate) || (end != null && !dateOk(end))) problems += DeviceValidation.DATE_OUT_OF_RANGE
        if (draft.kind != RecurrenceKind.ONCE && draft.interval > INTERVAL_MAX) problems += DeviceValidation.INTERVAL_TOO_LARGE
        if (problems.isNotEmpty() || ul == null) return ToRecord.Invalid(problems)
        return ToRecord.Ok(
            ScheduleRecord(
                id = id, revision = revision, enabled = draft.enabled, amountUl = ul,
                startDate = draft.startDate.toBle(), hour = draft.localTime.hour, minute = draft.localTime.minute,
                kind = when (draft.kind) {
                    RecurrenceKind.ONCE -> ScheduleKind.ONCE
                    RecurrenceKind.DAILY -> ScheduleKind.DAILY
                    RecurrenceKind.WEEKLY -> ScheduleKind.WEEKLY
                },
                interval = if (draft.kind == RecurrenceKind.ONCE) 1 else draft.interval.toLong(),
                weekdayMask = if (draft.kind == RecurrenceKind.WEEKLY) draft.weekdays.fold(0) { m, d -> m or (1 shl (d.value - 1)) } else 0,
                endDate = end?.toBle(), name = name,
            ),
        )
    }

    /**
     * A record read from the device as a domain [Schedule]; null when it cannot be represented
     * (revision beyond Int, a value the domain model rejects) — counted as unreadable, never guessed.
     */
    fun toSchedule(r: ScheduleRecord): Schedule? = try {
        if (r.revision !in 1..Int.MAX_VALUE.toLong() || r.interval > Int.MAX_VALUE) null
        else Schedule(
            id = r.id, revision = r.revision.toInt(), name = r.name, enabled = r.enabled,
            amountMl = Volume.microlitresToMl(r.amountUl), startDate = r.startDate.toLocal(), localTime = LocalTime.of(r.hour, r.minute),
            recurrence = when (r.kind) {
                ScheduleKind.ONCE -> Recurrence.Once
                ScheduleKind.DAILY -> Recurrence.Daily(r.interval.toInt())
                ScheduleKind.WEEKLY -> Recurrence.Weekly(DayOfWeek.entries.filter { r.weekdayMask and (1 shl (it.value - 1)) != 0 }.toSet(), r.interval.toInt())
            },
            endDate = r.endDate?.toLocal(), zone = DEFAULT_ZONE,
        )
    } catch (e: IllegalArgumentException) {
        null
    } catch (e: java.time.DateTimeException) {
        null
    }

    /** Same definition (everything except the revision), e.g. to recognize an applied write on read-back. */
    fun sameDefinition(a: ScheduleRecord, b: ScheduleRecord): Boolean = a.copy(revision = 0) == b.copy(revision = 0)

    private fun dateOk(d: LocalDate) = d.year in 2000..2099
    private fun LocalDate.toBle() = BleDate(year, monthValue, dayOfMonth)
    private fun BleDate.toLocal(): LocalDate = LocalDate.of(year, month, day)

    private fun ValidationError.toDevice(): DeviceValidation = when (this) {
        ValidationError.AMOUNT_NOT_FINITE -> DeviceValidation.AMOUNT_NOT_FINITE
        ValidationError.AMOUNT_NOT_POSITIVE -> DeviceValidation.AMOUNT_NOT_POSITIVE
        ValidationError.AMOUNT_TOO_PRECISE -> DeviceValidation.AMOUNT_TOO_PRECISE
        ValidationError.AMOUNT_OUT_OF_RANGE -> DeviceValidation.AMOUNT_OUT_OF_RANGE
        ValidationError.TIME_NONEXISTENT -> DeviceValidation.TIME_NONEXISTENT
        ValidationError.INTERVAL_TOO_SMALL -> DeviceValidation.INTERVAL_TOO_SMALL
        ValidationError.NO_WEEKDAYS -> DeviceValidation.NO_WEEKDAYS
        ValidationError.END_BEFORE_START -> DeviceValidation.END_BEFORE_START
    }
}
