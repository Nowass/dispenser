package cz.nowass.dispenser.data

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
}
