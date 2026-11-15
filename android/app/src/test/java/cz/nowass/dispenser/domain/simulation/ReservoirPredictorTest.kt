package cz.nowass.dispenser.domain.simulation

import cz.nowass.dispenser.domain.Recurrence
import cz.nowass.dispenser.domain.Schedule
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.time.DayOfWeek
import java.time.Duration
import java.time.Instant
import java.time.LocalDate
import java.time.LocalTime
import kotlin.random.Random

class ReservoirPredictorTest {

    private val predictor = ReservoirPredictor()
    private val now = at(2026, 11, 14, 12)
    private val eight = LocalTime.of(8, 0)

    private fun reservoir(remainingMl: Double, capacityMl: Double = 500.0, blocked: Boolean = false) =
        ReservoirState(ml(capacityMl), ml(remainingMl), now, blocked = blocked)

    @Test
    fun daily_coversCompleteDosesOnly() {
        val p = predictor.predict(reservoir(35.0), now, listOf(daily("s", eight, 10.0)))
        assertEquals(PredictionStatus.DEPLETES_WITHIN_HORIZON, p.status)
        assertEquals(3, p.coveredDoses) // 15., 16., 17. Nov; 5 ml left is not a partial dose
        assertEquals(LocalDate.of(2026, 11, 18), p.firstUncovered!!.localDate)
        assertEquals(at(2026, 11, 18, 8), p.depletionAt)
        assertEquals(5_000L, p.remainingAfterLastCoveredUl)
    }

    @Test
    fun weekly_selectedWeekdays() {
        val weekly = Schedule("w", 1, "w", true, 10.0, LocalDate.of(2026, 11, 2), eight, Recurrence.Weekly(setOf(DayOfWeek.MONDAY, DayOfWeek.THURSDAY)))
        val p = predictor.predict(reservoir(30.0), now, listOf(weekly))
        assertEquals(3, p.coveredDoses) // Mon 16, Thu 19, Mon 23
        assertEquals(LocalDate.of(2026, 11, 26), p.firstUncovered!!.localDate)
    }

    @Test
    fun irregularMultipleSchedules_differentVolumes_inChronologicalOrder() {
        val a = daily("a", eight, 10.0)
        val b = daily("b", LocalTime.of(20, 0), 2.5, interval = 3, start = LocalDate.of(2026, 11, 14))
        // 14th 20:00 b 2.5 | 15th 08 a 10 | 16th a 10 | 17th a 10, 20:00 b 2.5 | 18th a 10 (needs 10, has 5)
        val p = predictor.predict(reservoir(40.0), now, listOf(a, b))
        assertEquals(5, p.coveredDoses)
        assertEquals("a", p.firstUncovered!!.scheduleId)
        assertEquals(LocalDate.of(2026, 11, 18), p.firstUncovered!!.localDate)
        assertEquals(5_000L, p.remainingAfterLastCoveredUl)
    }

    @Test
    fun emptyOrDisabledSchedules_noFutureDoses() {
        assertEquals(PredictionStatus.NO_FUTURE_DOSES, predictor.predict(reservoir(100.0), now, emptyList()).status)
        assertEquals(PredictionStatus.NO_FUTURE_DOSES, predictor.predict(reservoir(100.0), now, listOf(daily("s", eight, 1.0, enabled = false))).status)
        val ended = daily("s", eight, 1.0, end = LocalDate.of(2026, 11, 10))
        assertEquals(PredictionStatus.NO_FUTURE_DOSES, predictor.predict(reservoir(100.0), now, listOf(ended)).status)
    }

    @Test
    fun insufficientForTheVeryNextDose() {
        val p = predictor.predict(reservoir(9.999), now, listOf(daily("s", eight, 10.0)))
        assertEquals(PredictionStatus.INSUFFICIENT_FOR_NEXT_DOSE, p.status)
        assertEquals(0, p.coveredDoses)
        assertEquals(at(2026, 11, 15, 8), p.depletionAt)
    }

    @Test
    fun blockedReservoir_reportsBlockedUntilRefill() {
        val p = predictor.predict(reservoir(400.0, blocked = true), now, listOf(daily("s", eight, 10.0)))
        assertEquals(PredictionStatus.BLOCKED_UNTIL_REFILL, p.status)
        assertEquals(0, p.coveredDoses)
        assertEquals(at(2026, 11, 15, 8), p.firstUncovered!!.scheduledAt)
    }

