package cz.nowass.dispenser.ui.devices

import cz.nowass.dispenser.data.InMemoryAppSettingsRepository
import cz.nowass.dispenser.data.device.ConnectOutcome
import cz.nowass.dispenser.data.device.DeviceRepository
import cz.nowass.dispenser.data.device.FakeDeviceRepository
import cz.nowass.dispenser.data.device.FirmwareStatusOutcome
import cz.nowass.dispenser.data.device.FirmwareUpdateController
import cz.nowass.dispenser.data.device.StartUpdateOutcome
import cz.nowass.dispenser.data.update.FirmwareUpdateChecker
import cz.nowass.dispenser.device.ops.LocalRefusal
import cz.nowass.dispenser.device.ops.OpResult
import cz.nowass.dispenser.device.ops.UpdateResult
import cz.nowass.dispenser.device.ops.UpdateStatusRecord
import cz.nowass.dispenser.device.ops.UpdateVerdict
import cz.nowass.dispenser.device.ops.UpdateVerdictRecord
import cz.nowass.dispenser.device.session.SessionState
import cz.nowass.dispenser.domain.update.FirmwareCheckResult
import cz.nowass.dispenser.domain.update.FirmwareOffer
import cz.nowass.dispenser.domain.update.UpdateCheckResult
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.test.UnconfinedTestDispatcher
import kotlinx.coroutines.test.resetMain
import kotlinx.coroutines.test.runTest
import kotlinx.coroutines.test.setMain
import org.junit.After
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

/** The firmware screen's logic: status, check, beta channel, confirmation, START_UPDATE and its verdicts. */
@OptIn(ExperimentalCoroutinesApi::class)
class FirmwareViewModelTest {
    private val sha = "0f".repeat(32)
    private val offer = FirmwareOffer("0.3.1", "firmware-v0.3.1", 1_430_528, sha, "Opravy.", false, null)
    private val fake = FakeDeviceRepository()
    private val settings = InMemoryAppSettingsRepository()
    private val checks = mutableListOf<Pair<String, Boolean>>()
    private var checkResult: FirmwareCheckResult = FirmwareCheckResult.Available(offer)
    private val checker = FirmwareUpdateChecker { running, beta -> checks += running to beta; checkResult }

    @Before fun setUp() = Dispatchers.setMain(UnconfinedTestDispatcher())
    @After fun tearDown() = Dispatchers.resetMain()

    private fun status(running: String = "0.3.0", capable: Boolean = true, last: Int = 0, lastVersion: String = "") =
        UpdateStatusRecord(running, capable, last, lastVersion)

    private fun connected(record: UpdateStatusRecord = status()) {
        fake.authenticated()
        fake.connectOutcome = ConnectOutcome.Connected(7, 0)
        fake.updateStatusOutcome = OpResult.Ok(record)
    }

    private fun vm() = FirmwareViewModel(FirmwareUpdateController(fake, checker, settings))

    @Test
    fun opening_reads_the_installed_version_and_the_last_result() = runTest {
        connected(status(last = 2, lastVersion = "0.3.1"))
        val vm = vm()
        val s = vm.state.value
        assertFalse(s.loading)
        assertEquals("0.3.0", s.record?.runningVersion)
        assertEquals(UpdateResult.ROLLED_BACK, s.record?.lastResult)
        assertTrue(s.connected)
        assertTrue(s.canCheck)
        assertFalse(s.canUpdate) // nothing offered yet
        assertTrue(checks.isEmpty()) // never checked without the owner asking
        // an unknown last_result is a failure, never "installed"
        assertEquals(UpdateResult.UNKNOWN, status(last = 99).lastResult)
    }

    @Test
    fun old_firmware_is_reported_as_not_updatable_over_the_air() = runTest {
        connected()
        fake.updateStatusOutcome = OpResult.Refused(LocalRefusal.Unavailable(DeviceRepository.UPDATE_UNSUPPORTED))
        val vm = vm()
        assertEquals(FirmwareStatusOutcome.Unsupported, vm.state.value.status)
        assertNull(vm.state.value.record)
        assertFalse(vm.state.value.canCheck)
        vm.check()
        assertTrue(checks.isEmpty())
    }

    @Test
    fun not_connected_offers_nothing() = runTest {
        val vm = vm() // connect fails (no address)
        assertEquals(FirmwareStatusOutcome.NotConnected, vm.state.value.status)
        assertFalse(vm.state.value.canCheck)
        assertEquals(0, fake.updateStatusCalls)
    }

    @Test
    fun check_uses_the_running_version_and_the_beta_channel() = runTest {
        connected(status(running = "0.3.0-dev"))
        val vm = vm()
        vm.check()
        assertEquals(listOf("0.3.0-dev" to false), checks)
        assertEquals(offer, vm.state.value.offer)
        assertTrue(vm.state.value.canUpdate)
        // switching the channel persists it and drops the previous result
        vm.setBeta(true)
        assertTrue(settings.betaFirmwareChannel.first())
        assertNull(vm.state.value.check)
        vm.check()
        assertEquals("0.3.0-dev" to true, checks.last())
        // and the network failure is reported as such
        checkResult = FirmwareCheckResult.Failed(UpdateCheckResult.Offline)
        vm.check()
        assertEquals(FirmwareCheckResult.Failed(UpdateCheckResult.Offline), vm.state.value.check)
        assertFalse(vm.state.value.canUpdate)
    }

