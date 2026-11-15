package cz.nowass.dispenser.data

import cz.nowass.dispenser.domain.ConnectionState
import cz.nowass.dispenser.domain.DEFAULT_ZONE
import cz.nowass.dispenser.domain.DataSource
import cz.nowass.dispenser.domain.DeviceStatus
import cz.nowass.dispenser.domain.simulation.HistoryEvent
import cz.nowass.dispenser.domain.simulation.ReservoirState
import cz.nowass.dispenser.domain.simulation.SimulationEngine
import cz.nowass.dispenser.domain.Schedule
import cz.nowass.dispenser.domain.ScheduleDraft
import cz.nowass.dispenser.domain.ScheduleValidator
import cz.nowass.dispenser.domain.SyncState
import cz.nowass.dispenser.domain.toSchedule
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import java.time.Clock
import java.time.Instant
import java.time.ZoneId
import java.util.UUID
import kotlin.time.Duration
import kotlin.time.Duration.Companion.minutes

/**
 * App repository: schedules are persisted locally through a [ScheduleStore]; device-side
 * state (reservoir, history, clock while simulating) comes from the dispensing simulation
 * ([simulation]); battery is a fixed demo value. Everything device-side is tagged DEMO /
 * SIMULATION and never presented as real hardware activity.
 *
 * - "Now": while a simulation runs, the visible simulation clock (D1); otherwise the injected
 *   phone [clock], re-emitted on each [clockTick] boundary.
 * - Schedule writes are validated, revision-checked atomically by the store, and stored as
 *   [SyncState.PENDING] ("Uloženo v telefonu"). No device acknowledgement is simulated.
 * - The simulation only reads schedules; it never writes them or their sync state.
 */
class LocalDispenserRepository(
    private val store: ScheduleStore,
    private val clock: Clock,
    simulationStore: SimulationStore = InMemorySimulationStore(),
    private val batteryPercent: Int? = DEMO_BATTERY_PERCENT,
    private val idGenerator: () -> String = { UUID.randomUUID().toString() },
    private val clockTick: Duration? = 1.minutes,
    private val zone: ZoneId = DEFAULT_ZONE,
    simulationEngine: SimulationEngine = SimulationEngine(zone = zone),
    simulationRunIds: () -> String = { UUID.randomUUID().toString() },
) : DispenserRepository {

    private val writeLock = Mutex()

    /** Simulation controls (DEMO); not part of the device contract. */
    val simulation: DispenserSimulator = DispenserSimulator(simulationStore, observeSchedules(), simulationEngine, simulationRunIds)

    private fun phoneNow(): Flow<Instant> = flow {
        emit(clock.instant())
        val tick = clockTick ?: return@flow
        val tickMs = tick.inWholeMilliseconds
        while (true) {
            // Align to the next tick boundary of the clock (e.g. the next full minute).
            delay(tickMs - Math.floorMod(clock.millis(), tickMs))
            emit(clock.instant())
        }
    }

    override fun observeDevice(): Flow<DeviceStatus> =
        combine(simulation.observeSimulation(), phoneNow()) { sim, phone ->
            DeviceStatus(
                source = DataSource.DEMO,
                connection = ConnectionState.DEMO,
                now = sim?.clock ?: phone,
                batteryPercent = batteryPercent,
                zone = zone,
                simulationActive = sim != null,
            )
        }.distinctUntilChanged()

    override fun observeSchedules(): Flow<List<Schedule>> =
        store.observe().map { snap -> snap.schedules.map { it.schedule } }.distinctUntilChanged()

    override fun observeScheduleSync(): Flow<Map<String, SyncState>> =
        store.observe().map { snap -> snap.schedules.associate { it.schedule.id to it.sync } }.distinctUntilChanged()

    override fun observeUnreadableScheduleCount(): Flow<Int> =
        store.observe().map { it.unreadableCount }.distinctUntilChanged()

    override fun observeReservoir(): Flow<ReservoirState?> =
        simulation.observeSimulation().map { it?.reservoir }.distinctUntilChanged()

    override fun observeHistory(): Flow<List<HistoryEvent>> = simulation.observeHistory()

    override suspend fun refillReservoir(remainingUl: Long): CommandResult = simulation.refill(remainingUl)

    override suspend fun createSchedule(draft: ScheduleDraft): ScheduleWriteResult {
        val errors = ScheduleValidator.validate(draft)
        if (errors.isNotEmpty()) return ScheduleWriteResult.Invalid(errors)
        return writeLock.withLock {
            repeat(MAX_ID_ATTEMPTS) {
                val schedule = draft.toSchedule(id = idGenerator(), revision = 1)
                if (store.insert(StoredSchedule(schedule, SyncState.PENDING))) {
                    return@withLock ScheduleWriteResult.Success(schedule)
                }
            }
            error("Could not generate a unique schedule id after $MAX_ID_ATTEMPTS attempts")
        }
    }

    override suspend fun updateSchedule(id: String, expectedRevision: Int, draft: ScheduleDraft): ScheduleWriteResult {
        val errors = ScheduleValidator.validate(draft)
        if (errors.isNotEmpty()) return ScheduleWriteResult.Invalid(errors)
        val next = draft.toSchedule(id = id, revision = expectedRevision + 1)
        return writeLock.withLock {
            when (store.replace(StoredSchedule(next, SyncState.PENDING), expectedRevision)) {
                StoreWriteOutcome.OK -> ScheduleWriteResult.Success(next)
                StoreWriteOutcome.CONFLICT -> ScheduleWriteResult.Conflict
                StoreWriteOutcome.NOT_FOUND -> ScheduleWriteResult.NotFound
            }
        }
    }

    override suspend fun deleteSchedule(id: String, expectedRevision: Int): ScheduleWriteResult =
        writeLock.withLock {
            // Event history is intentionally kept (audit trail).
            when (store.delete(id, expectedRevision)) {
                StoreWriteOutcome.OK -> ScheduleWriteResult.Success(null)
                StoreWriteOutcome.CONFLICT -> ScheduleWriteResult.Conflict
                StoreWriteOutcome.NOT_FOUND -> ScheduleWriteResult.NotFound
            }
        }

    companion object {
        private const val MAX_ID_ATTEMPTS = 5

        /** Battery monitoring is not implemented (A1.3 scope): fixed, clearly simulated value. */
        const val DEMO_BATTERY_PERCENT = 78
    }
}
