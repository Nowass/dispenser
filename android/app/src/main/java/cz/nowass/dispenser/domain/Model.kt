package cz.nowass.dispenser.domain

import java.time.DayOfWeek
import java.time.Instant
import java.time.LocalDate
import java.time.LocalTime
import java.time.ZoneId

/** Default schedule timezone (docs/REQUIREMENTS_DRAFT.md FR-031). */
val DEFAULT_ZONE: ZoneId = ZoneId.of("Europe/Prague")

/**
 * Where a piece of state comes from. Demo data must never be presented as a
 * physical-device reading (docs/CLAUDE.md, "Mock mode must be conspicuous").
 */
enum class DataSource { DEMO, DEVICE }

/**
 * Bounded recurrence subset (docs/ARCHITECTURE.md, "Calendar-style recurrence model").
 * MONTHLY is intentionally absent until short-month behavior is approved.
 */
sealed interface Recurrence {
    data object Once : Recurrence

    data class Daily(val intervalDays: Int = 1) : Recurrence {
        init { require(intervalDays >= 1) { "intervalDays must be >= 1" } }
    }

    data class Weekly(
        val weekdays: Set<DayOfWeek>,
        val intervalWeeks: Int = 1,
    ) : Recurrence {
        init {
            require(intervalWeeks >= 1) { "intervalWeeks must be >= 1" }
            require(weekdays.isNotEmpty()) { "weekdays must not be empty" }
        }
    }
}

/** One independent dosing schedule, expressed in local wall-clock time. */
data class Schedule(
    val id: String,
    val revision: Int,
    val name: String,
    val enabled: Boolean,
    val amountMl: Double,
    val startDate: LocalDate,
    val localTime: LocalTime,
    val recurrence: Recurrence,
    val endDate: LocalDate? = null,
    val zone: ZoneId = DEFAULT_ZONE,
) {
    init {
        require(amountMl.isFinite() && amountMl > 0.0) { "amountMl must be finite and positive" }
        require(endDate == null || !endDate.isBefore(startDate)) { "endDate before startDate" }
    }
}

/** A concrete planned occurrence of a schedule. */
data class DoseOccurrence(
    val scheduleId: String,
    val scheduleRevision: Int,
    val scheduleName: String,
    val localDate: LocalDate,
    val scheduledAt: Instant,
    val amountMl: Double,
) {
    /**
     * Deterministic occurrence identity: schedule id + local date.
     *
     * Deliberately revision-independent: editing a schedule (new revision) must
     * not create a second occurrence for a date already handled (FR-013b,
     * FR-014 at-most-once). Consequence: at most one occurrence per schedule per
     * local date; multiple doses per day use multiple schedules.
     */
    val id: String get() = occurrenceId(scheduleId, localDate)

    companion object {
        fun occurrenceId(scheduleId: String, localDate: LocalDate): String = "$scheduleId@$localDate"
    }
}

/**
 * Device confirmation state of a schedule configuration.
 *
 * - [PENDING]: saved on the phone only ("Uloženo v telefonu"); NOT transferred to or
 *   accepted by the dispenser. Every locally created/edited schedule is PENDING.
 * - [DEVICE_APPLIED]: read back and confirmed by the physical device. Reserved for the
 *   future BLE synchronization; nothing sets it today and nothing may simulate it.
 */
enum class SyncState { PENDING, DEVICE_APPLIED }

/**
 * - [DEMO]: simulated data, no device.
 * - [DISCONNECTED] / [CONNECTING] / [CONNECTED]: DEVICE mode; CONNECTED only while an authenticated
 *   session with the selected device is live.
 */
enum class ConnectionState { DEMO, DISCONNECTED, CONNECTING, CONNECTED, RECOVERY_REQUIRED }

/**
 * Whether this phone can still authenticate to the selected device (DEVICE mode, M1.10e.4a). Without
 * a usable credential DEVICE mode stays shown in a recovery-required state: cached values and
 * unresolved operations remain visible, every operation is disabled (no session can authenticate).
 */
enum class DeviceAccess {
    /** Not checked yet (startup): operations still need a live session. */
    CHECKING,
    OK,
    /** The credential's Keystore key is missing or invalidated. */
    KEY_MISSING,
    /** The credential record exists but cannot be used (tampered, truncated, unknown format). */
    RECORD_UNREADABLE,
    /** The confirmed credential record has disappeared. */
    RECORD_MISSING,
    ;

    val recoveryRequired: Boolean get() = this == KEY_MISSING || this == RECORD_UNREADABLE || this == RECORD_MISSING
}

/**
 * Freshness of device-sourced values as shown in the UI (decision D2). Cached values are never shown
 * as live: [Cached] always carries the time the device reported the value.
 */
sealed interface Freshness {
    /** DEMO mode: values are simulated (the DEMO badge and "simulace" tags say so). */
    data object NotApplicable : Freshness
    /** DEVICE mode: never read from the device. */
    data object Unknown : Freshness
    /** Read in the current authenticated session. */
    data class Live(val readAt: Instant) : Freshness
    /** The last device-confirmed value, read at [readAt]; informational only. */
    data class Cached(val readAt: Instant) : Freshness
}

/** Snapshot of the (demo or physical) dispenser. */
data class DeviceStatus(
    val source: DataSource,
    val connection: ConnectionState,
    /** Device clock: the simulation clock while a simulation runs, otherwise the phone clock. */
    val now: Instant,
    /** In DEMO this value is simulated, not measured. Null if unknown. */
    val batteryPercent: Int?,
    val zone: ZoneId = DEFAULT_ZONE,
    /** True while [now] is the simulation clock (D1); false = phone clock. */
    val simulationActive: Boolean = false,
    /** Freshness of [batteryPercent] and the reservoir estimate (one device status record). */
    val freshness: Freshness = Freshness.NotApplicable,
    /** DEVICE mode: credential health of the selected device; null in DEMO. */
    val access: DeviceAccess? = null,
    /** DEVICE mode: unresolved journal rows of the selected device (never hidden, never dropped). */
    val unresolvedOperations: Int = 0,
)
