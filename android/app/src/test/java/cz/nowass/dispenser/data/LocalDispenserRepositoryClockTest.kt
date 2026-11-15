package cz.nowass.dispenser.data

import cz.nowass.dispenser.domain.DEFAULT_ZONE
import cz.nowass.dispenser.domain.DataSource
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.take
import kotlinx.coroutines.flow.toList
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.advanceTimeBy
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.time.Clock
import java.time.Instant
import java.time.ZoneId
import kotlin.time.Duration.Companion.minutes

@OptIn(ExperimentalCoroutinesApi::class)
class LocalDispenserRepositoryClockTest {

    /** Settable clock for deterministic tick tests. */
    private class MutableClock(var now: Instant) : Clock() {
        override fun getZone(): ZoneId = DEFAULT_ZONE
        override fun withZone(zone: ZoneId?): Clock = this
        override fun instant(): Instant = now
    }

    @Test
    fun simulatedClock_isUsedExactly() = runTest {
        val fixed = Instant.parse("2031-02-03T04:05:06Z")
        val r = LocalDispenserRepository(InMemoryScheduleStore(), Clock.fixed(fixed, DEFAULT_ZONE), clockTick = null)
        val status = r.observeDevice().first()
        assertEquals(fixed, status.now)
        assertEquals(DataSource.DEMO, status.source) // device values remain simulated
    }

    @Test
    fun realClock_reportsCurrentPhoneTime() = runTest {
        val r = LocalDispenserRepository(InMemoryScheduleStore(), Clock.system(DEFAULT_ZONE), clockTick = null)
        val before = Instant.now()
        val now = r.observeDevice().first().now
        val after = Instant.now()
        assertTrue("$now not within [$before, $after]", !now.isBefore(before) && !now.isAfter(after))
    }

    @Test
    fun ticking_reEmitsOnEachMinuteBoundary() = runTest {
        val clock = MutableClock(Instant.parse("2026-11-15T10:00:00Z"))
        val r = LocalDispenserRepository(InMemoryScheduleStore(), clock, clockTick = 1.minutes)
        val seen = mutableListOf<Instant>()
        val job = launch { r.observeDevice().take(3).toList().forEach { seen += it.now } }
        runCurrent()
        clock.now = Instant.parse("2026-11-15T10:01:00Z"); advanceTimeBy(60_000); runCurrent()
        clock.now = Instant.parse("2026-11-15T10:02:00Z"); advanceTimeBy(60_000); runCurrent()
        job.join()
        assertEquals(
            listOf("2026-11-15T10:00:00Z", "2026-11-15T10:01:00Z", "2026-11-15T10:02:00Z").map(Instant::parse),
            seen,
        )
    }

    @Test
    fun fixtureRepository_usesFixtureClock_andAppDefaultsDoNot() = runTest {
        // Fixtures pair their fixed clock with fixture schedules only.
        assertEquals(MockSeed.START, MockDispenserRepository().observeDevice().first().now)
        assertEquals(3, MockDispenserRepository().observeSchedules().first().size)
    }
}
