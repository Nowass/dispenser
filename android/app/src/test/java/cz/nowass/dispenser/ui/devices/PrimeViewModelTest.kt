package cz.nowass.dispenser.ui.devices

import cz.nowass.dispenser.data.device.DeviceRepository
import cz.nowass.dispenser.data.device.FakeDeviceRepository
import cz.nowass.dispenser.data.device.PrimeController
import cz.nowass.dispenser.data.device.PrimeSupport
import cz.nowass.dispenser.device.ops.LocalRefusal
import cz.nowass.dispenser.device.ops.OpResult
import cz.nowass.dispenser.device.ops.PrimeStatusRecord
import cz.nowass.dispenser.device.ops.PrimeStopReason
import cz.nowass.dispenser.device.ops.PrimeVerdict
import cz.nowass.dispenser.device.ops.UpdateStatusRecord
import cz.nowass.dispenser.device.session.SessionState
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.advanceTimeBy
import kotlinx.coroutines.test.resetMain
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import kotlinx.coroutines.test.setMain
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

/**
 * The priming section's logic: support gate, START/STOP, single flight, the 1 s heartbeat, UNKNOWN
 * (never STOPPED) without a confirmed answer, device-reported stops, STOP when the screen is left.
 */
@OptIn(ExperimentalCoroutinesApi::class)
class PrimeViewModelTest {
    private val main = StandardTestDispatcher()
    private val fake = FakeDeviceRepository()

    @Before fun setUp() = Dispatchers.setMain(main)
    @After fun tearDown() = Dispatchers.resetMain()

    private fun rec(running: Boolean, verdict: Int = 0, elapsed: Long = 0, reason: Int = 0) =
        PrimeStatusRecord(verdict, running, elapsed, 30_000, 3_000, reason)

    /** A connected 0.3.3 device whose pump follows START / STOP. */
    private fun device(version: String = "0.3.3") {
        fake.authenticated()
        fake.updateStatusOutcome = OpResult.Ok(UpdateStatusRecord(version, true, 0, ""))
        var running = false
        var elapsed = 0L
        fake.primeStatusAnswer = { elapsed += 1000; OpResult.Ok(rec(running, elapsed = if (running) elapsed else 0)) }
        fake.primeStartAnswer = { running = true; elapsed = 0; OpResult.Ok(rec(true)) }
        fake.primeStopAnswer = { val was = running; running = false; OpResult.Ok(rec(false, elapsed = elapsed, reason = if (was) 1 else 0)) }
    }

    private fun TestScope.vm(): PrimeViewModel = PrimeViewModel(PrimeController(fake, backgroundScope)).also { runCurrent() }

    private fun statusCalls() = fake.primeCalls.count { it == "status" }

    @Test
    fun supported_firmware_reads_the_state_and_offers_start() = runTest(main) {
        device()
        val vm = vm()
        val s = vm.state.value
        assertEquals(PrimeSupport.Supported, s.support)
        assertEquals(PrimePhase.STOPPED, s.phase)
        assertTrue(s.canStart)
        assertEquals(listOf("status"), fake.primeCalls) // a read, never a START
    }

    @Test
    fun older_dev_or_unknown_firmware_is_not_offered() = runTest(main) {
        for (v in listOf("0.3.2", "0.3.0", "0.3.3-dev", "")) {
            device(v)
            fake.primeCalls.clear()
            val vm = vm()
            assertEquals(v, PrimeSupport.Unsupported, vm.state.value.support)
            assertFalse(vm.state.value.offered)
            vm.toggle()
            runCurrent()
            assertTrue(v, fake.primeCalls.isEmpty())
        }
        // firmware < 0.3.0 (no GET_UPDATE_STATUS at all)
        fake.updateStatusOutcome = OpResult.Refused(LocalRefusal.Unavailable(DeviceRepository.UPDATE_UNSUPPORTED))
        assertEquals(PrimeSupport.Unsupported, vm().state.value.support)
        // not connected: not known, the section stays hidden
        fake.connection.value = SessionState.Idle
        fake.updateStatusOutcome = OpResult.NotSent("not connected")
        assertNull(vm().state.value.support)
        assertTrue(fake.primeCalls.isEmpty())
    }

    @Test
    fun start_runs_heartbeats_every_second_and_stop_ends_them() = runTest(main) {
        device()
        val vm = vm()
        fake.primeCalls.clear()
        vm.toggle()
        runCurrent()
        assertEquals(PrimePhase.RUNNING, vm.state.value.phase)
        assertEquals(listOf("start"), fake.primeCalls)
        advanceTimeBy(999); runCurrent()
        assertEquals(0, statusCalls())
        advanceTimeBy(1); runCurrent()
        assertEquals(1, statusCalls())
        advanceTimeBy(3_000); runCurrent()
        assertEquals(4, statusCalls())
        assertEquals(4_000L, vm.state.value.last?.elapsedMs) // re-synced from each answer
        vm.toggle() // STOP
        runCurrent()
        assertEquals(PrimePhase.STOPPED, vm.state.value.phase)
        assertEquals(PrimeStopReason.USER, vm.state.value.last?.stopReason)
        assertEquals("stop", fake.primeCalls.last())
        advanceTimeBy(10_000); runCurrent()
        assertEquals("no heartbeat while stopped", 4, statusCalls())
    }

