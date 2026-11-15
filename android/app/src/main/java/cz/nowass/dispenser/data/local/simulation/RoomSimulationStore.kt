package cz.nowass.dispenser.data.local.simulation

import android.util.Log
import cz.nowass.dispenser.data.SimulationStore
import cz.nowass.dispenser.domain.simulation.DeviceReadiness
import cz.nowass.dispenser.domain.simulation.EventOrigin
import cz.nowass.dispenser.domain.simulation.EventReason
import cz.nowass.dispenser.domain.simulation.HistoryEvent
import cz.nowass.dispenser.domain.simulation.HistoryEventType
import cz.nowass.dispenser.domain.simulation.ReservoirState
import cz.nowass.dispenser.domain.simulation.SimulationState
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import java.time.Instant
import java.time.LocalDate

/**
 * Room-backed [SimulationStore] on `simulation.db`. Unreadable rows (should never occur)
 * are skipped/treated as inactive and logged without stored values; never repaired.
 */
class RoomSimulationStore(
    private val dao: SimulationDao,
    private val log: (String) -> Unit = { Log.w(TAG, it) },
) : SimulationStore {

    override fun observeState(): Flow<SimulationState?> =
        combine(dao.observeState(), dao.observeProcessed()) { state, processed -> state?.let { toDomain(it, processed) } }
            .distinctUntilChanged()

    override fun observeHistory(): Flow<List<HistoryEvent>> =
        dao.observeEvents().map { rows -> rows.mapNotNull(::toDomain) }.distinctUntilChanged()

    override suspend fun currentState(): SimulationState? = dao.getState()?.let { toDomain(it, dao.getProcessed()) }

    override suspend fun commit(state: SimulationState, newEvents: List<HistoryEvent>) {
        dao.commit(
            state = SimStateEntity(
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
            ),
            processed = state.processedOccurrences.map { (id, date) -> SimProcessedEntity(id, date.toString()) },
            newEvents = newEvents.map(::toEntity),
        )
    }

    override suspend fun clear() = dao.clearAll()

    private fun toDomain(e: SimStateEntity, processed: List<SimProcessedEntity>): SimulationState? = try {
        SimulationState(
            runId = e.runId,
            startedAt = Instant.ofEpochMilli(e.startedAtMs),
            clock = Instant.ofEpochMilli(e.clockMs),
            reservoir = ReservoirState(e.capacityUl, e.remainingUl, Instant.ofEpochMilli(e.reservoirUpdatedMs), e.blocked, e.lowWarningLatched),
            readiness = DeviceReadiness.valueOf(e.readiness),
            processedOccurrences = processed.associate { it.occurrenceId to LocalDate.parse(it.localDate) },
            nextSeq = e.nextSeq,
        )
    } catch (ex: RuntimeException) {
        log("Unreadable simulation state (${ex.javaClass.simpleName}); treated as inactive")
        null
    }

    private fun toDomain(e: SimEventEntity): HistoryEvent? = try {
        HistoryEvent(
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
    } catch (ex: RuntimeException) {
        log("Skipping unreadable simulation event '${e.id}' (${ex.javaClass.simpleName})")
        null
    }

    private fun toEntity(e: HistoryEvent) = SimEventEntity(
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

    private companion object { const val TAG = "RoomSimulationStore" }
}
