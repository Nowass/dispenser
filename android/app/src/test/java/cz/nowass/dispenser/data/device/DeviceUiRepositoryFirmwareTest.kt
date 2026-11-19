package cz.nowass.dispenser.data.device

import cz.nowass.dispenser.crypto.NativeNoiseProvider
import cz.nowass.dispenser.crypto.SetupLabel
import cz.nowass.dispenser.crypto.TestNative
import cz.nowass.dispenser.crypto.credentials.CredentialStore
import cz.nowass.dispenser.crypto.credentials.FakeKeyWrapper
import cz.nowass.dispenser.data.CommandResult
import cz.nowass.dispenser.data.ScheduleWriteResult
import cz.nowass.dispenser.device.fwsim.FirmwareGattPortFactory
import cz.nowass.dispenser.device.fwsim.FirmwareSim
import cz.nowass.dispenser.device.ops.ConfigRecord
import cz.nowass.dispenser.device.ops.ManagerSessionPort
import cz.nowass.dispenser.device.ops.OpResult
import cz.nowass.dispenser.device.ops.Readiness
import cz.nowass.dispenser.device.session.DeviceSessionManager
import cz.nowass.dispenser.device.session.OpenRequest
import cz.nowass.dispenser.device.session.OpenResult
import cz.nowass.dispenser.device.transport.BluetoothReadiness
import cz.nowass.dispenser.device.transport.GattTransportFactory
import cz.nowass.dispenser.domain.ConnectionState
import cz.nowass.dispenser.domain.DataSource
import cz.nowass.dispenser.domain.DeviceAccess
import cz.nowass.dispenser.device.session.SessionFailure
import cz.nowass.dispenser.device.ops.MessageType as M
import cz.nowass.dispenser.domain.Freshness
import cz.nowass.dispenser.domain.RecurrenceKind
import cz.nowass.dispenser.domain.ScheduleDraft
import cz.nowass.dispenser.domain.SyncState
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.time.Clock
import java.time.DayOfWeek
import java.time.LocalDate
import java.time.LocalTime

/**
 * M1.10e.4a: the DEVICE-mode adapter of the shared screens over the real repository, session and the
 * REAL firmware logic (libdsp_fwsim). Live values only while the session is live, cached values with
 * their read time afterwards and after a process restart, device-confirmed schedules, online-only
 * writes (NotSent when disconnected, nothing reaches the device).
 */
class DeviceUiRepositoryFirmwareTest {
    @get:Rule val tmp = TemporaryFolder()

    private val address = "A0:B1:C2:D3:E4:F5"
    private val deviceId = "a0b1c2d3e4f5"
    private val setup = ByteArray(16) { (0x30 + it).toByte() }
    private lateinit var noise: NativeNoiseProvider
    private lateinit var scope: CoroutineScope
    private val ports = FirmwareGattPortFactory()
    private val journal = InMemoryPendingOpsJournal()
    private val cache = InMemoryDeviceCacheStore() // survives the "process restart" below
    private lateinit var credDir: File
    private val wrapper = FakeKeyWrapper()
    private val trustedTime = PhoneTimeSource { PhoneTime(System.currentTimeMillis(), 100) } // test source only (D6)