    @Test
    fun endDates_allRemainingDosesCovered() {
        val p = predictor.predict(reservoir(100.0), now, listOf(daily("s", eight, 10.0, end = LocalDate.of(2026, 11, 17))))
        assertEquals(PredictionStatus.ALL_REMAINING_DOSES_COVERED, p.status)
        assertEquals(3, p.coveredDoses)
        assertNull(p.depletionAt)
        assertEquals(70_000L, p.remainingAfterLastCoveredUl)
    }

    @Test
    fun longIntervalBeyondHorizon_isUnknown() {
        val p = predictor.predict(reservoir(100.0), now, listOf(daily("s", eight, 10.0, interval = 2000, start = LocalDate.of(2026, 11, 14))))
        assertEquals(PredictionStatus.UNKNOWN_BEYOND_HORIZON, p.status)
        assertEquals(0, p.coveredDoses)
        assertNull(p.depletionAt)
    }

    @Test
    fun plentyOfStock_beyondHorizon_isUnknown_notFalsePrecision() {
        val p = predictor.predict(reservoir(500.0), now, listOf(daily("s", eight, 1.0)))
        assertEquals(PredictionStatus.UNKNOWN_BEYOND_HORIZON, p.status)
        assertEquals(365, p.coveredDoses)
        assertEquals(now.plus(Duration.ofDays(365)), p.horizonEnd)
        assertNull(p.depletionAt)
    }

    @Test
    fun tooManyOccurrences_isUnknown() {
        val p = ReservoirPredictor(maxOccurrences = 10).predict(reservoir(500.0), now, listOf(daily("s", eight, 1.0)))
        assertEquals(PredictionStatus.UNKNOWN_BEYOND_HORIZON, p.status)
    }

    @Test
    fun dstSpringGap_dayIsNotCountedAsADose() {
        val s = daily("s", LocalTime.of(2, 30), 10.0, start = LocalDate.of(2027, 3, 1))
        val p = predictor.predict(ReservoirState(ml(500.0), ml(30.0), at(2027, 3, 26, 12)), at(2027, 3, 26, 12), listOf(s))
        assertEquals(3, p.coveredDoses) // 27, 29, 30 (28 does not exist)
        assertEquals(LocalDate.of(2027, 3, 31), p.firstUncovered!!.localDate)
    }

    // ---- Prediction must agree with an actual simulation run ----

    @Test
    fun prediction_matchesSimulationEngine_forRandomScheduleSets() {
        val rnd = Random(20261115)
        val engine = SimulationEngine(limits = SimulationLimits(maxSpan = Duration.ofDays(366), maxOccurrences = 50_000))
        repeat(150) { iteration ->
            val schedules = (0 until rnd.nextInt(1, 4)).map { i ->
                val start = LocalDate.of(2026, 11, 1).plusDays(rnd.nextLong(0, 30))
                val time = LocalTime.of(rnd.nextInt(0, 24), listOf(0, 30)[rnd.nextInt(2)])
                val amount = rnd.nextInt(1, 40) / 2.0
                val rec = if (rnd.nextBoolean()) Recurrence.Daily(rnd.nextInt(1, 5))
                else Recurrence.Weekly(DayOfWeek.entries.filter { rnd.nextBoolean() }.toSet().ifEmpty { setOf(DayOfWeek.SUNDAY) }, rnd.nextInt(1, 3))
                Schedule("s$i", 1, "s$i", rnd.nextInt(10) != 0, amount, start, time, rec)
            }
            val remaining = ml(rnd.nextInt(0, 400).toDouble())
            val prediction = predictor.predict(ReservoirState(ml(500.0), remaining, now), now, schedules)

            val sim = engine.advanceTo(
                engine.start("r", now, ml(500.0), remaining).ok().state,
                now.plus(predictor.horizon),
                schedules,
            ).ok()
            val successes = sim.events.count { it.type == HistoryEventType.DISPENSE_SUCCESS }
            val firstBlocked: Instant? = sim.events.firstOrNull { it.type == HistoryEventType.DISPENSE_BLOCKED_INSUFFICIENT_STOCK }?.time

            val ctx = "iteration=$iteration schedules=$schedules remaining=$remaining"
            if (prediction.status != PredictionStatus.NO_FUTURE_DOSES) assertEquals(ctx, successes, prediction.coveredDoses)
            assertEquals(ctx, firstBlocked, prediction.depletionAt)
            if (firstBlocked == null) assertTrue(ctx, prediction.status != PredictionStatus.DEPLETES_WITHIN_HORIZON)
        }
    }
}
