package cz.nowass.dispenser.data.onboarding

import android.content.Intent
import cz.nowass.dispenser.crypto.NativeNoiseProvider
import cz.nowass.dispenser.crypto.SetupLabel
import cz.nowass.dispenser.crypto.TestNative
import cz.nowass.dispenser.crypto.credentials.CredentialState
import cz.nowass.dispenser.crypto.credentials.CredentialStore
import cz.nowass.dispenser.crypto.credentials.FakeKeyWrapper
import cz.nowass.dispenser.data.device.InMemoryPendingOpsJournal
import cz.nowass.dispenser.data.device.PendingKind
import cz.nowass.dispenser.data.device.PendingOp
import cz.nowass.dispenser.data.mode.AppModeController
import cz.nowass.dispenser.data.mode.InMemoryAppModeStore
import cz.nowass.dispenser.data.mode.ModeSwitchRefusal
import cz.nowass.dispenser.data.mode.ResolvedMode
import cz.nowass.dispenser.device.fwsim.FirmwareGattPortFactory
import cz.nowass.dispenser.device.fwsim.FirmwareSim
import cz.nowass.dispenser.device.session.DeviceSessionManager
import cz.nowass.dispenser.device.session.SecureSession
import cz.nowass.dispenser.device.transport.AssociationStart
import cz.nowass.dispenser.device.transport.BluetoothReadiness
import cz.nowass.dispenser.device.transport.DeviceAssociator
import cz.nowass.dispenser.device.transport.GattTransportFactory
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.filterNotNull
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

/**
 * M1.10e.4b onboarding against the REAL firmware logic (libdsp_fwsim): label entry, association
 * (scripted CDM result), CLAIM, wrong code, another phone, a lost ENROLL_RESULT confirmed after a
 * process restart or explicitly discarded, RECOVERY with the three presses, the switch to DEVICE mode,
 * and "Forget device" (local only: the device keeps the slot).
 */
class OnboardingFirmwareTest {
    @get:Rule val tmp = TemporaryFolder()

    private val address = "A0:B1:C2:D3:E4:F5"
    private val deviceId = "a0b1c2d3e4f5"
    private val setup = ByteArray(16) { (0x30 + it).toByte() }
    private lateinit var noise: NativeNoiseProvider
    private lateinit var scope: CoroutineScope
    private val ports = FirmwareGattPortFactory()

    /** The CDM dialog's result, scripted: the address the user "picked". */
    private class FakeAssociator(var picked: String?) : DeviceAssociator {
        val removed = mutableListOf<String>()
        override suspend fun start(): AssociationStart = AssociationStart.Unsupported
        override fun addressFrom(result: Intent?): String? = picked
        override fun disassociate(address: String) { removed += address }
    }

    @Before
    fun setUp() {
        noise = TestNative.load()
        FirmwareSim.load()
        assertTrue(FirmwareSim.reset(hexBytes(deviceId), setup))
        scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    }

    @After
    fun tearDown() {
        if (FirmwareSim.wakeRunning()) endWindow()
        val d = FirmwareSim.diagnostics()
        assertEquals("DSP_CHECK failures inside the firmware world", 0, d[0])
        assertEquals("security layer idle and wiped after the wake", 1, d[2])
        scope.cancel()
    }

    private fun hexBytes(h: String) = ByteArray(h.length / 2) { h.substring(2 * it, 2 * it + 2).toInt(16).toByte() }
    private fun wake(recoveryHold: Boolean = false) {
        assertTrue(FirmwareSim.buttonWake(recoveryHold))
        assertTrue(FirmwareSim.waitAdvertising(5_000))
    }
    private fun endWindow() {
        FirmwareSim.advance(24L * 3600 * 1000)
        assertTrue(FirmwareSim.joinWake(20_000))
    }

    private fun labelText(code: ByteArray = setup) = SetupLabel.format(deviceId, code)

    /** One phone (process): credential store, session manager, mode and the onboarding controller. */
    private inner class Phone(val dir: File = tmp.newFolder(), val wrapper: FakeKeyWrapper = FakeKeyWrapper(), val modeStore: InMemoryAppModeStore = InMemoryAppModeStore()) {
        val store = CredentialStore(dir, wrapper, noise)
        val manager = DeviceSessionManager(GattTransportFactory(ports, { BluetoothReadiness.Ready }, scope), noise, store, scope)
        val modes = AppModeController(modeStore, { CredentialStore.hasConfirmedRecord(dir, it) }, scope).also { it.start() }
        val associator = FakeAssociator(address)
        var after = 0
        fun controller() = OnboardingController({ BluetoothReadiness.Ready }, associator, manager, store, modeStore, modes, afterEnrollment = { after++ })
    }

    private suspend fun Phone.claim(c: OnboardingController = controller()): EnrollOutcome {
        assertEquals(LabelEntry.Accepted(deviceId), c.enterLabel(labelText()))
        assertEquals(AssociationOutcome.Associated(address), c.finishAssociation(null))
        return c.enroll(OnboardingPurpose.CLAIM, "Pixel")
    }

