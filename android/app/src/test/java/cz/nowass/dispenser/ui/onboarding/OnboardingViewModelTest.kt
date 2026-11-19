package cz.nowass.dispenser.ui.onboarding

import android.content.Intent
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.ViewModelStore
import cz.nowass.dispenser.crypto.NativeNoiseProvider
import cz.nowass.dispenser.crypto.SetupLabel
import cz.nowass.dispenser.crypto.TestNative
import cz.nowass.dispenser.crypto.credentials.CredentialStore
import cz.nowass.dispenser.crypto.credentials.FakeKeyWrapper
import cz.nowass.dispenser.data.mode.AppModeController
import cz.nowass.dispenser.data.mode.InMemoryAppModeStore
import cz.nowass.dispenser.data.mode.ResolvedMode
import cz.nowass.dispenser.data.onboarding.EnrollOutcome
import cz.nowass.dispenser.data.onboarding.LabelEntry
import cz.nowass.dispenser.data.onboarding.OnboardingController
import cz.nowass.dispenser.data.onboarding.OnboardingPurpose
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
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.asCoroutineDispatcher
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.filterNotNull
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.test.resetMain
import kotlinx.coroutines.test.setMain
import kotlinx.coroutines.withTimeout
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.util.concurrent.Executors

/** The onboarding ViewModel over the real controller, session and firmware (M1.10e.4b). */
@OptIn(ExperimentalCoroutinesApi::class)
class OnboardingViewModelTest {
    @get:Rule val tmp = TemporaryFolder()
    private val address = "A0:B1:C2:D3:E4:F5"
    private val deviceId = "a0b1c2d3e4f5"
    private val setup = ByteArray(16) { (0x30 + it).toByte() }
    private lateinit var noise: NativeNoiseProvider
    private lateinit var scope: CoroutineScope
    private val main = Executors.newSingleThreadExecutor().asCoroutineDispatcher()
    private val ports = FirmwareGattPortFactory()

    private val associator = object : DeviceAssociator {
        override suspend fun start(): AssociationStart = AssociationStart.Unsupported
        override fun addressFrom(result: Intent?): String? = address
        override fun disassociate(address: String) = Unit
    }

    @Before
    fun setUp() {
        Dispatchers.setMain(main)
        noise = TestNative.load()
        FirmwareSim.load()
        assertTrue(FirmwareSim.reset(ByteArray(6) { deviceId.substring(2 * it, 2 * it + 2).toInt(16).toByte() }, setup))
        scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    }

    @After
    fun tearDown() {
        if (FirmwareSim.wakeRunning()) {
            FirmwareSim.advance(24L * 3600 * 1000)
            assertTrue(FirmwareSim.joinWake(20_000))
        }
        scope.cancel()
        Dispatchers.resetMain()
        main.close()
    }

    private val dir by lazy { tmp.newFolder("cred") }
    private val wrapper = FakeKeyWrapper()
    private val modeStore = InMemoryAppModeStore()
    private val store by lazy { CredentialStore(dir, wrapper, noise) }
    private val manager by lazy { DeviceSessionManager(GattTransportFactory(ports, { BluetoothReadiness.Ready }, scope), noise, store, scope) }
    private val modes by lazy { AppModeController(modeStore, { CredentialStore.hasConfirmedRecord(dir, it) }, scope).also { it.start() } }
    private fun controller() = OnboardingController({ BluetoothReadiness.Ready }, associator, manager, store, modeStore, modes)

    private fun wake() {
        assertTrue(FirmwareSim.buttonWake(false))
        assertTrue(FirmwareSim.waitAdvertising(5_000))
    }

    private suspend fun OnboardingViewModel.awaitStep(step: OnboardingStep) = withTimeout(20_000) { state.first { it.step == step } }

    private fun vmIn(store: ViewModelStore, c: OnboardingController = controller()) =
        ViewModelProvider(store, OnboardingViewModel.factory(OnboardingPurpose.CLAIM) { c })[OnboardingViewModel::class.java]

    @Test
    fun the_whole_claim_flow_ends_in_device_mode() = runBlocking {
        wake()
        val vm = vmIn(ViewModelStore())
        vm.start()
        vm.awaitStep(OnboardingStep.LABEL)
        assertTrue(!vm.submitLabel("DSP2:$deviceId:XYZ"))
        assertEquals(LabelEntry.Problem.CODE, vm.state.value.labelProblem)
        assertTrue(vm.submitLabel(SetupLabel.format(deviceId, setup)))
        assertEquals(deviceId, vm.awaitStep(OnboardingStep.ASSOCIATE).deviceId)
        vm.onAssociationResult(null)
        vm.awaitStep(OnboardingStep.NAME)
        vm.enroll("Pixel")
        vm.awaitStep(OnboardingStep.DONE)
        vm.finish()
        assertEquals(ResolvedMode.Device(deviceId), withTimeout(10_000) { modes.mode.filterNotNull().first { it is ResolvedMode.Device } })
        manager.end()
    }

    @Test
    fun a_staged_enrollment_is_shown_first_and_clearing_the_viewmodel_zeroizes_the_label() = runBlocking {
        wake()
        ports.dropFrameType = SecureSession.T_ENROLL_RESULT
        ports.disconnectAfterDrop = true
        val owner = ViewModelStore()
        val c = controller()
        val vm = vmIn(owner, c)
        vm.start()
        vm.awaitStep(OnboardingStep.LABEL)
        vm.submitLabel(SetupLabel.format(deviceId, setup))
        vm.onAssociationResult(null)
        vm.awaitStep(OnboardingStep.NAME)
        vm.enroll("Pixel")
        assertEquals(deviceId, vm.awaitStep(OnboardingStep.UNCONFIRMED).deviceId)

        // a new screen (process restart or re-entry) starts with the staged enrollment
        val again = vmIn(ViewModelStore())
        assertTrue(again.awaitStep(OnboardingStep.UNCONFIRMED).confirmingStaged)

        // the flow's label is zeroized when its ViewModel is cleared
        val c2 = controller()
        val other = ViewModelStore()
        val vm2 = vmIn(other, c2)
        vm2.awaitStep(OnboardingStep.UNCONFIRMED) // init picked the staged device
        assertTrue(vm2.submitLabel(SetupLabel.format(deviceId, setup)))
        other.clear()
        c2.finishAssociation(null)
        assertEquals(EnrollOutcome.Failed(EnrollOutcome.Reason.INCOMPLETE), c2.enroll(OnboardingPurpose.CLAIM, "Pixel"))
        assertNull(modeStore.stored.first().deviceId)
    }
}
