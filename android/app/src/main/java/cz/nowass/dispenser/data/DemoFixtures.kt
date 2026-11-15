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
import cz.nowass.dispenser.domain.SyncState
import java.time.Clock
import java.time.DayOfWeek
import java.time.Instant
import java.time.LocalDate
import java.time.LocalTime
import java.time.ZonedDateTime
import java.util.UUID

/**
 * Fixture repository for previews and tests: in-memory store pre-filled with
 * [MockSeed.schedules] (as PENDING, i.e. phone-only), a fixed clock at the fixture time
 * and no clock ticking. Not used by the running app.
 */
fun MockDispenserRepository(
    seed: MockSeed = MockSeed.default(),
    idGenerator: () -> String = { UUID.randomUUID().toString() },
): LocalDispenserRepository = LocalDispenserRepository(
    store = InMemoryScheduleStore(seed.schedules.map { StoredSchedule(it, SyncState.PENDING) }),
    clock = Clock.fixed(seed.device.now, seed.device.zone),
    simulatedDevice = seed.simulatedDevice,
    idGenerator = idGenerator,
    clockTick = null,
    events = seed.events,
)

/**
 * Deterministic demo fixtures for Compose previews, unit tests and explicit simulation
 * scenarios. Never inserted into the app's database (a fresh install starts empty).
 * The fixture clock ([START]) is only used together with fixture schedules.
 */
data class MockSeed(
    val device: DeviceStatus,
    val schedules: List<Schedule>,
    val reservoir: ReservoirEstimate,
    val events: List<DispenserEvent>,
) {
    /** Simulated device values matching this fixture. */
    val simulatedDevice: SimulatedDevice get() = SimulatedDevice(device.batteryPercent, reservoir)

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
