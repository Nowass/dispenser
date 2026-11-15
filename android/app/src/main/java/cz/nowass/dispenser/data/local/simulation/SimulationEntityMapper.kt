package cz.nowass.dispenser.data.local.simulation

import cz.nowass.dispenser.data.backup.RawColumns.bool
import cz.nowass.dispenser.data.backup.RawColumns.int
import cz.nowass.dispenser.data.backup.RawColumns.long
import cz.nowass.dispenser.data.backup.RawColumns.longOrNull
import cz.nowass.dispenser.data.backup.RawColumns.str
import cz.nowass.dispenser.data.backup.RawColumns.strOrNull
import cz.nowass.dispenser.domain.simulation.DeviceReadiness
import cz.nowass.dispenser.domain.simulation.EventOrigin
import cz.nowass.dispenser.domain.simulation.EventReason
import cz.nowass.dispenser.domain.simulation.HistoryEvent
import cz.nowass.dispenser.domain.simulation.HistoryEventType
import cz.nowass.dispenser.domain.simulation.ReservoirState
import cz.nowass.dispenser.domain.simulation.SimulationState
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import java.time.Instant
import java.time.LocalDate

/**
 * Domain <-> Room conversion for the simulation tables, plus the lossless JSON column mapping
 * used by the internal pre-restore safety snapshot. Domain decoding throws on invalid rows;
 * callers decide how to handle that.
 */
object SimulationEntityMapper {

    fun toEntity(state: SimulationState) = SimStateEntity(
        runId = state.runId,
        startedAtMs = state.startedAt.toEpochMilli(),
        clockMs = state.clock.toEpochMilli(),
        capacityUl = state.reservoir.capacityUl,
        remainingUl = state.reservoir.remainingUl,
        reservoirUpdatedMs = state.reservoir.updatedAt.toEpochMilli(),
        blocked = state.reservoir.blocked,
        lowWarningLatched = state.reservoir.lowWarningLatched,
        readiness = state.readiness.name,
        nextSeq = state.nextSeq,
    )

    fun processedOf(state: SimulationState): List<SimProcessedEntity> =
        state.processedOccurrences.map { (id, date) -> SimProcessedEntity(id, date.toString()) }

    fun toDomain(e: SimStateEntity, processed: List<SimProcessedEntity>) = SimulationState(
        runId = e.runId,
        startedAt = Instant.ofEpochMilli(e.startedAtMs),
        clock = Instant.ofEpochMilli(e.clockMs),
        reservoir = ReservoirState(e.capacityUl, e.remainingUl, Instant.ofEpochMilli(e.reservoirUpdatedMs), e.blocked, e.lowWarningLatched),
        readiness = DeviceReadiness.valueOf(e.readiness),
        processedOccurrences = processed.associate { it.occurrenceId to LocalDate.parse(it.localDate) },
        nextSeq = e.nextSeq,
    )

    fun toDomain(e: SimEventEntity) = HistoryEvent(
        id = e.id,
        seq = e.seq,
        time = Instant.ofEpochMilli(e.timeMs),
        type = HistoryEventType.valueOf(e.type),
        origin = EventOrigin.valueOf(e.origin),
        reason = EventReason.valueOf(e.reason),
        scheduleId = e.scheduleId,
        scheduleNameSnapshot = e.scheduleName,
        occurrenceId = e.occurrenceId,
        requestedUl = e.requestedUl,
        dispensedUl = e.dispensedUl,
        reservoirBeforeUl = e.reservoirBeforeUl,
        reservoirAfterUl = e.reservoirAfterUl,
        capacityUl = e.capacityUl,
    )

    fun toEntity(e: HistoryEvent) = SimEventEntity(
        id = e.id,
        seq = e.seq,
        timeMs = e.time.toEpochMilli(),
        type = e.type.name,
        origin = e.origin.name,
        reason = e.reason.name,
        scheduleId = e.scheduleId,
        scheduleName = e.scheduleNameSnapshot,
        occurrenceId = e.occurrenceId,
        requestedUl = e.requestedUl,
        dispensedUl = e.dispensedUl,
        reservoirBeforeUl = e.reservoirBeforeUl,
        reservoirAfterUl = e.reservoirAfterUl,
        capacityUl = e.capacityUl,
    )

    // ---- Lossless raw JSON columns (safety snapshot only) ----

    fun rawState(e: SimStateEntity): JsonObject = buildJsonObject {
        put("id", e.id)
        put("run_id", e.runId)
        put("started_at_ms", e.startedAtMs)
        put("clock_ms", e.clockMs)
        put("capacity_ul", e.capacityUl)
        put("remaining_ul", e.remainingUl)
        put("reservoir_updated_ms", e.reservoirUpdatedMs)
        put("blocked", e.blocked)
        put("low_warning_latched", e.lowWarningLatched)
        put("readiness", e.readiness)
        put("next_seq", e.nextSeq)
    }

    fun stateFromRaw(o: JsonObject) = SimStateEntity(
        id = o.int("id"),
        runId = o.str("run_id"),
        startedAtMs = o.long("started_at_ms"),
        clockMs = o.long("clock_ms"),
        capacityUl = o.long("capacity_ul"),
        remainingUl = o.long("remaining_ul"),
        reservoirUpdatedMs = o.long("reservoir_updated_ms"),
        blocked = o.bool("blocked"),
        lowWarningLatched = o.bool("low_warning_latched"),
        readiness = o.str("readiness"),
        nextSeq = o.long("next_seq"),
    )

    fun rawProcessed(e: SimProcessedEntity): JsonObject = buildJsonObject {
        put("occurrence_id", e.occurrenceId)
        put("local_date", e.localDate)
    }

    fun processedFromRaw(o: JsonObject) = SimProcessedEntity(o.str("occurrence_id"), o.str("local_date"))

    fun rawEvent(e: SimEventEntity): JsonObject = buildJsonObject {
        put("id", e.id)
        put("seq", e.seq)
        put("time_ms", e.timeMs)
        put("type", e.type)
        put("origin", e.origin)
        put("reason", e.reason)
        put("schedule_id", e.scheduleId)
        put("schedule_name", e.scheduleName)
        put("occurrence_id", e.occurrenceId)
        put("requested_ul", e.requestedUl)
        put("dispensed_ul", e.dispensedUl)
        put("reservoir_before_ul", e.reservoirBeforeUl)
        put("reservoir_after_ul", e.reservoirAfterUl)
        put("capacity_ul", e.capacityUl)
    }

    fun eventFromRaw(o: JsonObject) = SimEventEntity(
        id = o.str("id"),
        seq = o.long("seq"),
        timeMs = o.long("time_ms"),
        type = o.str("type"),
        origin = o.str("origin"),
        reason = o.str("reason"),
        scheduleId = o.strOrNull("schedule_id"),
        scheduleName = o.strOrNull("schedule_name"),
        occurrenceId = o.strOrNull("occurrence_id"),
        requestedUl = o.longOrNull("requested_ul"),
        dispensedUl = o.long("dispensed_ul"),
        reservoirBeforeUl = o.long("reservoir_before_ul"),
        reservoirAfterUl = o.long("reservoir_after_ul"),
        capacityUl = o.long("capacity_ul"),
    )
}
