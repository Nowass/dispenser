package cz.nowass.dispenser.device.session

import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.advanceTimeBy
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** The background grace timer in virtual time (4.4): start, cancel, expiry once, no stale timers. */
@OptIn(ExperimentalCoroutinesApi::class)
class BackgroundGraceTest {
    private var expiries = 0
    private fun TestScope.grace(ms: Long = 30_000) = BackgroundGrace(backgroundScope, ms) { expiries++ }

    @Test
    fun returning_before_the_timeout_cancels_it() = runTest {
        val g = grace()
        g.activityStarted()
        g.activityStopped() // background: timer starts
        assertTrue(g.inBackground)
        advanceTimeBy(29_000); runCurrent()
        g.activityStarted() // back before 30 s
        advanceTimeBy(60_000); runCurrent()
        assertEquals(0, expiries)
        assertFalse(g.expired)
    }

    @Test
    fun staying_in_the_background_expires_once() = runTest {
        val g = grace()
        g.activityStarted()
        g.activityStopped()
        advanceTimeBy(29_999); runCurrent()
        assertEquals(0, expiries)
        advanceTimeBy(2); runCurrent()
        assertEquals(1, expiries)
        assertTrue(g.expired)
        advanceTimeBy(600_000); runCurrent()
        assertEquals("never repeated", 1, expiries)
        g.activityStarted()
        assertFalse("cleared when the app returns", g.expired)
    }

    @Test
    fun repeated_toggles_never_fire_a_stale_timer() = runTest {
        val g = grace()
        g.activityStarted()
        repeat(20) {
            g.activityStopped()
            advanceTimeBy(20_000); runCurrent()
            g.activityStarted()
            advanceTimeBy(20_000); runCurrent()
        }
        assertEquals(0, expiries)
        g.activityStopped() // only the last background period counts, measured from its own start
        advanceTimeBy(29_000); runCurrent()
        assertEquals(0, expiries)
        advanceTimeBy(1_001); runCurrent()
        assertEquals(1, expiries)
    }

    @Test
    fun two_activities_background_only_when_both_stopped() = runTest {
        val g = grace()
        g.activityStarted()
        g.activityStarted() // e.g. the system association dialog over the app
        g.activityStopped()
        advanceTimeBy(60_000); runCurrent()
        assertEquals(0, expiries)
        g.activityStopped()
        advanceTimeBy(30_001); runCurrent()
        assertEquals(1, expiries)
        g.activityStopped() // unbalanced: ignored
        advanceTimeBy(60_000); runCurrent()
        assertEquals(1, expiries)
    }

    @Test
    fun the_grace_is_configurable() = runTest {
        val g = grace(ms = 5_000)
        g.activityStarted()
        g.activityStopped()
        advanceTimeBy(5_001); runCurrent()
        assertEquals(1, expiries)
        assertEquals(30_000L, BackgroundGrace.DEFAULT_GRACE_MS)
    }
}
