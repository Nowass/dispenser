package cz.nowass.dispenser.domain.simulation

import cz.nowass.dispenser.domain.Volume
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test
import java.time.Instant

class ReservoirTest {
    private val t = Instant.parse("2026-11-15T10:00:00Z")

    @Test
    fun invalidStates_areRejected() {
        assertThrows(IllegalArgumentException::class.java) { ReservoirState(0, 0, t) }        // zero capacity (D8)
        assertThrows(IllegalArgumentException::class.java) { ReservoirState(-1, 0, t) }
        assertThrows(IllegalArgumentException::class.java) { ReservoirState(Volume.MAX_MICROLITRES + 1, 0, t) }
        assertThrows(IllegalArgumentException::class.java) { ReservoirState(100, -1, t) }     // negative volume
        assertThrows(IllegalArgumentException::class.java) { ReservoirState(100, 101, t) }    // above capacity
        ReservoirState(100, 0, t)
        ReservoirState(100, 100, t)
    }

    @Test
    fun canDispense_requiresFullVolume_andNotBlocked() {
        val r = ReservoirState(500_000, 10_000, t)
        assertTrue(r.canDispense(10_000))   // exact remaining
        assertFalse(r.canDispense(10_001))  // never partial
        assertFalse(r.canDispense(0))
        assertFalse(r.copy(blocked = true).canDispense(1))
    }

    @Test
    fun lowStockThreshold_boundaries() {
        val p = LowStockPolicy() // 20 % PROPOSED
        assertFalse(p.isLow(20_000, 100_000)) // exactly 20 % is not low
        assertTrue(p.isLow(19_999, 100_000))
        assertTrue(p.isLow(0, 100_000))
        assertFalse(LowStockPolicy(0).isLow(0, 100_000))
        assertTrue(LowStockPolicy(100).isLow(99_999, 100_000))
        assertFalse(LowStockPolicy(100).isLow(100_000, 100_000))
        assertFalse(p.isLow(Volume.MAX_MICROLITRES / 5, Volume.MAX_MICROLITRES - 4)) // no overflow at the range limit
        assertThrows(IllegalArgumentException::class.java) { LowStockPolicy(101) }
        assertThrows(IllegalArgumentException::class.java) { LowStockPolicy(-1) }
    }

    @Test
    fun status_derivation() {
        val p = LowStockPolicy()
        assertEquals(ReservoirStatus.NORMAL, ReservoirState(100_000, 50_000, t).status(p))
        assertEquals(ReservoirStatus.LOW, ReservoirState(100_000, 10_000, t).status(p))
        assertEquals(ReservoirStatus.BLOCKED, ReservoirState(100_000, 90_000, t, blocked = true).status(p))
    }

    @Test
    fun historyEvent_invariants() {
        assertThrows(IllegalArgumentException::class.java) {
            HistoryEvent("x", 1, t, HistoryEventType.DISPENSE_BLOCKED_INSUFFICIENT_STOCK, EventOrigin.SIMULATION, EventReason.INSUFFICIENT_STOCK,
                dispensedUl = 5, reservoirBeforeUl = 10, reservoirAfterUl = 10, capacityUl = 100)
        }
        assertThrows(IllegalArgumentException::class.java) {
            HistoryEvent("x", 1, t, HistoryEventType.DISPENSE_SUCCESS, EventOrigin.SIMULATION, EventReason.SCHEDULED,
                dispensedUl = 5, reservoirBeforeUl = 10, reservoirAfterUl = -5, capacityUl = 100)
        }
    }

    @Test
    fun primingRun_mayConsumeLiquid_otherServiceEventsMayNot() {
        HistoryEvent("p", 1, t, HistoryEventType.PRIMING_PUMP_RUN, EventOrigin.DEVICE, EventReason.SERVICE_BUTTON,
            dispensedUl = 1_500, reservoirBeforeUl = 100_000, reservoirAfterUl = 98_500, capacityUl = 500_000)
        assertThrows(IllegalArgumentException::class.java) {
            HistoryEvent("s", 2, t, HistoryEventType.SERVICE_STARTED, EventOrigin.DEVICE, EventReason.SERVICE_BUTTON,
                dispensedUl = 1, reservoirBeforeUl = 10, reservoirAfterUl = 10, capacityUl = 100)
        }
    }

    @Test
    fun simulation_neverProducesReservedServiceOrHardwareEvents() {
        val engine = SimulationEngine()
        val schedules = listOf(daily("a", java.time.LocalTime.of(8, 0), 7.5), daily("b", java.time.LocalTime.of(20, 0), 3.0, interval = 2))
        var s = (engine.start("r", at(2026, 11, 14, 0), 100_000, 60_000) as SimulationResult.Ok).state
        val all = ArrayList<HistoryEvent>()
        repeat(30) { i ->
            val r = (if (i % 7 == 3) engine.simulateOutage(s, s.clock.plusSeconds(86_400), schedules) else engine.advanceDays(s, 1, schedules)) as SimulationResult.Ok
            all += r.events; s = r.state
            if (s.reservoir.blocked) { val f = engine.refill(s, 100_000) as SimulationResult.Ok; all += f.events; s = f.state }
        }
        val reserved = setOf(
            HistoryEventType.DISPENSE_INTERRUPTED_UNCERTAIN, HistoryEventType.SERVICE_STARTED, HistoryEventType.PRIMING_PUMP_RUN,
            HistoryEventType.TUBING_PRIMED, HistoryEventType.SERVICE_COMPLETED,
        )
        assertTrue(all.isNotEmpty())
        assertTrue(all.none { it.type in reserved || it.origin != EventOrigin.SIMULATION })
        assertTrue(s.readiness == DeviceReadiness.READY) // the simulation never enters service mode by itself
    }
}
