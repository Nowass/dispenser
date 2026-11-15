package cz.nowass.dispenser.domain.simulation

import cz.nowass.dispenser.domain.DEFAULT_ZONE
import cz.nowass.dispenser.domain.Recurrence
import cz.nowass.dispenser.domain.Schedule
import cz.nowass.dispenser.domain.simulation.HistoryEventType.DISPENSE_BLOCKED_INSUFFICIENT_STOCK
import cz.nowass.dispenser.domain.simulation.HistoryEventType.DISPENSE_SKIPPED
import cz.nowass.dispenser.domain.simulation.HistoryEventType.DISPENSE_SUCCESS
import cz.nowass.dispenser.domain.simulation.HistoryEventType.RESERVOIR_LOW_WARNING
import cz.nowass.dispenser.domain.simulation.HistoryEventType.RESERVOIR_REFILLED
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.time.DayOfWeek
import java.time.Duration
import java.time.Instant
import java.time.LocalDate
import java.time.LocalTime

class SimulationEngineTest {

    private val engine = SimulationEngine()
    private val eight = LocalTime.of(8, 0)

    /** Default reservoir 400 of 500 ml (80 %), far from the low threshold unless a test wants it. */
    private fun start(at: Instant, capacityMl: Double = 500.0, remainingMl: Double = 400.0) =
        engine.start("r", at, ml(capacityMl), ml(remainingMl)).ok().state

    // ---- Basic execution ----

    @Test
    fun singleDose_producesOneSuccessEvent_withSnapshotAndVolumes() {
        val s0 = start(at(2026, 11, 14, 7))
        val sch = daily("s", eight, 10.0, name = "Ranní dávka")
        val r = engine.advanceTo(s0, at(2026, 11, 14, 9), listOf(sch)).ok()

        val e = r.events.single()
        assertEquals(DISPENSE_SUCCESS, e.type)
        assertEquals(EventOrigin.SIMULATION, e.origin)
        assertEquals(EventReason.SCHEDULED, e.reason)
        assertEquals(at(2026, 11, 14, 8), e.time)
        assertEquals("s", e.scheduleId)
        assertEquals("Ranní dávka", e.scheduleNameSnapshot)
        assertEquals("s@2026-11-14", e.occurrenceId)
        assertEquals(10_000L, e.requestedUl)
        assertEquals(10_000L, e.dispensedUl)
        assertEquals(400_000L, e.reservoirBeforeUl)
        assertEquals(390_000L, e.reservoirAfterUl)
        assertEquals(500_000L, e.capacityUl)
        assertEquals("r-1", e.id)
        assertEquals(at(2026, 11, 14, 9), r.state.clock)
        assertEquals(390_000L, r.state.reservoir.remainingUl)
    }

    @Test
    fun multipleSchedules_processedChronologically_tiesBySchedulId() {
        val s0 = start(at(2026, 11, 14, 0))
        val list = listOf(
            daily("b", eight, 1.0),
            daily("a", eight, 2.0),
            daily("c", LocalTime.of(7, 30), 3.0),
            daily("d", LocalTime.of(20, 0), 4.0),
        )
        val events = engine.advanceDays(s0, 1, list).ok().events
        assertEquals(listOf("c", "a", "b", "d"), events.map { it.scheduleId })
        assertEquals(listOf(1L, 2L, 3L, 4L), events.map { it.seq })
        // Running balance is consistent from event to event.
        events.zipWithNext().forEach { (x, y) -> assertEquals(x.reservoirAfterUl, y.reservoirBeforeUl) }
        assertEquals(400_000L - 10_000L, events.last().reservoirAfterUl)
    }

    @Test
    fun occurrenceAtStartInstant_orBeforeStart_isNeverExecuted() {
        val sch = daily("s", eight, 10.0)
        assertTrue(engine.advanceTo(start(at(2026, 11, 14, 8)), at(2026, 11, 14, 12), listOf(sch)).ok().events.isEmpty())
        // Missed before the simulation start: not caught up.
        assertTrue(engine.advanceTo(start(at(2026, 11, 14, 12)), at(2026, 11, 14, 23), listOf(sch)).ok().events.isEmpty())
    }

    // ---- Idempotence / time direction ----

    @Test
    fun repeatedEvaluation_andStepwiseAdvance_produceNoDuplicates() {
        val list = listOf(daily("a", eight, 5.0), daily("b", LocalTime.of(19, 30), 2.5))
        val s0 = start(at(2026, 11, 14, 0))
        val big = engine.advanceDays(s0, 5, list).ok()

        var s = s0
        val stepped = ArrayList<HistoryEvent>()
        repeat(5 * 24) {
            val r = engine.advanceBy(s, Duration.ofHours(1), list).ok()
            stepped += r.events
            s = engine.advanceTo(r.state, r.state.clock, list).ok().also { again -> assertTrue(again.events.isEmpty()) }.state
        }
        assertEquals(big.events, stepped)
        assertEquals(big.state.reservoir, s.reservoir)
        assertEquals(big.events.size, big.events.map { it.occurrenceId }.toSet().size)
    }