    @Test
    fun claim_enrolls_stores_the_address_and_switches_to_device_mode() = runBlocking {
        val p = Phone()
        wake()
        val c = p.controller()
        val r = p.claim(c)
        assertEquals(EnrollOutcome.Enrolled(deviceId, 0), r)
        assertTrue(p.store.load(deviceId) is CredentialState.Present)
        assertEquals(mapOf(deviceId to address), p.modeStore.addresses.first())
        assertEquals("journal resolution / status read requested", 1, p.after)
        // the flow's secret is gone: a second enrollment has nothing to use
        assertEquals(EnrollOutcome.Failed(EnrollOutcome.Reason.INCOMPLETE), c.enroll(OnboardingPurpose.CLAIM, "Pixel"))
        assertEquals(ResolvedMode.Demo, p.modes.mode.filterNotNull().first())
        assertTrue(c.enterDeviceMode(deviceId))
        assertEquals(ResolvedMode.Device(deviceId), p.modes.mode.value)
        p.manager.end()
    }

    @Test
    fun label_entry_is_tolerant_but_checked() = runBlocking {
        val c = Phone().controller()
        // manual entry: lower case, spaces and hyphens in the code part
        val typed = labelText().let { t -> val (pre, id, code) = t.split(':'); "$pre:${id.uppercase()}:${code.lowercase().chunked(4).joinToString("-")}" }
        assertEquals(LabelEntry.Accepted(deviceId), c.enterLabel(typed))
        assertEquals(LabelEntry.Rejected(LabelEntry.Problem.FORMAT), c.enterLabel("XYZ:$deviceId:abc"))
        assertEquals(LabelEntry.Rejected(LabelEntry.Problem.DEVICE_ID), c.enterLabel("DSP2:a0b1:${labelText().split(':')[2]}"))
        assertEquals(LabelEntry.Rejected(LabelEntry.Problem.CODE), c.enterLabel("DSP2:$deviceId:ABC"))
        val code = labelText().split(':')[2]
        val typo = code.substring(0, 5) + (if (code[5] == '2') '3' else '2') + code.substring(6)
        assertEquals(LabelEntry.Rejected(LabelEntry.Problem.CHECK), c.enterLabel("DSP2:$deviceId:$typo"))
    }

    @Test
    fun picking_another_device_is_reported_before_connecting() = runBlocking {
        val p = Phone()
        val c = p.controller()
        c.enterLabel(labelText())
        p.associator.picked = "11:22:33:44:55:66"
        assertEquals(AssociationOutcome.OtherDevice("11:22:33:44:55:66"), c.finishAssociation(null))
        assertTrue(p.modeStore.addresses.first().isEmpty())
        assertEquals(EnrollOutcome.Failed(EnrollOutcome.Reason.INCOMPLETE), c.enroll(OnboardingPurpose.CLAIM, "Pixel"))
        p.associator.picked = null
        assertEquals(AssociationOutcome.Cancelled, c.finishAssociation(null))
    }

    @Test
    fun a_wrong_code_fails_closed_then_the_right_code_works() = runBlocking {
        val p = Phone()
        wake()
        val c = p.controller()
        c.enterLabel(labelText(ByteArray(16) { 0x55 }))
        c.finishAssociation(null)
        // the device rejects message 3 with a plain (unauthenticated) error: the staged key stays (11.7)
        assertEquals(EnrollOutcome.Failed(EnrollOutcome.Reason.HANDSHAKE, staged = true), c.enroll(OnboardingPurpose.CLAIM, "Pixel"))
        assertEquals(CredentialState.Absent, p.store.load(deviceId))
        assertTrue(p.store.loadPending(deviceId) is CredentialState.Present)
        // no new enrollment over it: confirm (fails: nothing was stored) or discard explicitly first
        assertTrue(FirmwareSim.waitAdvertising(5_000))
        assertEquals(EnrollOutcome.Failed(EnrollOutcome.Reason.STAGED_PENDING, staged = true), p.claim(c))
        c.selectStaged(deviceId)
        c.useAddress(address)
        assertTrue(FirmwareSim.waitAdvertising(5_000))
        val confirm = c.confirmStaged()
        // the device did not store it (still unclaimed): reported, the staged key still kept until discarded
        assertEquals(EnrollOutcome.Failed(EnrollOutcome.Reason.NOTHING_STAGED, staged = true), confirm)
        c.discardStaged()
        assertTrue(FirmwareSim.waitAdvertising(5_000))
        assertEquals(EnrollOutcome.Enrolled(deviceId, 0), p.claim(c))
        p.manager.end()
    }

    @Test
    fun a_claimed_device_cannot_be_claimed_by_another_phone() = runBlocking {
        val a = Phone()
        wake()
        assertTrue(a.claim() is EnrollOutcome.Enrolled)
        a.manager.end()
        assertTrue(FirmwareSim.joinWake(20_000))
        wake()
        val b = Phone()
        assertEquals(EnrollOutcome.Failed(EnrollOutcome.Reason.ALREADY_CLAIMED), b.claim())
        assertEquals(CredentialState.Absent, b.store.load(deviceId))
    }

