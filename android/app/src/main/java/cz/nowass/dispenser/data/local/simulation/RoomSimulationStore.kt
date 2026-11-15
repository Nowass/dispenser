package cz.nowass.dispenser.data.local.simulation

import android.util.Log
import cz.nowass.dispenser.data.SimulationStore
import cz.nowass.dispenser.data.backup.RawSimulationRows
import cz.nowass.dispenser.domain.simulation.HistoryEvent
import cz.nowass.dispenser.domain.simulation.SimulationState
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map

/**
 * Room-backed [SimulationStore] on `simulation.db`. Unreadable rows (should never occur)
 * are skipped/treated as inactive and logged without stored values; never repaired.
 */
class RoomSimulationStore(
    private val dao: SimulationDao,
    private val log: (String) -> Unit = { Log.w(TAG, it) },
) : SimulationStore {

    override fun observeState(): Flow<SimulationState?> =
        combine(dao.observeState(), dao.observeProcessed()) { state, processed -> state?.let { decode(it, processed) } }
            .distinctUntilChanged()

    override fun observeHistory(): Flow<List<HistoryEvent>> =
        dao.observeEvents().map { rows -> rows.mapNotNull(::decode) }.distinctUntilChanged()

    override suspend fun currentState(): SimulationState? = dao.getState()?.let { decode(it, dao.getProcessed()) }

    override suspend fun commit(state: SimulationState, newEvents: List<HistoryEvent>) {
        dao.commit(
            state = SimulationEntityMapper.toEntity(state),
            processed = SimulationEntityMapper.processedOf(state),
            newEvents = newEvents.map(SimulationEntityMapper::toEntity),
        )
    }

    override suspend fun clear() = dao.clearAll()

    override suspend fun deactivate() = dao.deactivate()

    override suspend fun replaceAll(state: SimulationState?, history: List<HistoryEvent>) = dao.replaceAll(
        state = state?.let(SimulationEntityMapper::toEntity),
        processed = state?.let(SimulationEntityMapper::processedOf).orEmpty(),
        events = history.map(SimulationEntityMapper::toEntity),
    )

    override suspend fun captureRaw(): RawSimulationRows = RawSimulationRows(
        state = listOfNotNull(dao.getState()).map(SimulationEntityMapper::rawState),
        processed = dao.getProcessed().map(SimulationEntityMapper::rawProcessed),
        events = dao.getEvents().map(SimulationEntityMapper::rawEvent),
    )

    override suspend fun restoreRaw(rows: RawSimulationRows) = dao.replaceAll(
        state = rows.state.singleOrNull()?.let(SimulationEntityMapper::stateFromRaw)
            .also { require(rows.state.size <= 1) { "more than one state row" } },
        processed = rows.processed.map(SimulationEntityMapper::processedFromRaw),
        events = rows.events.map(SimulationEntityMapper::eventFromRaw),
    )

    private fun decode(e: SimStateEntity, processed: List<SimProcessedEntity>): SimulationState? = try {
        SimulationEntityMapper.toDomain(e, processed)
    } catch (ex: RuntimeException) {
        log("Unreadable simulation state (${ex.javaClass.simpleName}); treated as inactive")
        null
    }

    private fun decode(e: SimEventEntity): HistoryEvent? = try {
        SimulationEntityMapper.toDomain(e)
    } catch (ex: RuntimeException) {
        log("Skipping unreadable simulation event '${e.id}' (${ex.javaClass.simpleName})")
        null
    }

    private companion object { const val TAG = "RoomSimulationStore" }
}