    @Test
    fun backwardTime_isRejected() {
        val s = engine.advanceTo(start(at(2026, 11, 14, 0)), at(2026, 11, 15, 0), emptyList()).ok().state
        assertEquals(SimulationResult.Rejected(RejectReason.TARGET_BEFORE_CLOCK), engine.advanceTo(s, at(2026, 11, 14, 12), emptyList()))
        assertEquals(SimulationResult.Rejected(RejectReason.TARGET_BEFORE_CLOCK), engine.advanceBy(s, Duration.ofHours(-1), emptyList()))
        assertEquals(SimulationResult.Rejected(RejectReason.TARGET_BEFORE_CLOCK), engine.advanceDays(s, -1, emptyList()))
        assertEquals(SimulationResult.Rejected(RejectReason.TARGET_BEFORE_CLOCK), engine.simulateOutage(s, at(2026, 11, 14, 0), emptyList()))
    }

    // ---- Schedule configuration effects ----

    @Test
    fun disabledAndDeletedSchedules_doNotExecute() {
        val s0 = start(at(2026, 11, 14, 0))
        assertTrue(engine.advanceDays(s0, 3, listOf(daily("s", eight, 10.0, enabled = false))).ok().events.isEmpty())
        assertTrue(engine.advanceDays(s0, 3, emptyList()).ok().events.isEmpty())
    }

    @Test
    fun editedSchedule_sameDay_isNotExecutedTwice_andHistoryKeepsSnapshot() {
        val original = daily("s", eight, 10.0, name = "Původní")
        val r1 = engine.advanceTo(start(at(2026, 11, 14, 0)), at(2026, 11, 14, 9), listOf(original)).ok()
        // Same schedule edited after today's dose: new time 20:00, new name and volume.
        val edited = original.copy(revision = 2, name = "Upravená", amountMl = 3.0, localTime = LocalTime.of(20, 0))
        val r2 = engine.advanceTo(r1.state, at(2026, 11, 15, 21), listOf(edited)).ok()

        assertEquals(listOf(LocalDate.of(2026, 11, 15)), r2.events.map { it.time.atZone(DEFAULT_ZONE).toLocalDate() })
        assertEquals("Upravená", r2.events.single().scheduleNameSnapshot)
        assertEquals(3_000L, r2.events.single().requestedUl)
        // Earlier event is an immutable value: unchanged by the edit.
        assertEquals("Původní", r1.events.single().scheduleNameSnapshot)
        assertEquals(10_000L, r1.events.single().requestedUl)
    }

    // ---- Outage (missed doses) ----

    @Test
    fun outage_skipsWithoutCatchUp_reservoirUnchanged() {
        val sch = daily("s", eight, 10.0)
        val s0 = start(at(2026, 11, 14, 0))
        val out = engine.simulateOutage(s0, at(2026, 11, 16, 12), listOf(sch)).ok()
        assertEquals(listOf(DISPENSE_SKIPPED, DISPENSE_SKIPPED, DISPENSE_SKIPPED), out.events.map { it.type })
        assertTrue(out.events.all { it.reason == EventReason.DEVICE_OUTAGE && it.dispensedUl == 0L && it.reservoirBeforeUl == it.reservoirAfterUl })
        assertEquals(s0.reservoir.remainingUl, out.state.reservoir.remainingUl)

        val back = engine.advanceTo(out.state, at(2026, 11, 17, 12), listOf(sch)).ok()
        assertEquals(listOf("s@2026-11-17"), back.events.map { it.occurrenceId }) // no catch-up
    }

    // ---- DST ----

    @Test
    fun dst_springGapSkipped_autumnOverlapExecutedOnce() {
        val spring = daily("s", LocalTime.of(2, 30), 1.0, start = LocalDate.of(2027, 3, 1))
        val sp = engine.advanceDays(start(at(2027, 3, 27, 0)), 3, listOf(spring)).ok().events
        assertEquals(listOf("s@2027-03-27", "s@2027-03-29"), sp.map { it.occurrenceId })

        val autumn = daily("s", LocalTime.of(2, 30), 1.0, start = LocalDate.of(2026, 10, 1))
        var s = start(at(2026, 10, 24, 23))
        val all = ArrayList<HistoryEvent>()
        repeat(12) { // half-hour steps straight through the repeated hour
            val r = engine.advanceBy(s, Duration.ofMinutes(30), listOf(autumn)).ok(); all += r.events; s = r.state
        }
        assertEquals(listOf(Instant.parse("2026-10-25T00:30:00Z")), all.map { it.time })
    }

