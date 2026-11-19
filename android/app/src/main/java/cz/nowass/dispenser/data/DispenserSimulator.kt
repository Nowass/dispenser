package cz.nowass.dispenser.data

import cz.nowass.dispenser.domain.Schedule
import cz.nowass.dispenser.domain.simulation.HistoryEvent
import cz.nowass.dispenser.domain.simulation.LowStockPolicy
import cz.nowass.dispenser.domain.simulation.RejectReason
import cz.nowass.dispenser.domain.simulation.SimulationEngine
import cz.nowass.dispenser.domain.simulation.SimulationResult
import cz.nowass.dispenser.domain.simulation.SimulationState
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import java.time.Duration
import java.time.Instant
import java.util.UUID

/** Outcome of a device or simulation command. */
sealed interface CommandResult {
    data class Done(val newEvents: Int) : CommandResult
    data class Rejected(val reason: RejectReason) : CommandResult
    /** No simulation is running (and no physical device yet). */
    data object NotActive : CommandResult
    /** start() while a simulation is already running; reset first. */
    data object AlreadyActive : CommandResult

    /** DEVICE-mode outcomes of a device command (refill); never produced by the simulation. */
    sealed interface Device : CommandResult

    /** Provably never reached the device; nothing changed. */
    data object NotSent : Device

    /** May have been applied: NOT a failure; verified by read-back at the next session, never re-sent automatically. */
    data object Uncertain : Device

    /** An earlier command of this kind awaits verification; nothing was sent. */
    data object AwaitingVerification : Device

    /** The device refused ([code]: BLE_PROTOCOL.md 10.1); nothing changed. */
    data class DeviceRejected(val code: Int) : Device

    /** Refused by the phone before sending (a device limit, unknown capacity); nothing was sent. */
    data object NotAccepted : Device
}

/**
 * Simulation-only controls (DEMO). Not part of the device contract: a future
 * BLE repository will not provide them.
 */
interface SimulationController {
    val lowStockPolicy: LowStockPolicy

    /** Null while no simulation is active (the app then runs on the phone clock). */
    fun observeSimulation(): Flow<SimulationState?>

    suspend fun start(at: Instant, capacityUl: Long, remainingUl: Long): CommandResult
    suspend fun advanceHours(hours: Long): CommandResult
    suspend fun advanceDays(days: Long): CommandResult
    suspend fun advanceTo(target: Instant): CommandResult

    /** Explicit device-outage scenario (D3): doses until [target] are skipped, never caught up. */
    suspend fun simulateOutageUntil(target: Instant): CommandResult

    /** Clears simulation state and history only (never schedules). */
    suspend fun reset()
}

/**
 * Runs [SimulationEngine] commands against a [SimulationStore]. Commands are serialized;
 * schedules are only ever read (never written), and every result is committed atomically.
 */
class DispenserSimulator(
    private val store: SimulationStore,
    private val schedules: Flow<List<Schedule>>,
    private val engine: SimulationEngine = SimulationEngine(),
    private val runIdGenerator: () -> String = { UUID.randomUUID().toString() },
) : SimulationController {

    private val lock = Mutex()

    override val lowStockPolicy: LowStockPolicy get() = engine.lowStock

    override fun observeSimulation(): Flow<SimulationState?> = store.observeState()

    fun observeHistory(): Flow<List<HistoryEvent>> = store.observeHistory()

    override suspend fun start(at: Instant, capacityUl: Long, remainingUl: Long): CommandResult = lock.withLock {
        if (store.currentState() != null) return@withLock CommandResult.AlreadyActive
        // History may survive a deactivation (backup restore); keep seq increasing across runs.
        val firstSeq = (store.observeHistory().first().maxOfOrNull { it.seq } ?: 0) + 1
        apply(engine.start(runIdGenerator(), at, capacityUl, remainingUl, firstSeq))
    }

    override suspend fun advanceHours(hours: Long) = command { s, list -> engine.advanceBy(s, Duration.ofHours(hours), list) }
    override suspend fun advanceDays(days: Long) = command { s, list -> engine.advanceDays(s, days, list) }
    override suspend fun advanceTo(target: Instant) = command { s, list -> engine.advanceTo(s, target, list) }
    override suspend fun simulateOutageUntil(target: Instant) = command { s, list -> engine.simulateOutage(s, target, list) }

    /** Device-facing refill (D7); available only while the simulation runs. */
    suspend fun refill(remainingUl: Long): CommandResult = command { s, _ -> engine.refill(s, remainingUl) }

    override suspend fun reset() = lock.withLock { store.clear() }

    /** Runs [block] while no simulation command can execute (backup export / restore). */
    suspend fun <T> withCommandsPaused(block: suspend () -> T): T = lock.withLock { block() }

    private suspend fun command(block: (SimulationState, List<Schedule>) -> SimulationResult): CommandResult = lock.withLock {
        val state = store.currentState() ?: return@withLock CommandResult.NotActive
        apply(block(state, schedules.first()))
    }

    private suspend fun apply(result: SimulationResult): CommandResult = when (result) {
        is SimulationResult.Ok -> {
            store.commit(result.state, result.events)
            CommandResult.Done(result.events.size)
        }
        is SimulationResult.Rejected -> CommandResult.Rejected(result.reason)
    }
}
