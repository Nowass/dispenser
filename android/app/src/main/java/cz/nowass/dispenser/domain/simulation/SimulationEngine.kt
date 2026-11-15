package cz.nowass.dispenser.domain.simulation

import cz.nowass.dispenser.domain.DEFAULT_ZONE
import cz.nowass.dispenser.domain.DoseOccurrence
import cz.nowass.dispenser.domain.RecurrenceEngine
import cz.nowass.dispenser.domain.Schedule
import cz.nowass.dispenser.domain.Volume
import java.time.Duration
import java.time.Instant
import java.time.LocalDate
import java.time.ZoneId

/**
 * Complete, immutable simulation state. Persistable as-is (A1.3b).
 *
 * @property clock simulated "now"; only ever moves forward (D4). Occurrences are processed
 *   in (clock, target], so each instant is evaluated once.
 * @property processedOccurrences occurrence id -> local date, for occurrences already
 *   processed. Guards against a re-edited schedule producing the same occurrence id again on
 *   the same date (revision-independent ids). Entries older than the clock's local date can
 *   never recur and are pruned.
 */
data class SimulationState(
    val runId: String,
    val startedAt: Instant,
    val clock: Instant,
    val reservoir: ReservoirState,
    val readiness: DeviceReadiness = DeviceReadiness.READY,
    val processedOccurrences: Map<String, LocalDate> = emptyMap(),
    val nextSeq: Long = 1,
)

/** Explicit bounds for a single command (D6); larger requests are rejected, never truncated. */
data class SimulationLimits(
    val maxSpan: Duration = Duration.ofDays(366),
    val maxOccurrences: Int = 10_000,
)

enum class RejectReason {
    /** Target before the simulated clock: time never moves backwards (D4); use reset instead. */
    TARGET_BEFORE_CLOCK,
    RANGE_TOO_LONG,
    TOO_MANY_OCCURRENCES,
    /** Capacity ≤ 0 or beyond the storage range (D8). */
    INVALID_CAPACITY,
    /** Remaining volume < 0, > capacity, or a refill of 0 (D7). */
    INVALID_VOLUME,
}

sealed interface SimulationResult {
    data class Ok(val state: SimulationState, val events: List<HistoryEvent>) : SimulationResult
    data class Rejected(val reason: RejectReason) : SimulationResult
}

/**
 * Deterministic dispensing simulation: pure functions from state + inputs to new state and
 * events. No clocks, threads, alarms or I/O, so it can serve as a reference for firmware
 * logic validation (docs/ARCHITECTURE.md).
 *
 * Rules:
 * - Only enabled schedules execute; occurrences come from [RecurrenceEngine] (DST rules kept).
 * - Occurrences are processed chronologically (time, then schedule id), each at most once.
 * - Doses are never partial; the estimate never goes negative.
 * - Insufficient stock blocks the dose and every following dose until a refill (D2).
 * - The low-stock warning is issued once per episode (D5).
 * - Advancing time = device running (D3); an explicit outage skips doses without catch-up.
 * - Occurrences at or before the simulation start are never executed (no catch-up).
 */