    @Test
    fun advanceDays_keepsLocalTimeAcrossDst() {
        val s0 = start(at(2027, 3, 27, 8))
        assertEquals(at(2027, 3, 28, 8), engine.advanceDays(s0, 1, emptyList()).ok().state.clock) // 23 h later
        assertEquals(Duration.ofHours(23), Duration.between(s0.clock, at(2027, 3, 28, 8)))
    }

    // ---- Stock, blocking, refill ----

    @Test
    fun insufficientStock_blocksUntilRefill_noPartialNoNegative() {
        val sch = daily("s", eight, 10.0)
        // 15 % at start: the low warning is issued at start and latched, so it is not repeated later.
        val started = engine.start("r", at(2026, 11, 14, 0), ml(100.0), ml(15.0)).ok()
        assertEquals(listOf(RESERVOIR_LOW_WARNING), started.events.map { it.type })
        val s0 = started.state
        val r = engine.advanceDays(s0, 3, listOf(sch)).ok()
        assertEquals(
            listOf(DISPENSE_SUCCESS, DISPENSE_BLOCKED_INSUFFICIENT_STOCK, DISPENSE_BLOCKED_INSUFFICIENT_STOCK),
            r.events.map { it.type },
        )
        val (first, second) = r.events.filter { it.type == DISPENSE_BLOCKED_INSUFFICIENT_STOCK }
        assertEquals(EventReason.INSUFFICIENT_STOCK, first.reason)
        assertEquals(EventReason.BLOCKED_UNTIL_REFILL, second.reason)
        assertTrue(r.events.filter { it.type == DISPENSE_BLOCKED_INSUFFICIENT_STOCK }.all { it.dispensedUl == 0L && it.reservoirBeforeUl == 5_000L && it.reservoirAfterUl == 5_000L })
        assertEquals(5_000L, r.state.reservoir.remainingUl)
        assertTrue(r.state.reservoir.blocked)

        val refilled = engine.refill(r.state, ml(100.0)).ok()
        val refillEvent = refilled.events.single()
        assertEquals(RESERVOIR_REFILLED, refillEvent.type)
        assertEquals(5_000L, refillEvent.reservoirBeforeUl)
        assertEquals(100_000L, refillEvent.reservoirAfterUl)
        assertTrue(!refilled.state.reservoir.blocked && !refilled.state.reservoir.lowWarningLatched)

        assertEquals(listOf(DISPENSE_SUCCESS), engine.advanceDays(refilled.state, 1, listOf(sch)).ok().events.map { it.type })
    }

    @Test
    fun blockedState_isSticky_evenForSmallerLaterDoses() {
        val big = daily("big", eight, 10.0)
        val small = daily("small", LocalTime.of(9, 0), 1.0)
        val r = engine.advanceTo(start(at(2026, 11, 14, 0), remainingMl = 5.0), at(2026, 11, 14, 10), listOf(big, small)).ok()
        val doses = r.events.filter { it.scheduleId != null }
        assertEquals(listOf(DISPENSE_BLOCKED_INSUFFICIENT_STOCK, DISPENSE_BLOCKED_INSUFFICIENT_STOCK), doses.map { it.type })
        assertEquals(EventReason.BLOCKED_UNTIL_REFILL, doses[1].reason) // 1 ml would fit, but D2 blocks until refill
        assertEquals(5_000L, r.state.reservoir.remainingUl)
    }

    @Test
    fun exactRemainingVolume_isDispensed_thenNextIsBlocked() {
        val sch = daily("s", eight, 10.0)
        val r = engine.advanceDays(start(at(2026, 11, 14, 0), remainingMl = 10.0), 2, listOf(sch)).ok()
        val doses = r.events.filter { it.scheduleId != null }
        assertEquals(listOf(DISPENSE_SUCCESS, DISPENSE_BLOCKED_INSUFFICIENT_STOCK), doses.map { it.type })
        assertEquals(0L, doses[0].reservoirAfterUl)
    }

    @Test
    fun lowWarning_oncePerEpisode_rearmedByRefill() {
        val sch = daily("s", eight, 3.0)
        val s0 = start(at(2026, 11, 14, 0), capacityMl = 100.0, remainingMl = 25.0)
        val r1 = engine.advanceDays(s0, 5, listOf(sch)).ok() // 25 -> 22 -> 19 (low) -> 16 -> 13 -> 10
        assertEquals(1, r1.events.count { it.type == RESERVOIR_LOW_WARNING })
        val warning = r1.events.single { it.type == RESERVOIR_LOW_WARNING }
        assertEquals(19_000L, warning.reservoirAfterUl)
        assertEquals(at(2026, 11, 15, 8), warning.time)

        val refilled = engine.refill(r1.state, ml(30.0)).ok()
        assertTrue(refilled.events.none { it.type == RESERVOIR_LOW_WARNING })
        val r2 = engine.advanceDays(refilled.state, 5, listOf(sch)).ok() // 30 -> 27 -> 24 -> 21 -> 18 (low again)
        assertEquals(1, r2.events.count { it.type == RESERVOIR_LOW_WARNING })
    }