    @Before
    fun setUp() {
        noise = TestNative.load()
        FirmwareSim.load()
        assertTrue(FirmwareSim.reset(hexBytes(deviceId), setup))
        scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
        credDir = tmp.newFolder("cred")
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

    private fun hexBytes(h: String) = ByteArray(h.length / 2) { h.substring(2 * it, 2 * it + 2).toInt(16).toByte() }

    private inner class Phone {
        val store = CredentialStore(credDir, wrapper, noise)
        val manager = DeviceSessionManager(GattTransportFactory(ports, { BluetoothReadiness.Ready }, scope), noise, store, scope)
        val repo = DeviceDispenserRepository(deviceId, { address }, ManagerSessionPort(manager), journal, cache,
            CredentialSeqAllocator(store), trustedTime, scope)
        val ui = DeviceUiRepository(repo, Clock.systemUTC(), clockTick = null)

        suspend fun claim() {
            val r = (SetupLabel.parse(SetupLabel.format(deviceId, setup)) as SetupLabel.Result.Ok).label.use { manager.open(address, OpenRequest.Claim(it, "A")) }
            assertTrue("claim: $r", r is OpenResult.Opened)
        }
    }

    private fun wake() {
        assertTrue(FirmwareSim.buttonWake(false))
        assertTrue(FirmwareSim.waitAdvertising(5_000))
    }

    private val draft = ScheduleDraft("Ranní", 2.0, LocalDate.of(2027, 1, 4), LocalTime.of(8, 0), RecurrenceKind.WEEKLY, 1, setOf(DayOfWeek.MONDAY))

    @Test
    fun shared_screens_see_live_then_cached_device_values_and_online_only_writes() = runBlocking {
        val p = Phone()
        wake()
        p.claim()
        assertTrue(p.repo.connect() is ConnectOutcome.Connected)
        val r = p.repo
        // A reservoir the device knows (capacity set in service mode), through the repository itself.
        assertTrue(r.setReadiness(Readiness.SERVICE_REQUIRED) is OpResult.Ok)
        val cfg = (r.refreshConfig() as OpResult.Ok).value
        assertTrue(r.setConfig(cfg.revision, ConfigRecord.F_CAPACITY, cfg.copy(reservoirCapacityUl = 200_000)) is OpResult.Ok)
        assertTrue(r.refreshStatus() is OpResult.Ok)

        // Connected: live values, the device's own estimate, CONNECTED.
        var d = p.ui.observeDevice().first()
        assertEquals(DataSource.DEVICE, d.source)
        assertEquals(ConnectionState.CONNECTED, d.connection)
        assertTrue("${d.freshness}", d.freshness is Freshness.Live)
        assertEquals(200_000L, p.ui.observeReservoir().first()!!.capacityUl)

        // Refill and a schedule through the shared-screen contract, confirmed by the device.
        assertEquals(CommandResult.Done(0), p.ui.refillReservoir(150_000))
        assertEquals(150_000L, p.ui.observeReservoir().first()!!.remainingUl)
        val created = p.ui.createSchedule(draft)
        assertTrue("$created", created is ScheduleWriteResult.Success)
        val id = (created as ScheduleWriteResult.Success).schedule!!.id
        assertEquals(listOf(id), p.ui.observeSchedules().first().map { it.id })
        assertEquals(mapOf(id to SyncState.DEVICE_APPLIED), p.ui.observeScheduleSync().first())
        assertTrue(p.ui.observeScheduleFreshness().first() is Freshness.Live)

        // Disconnected: the same values, cached with their read time, never live; writes are not sent.
        r.disconnect(graceful = true)
        d = withTimeout(5_000) { p.ui.observeDevice().first { it.connection == ConnectionState.DISCONNECTED } }
        assertTrue("${d.freshness}", d.freshness is Freshness.Cached)
        assertTrue(p.ui.observeScheduleFreshness().first() is Freshness.Cached)
        assertEquals(150_000L, p.ui.observeReservoir().first()!!.remainingUl)
        val before = ports.sentFrames.values.sum()
        assertEquals(ScheduleWriteResult.NotSent, p.ui.createSchedule(draft.copy(name = "Večerní")))
        assertEquals(CommandResult.NotSent, p.ui.refillReservoir(100_000))
        assertEquals("nothing reached the device", before, ports.sentFrames.values.sum())

        // Process restart (new manager, repository and adapter): cached values only, still never live.
        val q = Phone()
        val cached = withTimeout(5_000) { q.ui.observeDevice().first { it.freshness !is Freshness.Unknown } }
        assertTrue("${cached.freshness}", cached.freshness is Freshness.Cached)
        assertEquals(ConnectionState.DISCONNECTED, cached.connection)
        assertNotNull(q.ui.observeReservoir().first())
        assertEquals(listOf(id), withTimeout(5_000) { q.ui.observeSchedules().first { it.isNotEmpty() } }.map { it.id })
    }

    /**
     * Review of 4a: the selected device's credential becomes unusable (Keystore key gone, record
     * unreadable, record gone) while an unresolved manual dose is journaled. DEVICE mode shows the
     * recovery-required state with the cached values and the unresolved row; no operation is possible,
     * nothing is sent or re-issued, the row is never dropped; once the credential is usable again the
     * row is resolved by read-back at the next session, never re-sent with a new seq.
     */
    @Test
    fun unusable_credential_keeps_unresolved_operations_and_blocks_everything() = runBlocking {
        val p = Phone()
        wake()
        p.claim()
        assertTrue(p.repo.connect() is ConnectOutcome.Connected)
        assertTrue(p.repo.refreshStatus() is OpResult.Ok)
        p.repo.disconnect(graceful = true)
        val rowId = journal.insert(PendingOp(deviceId = deviceId, kind = PendingKind.MANUAL_DOSE, createdAtMs = 1, seq = 41, volumeUl = 1_000))
        val record = File(credDir, "$deviceId.cred")
        val goodRecord = record.readBytes()
        val alias = wrapper.keys.keys.single()
        val goodKey = wrapper.keys.getValue(alias)

        data class Loss(val name: String, val apply: () -> Unit, val access: DeviceAccess, val failure: SessionFailure)
        val losses = listOf(
            Loss("key missing", { wrapper.deleteKey(alias) }, DeviceAccess.KEY_MISSING, SessionFailure.CredentialLost),
            Loss("record unreadable", { record.writeBytes(goodRecord.copyOf().also { it[it.size - 1] = (it[it.size - 1].toInt() xor 1).toByte() }) },
                DeviceAccess.RECORD_UNREADABLE, SessionFailure.CredentialUnreadable),
            Loss("record missing", { record.delete() }, DeviceAccess.RECORD_MISSING, SessionFailure.NoCredential),
        )
        for (loss in losses) {
            loss.apply()
            val q = Phone() // process restart with the damaged credential
            val access = DeviceUiRepository.credentialAccess(q.store, deviceId)
            assertEquals(loss.name, loss.access, access)
            val ui = DeviceUiRepository(q.repo, Clock.systemUTC(), clockTick = null, access = kotlinx.coroutines.flow.MutableStateFlow(access))
            val d = withTimeout(5_000) { ui.observeDevice().first { it.freshness !is Freshness.Unknown } }
            assertEquals(loss.name, ConnectionState.RECOVERY_REQUIRED, d.connection)
            assertEquals(loss.name, 1, d.unresolvedOperations)
            assertTrue(loss.name, d.freshness is Freshness.Cached)

            val before = ports.sentFrames.toMap()
            assertEquals(loss.name, ScheduleWriteResult.NotSent, ui.createSchedule(draft))
            assertEquals(loss.name, CommandResult.NotSent, ui.refillReservoir(1_000))
            // Even an explicit connection attempt cannot authenticate, and resolution never runs.
            if (!FirmwareSim.waitAdvertising(3_000)) wake()
            assertEquals(loss.name, ConnectOutcome.Failed(loss.failure), q.repo.connect())
            val sentNow = ports.sentFrames.toMap()
            for (t in listOf(M.GET_RESULT, M.MANUAL_DOSE, M.REFILL, M.CREATE_SCHEDULE)) {
                assertEquals("${loss.name}: frame 0x${t.toString(16)} sent", before[t] ?: 0, sentNow[t] ?: 0)
            }
            assertEquals(loss.name, listOf(rowId), journal.list(deviceId).filter { it.unresolved }.map { it.id })
            // restore the credential for the next case
            record.writeBytes(goodRecord)
            wrapper.keys[alias] = goodKey
        }

        // Usable again: the row is resolved by read-back (GET_RESULT) at the next session, never re-sent.
        val r = Phone()
        assertEquals(DeviceAccess.OK, DeviceUiRepository.credentialAccess(r.store, deviceId))
        if (!FirmwareSim.waitAdvertising(3_000)) wake()
        assertTrue(r.repo.connect() is ConnectOutcome.Connected)
        assertTrue("GET_RESULT sent", ports.sent(M.GET_RESULT) >= 1)
        assertEquals("never re-issued", 0, ports.sent(M.MANUAL_DOSE))
        assertTrue("row kept until the user decides", journal.list(deviceId).any { it.id == rowId })
    }
}
