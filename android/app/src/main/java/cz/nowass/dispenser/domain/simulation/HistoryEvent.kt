package cz.nowass.dispenser.domain.simulation

import java.time.Instant

/** Where an event comes from. Simulated events must never be presented as hardware activity. */
enum class EventOrigin { SIMULATION, DEVICE }

enum class HistoryEventType {
    DISPENSE_SUCCESS,
    DISPENSE_BLOCKED_INSUFFICIENT_STOCK,
    DISPENSE_SKIPPED,
    RESERVOIR_REFILLED,
    RESERVOIR_LOW_WARNING,

    // ---- Reserved for hardware / service integration; never produced by the A1.3 simulation ----

    /** Dose attempt interrupted (reset/fault); delivered amount unknown (ARCHITECTURE "at-most-once"). */
    DISPENSE_INTERRUPTED_UNCERTAIN,

    /** Container replacement / service started; normal dispensing suspended (FR-025/FR-026). */
    SERVICE_STARTED,

    /**
     * One button-held priming pump run in service mode (FR-026). [HistoryEvent.dispensedUl] is the
     * estimated volume (runtime × calibrated flow) and is deducted from the reservoir estimate.
     */
    PRIMING_PUMP_RUN,

    /** User confirmed the tubing is primed (4 short presses or BLE), FR-025/FR-026. */
    TUBING_PRIMED,

    /** Service finished; device ready for normal operation (FR-025). */
    SERVICE_COMPLETED,
}

enum class EventReason {
    SCHEDULED,
    /** The dose exceeded the estimated remaining volume. */
    INSUFFICIENT_STOCK,
    /** Dispensing stays blocked after an insufficient-stock event until a refill (D2). */
    BLOCKED_UNTIL_REFILL,
    /** Explicit outage scenario: device unavailable, dose skipped without catch-up (D3). */
    DEVICE_OUTAGE,
    /** Device not READY (e.g. future service mode). */
    DEVICE_NOT_READY,
    MANUAL_REFILL,
    LOW_THRESHOLD_CROSSED,

    // ---- Reserved for the firmware service state machine (FR-026) ----
    /** Service gesture recognized on the physical button (4 short presses / hold). */
    SERVICE_BUTTON,
    /** Service start or readiness confirmation via BLE. */
    SERVICE_BLE,
    /** Priming run stopped by the hard maximum continuous runtime. */
    PRIMING_RUNTIME_LIMIT,
}

/**
 * Immutable history record (D9). Execution events snapshot the schedule name and dose volume
 * at processing time, so later schedule edits or deletions never change past history.
 *
 * Volumes are integer microlitres. Only successful doses and priming pump runs consume liquid;
 * for every other type [dispensedUl] is 0.
 */
data class HistoryEvent(
    val id: String,
    /** Monotonic within a simulation run; defines order for events at the same instant. */
    val seq: Long,
    val time: Instant,
    val type: HistoryEventType,
    val origin: EventOrigin,
    val reason: EventReason,
    val scheduleId: String? = null,
    val scheduleNameSnapshot: String? = null,
    val occurrenceId: String? = null,
    val requestedUl: Long? = null,
    val dispensedUl: Long = 0,
    val reservoirBeforeUl: Long,
    val reservoirAfterUl: Long,
    val capacityUl: Long,
) {
    init {
        require(dispensedUl >= 0) { "dispensed must be >= 0" }
        require(reservoirBeforeUl in 0..capacityUl && reservoirAfterUl in 0..capacityUl) { "reservoir values out of range" }
        if (type != HistoryEventType.DISPENSE_SUCCESS && type != HistoryEventType.PRIMING_PUMP_RUN) {
            require(dispensedUl == 0L) { "only successful doses and priming runs consume liquid" }
        }
    }
}