    @Test
    fun beta_channel_is_off_by_default_and_restored() = runTest {
        connected()
        assertFalse(vm().state.value.beta)
        settings.setBetaFirmwareChannel(true)
        assertTrue(vm().state.value.beta)
    }

    @Test
    fun update_needs_the_explicit_confirmation() = runTest {
        connected()
        fake.startUpdateOutcome = OpResult.Ok(UpdateVerdictRecord(0))
        val vm = vm()
        vm.check()
        vm.requestUpdate()
        assertTrue(vm.state.value.confirming)
        assertTrue(fake.startUpdateRequests.isEmpty())
        vm.cancelUpdate()
        assertFalse(vm.state.value.confirming)
        assertTrue(fake.startUpdateRequests.isEmpty())
        vm.requestUpdate()
        vm.confirmUpdate()
        val sent = fake.startUpdateRequests.single()
        assertEquals("0.3.1", sent.version)
        assertEquals("firmware-v0.3.1", sent.tag)
        assertEquals(1_430_528L, sent.size)
        assertArrayEquals(ByteArray(32) { 0x0f }, sent.sha256)
        assertTrue(vm.state.value.accepted)
        // accepted: the device ends the session (expected); nothing more is offered until a new status
        fake.connection.value = SessionState.Idle
        assertFalse(vm.state.value.canUpdate)
        assertFalse(vm.state.value.canCheck)
        vm.requestUpdate()
        assertFalse(vm.state.value.confirming)
    }

    @Test
    fun reconnect_after_an_accepted_update_shows_the_result() = runTest {
        connected()
        fake.startUpdateOutcome = OpResult.Ok(UpdateVerdictRecord(0))
        val vm = vm()
        vm.check()
        vm.requestUpdate()
        vm.confirmUpdate()
        fake.connection.value = SessionState.Idle
        fake.updateStatusOutcome = OpResult.Ok(status(running = "0.3.1", last = 1, lastVersion = "0.3.1"))
        fake.authenticated(8)
        val s = vm.state.value
        assertEquals("0.3.1", s.record?.runningVersion)
        assertEquals(UpdateResult.INSTALLED, s.record?.lastResult)
        assertNull(s.start)
        assertNull(s.check)
        assertTrue(s.canCheck)
    }

    @Test
    fun every_verdict_is_kept_for_display_and_refusals_allow_a_new_attempt() = runTest {
        connected()
        val vm = vm()
        vm.check()
        for (wire in listOf(1, 2, 3, 4, 5, 6, 7, 42)) {
            fake.startUpdateOutcome = OpResult.Ok(UpdateVerdictRecord(wire))
            vm.requestUpdate()
            vm.confirmUpdate()
            val v = (vm.state.value.start as StartUpdateOutcome.Answered).verdict
            assertEquals(wire, v.wire)
            assertEquals(if (wire == 42) UpdateVerdict.UNKNOWN else UpdateVerdict.entries.first { it.wire == wire }, v.verdict)
            assertFalse(vm.state.value.accepted)
            assertTrue("refused ($wire): another attempt is possible", vm.state.value.canUpdate)
        }
        assertEquals(8, fake.startUpdateRequests.size)
    }

    @Test
    fun failures_of_the_request_are_mapped() = runTest {
        connected()
        val vm = vm()
        vm.check()
        val cases = listOf(
            OpResult.Uncertain("lost") to StartUpdateOutcome.Uncertain,
            OpResult.NotSent("not connected") to StartUpdateOutcome.NotConnected,
            OpResult.Refused(LocalRefusal.Unavailable(DeviceRepository.UPDATE_UNSUPPORTED)) to StartUpdateOutcome.Unsupported,
            OpResult.Refused(LocalRefusal.InProgress) to StartUpdateOutcome.NotSent,
        )
        for ((r, want) in cases) {
            fake.startUpdateOutcome = r
            vm.requestUpdate()
            vm.confirmUpdate()
            assertEquals(want, vm.state.value.start)
        }
    }

    @Test
    fun disabled_while_disconnected_not_capable_or_in_flight() = runTest {
        connected(status(capable = false))
        val vm = vm()
        vm.check()
        assertFalse("not capable", vm.state.value.canUpdate)

        connected()
        val vm2 = vm()
        vm2.check()
        fake.connection.value = SessionState.Idle
        assertFalse("disconnected", vm2.state.value.canUpdate)
        fake.authenticated()
        assertTrue(vm2.state.value.canUpdate)

        val gate = CompletableDeferred<Unit>()
        fake.startUpdateHook = { gate.await() }
        fake.startUpdateOutcome = OpResult.Ok(UpdateVerdictRecord(4))
        vm2.requestUpdate()
        vm2.confirmUpdate()
        assertTrue(vm2.state.value.starting)
        assertFalse(vm2.state.value.canUpdate)
        assertFalse(vm2.state.value.canCheck)
        vm2.requestUpdate()
        vm2.confirmUpdate() // ignored while one is in flight
        gate.complete(Unit)
        assertEquals(1, fake.startUpdateRequests.size)
        assertFalse(vm2.state.value.starting)
    }
}