    @Test
    fun repeated_taps_send_one_start() = runTest(main) {
        device()
        val gate = CompletableDeferred<Unit>()
        fake.primeStartAnswer = { gate.await(); OpResult.Ok(rec(true)) }
        val vm = vm()
        fake.primeCalls.clear()
        repeat(5) { vm.toggle(); runCurrent() }
        assertEquals(PrimePhase.STARTING, vm.state.value.phase)
        assertEquals(listOf("start"), fake.primeCalls)
        gate.complete(Unit)
        runCurrent()
        assertEquals(PrimePhase.RUNNING, vm.state.value.phase)
        assertEquals(listOf("start"), fake.primeCalls)
    }

    @Test
    fun stop_does_not_wait_for_a_pending_heartbeat_and_a_late_heartbeat_never_overrides_it() = runTest(main) {
        device()
        val vm = vm()
        fake.primeCalls.clear()
        vm.toggle(); runCurrent()
        val hb = CompletableDeferred<Unit>()
        fake.primeStatusAnswer = { hb.await(); OpResult.Ok(rec(true, elapsed = 1000)) }
        advanceTimeBy(1_000); runCurrent() // a heartbeat is on its way, unanswered
        assertEquals(1, statusCalls())
        vm.toggle() // STOP right away
        runCurrent()
        assertEquals("stop", fake.primeCalls.last())
        assertEquals(PrimePhase.STOPPED, vm.state.value.phase)
        hb.complete(Unit) // the late "running" answer is dropped
        runCurrent()
        assertEquals(PrimePhase.STOPPED, vm.state.value.phase)
        advanceTimeBy(5_000); runCurrent()
        assertEquals(1, statusCalls())
    }

    @Test
    fun a_lost_answer_or_disconnect_is_unknown_never_stopped() = runTest(main) {
        device()
        val vm = vm()
        fake.primeCalls.clear()
        vm.toggle(); runCurrent()
        fake.primeStatusAnswer = { OpResult.Uncertain("timeout") }
        advanceTimeBy(1_000); runCurrent()
        assertEquals(PrimePhase.UNKNOWN, vm.state.value.phase)
        assertTrue(vm.state.value.failed)
        assertTrue("the safe action is offered", vm.state.value.canStop)
        advanceTimeBy(5_000); runCurrent()
        assertEquals("no heartbeat when unknown", 1, statusCalls())

        // a failed START is not STOPPED either
        device()
        val vm2 = vm()
        fake.primeStartAnswer = { OpResult.Uncertain("timeout") }
        vm2.toggle(); runCurrent()
        assertEquals(PrimePhase.UNKNOWN, vm2.state.value.phase)

        // the connection drops while running
        device()
        val vm3 = vm()
        vm3.toggle(); runCurrent()
        assertEquals(PrimePhase.RUNNING, vm3.state.value.phase)
        val before = statusCalls()
        fake.connection.value = SessionState.Idle
        runCurrent()
        assertEquals(PrimePhase.UNKNOWN, vm3.state.value.phase)
        assertFalse(vm3.state.value.connected)
        assertFalse(vm3.state.value.canStart)
        advanceTimeBy(5_000); runCurrent()
        assertEquals(before, statusCalls())
    }

    @Test
    fun device_reported_stops_show_the_reason_and_need_a_new_start() = runTest(main) {
        for ((wire, reason) in listOf(2 to PrimeStopReason.MAX_TIME, 3 to PrimeStopReason.HEARTBEAT, 7 to PrimeStopReason.DOSE, 77 to PrimeStopReason.UNKNOWN)) {
            device()
            val vm = vm()
            vm.toggle(); runCurrent()
            fake.primeStatusAnswer = { OpResult.Ok(rec(false, elapsed = 30_000, reason = wire)) }
            advanceTimeBy(1_000); runCurrent()
            assertEquals(PrimePhase.STOPPED, vm.state.value.phase)
            assertEquals(reason, vm.state.value.last?.stopReason)
            val starts = fake.primeCalls.count { it == "start" }
            advanceTimeBy(60_000); runCurrent()
            assertEquals("never restarted automatically", starts, fake.primeCalls.count { it == "start" })
            assertTrue(vm.state.value.canStart)
        }
    }

    @Test
    fun refusals_keep_the_pump_stopped_and_are_reported() = runTest(main) {
        device()
        val vm = vm()
        fake.primeStartAnswer = { OpResult.Ok(rec(false, verdict = 2)) }
        vm.toggle(); runCurrent()
        assertEquals(PrimePhase.STOPPED, vm.state.value.phase)
        assertEquals(PrimeVerdict.BATTERY, vm.state.value.refusal)
        advanceTimeBy(5_000); runCurrent()
        assertEquals(1, statusCalls()) // only the initial read
        fake.primeStartAnswer = { OpResult.Ok(rec(true)) }
        vm.toggle(); runCurrent()
        assertNull(vm.state.value.refusal)
    }

    @Test
    fun leaving_the_screen_while_running_sends_stop() = runTest(main) {
        device()
        val vm = vm()
        vm.toggle(); runCurrent()
        vm.leave()
        runCurrent()
        assertEquals("stop", fake.primeCalls.last())
        assertEquals(PrimePhase.STOPPED, vm.state.value.phase)
        val n = fake.primeCalls.size
        vm.leave() // stopped: nothing more
        advanceTimeBy(5_000); runCurrent()
        assertEquals(n, fake.primeCalls.size)
        // leaving while stopped sends nothing
        device()
        val idle = vm()
        fake.primeCalls.clear()
        idle.leave(); runCurrent()
        assertTrue(fake.primeCalls.isEmpty())
    }
}
