package cz.nowass.dispenser.data

import cz.nowass.dispenser.data.backup.RawSimulationRows
import cz.nowass.dispenser.data.local.simulation.SimulationEntityMapper
import cz.nowass.dispenser.domain.simulation.HistoryEvent
import cz.nowass.dispenser.domain.simulation.SimulationState
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.update

/**
 * Persistence port for the dispensing simulation, fully separate from schedule storage.
 * The app uses [cz.nowass.dispenser.data.local.simulation.RoomSimulationStore] on its own
 * `simulation.db`; tests and previews use [InMemorySimulationStore].
 */
interface SimulationStore {
    /** Current simulation state, or null when no simulation is active. */
    fun observeState(): Flow<SimulationState?>

    /** All simulation history events in processing order (seq ascending). */
    fun observeHistory(): Flow<List<HistoryEvent>>

    suspend fun currentState(): SimulationState?

    /** Atomically stores the new state and appends [newEvents] (immutable once written). */
    suspend fun commit(state: SimulationState, newEvents: List<HistoryEvent>)

    /** Reset: removes simulation state and history only. */
    suspend fun clear()

    /**
     * Atomically ends the active run (state and processed occurrences) while keeping the
     * history (backup restore with changed schedules, R2). A new run needs an explicit start.
     */
    suspend fun deactivate()

    /** Atomically replaces state (null = inactive), processed set and history (backup restore). */
    suspend fun replaceAll(state: SimulationState?, history: List<HistoryEvent>)

    /** Exact copy of the stored tables for the pre-restore safety snapshot. */
    suspend fun captureRaw(): RawSimulationRows

    /** Atomically restores an exact copy taken by [captureRaw] (rollback). */
    suspend fun restoreRaw(rows: RawSimulationRows)
}

class InMemorySimulationStore(
    initialState: SimulationState? = null,
    initialHistory: List<HistoryEvent> = emptyList(),
) : SimulationStore {

    private data class Data(val state: SimulationState?, val history: List<HistoryEvent>)

    private val data = MutableStateFlow(Data(initialState, initialHistory))

    override fun observeState(): Flow<SimulationState?> = data.map { it.state }
    override fun observeHistory(): Flow<List<HistoryEvent>> = data.map { it.history }
    override suspend fun currentState(): SimulationState? = data.value.state

    override suspend fun commit(state: SimulationState, newEvents: List<HistoryEvent>) {
        data.update { Data(state, it.history + newEvents) }
    }

    override suspend fun clear() {
        data.value = Data(null, emptyList())
    }

    override suspend fun deactivate() {
        data.update { it.copy(state = null) }
    }

    override suspend fun replaceAll(state: SimulationState?, history: List<HistoryEvent>) {
        data.value = Data(state, history.sortedBy { it.seq })
    }

    override suspend fun captureRaw(): RawSimulationRows {
        val d = data.value
        return RawSimulationRows(
            state = listOfNotNull(d.state).map { SimulationEntityMapper.rawState(SimulationEntityMapper.toEntity(it)) },
            processed = d.state?.let(SimulationEntityMapper::processedOf).orEmpty().map(SimulationEntityMapper::rawProcessed),
            events = d.history.map { SimulationEntityMapper.rawEvent(SimulationEntityMapper.toEntity(it)) },
        )
    }

    override suspend fun restoreRaw(rows: RawSimulationRows) {
        val processed = rows.processed.map(SimulationEntityMapper::processedFromRaw)
        val state = rows.state.singleOrNull()?.let { SimulationEntityMapper.toDomain(SimulationEntityMapper.stateFromRaw(it), processed) }
        val events = rows.events.map { SimulationEntityMapper.toDomain(SimulationEntityMapper.eventFromRaw(it)) }
        data.value = Data(state, events)
    }
}
