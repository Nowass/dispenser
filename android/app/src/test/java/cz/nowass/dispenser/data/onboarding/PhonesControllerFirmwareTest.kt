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
import cz.nowass.dispenser.data.device.DeviceUiRepository
import cz.nowass.dispenser.device.session.BackgroundGrace
import cz.nowass.dispenser.domain.ConnectionState
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.withTimeout
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
 * The application-wide session lifecycle against the REAL firmware logic. The session belongs to the
 * process-wide DeviceSessionManager: screens (the phone list here) load and observe but never end it;
 * the background grace ends it with a plain disconnect; only an explicit END_SESSION ends the device's
 * service window (decision C10). Frame counts on the simulated link prove what the phone sent:
 * 0x01 HELLO (one per connection), 0x02 handshake messages, 0x7E END_SESSION.
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
    /** What Settings, Overview and Scheduling observe (one shared source: the manager's state). */
    private val ui by lazy { DeviceUiRepository(repo, java.time.Clock.systemUTC(), clockTick = null) }
    private suspend fun uiConnection(): ConnectionState = ui.observeDevice().first().connection

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
    private fun hellos() = ports.sent(0x01)
    private fun handshakes() = ports.sent(0x02)
    private fun endSessions() = ports.sent(0x7E)

    // ---- A. navigation ---------------------------------------------------------------------

    @Test
    fun navigation_keeps_one_authenticated_session_and_the_list_reuses_it() = runBlocking {
        claimed()
        val first = PhonesController(repo) // Settings → Device → Phones
        val listed = (first.load() as PhonesOutcome.Listed).phones
        assertEquals(listOf(0), listed.map { it.slot })
        assertTrue(listed.single().isThisPhone)
        val g1 = generation()
        val hello1 = hellos()
        val hs1 = handshakes()
        assertEquals(ConnectionState.CONNECTED, uiConnection())

        // leave Phones, Settings, Overview, Scheduling for 60 s of device time: nothing ends the session
        FirmwareSim.advance(60_000)
        delay(500)
        assertTrue("authenticated session alive after 60 s", manager.hasLiveSession(deviceId))
        assertEquals(ConnectionState.CONNECTED, uiConnection())

        // back to Phones: the same session, no new connection, no handshake
        val second = PhonesController(repo)
        assertEquals(listOf(0), (second.load() as PhonesOutcome.Listed).phones.map { it.slot })
        assertEquals(g1, generation())
        assertEquals("no new connection (HELLO)", hello1, hellos())
        assertEquals("no new handshake", hs1, handshakes())
        assertEquals("no END_SESSION", 0, endSessions())
        assertTrue(FirmwareSim.wakeRunning())
    }

    // ---- B. background grace ---------------------------------------------------------------

    @Test
    fun background_grace_expiry_disconnects_without_end_session_and_the_window_continues() = runBlocking {
        claimed()
        assertTrue(repo.connect() is ConnectOutcome.Connected)
        val grace = BackgroundGrace(scope, graceMs = 300) { manager.end(graceful = false) } // short real-time grace
        grace.activityStarted()
        grace.activityStopped()
        delay(100)
        grace.activityStarted() // back before the grace expires
        delay(600)
        assertTrue("kept across a short background", manager.hasLiveSession(deviceId))
        grace.activityStopped()
        withTimeout(5_000) { manager.state.first { it == SessionState.Idle } }
        assertEquals(ConnectionState.DISCONNECTED, uiConnection())
        assertEquals("no END_SESSION on grace expiry", 0, endSessions())
        assertTrue("the device advertises again", FirmwareSim.waitAdvertising(5_000))
        assertTrue("the service window continues", FirmwareSim.wakeRunning())
        grace.activityStarted()
        assertTrue("a later connection works", repo.connect() is ConnectOutcome.Connected)
    }

    // ---- C. concurrency --------------------------------------------------------------------

    @Test
    fun concurrent_screens_share_one_connection() = runBlocking {
        claimed()
        val hello0 = hellos()
        val outcomes = (0 until 4).map { async(Dispatchers.Default) { repo.connect() } }.awaitAll()
        assertTrue(outcomes.toString(), outcomes.all { it is ConnectOutcome.Connected })
        assertEquals("one generation", 1, outcomes.map { (it as ConnectOutcome.Connected).generation }.toSet().size)
        assertEquals("exactly one new connection", hello0 + 1, hellos())
        // a cancelled screen while connecting does not abort the open: the session still appears
        manager.closeNow(deviceId)
        assertTrue(FirmwareSim.waitAdvertising(5_000))
        val screen = scope.async { PhonesController(repo).load() }
        withTimeout(5_000) { manager.state.first { it is SessionState.Connecting || it is SessionState.Handshaking || it is SessionState.Authenticated } }
        screen.cancel()
        withTimeout(10_000) { manager.state.first { it is SessionState.Authenticated } }
        assertEquals(ConnectionState.CONNECTED, uiConnection())
    }

    // ---- D. errors -------------------------------------------------------------------------

    @Test
    fun unexpected_link_loss_reaches_every_observer_and_a_new_session_works() = runBlocking {
        claimed()
        assertTrue(repo.connect() is ConnectOutcome.Connected)
        val g1 = generation()
        ports.dropFrameType = 0x10 // the GET_STATUS answer is lost, then the link drops
        ports.disconnectAfterDrop = true
        repo.refreshStatus()
        withTimeout(5_000) { manager.state.first { it == SessionState.Idle } }
        assertEquals("no stale CONNECTED", ConnectionState.DISCONNECTED, uiConnection())
        assertFalse(manager.hasLiveSession(deviceId))
        assertTrue(FirmwareSim.waitAdvertising(5_000))
        assertTrue(repo.connect() is ConnectOutcome.Connected)
        assertNotEquals(g1, generation())
        assertEquals(ConnectionState.CONNECTED, uiConnection())
    }

    @Test
    fun failed_authentication_never_shows_connected() = runBlocking {
        assertTrue(FirmwareSim.buttonWake(false))
        assertTrue(FirmwareSim.waitAdvertising(5_000))
        // an unclaimed device: SESSION has no credential to use
        assertTrue(repo.connect() is ConnectOutcome.Failed)
        assertEquals(SessionState.Idle, manager.state.value)
        assertEquals(ConnectionState.DISCONNECTED, uiConnection())
    }

    @Test
    fun explicit_end_session_still_ends_the_service_window() = runBlocking {
        claimed()
        assertTrue(repo.connect() is ConnectOutcome.Connected)
        repo.disconnect(graceful = true) // an explicit, authorized END_SESSION
        assertEquals(1, endSessions())
        assertFalse(manager.hasLiveSession(deviceId))
        assertTrue("the window ends without any time passing", FirmwareSim.joinWake(20_000))
        assertFalse(FirmwareSim.wakeRunning())
    }
}
