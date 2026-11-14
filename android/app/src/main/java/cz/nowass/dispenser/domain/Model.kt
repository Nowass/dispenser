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
 * Whether a schedule configuration is confirmed. Demo mode only ever reaches
 * [SIMULATED_ACK]; [DEVICE_APPLIED] is reserved for a future BLE read-back.
 */
enum class SyncState { PENDING, SIMULATED_ACK, DEVICE_APPLIED }

/**
 * Reservoir state. The remaining volume is always a model estimate based on
 * commanded doses; it is never a measurement (FR-006, FR-020).
 */
data class ReservoirEstimate(
    val capacityMl: Double,
    val estimatedRemainingMl: Double,
    val source: DataSource,
) {
    val fraction: Double
        get() = if (capacityMl <= 0.0) 0.0 else (estimatedRemainingMl / capacityMl).coerceIn(0.0, 1.0)

    /** FR-009 / FR-022: a dose may run only if the estimate covers its full volume. */
    fun canCover(amountMl: Double): Boolean = estimatedRemainingMl >= amountMl
}

enum class ConnectionState { DEMO, DISCONNECTED, CONNECTED }

/** Snapshot of the (demo or physical) dispenser. */
data class DeviceStatus(
    val source: DataSource,
    val connection: ConnectionState,
    /** Device clock. In DEMO this is a simulated clock, not the phone's clock. */
    val now: Instant,
    /** In DEMO this value is simulated, not measured. Null if unknown. */
    val batteryPercent: Int?,
    val zone: ZoneId = DEFAULT_ZONE,
)

enum class EventType { COMPLETED, SKIPPED, BLOCKED, REFILL, INTERRUPTED_UNCERTAIN, LOW_STOCK }

/** An entry in the dispenser event history. */
data class DispenserEvent(
    val seq: Long,
    val time: Instant,
    val type: EventType,
    val scheduleId: String?,
    val amountMl: Double?,
    val source: DataSource,
)
