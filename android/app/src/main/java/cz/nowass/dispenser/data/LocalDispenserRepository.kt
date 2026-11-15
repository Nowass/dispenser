package cz.nowass.dispenser.data

import cz.nowass.dispenser.domain.ConnectionState
import cz.nowass.dispenser.domain.DEFAULT_ZONE
import cz.nowass.dispenser.domain.DataSource
import cz.nowass.dispenser.domain.DeviceStatus
import cz.nowass.dispenser.domain.DispenserEvent
import cz.nowass.dispenser.domain.ReservoirEstimate
import cz.nowass.dispenser.domain.Schedule
import cz.nowass.dispenser.domain.ScheduleDraft
import cz.nowass.dispenser.domain.ScheduleValidator
import cz.nowass.dispenser.domain.SyncState
import cz.nowass.dispenser.domain.toSchedule
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import java.time.Clock
import java.time.ZoneId
import java.util.UUID
import kotlin.time.Duration
import kotlin.time.Duration.Companion.minutes

/** Device-side values that are still simulated (no hardware yet); always tagged DEMO. */
data class SimulatedDevice(
    val batteryPercent: Int?,
    val reservoir: ReservoirEstimate,
) {
    companion object {
        /** Demo battery and reservoir estimate shown until a real device is connected. */
        fun demo(): SimulatedDevice = SimulatedDevice(
            batteryPercent = 78,
            reservoir = ReservoirEstimate(capacityMl = 500.0, estimatedRemainingMl = 312.0, source = DataSource.DEMO),
        )
    }
}

/**
 * App repository for A1.2c: schedules are persisted locally through a [ScheduleStore];
 * device values (battery, reservoir, history) are still simulated and tagged DEMO.
 *
 * - "Now" comes from the injected [clock] (the phone clock in the app; fixed in tests and
 *   previews). With a [clockTick] the device status re-emits on each tick boundary so
 *   time-dependent UI (next dose, past occurrences) stays current.
 * - Every write is validated, revision-checked atomically by the store, and stored as
 *   [SyncState.PENDING] ("Uloženo v telefonu"). No device acknowledgement is simulated.
 * - A future BLE repository can reuse the same [ScheduleStore] as its local cache.
 */
class LocalDispenserRepository(
    private val store: ScheduleStore,
    private val clock: Clock,
    private val simulatedDevice: SimulatedDevice = SimulatedDevice.demo(),
    private val idGenerator: () -> String = { UUID.randomUUID().toString() },
    private val clockTick: Duration? = 1.minutes,
    events: List<DispenserEvent> = emptyList(),
    private val zone: ZoneId = DEFAULT_ZONE,
) : DispenserRepository {

    private val reservoir = MutableStateFlow(simulatedDevice.reservoir)
    private val events = MutableStateFlow(events)
    private val writeLock = Mutex()

    override fun observeDevice(): Flow<DeviceStatus> = flow {
        emit(deviceStatus())
        val tick = clockTick ?: return@flow
        val tickMs = tick.inWholeMilliseconds
        while (true) {
            // Align to the next tick boundary of the clock (e.g. the next full minute).
            delay(tickMs - Math.floorMod(clock.millis(), tickMs))
            emit(deviceStatus())
        }
    }.distinctUntilChanged()

    private fun deviceStatus() = DeviceStatus(
        source = DataSource.DEMO,
        connection = ConnectionState.DEMO,
        now = clock.instant(),
        batteryPercent = simulatedDevice.batteryPercent,
        zone = zone,
    )

    override fun observeSchedules(): Flow<List<Schedule>> =
        store.observe().map { snap -> snap.schedules.map { it.schedule } }.distinctUntilChanged()

    override fun observeScheduleSync(): Flow<Map<String, SyncState>> =
        store.observe().map { snap -> snap.schedules.associate { it.schedule.id to it.sync } }.distinctUntilChanged()

    override fun observeUnreadableScheduleCount(): Flow<Int> =
        store.observe().map { it.unreadableCount }.distinctUntilChanged()

    override fun observeReservoir(): Flow<ReservoirEstimate> = reservoir.asStateFlow()
    override fun observeEvents(): Flow<List<DispenserEvent>> = events.asStateFlow()

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

    private companion object {
        const val MAX_ID_ATTEMPTS = 5
    }
}