class SimulationEngine(
    val lowStock: LowStockPolicy = LowStockPolicy(),
    val limits: SimulationLimits = SimulationLimits(),
    private val zone: ZoneId = DEFAULT_ZONE,
) {

    /** Starts a new run at [at] with the given reservoir (D8: capacity > 0). */
    fun start(runId: String, at: Instant, capacityUl: Long, remainingUl: Long): SimulationResult {
        if (capacityUl !in 1..Volume.MAX_MICROLITRES) return SimulationResult.Rejected(RejectReason.INVALID_CAPACITY)
        if (remainingUl !in 0..capacityUl) return SimulationResult.Rejected(RejectReason.INVALID_VOLUME)
        val state = SimulationState(
            runId = runId,
            startedAt = at,
            clock = at,
            reservoir = ReservoirState(capacityUl, remainingUl, updatedAt = at),
        )
        val events = Events(state)
        val reservoir = events.lowWarningIfNeeded(state.reservoir, at)
        return SimulationResult.Ok(events.finish(state.copy(reservoir = reservoir)), events.list)
    }

    /** Device running until [target]: every due occurrence in (clock, target] is evaluated. */
    fun advanceTo(state: SimulationState, target: Instant, schedules: List<Schedule>): SimulationResult =
        process(state, target, schedules, outage = false)

    fun advanceBy(state: SimulationState, duration: Duration, schedules: List<Schedule>): SimulationResult =
        if (duration.isNegative) SimulationResult.Rejected(RejectReason.TARGET_BEFORE_CLOCK)
        else advanceTo(state, state.clock.plus(duration), schedules)

    /** Calendar days in the schedule zone: same local time on the target day, across DST. */
    fun advanceDays(state: SimulationState, days: Long, schedules: List<Schedule>): SimulationResult =
        if (days < 0) SimulationResult.Rejected(RejectReason.TARGET_BEFORE_CLOCK)
        else advanceTo(state, state.clock.atZone(zone).plusDays(days).toInstant(), schedules)

    /**
     * Explicit outage scenario until [target] (D3): occurrences in (clock, target] are recorded
     * as [HistoryEventType.DISPENSE_SKIPPED] and never caught up; the reservoir is unchanged.
     */
    fun simulateOutage(state: SimulationState, target: Instant, schedules: List<Schedule>): SimulationResult =
        process(state, target, schedules, outage = true)

    /** Refill: sets the absolute remaining volume (D7), clears blocked state and warning latch. */
    fun refill(state: SimulationState, newRemainingUl: Long): SimulationResult {
        val r = state.reservoir
        if (newRemainingUl !in 1..r.capacityUl) return SimulationResult.Rejected(RejectReason.INVALID_VOLUME)
        val events = Events(state)
        val refilled = r.copy(remainingUl = newRemainingUl, updatedAt = state.clock, blocked = false, lowWarningLatched = false)
        events.add(
            time = state.clock,
            type = HistoryEventType.RESERVOIR_REFILLED,
            reason = EventReason.MANUAL_REFILL,
            before = r.remainingUl,
            after = newRemainingUl,
            capacity = r.capacityUl,
        )
        val checked = events.lowWarningIfNeeded(refilled, state.clock)
        return SimulationResult.Ok(events.finish(state.copy(reservoir = checked)), events.list)
    }

    // ---- Core processing ----

    private fun process(state: SimulationState, target: Instant, schedules: List<Schedule>, outage: Boolean): SimulationResult {
        if (target.isBefore(state.clock)) return SimulationResult.Rejected(RejectReason.TARGET_BEFORE_CLOCK)
        if (Duration.between(state.clock, target) > limits.maxSpan) return SimulationResult.Rejected(RejectReason.RANGE_TOO_LONG)

        val due = dueOccurrences(state, target, schedules) ?: return SimulationResult.Rejected(RejectReason.TOO_MANY_OCCURRENCES)

        val events = Events(state)
        var reservoir = state.reservoir
        val processed = state.processedOccurrences.toMutableMap()

        for (occ in due) {
            if (occ.id in processed) continue // at most once per occurrence id
            processed[occ.id] = occ.localDate
            val requested = requestedMicrolitres(occ)
            // Snapshot of name and volume at processing time (D9): later edits never alter history.
            val common = DoseSnapshot(occ, occ.scheduleName, requested)
            reservoir = when {
                outage -> skip(events, reservoir, common, EventReason.DEVICE_OUTAGE)
                state.readiness != DeviceReadiness.READY -> skip(events, reservoir, common, EventReason.DEVICE_NOT_READY)
                reservoir.canDispense(requested) -> dispense(events, reservoir, common)
                else -> block(events, reservoir, common)
            }
        }

        val clockDate = target.atZone(zone).toLocalDate()
        val pruned = processed.filterValues { !it.isBefore(clockDate.minusDays(1)) }
        val next = state.copy(clock = target, reservoir = reservoir, processedOccurrences = pruned)
        return SimulationResult.Ok(events.finish(next), events.list)
    }

    /** Occurrences of enabled schedules in (clock, target], ordered by time then schedule id. */
    private fun dueOccurrences(state: SimulationState, target: Instant, schedules: List<Schedule>): List<DoseOccurrence>? {
        val all = ArrayList<DoseOccurrence>()
        for (schedule in schedules) {
            if (!schedule.enabled) continue
            val remainingBudget = limits.maxOccurrences - all.size
            val occ = RecurrenceEngine.occurrencesBetween(schedule, state.clock, target, remainingBudget) ?: return null
            all += occ
        }
        return all.sortedWith(compareBy<DoseOccurrence>({ it.scheduledAt }, { it.scheduleId }))
    }

    private data class DoseSnapshot(val occurrence: DoseOccurrence, val name: String, val requestedUl: Long)

    private fun dispense(events: Events, r: ReservoirState, d: DoseSnapshot): ReservoirState {
        val after = r.copy(remainingUl = r.remainingUl - d.requestedUl, updatedAt = d.occurrence.scheduledAt)
        events.addDose(d, HistoryEventType.DISPENSE_SUCCESS, EventReason.SCHEDULED, r, after, dispensed = d.requestedUl)
        return events.lowWarningIfNeeded(after, d.occurrence.scheduledAt)
    }

    private fun block(events: Events, r: ReservoirState, d: DoseSnapshot): ReservoirState {
        val reason = if (r.blocked) EventReason.BLOCKED_UNTIL_REFILL else EventReason.INSUFFICIENT_STOCK
        val after = r.copy(blocked = true, updatedAt = d.occurrence.scheduledAt)
        events.addDose(d, HistoryEventType.DISPENSE_BLOCKED_INSUFFICIENT_STOCK, reason, r, after, dispensed = 0)
        return after
    }

    private fun skip(events: Events, r: ReservoirState, d: DoseSnapshot, reason: EventReason): ReservoirState {
        events.addDose(d, HistoryEventType.DISPENSE_SKIPPED, reason, r, r, dispensed = 0)
        return r
    }

    private fun requestedMicrolitres(occ: DoseOccurrence): Long =
        when (val c = Volume.mlToMicrolitres(occ.amountMl)) {
            is Volume.Conversion.Ok -> c.microlitres
            else -> error("Schedule ${occ.scheduleId} has a non-storable amount; schedules are validated on write")
        }

    /** Collects events with run-scoped sequential ids. */
    private inner class Events(private val state: SimulationState) {
        val list = ArrayList<HistoryEvent>()
        private var seq = state.nextSeq

        fun add(
            time: Instant,
            type: HistoryEventType,
            reason: EventReason,
            before: Long,
            after: Long,
            capacity: Long,
            dose: DoseSnapshot? = null,
            dispensed: Long = 0,
        ) {
            list += HistoryEvent(
                id = "${state.runId}-$seq",
                seq = seq,
                time = time,
                type = type,
                origin = EventOrigin.SIMULATION,
                reason = reason,
                scheduleId = dose?.occurrence?.scheduleId,
                scheduleNameSnapshot = dose?.name,
                occurrenceId = dose?.occurrence?.id,
                requestedUl = dose?.requestedUl,
                dispensedUl = dispensed,
                reservoirBeforeUl = before,
                reservoirAfterUl = after,
                capacityUl = capacity,
            )
            seq++
        }

        fun addDose(d: DoseSnapshot, type: HistoryEventType, reason: EventReason, before: ReservoirState, after: ReservoirState, dispensed: Long) =
            add(d.occurrence.scheduledAt, type, reason, before.remainingUl, after.remainingUl, before.capacityUl, d, dispensed)

        /** Issues the low-stock warning once per episode (latched until refill). */
        fun lowWarningIfNeeded(r: ReservoirState, at: Instant): ReservoirState {
            if (r.lowWarningLatched || !lowStock.isLow(r.remainingUl, r.capacityUl)) return r
            add(at, HistoryEventType.RESERVOIR_LOW_WARNING, EventReason.LOW_THRESHOLD_CROSSED, r.remainingUl, r.remainingUl, r.capacityUl)
            return r.copy(lowWarningLatched = true)
        }

        fun finish(s: SimulationState) = s.copy(nextSeq = seq)
    }
}
