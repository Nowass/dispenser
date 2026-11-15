package cz.nowass.dispenser.data

import cz.nowass.dispenser.domain.DEFAULT_ZONE
import cz.nowass.dispenser.domain.Recurrence
import cz.nowass.dispenser.domain.RecurrenceKind
import cz.nowass.dispenser.domain.Schedule
import cz.nowass.dispenser.domain.ScheduleDraft
import cz.nowass.dispenser.domain.SyncState
import cz.nowass.dispenser.domain.simulation.HistoryEventType
import cz.nowass.dispenser.domain.simulation.RejectReason
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.time.Clock
import java.time.Instant
import java.time.LocalDate
import java.time.LocalTime
import java.time.ZonedDateTime

class DispenserSimulatorTest {

    private val phoneNow = ZonedDateTime.of(2026, 11, 15, 12, 0, 0, 0, DEFAULT_ZONE).toInstant()
    private val simStart = ZonedDateTime.of(2026, 11, 20, 7, 0, 0, 0, DEFAULT_ZONE).toInstant()
    private val morning = Schedule("m", 1, "Ranní", true, 10.0, LocalDate.of(2026, 11, 1), LocalTime.of(8, 0), Recurrence.Daily())

    private fun repo(scheduleStore: InMemoryScheduleStore = InMemoryScheduleStore(listOf(StoredSchedule(morning, SyncState.PENDING))), simStore: SimulationStore = InMemorySimulationStore()) =
        LocalDispenserRepository(scheduleStore, Clock.fixed(phoneNow, DEFAULT_ZONE), simulationStore = simStore, clockTick = null, simulationRunIds = { "run" })

    @Test
    fun inactive_usesPhoneClock_unknownReservoir_commandsNotActive() = runTest {
        val r = repo()
        val device = r.observeDevice().first()
        assertEquals(phoneNow, device.now)
        assertFalse(device.simulationActive)
        assertNull(r.observeReservoir().first())
        assertEquals(CommandResult.NotActive, r.simulation.advanceDays(1))
        assertEquals(CommandResult.NotActive, r.refillReservoir(1_000))
    }

    @Test
    fun active_usesSimulationClock_processesDoses_andRefills() = runTest {
        val r = repo()
        // 25 ml of 500 ml = 5 %: the low-stock warning is issued at start (once per episode).
        assertEquals(CommandResult.Done(1), r.simulation.start(simStart, 500_000, 25_000))
        assertEquals(CommandResult.AlreadyActive, r.simulation.start(simStart, 500_000, 25_000))
        assertEquals(simStart, r.observeDevice().first().now)
        assertTrue(r.observeDevice().first().simulationActive)

        assertTrue(r.simulation.advanceDays(3) is CommandResult.Done) // 20, 21 ok (25 -> 5), 22 blocked
        val history = r.observeHistory().first()
        assertEquals(
            listOf(HistoryEventType.RESERVOIR_LOW_WARNING, HistoryEventType.DISPENSE_SUCCESS, HistoryEventType.DISPENSE_SUCCESS, HistoryEventType.DISPENSE_BLOCKED_INSUFFICIENT_STOCK),
            history.map { it.type },
        )
        assertEquals(listOf("run-1", "run-2", "run-3", "run-4"), history.map { it.id })
        assertTrue(r.observeReservoir().first()!!.blocked)

        assertEquals(CommandResult.Rejected(RejectReason.INVALID_VOLUME), r.refillReservoir(600_000))
        assertEquals(CommandResult.Done(1), r.refillReservoir(500_000))
        assertEquals(500_000L, r.observeReservoir().first()!!.remainingUl)
        assertFalse(r.observeReservoir().first()!!.blocked)
    }

    @Test
    fun backwardTarget_isRejected_outageSkips() = runTest {
        val r = repo()
        r.simulation.start(simStart, 500_000, 400_000)
        assertEquals(CommandResult.Rejected(RejectReason.TARGET_BEFORE_CLOCK), r.simulation.advanceTo(simStart.minusSeconds(60)))
        r.simulation.simulateOutageUntil(simStart.plusSeconds(3 * 86_400))
        assertTrue(r.observeHistory().first().all { it.type == HistoryEventType.DISPENSE_SKIPPED })
        assertEquals(400_000L, r.observeReservoir().first()!!.remainingUl)
    }

    @Test
    fun simulation_neverWritesSchedules_andResetClearsOnlySimulation() = runTest {
        val schedules = InMemoryScheduleStore(listOf(StoredSchedule(morning, SyncState.PENDING)))
        val r = repo(schedules)
        r.createSchedule(ScheduleDraft("Večer", 5.0, LocalDate.of(2026, 11, 1), LocalTime.of(20, 0), RecurrenceKind.DAILY))
        val before = schedules.observe().first()

        r.simulation.start(simStart, 500_000, 400_000)
        r.simulation.advanceDays(10)
        r.refillReservoir(500_000)
        assertEquals(before, schedules.observe().first()) // revisions and PENDING state untouched

        r.simulation.reset()
        assertNull(r.simulation.observeSimulation().first())
        assertTrue(r.observeHistory().first().isEmpty())
        assertEquals(phoneNow, r.observeDevice().first().now) // back to the phone clock
        assertEquals(before, schedules.observe().first())
    }

    @Test
    fun scheduleEditsAffectOnlyFutureUnprocessedDoses() = runTest {
        val schedules = InMemoryScheduleStore(listOf(StoredSchedule(morning, SyncState.PENDING)))
        val r = repo(schedules)
        r.simulation.start(simStart, 500_000, 400_000)
        r.simulation.advanceDays(1)
        val first = r.observeHistory().first().single()
        r.updateSchedule("m", 1, ScheduleDraft("Přejmenovaná", 2.0, LocalDate.of(2026, 11, 1), LocalTime.of(8, 0), RecurrenceKind.DAILY))
        r.simulation.advanceDays(1)
        val history = r.observeHistory().first()
        assertEquals(first, history.first()) // immutable (D9)
        assertEquals("Přejmenovaná", history.last().scheduleNameSnapshot)
        assertEquals(2_000L, history.last().requestedUl)
        assertEquals(Instant.parse("2026-11-21T07:00:00Z"), history.last().time)
    }
}
