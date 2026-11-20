package cz.nowass.dispenser.data.onboarding

import cz.nowass.dispenser.crypto.NativeNoiseProvider
import cz.nowass.dispenser.crypto.SetupLabel
import cz.nowass.dispenser.crypto.TestNative
import cz.nowass.dispenser.crypto.credentials.CredentialStore
import cz.nowass.dispenser.crypto.credentials.FakeKeyWrapper
import cz.nowass.dispenser.data.device.ConnectOutcome
import cz.nowass.dispenser.data.device.CredentialSeqAllocator
import cz.nowass.dispenser.data.device.DeviceDispenserRepository
import cz.nowass.dispenser.data.device.InMemoryDeviceCacheStore
import cz.nowass.dispenser.data.device.InMemoryPendingOpsJournal
import cz.nowass.dispenser.data.device.UnavailablePhoneTimeSource
import cz.nowass.dispenser.device.fwsim.FirmwareGattPortFactory
import cz.nowass.dispenser.device.fwsim.FirmwareSim
import cz.nowass.dispenser.device.ops.ManagerSessionPort
import cz.nowass.dispenser.device.session.DeviceSessionManager
import cz.nowass.dispenser.device.session.OpenRequest
import cz.nowass.dispenser.device.session.OpenResult
import cz.nowass.dispenser.device.session.SessionState
import cz.nowass.dispenser.device.transport.BluetoothReadiness
import cz.nowass.dispenser.device.transport.GattTransportFactory
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.runBlocking
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/**
 * The phone list's connection lifecycle against the REAL firmware logic (first hardware CLAIM,
 * 2026-11-20: leaving the list sent an encrypted END_SESSION, which ended the device's service
 * window, so every further connection needed a button press). Leaving the screen now only drops
 * the link; an explicit END_SESSION keeps its meaning (decision C10).
 */
class PhonesControllerFirmwareTest {
    @get:Rule val tmp = TemporaryFolder()
    private val address = "A0:B1:C2:D3:E4:F5"
    private val deviceId = "a0b1c2d3e4f5"
    private val setup = ByteArray(16) { (0x30 + it).toByte() }
    private lateinit var noise: NativeNoiseProvider
    private lateinit var scope: CoroutineScope
    private val ports = FirmwareGattPortFactory()

    @Before
    fun setUp() {
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
        val d = FirmwareSim.diagnostics()
        assertEquals("DSP_CHECK failures inside the firmware world", 0, d[0])
        assertEquals("security layer idle and wiped after the wake", 1, d[2])
        scope.cancel()
    }

    private val store by lazy { CredentialStore(tmp.newFolder("A"), FakeKeyWrapper(), noise) }
    private val manager by lazy { DeviceSessionManager(GattTransportFactory(ports, { BluetoothReadiness.Ready }, scope), noise, store, scope) }
    private val repo by lazy {
        DeviceDispenserRepository(deviceId, { address }, ManagerSessionPort(manager), InMemoryPendingOpsJournal(),
            InMemoryDeviceCacheStore(), CredentialSeqAllocator(store), UnavailablePhoneTimeSource, scope)
    }

    /** CLAIM in a button window, then the link is dropped (the window continues). */
    private suspend fun claimed() {
        assertTrue(FirmwareSim.buttonWake(false))
        assertTrue(FirmwareSim.waitAdvertising(5_000))
        val r = (SetupLabel.parse(SetupLabel.format(deviceId, setup)) as SetupLabel.Result.Ok).label
            .use { manager.open(address, OpenRequest.Claim(it, "SM-S911B")) }
        assertTrue("claim: $r", r is OpenResult.Opened)
        manager.closeNow(deviceId)
        assertTrue(FirmwareSim.waitAdvertising(5_000))
    }

    private fun generation(): Long = (manager.state.value as SessionState.Authenticated).generation

    @Test
    fun leaving_the_phone_list_keeps_the_window_and_the_list_reopens_with_a_new_ik_session() = runBlocking {
        claimed()
        val first = PhonesController(repo)
        val listed = (first.load() as PhonesOutcome.Listed).phones
        assertEquals(listOf(0), listed.map { it.slot })
        assertTrue(listed.single().isThisPhone)
        val g1 = generation()
        // the screen stays open for 60 s (device time): the authenticated session is not dropped
        FirmwareSim.advance(60_000)
        delay(500)
        assertTrue("authenticated session alive after 60 s", manager.hasLiveSession(deviceId))
        assertEquals(g1, generation())

        first.finish() // leaving the screen
        assertFalse(manager.hasLiveSession(deviceId))
        assertTrue("the device advertises again (no END_SESSION)", FirmwareSim.waitAdvertising(5_000))
        assertTrue("the service window continues", FirmwareSim.wakeRunning())

        // reopened without a button press: a new IK SESSION
        val second = PhonesController(repo)
        assertEquals(listOf(0), (second.load() as PhonesOutcome.Listed).phones.map { it.slot })
        assertNotEquals(g1, generation())
        second.finish()
        assertTrue(FirmwareSim.waitAdvertising(5_000))
        assertTrue(FirmwareSim.wakeRunning())
    }

    @Test
    fun explicit_end_session_still_ends_the_service_window() = runBlocking {
        claimed()
        assertTrue(repo.connect() is ConnectOutcome.Connected)
        repo.disconnect(graceful = true) // the explicit "Done": encrypted END_SESSION
        assertFalse(manager.hasLiveSession(deviceId))
        assertTrue("the window ends without any time passing", FirmwareSim.joinWake(20_000))
        assertFalse(FirmwareSim.wakeRunning())
    }
}