    @Test
    fun refill_validation_andRefillToLowLevelWarnsImmediately() {
        val s = start(at(2026, 11, 14, 0), capacityMl = 100.0, remainingMl = 50.0)
        assertEquals(SimulationResult.Rejected(RejectReason.INVALID_VOLUME), engine.refill(s, 0))
        assertEquals(SimulationResult.Rejected(RejectReason.INVALID_VOLUME), engine.refill(s, -1))
        assertEquals(SimulationResult.Rejected(RejectReason.INVALID_VOLUME), engine.refill(s, ml(100.0) + 1))
        assertEquals(100_000L, engine.refill(s, ml(100.0)).ok().state.reservoir.remainingUl)
        assertEquals(listOf(RESERVOIR_REFILLED, RESERVOIR_LOW_WARNING), engine.refill(s, ml(10.0)).ok().events.map { it.type })
    }

    @Test
    fun start_validation_andInitialLowStockWarning() {
        assertEquals(SimulationResult.Rejected(RejectReason.INVALID_CAPACITY), engine.start("r", at(2026, 11, 14, 0), 0, 0))
        assertEquals(SimulationResult.Rejected(RejectReason.INVALID_CAPACITY), engine.start("r", at(2026, 11, 14, 0), -5, 0))
        assertEquals(SimulationResult.Rejected(RejectReason.INVALID_VOLUME), engine.start("r", at(2026, 11, 14, 0), 100, 101))
        assertEquals(SimulationResult.Rejected(RejectReason.INVALID_VOLUME), engine.start("r", at(2026, 11, 14, 0), 100, -1))
        assertEquals(listOf(RESERVOIR_LOW_WARNING), engine.start("r", at(2026, 11, 14, 0), 100_000, 10_000).ok().events.map { it.type })
        assertTrue(engine.start("r", at(2026, 11, 14, 0), 100_000, 50_000).ok().events.isEmpty())
    }

    @Test
    fun configurableLowThreshold_isRespected() {
        val strict = SimulationEngine(LowStockPolicy(50))
        assertEquals(listOf(RESERVOIR_LOW_WARNING), strict.start("r", at(2026, 11, 14, 0), 100_000, 40_000).ok().events.map { it.type })
    }

    // ---- Limits / readiness ----

    @Test
    fun excessiveRanges_areRejectedExplicitly() {
        val s = start(at(2026, 11, 14, 0))
        assertEquals(SimulationResult.Rejected(RejectReason.RANGE_TOO_LONG), engine.advanceDays(s, 367, emptyList()))
        val tight = SimulationEngine(limits = SimulationLimits(maxOccurrences = 5))
        assertEquals(SimulationResult.Rejected(RejectReason.TOO_MANY_OCCURRENCES), tight.advanceDays(s, 10, listOf(daily("s", eight, 1.0))))
        assertTrue(tight.advanceDays(s, 4, listOf(daily("s", eight, 1.0))) is SimulationResult.Ok)
    }

    @Test
    fun serviceRequired_suspendsDispensing_reservedState() {
        val s = start(at(2026, 11, 14, 0)).copy(readiness = DeviceReadiness.SERVICE_REQUIRED)
        val r = engine.advanceDays(s, 1, listOf(daily("s", eight, 10.0))).ok()
        assertEquals(EventReason.DEVICE_NOT_READY, r.events.single().reason)
        assertEquals(DISPENSE_SKIPPED, r.events.single().type)
        assertEquals(s.reservoir.remainingUl, r.state.reservoir.remainingUl)
    }

    @Test
    fun weeklySchedule_andLongRun_keepIdsUniqueAndProcessedSetBounded() {
        val weekly = Schedule("w", 1, "w", true, 2.0, LocalDate.of(2026, 11, 2), eight, Recurrence.Weekly(setOf(DayOfWeek.MONDAY, DayOfWeek.THURSDAY)))
        val r = engine.advanceDays(start(at(2026, 11, 14, 0), capacityMl = 1000.0, remainingMl = 1000.0), 300, listOf(weekly, daily("d", eight, 1.0))).ok()
        assertEquals(r.events.size, r.events.map { it.id }.toSet().size)
        assertEquals((1L..r.events.size).toList(), r.events.map { it.seq })
        assertTrue(r.state.processedOccurrences.size <= 4)
        assertTrue(r.events.zipWithNext().all { (a, b) -> !b.time.isBefore(a.time) })
    }
}
