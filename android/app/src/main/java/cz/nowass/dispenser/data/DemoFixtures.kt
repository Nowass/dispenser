package cz.nowass.dispenser.data

import cz.nowass.dispenser.domain.ConnectionState
import cz.nowass.dispenser.domain.DEFAULT_ZONE
import cz.nowass.dispenser.domain.DataSource
import cz.nowass.dispenser.domain.DeviceStatus
import cz.nowass.dispenser.domain.DoseOccurrence
import cz.nowass.dispenser.domain.simulation.EventOrigin
import cz.nowass.dispenser.domain.simulation.EventReason
import cz.nowass.dispenser.domain.simulation.HistoryEvent
import cz.nowass.dispenser.domain.simulation.HistoryEventType
import cz.nowass.dispenser.domain.simulation.ReservoirState
import cz.nowass.dispenser.domain.simulation.SimulationState
import cz.nowass.dispenser.domain.Recurrence
import cz.nowass.dispenser.domain.Schedule
import cz.nowass.dispenser.domain.SyncState
import java.time.Clock
import java.time.DayOfWeek
import java.time.Instant
import java.time.LocalDate
import java.time.LocalTime
import java.time.ZonedDateTime
import java.util.UUID

/**
 * Fixture repository for previews and tests: in-memory stores pre-filled with
 * [MockSeed.schedules] (as PENDING, i.e. phone-only) and, if [simulationActive], an active
 * demo simulation at the fixture time with sample history. Not used by the running app.
 */
fun MockDispenserRepository(
    seed: MockSeed = MockSeed.default(),
    idGenerator: () -> String = { UUID.randomUUID().toString() },
    simulationActive: Boolean = true,
): LocalDispenserRepository = LocalDispenserRepository(
    store = InMemoryScheduleStore(seed.schedules.map { StoredSchedule(it, SyncState.PENDING) }),
    clock = Clock.fixed(seed.device.now, seed.device.zone),
    simulationStore = if (simulationActive) InMemorySimulationStore(seed.simulation, seed.events) else InMemorySimulationStore(),
    batteryPercent = seed.device.batteryPercent,
    idGenerator = idGenerator,
    clockTick = null,
)

/**
 * Deterministic demo fixtures for Compose previews, unit tests and explicit simulation
 * scenarios. Never inserted into the app's database (a fresh install starts empty).
 * The fixture clock ([START]) is only used together with fixture schedules.
 */
data class MockSeed(
    val device: DeviceStatus,
    val schedules: List<Schedule>,
    val reservoir: ReservoirState,
    val events: List<HistoryEvent>,
) {
    /** Active demo simulation at the fixture time (for previews/tests only). */
    val simulation: SimulationState
        get() = SimulationState(
            runId = FIXTURE_RUN_ID,
            startedAt = events.firstOrNull()?.time ?: device.now,
            clock = device.now,
            reservoir = reservoir,
            nextSeq = events.size + 1L,
        )

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
            reservoir = ReservoirState(capacityUl = 500_000, remainingUl = 312_000, updatedAt = ZonedDateTime.of(2026, 11, 12, 19, 30, 0, 0, DEFAULT_ZONE).toInstant()),
            events = listOf(
                event(1, 2026, 11, 2, 7, 45, HistoryEventType.RESERVOIR_REFILLED, EventReason.MANUAL_REFILL, null, null, 0, 40_000, 500_000),
                event(2, 2026, 11, 12, 8, 0, HistoryEventType.DISPENSE_SUCCESS, EventReason.SCHEDULED, "demo-morning", 10_000, 10_000, 327_000, 317_000),
                event(3, 2026, 11, 12, 19, 30, HistoryEventType.DISPENSE_SUCCESS, EventReason.SCHEDULED, "demo-evening", 5_000, 5_000, 317_000, 312_000),
                event(4, 2026, 11, 13, 8, 0, HistoryEventType.DISPENSE_SKIPPED, EventReason.DEVICE_OUTAGE, "demo-morning", 10_000, 0, 312_000, 312_000),
            ),
        )

        const val FIXTURE_RUN_ID = "demo"

        private val NAMES = mapOf("demo-morning" to "Ranní dávka", "demo-evening" to "Večerní doplnění")

        private fun event(
            seq: Long, y: Int, mo: Int, d: Int, h: Int, mi: Int,
            type: HistoryEventType, reason: EventReason, scheduleId: String?,
            requestedUl: Long?, dispensedUl: Long, beforeUl: Long, afterUl: Long,
        ): HistoryEvent {
            val time = ZonedDateTime.of(y, mo, d, h, mi, 0, 0, DEFAULT_ZONE)
            return HistoryEvent(
                id = "$FIXTURE_RUN_ID-$seq",
                seq = seq,
                time = time.toInstant(),
                type = type,
                origin = EventOrigin.SIMULATION,
                reason = reason,
                scheduleId = scheduleId,
                scheduleNameSnapshot = scheduleId?.let(NAMES::get),
                occurrenceId = scheduleId?.let { DoseOccurrence.occurrenceId(it, time.toLocalDate()) },
                requestedUl = requestedUl,
                dispensedUl = dispensedUl,
                reservoirBeforeUl = beforeUl,
                reservoirAfterUl = afterUl,
                capacityUl = 500_000,
            )
        }
    }
}
