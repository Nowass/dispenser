package cz.nowass.dispenser.data

import cz.nowass.dispenser.domain.ConnectionState
import cz.nowass.dispenser.domain.DEFAULT_ZONE
import cz.nowass.dispenser.domain.DataSource
import cz.nowass.dispenser.domain.DeviceStatus
import cz.nowass.dispenser.domain.DispenserEvent
import cz.nowass.dispenser.domain.EventType
import cz.nowass.dispenser.domain.Recurrence
import cz.nowass.dispenser.domain.ReservoirEstimate
import cz.nowass.dispenser.domain.Schedule
import cz.nowass.dispenser.domain.ScheduleDraft
import cz.nowass.dispenser.domain.ScheduleValidator
import cz.nowass.dispenser.domain.SyncState
import cz.nowass.dispenser.domain.toSchedule
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import java.util.UUID
import kotlin.time.Duration
import kotlin.time.Duration.Companion.milliseconds
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.time.DayOfWeek
import java.time.Instant
import java.time.LocalDate
import java.time.LocalTime
import java.time.ZonedDateTime

/**
 * Deterministic in-memory DEMO backend. Every value it emits is tagged
 * [DataSource.DEMO]; nothing here talks to hardware, Bluetooth or the network.
 *
 * The demo uses its own simulated clock (seeded at [MockSeed.START]) so the
 * dashboard is reproducible regardless of the phone's real time.
 *
 * Schedule writes are serialized by a mutex, check the expected revision, and
 * go PENDING -> SIMULATED_ACK after [simulatedAckDelay] (a stand-in for a
 * future device round-trip; never presented as real device sync).
 */
class MockDispenserRepository(
    seed: MockSeed = MockSeed.default(),
    private val idGenerator: () -> String = { "demo-" + UUID.randomUUID() },
    private val simulatedAckDelay: Duration = 400.milliseconds,
) : DispenserRepository {

    private val device = MutableStateFlow(seed.device)
    private val schedules = MutableStateFlow(seed.schedules)
    private val reservoir = MutableStateFlow(seed.reservoir)
    private val events = MutableStateFlow(seed.events)
    private val sync = MutableStateFlow(seed.schedules.associate { it.id to SyncState.SIMULATED_ACK })
    private val writeLock = Mutex()

    override fun observeDevice(): Flow<DeviceStatus> = device.asStateFlow()
    override fun observeSchedules(): Flow<List<Schedule>> = schedules.asStateFlow()
    override fun observeReservoir(): Flow<ReservoirEstimate> = reservoir.asStateFlow()
    override fun observeEvents(): Flow<List<DispenserEvent>> = events.asStateFlow()
    override fun observeScheduleSync(): Flow<Map<String, SyncState>> = sync.asStateFlow()

    override suspend fun createSchedule(draft: ScheduleDraft): ScheduleWriteResult {
        val errors = ScheduleValidator.validate(draft)
        if (errors.isNotEmpty()) return ScheduleWriteResult.Invalid(errors)
        val created = writeLock.withLock {
            var id = idGenerator()
            while (schedules.value.any { it.id == id }) id = idGenerator()
            val schedule = draft.toSchedule(id = id, revision = 1)
            schedules.update { it + schedule }
            sync.update { it + (id to SyncState.PENDING) }
            schedule
        }
        simulateAck(created)
        return ScheduleWriteResult.Success(created)
    }

    override suspend fun updateSchedule(id: String, expectedRevision: Int, draft: ScheduleDraft): ScheduleWriteResult {
        val errors = ScheduleValidator.validate(draft)
        if (errors.isNotEmpty()) return ScheduleWriteResult.Invalid(errors)
        val updated = writeLock.withLock {
            val current = schedules.value.firstOrNull { it.id == id } ?: return ScheduleWriteResult.NotFound
            if (current.revision != expectedRevision) return ScheduleWriteResult.Conflict
            val next = draft.toSchedule(id = id, revision = current.revision + 1)
            schedules.update { list -> list.map { if (it.id == id) next else it } }
            sync.update { it + (id to SyncState.PENDING) }
            next
        }
        simulateAck(updated)
        return ScheduleWriteResult.Success(updated)
    }

    override suspend fun deleteSchedule(id: String, expectedRevision: Int): ScheduleWriteResult =
        writeLock.withLock {
            val current = schedules.value.firstOrNull { it.id == id } ?: return ScheduleWriteResult.NotFound
            if (current.revision != expectedRevision) return ScheduleWriteResult.Conflict
            schedules.update { list -> list.filterNot { it.id == id } }
            sync.update { it - id }
            // Event history is intentionally kept (audit trail).
            ScheduleWriteResult.Success(null)
        }

    /** Marks [schedule] acknowledged unless it was changed or deleted meanwhile. */
    private suspend fun simulateAck(schedule: Schedule) {
        delay(simulatedAckDelay)
        writeLock.withLock {
            val current = schedules.value.firstOrNull { it.id == schedule.id }
            if (current?.revision == schedule.revision) {
                sync.update { it + (schedule.id to SyncState.SIMULATED_ACK) }
            }
        }
    }
}