    @Test
    fun lost_enroll_result_is_confirmed_after_a_process_restart() = runBlocking {
        val first = Phone()
        wake()
        ports.dropFrameType = SecureSession.T_ENROLL_RESULT
        ports.disconnectAfterDrop = true
        assertEquals(EnrollOutcome.Unconfirmed(deviceId), first.claim())
        assertEquals(listOf(deviceId), first.controller().stagedDevices())
        assertEquals("never shown as enrolled", ModeSwitchRefusal.NO_CONFIRMED_ENROLLMENT, first.modes.enterDevice(deviceId))
        // process restart: same files, new everything else; the address is still known
        val again = Phone(first.dir, first.wrapper, first.modeStore)
        val c = again.controller()
        val staged = c.stagedDevices().single()
        c.selectStaged(staged)
        c.useAddress(assertNotNullAnd(c.knownAddress()))
        assertTrue(FirmwareSim.waitAdvertising(5_000))
        assertEquals(EnrollOutcome.Enrolled(deviceId, 0), c.confirmStaged())
        assertTrue(c.stagedDevices().isEmpty())
        assertTrue(c.enterDeviceMode(deviceId))
        again.manager.end()
    }

    @Test
    fun a_staged_enrollment_is_discarded_only_explicitly() = runBlocking {
        val p = Phone()
        wake()
        ports.dropFrameType = SecureSession.T_ENROLL_RESULT
        ports.disconnectAfterDrop = true
        val c = p.controller()
        assertEquals(EnrollOutcome.Unconfirmed(deviceId), p.claim(c))
        assertTrue(p.store.loadPending(deviceId) is CredentialState.Present)
        c.discardStaged()
        assertEquals(CredentialState.Absent, p.store.loadPending(deviceId))
        assertTrue(c.stagedDevices().isEmpty())
    }

    @Test
    fun recovery_needs_the_boot_hold_and_three_presses() = runBlocking {
        val a = Phone()
        wake()
        assertTrue(a.claim() is EnrollOutcome.Enrolled)
        a.manager.end()
        assertTrue(FirmwareSim.joinWake(20_000))

        // without the boot hold the device refuses
        val b = Phone()
        wake()
        val c = b.controller()
        c.enterLabel(labelText())
        c.finishAssociation(null)
        assertEquals(EnrollOutcome.Failed(EnrollOutcome.Reason.RECOVERY_NOT_ARMED), c.enroll(OnboardingPurpose.RECOVERY, "Nový"))
        endWindow()

        // armed: the UI phase asks for the presses, then the phone owns the device alone
        wake(recoveryHold = true)
        val r = async { c.enroll(OnboardingPurpose.RECOVERY, "Nový") }
        assertEquals(EnrollPhase.ConfirmOnDevice::class, withTimeout(15_000) { c.phase.filterNotNull().first { it is EnrollPhase.ConfirmOnDevice } }::class)
        FirmwareSim.press3()
        assertEquals(EnrollOutcome.Enrolled(deviceId, 0), r.await())
        b.manager.end()
        assertTrue(FirmwareSim.joinWake(20_000))
        val own = FirmwareSim.ownership()
        assertNotNull(own.slots[0])
        assertTrue(own.slots.drop(1).all { it == null })
    }

    @Test
    fun forgetting_is_local_and_keeps_unresolved_operations() = runBlocking {
        val p = Phone()
        wake()
        val c = p.controller()
        assertTrue(p.claim(c) is EnrollOutcome.Enrolled)
        assertTrue(c.enterDeviceMode(deviceId))
        val journal = InMemoryPendingOpsJournal()
        journal.insert(PendingOp(deviceId = deviceId, kind = PendingKind.MANUAL_DOSE, createdAtMs = 1, seq = 9, volumeUl = 1000))

        ForgetDevice(p.store, p.modeStore, p.associator, p.modes, endSession = { p.manager.end() })(deviceId)

        assertEquals(CredentialState.Absent, p.store.load(deviceId))
        assertEquals(CredentialState.Absent, p.store.loadPending(deviceId))
        assertTrue(p.modeStore.addresses.first().isEmpty())
        assertEquals(listOf(address), p.associator.removed)
        assertEquals(ResolvedMode.Demo, p.modes.mode.value)
        assertNull(p.modes.selectedDevice.first())
        assertEquals(ModeSwitchRefusal.NO_CONFIRMED_ENROLLMENT, p.modes.enterDevice(deviceId))
        assertEquals("unresolved operations are not dropped", 1, journal.list(deviceId).size)
        // local only: the device still holds this phone's slot until another phone or a recovery removes it
        assertTrue(FirmwareSim.joinWake(20_000))
        assertNotNull(FirmwareSim.ownership().slots[0])
        assertFalse(p.wrapper.keys.isNotEmpty())
    }

    private fun <T> assertNotNullAnd(v: T?): T = checkNotNull(v)
}