/** Seed data for the DEMO backend. */
data class MockSeed(
    val device: DeviceStatus,
    val schedules: List<Schedule>,
    val reservoir: ReservoirEstimate,
    val events: List<DispenserEvent>,
) {
    companion object {
        /** Saturday 14 Nov 2026, 07:30 Europe/Prague. */
        val START: Instant = ZonedDateTime.of(2026, 11, 14, 7, 30, 0, 0, DEFAULT_ZONE).toInstant()

        private val SEED_START_DATE: LocalDate = LocalDate.of(2026, 11, 2)

        fun default(): MockSeed = MockSeed(
            device = DeviceStatus(
                source = DataSource.DEMO,
                connection = ConnectionState.DEMO,
                now = START,
                batteryPercent = 78,
            ),
            schedules = listOf(
                Schedule(
                    id = "demo-morning",
                    revision = 1,
                    name = "Ranní dávka",
                    enabled = true,
                    amountMl = 10.0,
                    startDate = SEED_START_DATE,
                    localTime = LocalTime.of(8, 0),
                    recurrence = Recurrence.Daily(intervalDays = 1),
                ),
                Schedule(
                    id = "demo-evening",
                    revision = 1,
                    name = "Večerní doplnění",
                    enabled = true,
                    amountMl = 5.0,
                    startDate = SEED_START_DATE,
                    localTime = LocalTime.of(19, 30),
                    recurrence = Recurrence.Weekly(
                        weekdays = setOf(DayOfWeek.MONDAY, DayOfWeek.THURSDAY),
                    ),
                ),
                Schedule(
                    id = "demo-weekend",
                    revision = 2,
                    name = "Víkendová dávka",
                    enabled = false,
                    amountMl = 15.0,
                    startDate = SEED_START_DATE,
                    localTime = LocalTime.of(10, 0),
                    recurrence = Recurrence.Weekly(
                        weekdays = setOf(DayOfWeek.SATURDAY),
                        intervalWeeks = 2,
                    ),
                ),
            ),
            reservoir = ReservoirEstimate(
                capacityMl = 500.0,
                estimatedRemainingMl = 312.0,
                source = DataSource.DEMO,
            ),
            events = listOf(
                event(1, 2026, 11, 2, 7, 45, EventType.REFILL, null, 500.0),
                event(2, 2026, 11, 12, 8, 0, EventType.COMPLETED, "demo-morning", 10.0),
                event(3, 2026, 11, 12, 19, 30, EventType.COMPLETED, "demo-evening", 5.0),
                event(4, 2026, 11, 13, 8, 0, EventType.SKIPPED, "demo-morning", 10.0),
            ),
        )

        private fun event(
            seq: Long, y: Int, mo: Int, d: Int, h: Int, mi: Int,
            type: EventType, scheduleId: String?, amountMl: Double?,
        ) = DispenserEvent(
            seq = seq,
            time = ZonedDateTime.of(y, mo, d, h, mi, 0, 0, DEFAULT_ZONE).toInstant(),
            type = type,
            scheduleId = scheduleId,
            amountMl = amountMl,
            source = DataSource.DEMO,
        )
    }
}
