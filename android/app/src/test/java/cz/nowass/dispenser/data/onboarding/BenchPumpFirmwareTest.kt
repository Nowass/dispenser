package cz.nowass.dispenser.data.onboarding

import cz.nowass.dispenser.crypto.NativeNoiseProvider
import cz.nowass.dispenser.crypto.SetupLabel
import cz.nowass.dispenser.crypto.TestNative
import cz.nowass.dispenser.crypto.credentials.CredentialStore
import cz.nowass.dispenser.crypto.credentials.FakeKeyWrapper
import cz.nowass.dispenser.data.device.CredentialSeqAllocator
import cz.nowass.dispenser.data.device.DeviceDispenserRepository
import cz.nowass.dispenser.data.device.InMemoryDeviceCacheStore
import cz.nowass.dispenser.data.device.InMemoryPendingOpsJournal
import cz.nowass.dispenser.data.device.UnavailablePhoneTimeSource
import cz.nowass.dispenser.device.fwsim.FirmwareGattPortFactory
import cz.nowass.dispenser.device.ops.ConfigRecord
import cz.nowass.dispenser.device.ops.DeviceErrorCode
import cz.nowass.dispenser.device.ops.OpResult
import cz.nowass.dispenser.device.ops.Readiness
import cz.nowass.dispenser.device.fwsim.FirmwareSim
import cz.nowass.dispenser.device.ops.ManagerSessionPort
import cz.nowass.dispenser.device.session.DeviceSessionManager
import cz.nowass.dispenser.device.session.OpenRequest
import cz.nowass.dispenser.device.session.OpenResult
import cz.nowass.dispenser.device.transport.BluetoothReadiness
import cz.nowass.dispenser.device.transport.GattTransportFactory
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.runBlocking
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/**
 * BENCH ONLY pump configuration against the REAL firmware logic: the controller writes flow, limits,
 * interval and the explicit bench calibration through SET_CONFIG; READY only on request and only with
 * every gate open; calibration and limits cannot change while READY (refused locally and, when sent
 * anyway, by the firmware); a READY refusal reports the firmware's closed gate and pump problem.
 */
class BenchPumpFirmwareTest {
    @get:Rule val tmp = TemporaryFolder()
    private val address = "A0:B1:C2:D3:E4:F5"
    private val deviceId = "a0b1c2d3e4f5"
    private val setup = ByteArray(16) { (0x30 + it).toByte() }
    private lateinit var noise: NativeNoiseProvider
    private lateinit var scope: CoroutineScope
    private val ports = FirmwareGattPortFactory()
    private val now = 1_795_200_000L

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
        assertEquals("pump oracle", 0, d[5])
        assertEquals("GPIO oracle", 0, d[7])
        scope.cancel()
    }

    private val store by lazy { CredentialStore(tmp.newFolder("A"), FakeKeyWrapper(), noise) }
    private val manager by lazy { DeviceSessionManager(GattTransportFactory(ports, { BluetoothReadiness.Ready }, scope), noise, store, scope) }
    private val repo by lazy {
        DeviceDispenserRepository(deviceId, { address }, ManagerSessionPort(manager), InMemoryPendingOpsJournal(),
            InMemoryDeviceCacheStore(), CredentialSeqAllocator(store), UnavailablePhoneTimeSource, scope)
    }

    private suspend fun claimed() {
        assertTrue(FirmwareSim.buttonWake(false))
        assertTrue(FirmwareSim.waitAdvertising(5_000))
        val r = (SetupLabel.parse(SetupLabel.format(deviceId, setup)) as SetupLabel.Result.Ok).label
            .use { manager.open(address, OpenRequest.Claim(it, "SM-S911B")) }
        assertTrue("claim: $r", r is OpenResult.Opened)
    }

    private val form = BenchPumpForm("1000", "15", "60", "20000", "60", calibrateNow = true)

    @Test
    fun configure_then_ready_then_locked_then_not_ready() = runBlocking {
        claimed()
        val c = BenchPumpController(repo) { now }
        var snap = (c.load() as BenchOutcome.Loaded).snapshot
        if (snap.ready) snap = (c.setReady(false) as BenchOutcome.Loaded).snapshot
        assertEquals(Readiness.SERVICE_REQUIRED, snap.status.readiness)
        val hw = snap.config.hwMaxRuntimeMs
        assertTrue("the simulator has a hardware maximum", hw > 0)

        // invalid input never reaches the device
        val configs0 = ports.sent(0x21)
        assertTrue(c.save(snap, form.copy(maxRuntimeMs = (hw + 1).toString())) is BenchOutcome.Invalid)
        assertEquals(configs0, ports.sent(0x21))

        // save: read back from the device
        snap = (c.save(snap, form) as BenchOutcome.Loaded).snapshot
        assertEquals(1000L, snap.config.pumpFlowUlPerS)
        assertEquals(15_000L, snap.config.maxDoseUl)
        assertEquals(60_000L, snap.config.maxDailyUl)
        assertEquals(20_000L, snap.config.maxRuntimeMs)
        assertEquals(60L, snap.config.minIntervalS)
        assertEquals(now, snap.config.calibratedAt)
        assertEquals("saving never sets READY", Readiness.SERVICE_REQUIRED, snap.status.readiness)
        assertTrue("pump configuration complete: ${snap.problems}", snap.problems.isEmpty())

        // READY only on request; accepted with every gate (except the clock) open
        snap = (c.setReady(true) as BenchOutcome.Loaded).snapshot
        assertEquals(Readiness.READY, snap.status.readiness)

        // while READY: refused locally (nothing sent) ...
        val configs1 = ports.sent(0x21)
        assertEquals(BenchOutcome.MustBeNotReady, c.save(snap, form.copy(flowUlPerS = "900")))
        assertEquals(configs1, ports.sent(0x21))
        // ... and by the firmware when sent anyway (its rule is unchanged)
        val direct = repo.setConfig(snap.config.revision, ConfigRecord.F_FLOW, snap.config.copy(pumpFlowUlPerS = 900))
        assertTrue("$direct", direct is OpResult.Rejected && direct.error.code == DeviceErrorCode.REFUSED)

        snap = (c.setReady(false) as BenchOutcome.Loaded).snapshot
        assertEquals(Readiness.SERVICE_REQUIRED, snap.status.readiness)
    }

    /**
     * Regression of the first GPIO bench session (2026-11-20): schedules could be created and edited
     * before the bench pump configuration, and were refused afterwards with the generic "Zařízení změnu
     * odmítlo". NOT READY and an incomplete pump configuration never block schedule writes; the only
     * pump-configuration rule in schedule validation is amount <= max_dose_ul (firmware check_input,
     * LIMIT, detail = DSP_DOSING_L_DOSE, aux = max_dose_ul). A max dose typed as "15" in the µl field
     * (meant: 15 ml) refuses every schedule above 15 µl.
     */
    @Test
    fun schedule_writes_in_not_ready_and_the_max_dose_rule() = runBlocking {
        claimed()
        val c = BenchPumpController(repo) { now }
        var snap = (c.load() as BenchOutcome.Loaded).snapshot
        if (snap.ready) snap = (c.setReady(false) as BenchOutcome.Loaded).snapshot
        val draft = cz.nowass.dispenser.domain.ScheduleDraft("Ranní", 15.0, java.time.LocalDate.of(2027, 1, 4),
            java.time.LocalTime.of(8, 0), cz.nowass.dispenser.domain.RecurrenceKind.DAILY)
        // NOT READY, pump configuration incomplete: schedules are configurable
        val created = repo.createSchedule(draft)
        assertTrue("$created", created is OpResult.Ok)
        val id = (created as OpResult.Ok).value.id
        val rev = created.value.revision!!
        assertTrue(repo.refreshSchedules() is OpResult.Ok) // the schedule list the warning is based on
        // max dose 15 µl (the unit mistake of the session; the form now takes ml: 0,015 ml): saved with
        // a warning that the existing 15 ml schedule is now refused ...
        val saved = c.save(snap, form.copy(maxDoseMl = "0,015", maxDailyMl = "0,06")) as BenchOutcome.Loaded
        snap = saved.snapshot
        assertEquals(15L, snap.config.maxDoseUl)
        assertEquals(1, saved.schedulesAboveMaxDose)
        val again = repo.createSchedule(draft.copy(name = "Druhý"))
        assertTrue("$again", again is OpResult.Rejected && again.error.code == DeviceErrorCode.LIMIT &&
            again.error.detail == 1L && again.error.aux == 15L)
        val edit = repo.updateSchedule(id, rev, draft.copy(amountMl = 16.0))
        assertTrue("$edit", edit is OpResult.Rejected && edit.error.code == DeviceErrorCode.LIMIT)
        // ... and the screens show the reason, not the generic refusal
        val ui = cz.nowass.dispenser.data.device.DeviceUiRepository(repo, java.time.Clock.systemUTC(), clockTick = null)
        assertEquals(cz.nowass.dispenser.data.ScheduleWriteResult.AboveMaxDose(15), ui.createSchedule(draft.copy(name = "Čtvrtý")))
        // max dose 15 ml (15,000 µl): the 15 ml schedule is accepted (amount <= max dose)
        snap = (c.save(snap, form.copy(maxDoseMl = "15", maxDailyMl = "60")) as BenchOutcome.Loaded).snapshot
        assertTrue(repo.createSchedule(draft.copy(name = "Třetí")) is OpResult.Ok)
        assertEquals("never READY", Readiness.SERVICE_REQUIRED, snap.status.readiness)
    }

    @Test
    fun ready_refused_by_the_firmware_names_the_missing_calibration() = runBlocking {
        claimed()
        val c = BenchPumpController(repo) { now }
        var snap = (c.load() as BenchOutcome.Loaded).snapshot
        if (snap.ready) snap = (c.setReady(false) as BenchOutcome.Loaded).snapshot
        snap = (c.save(snap, form) as BenchOutcome.Loaded).snapshot
        // the calibration is removed (calibrated_at 0, allowed in SERVICE_REQUIRED)
        val cleared = repo.setConfig(snap.config.revision, ConfigRecord.F_CALIBRATED_AT, snap.config.copy(calibratedAt = 0))
        assertTrue("$cleared", cleared is OpResult.Ok)
        val res = c.setReady(true)
        assertEquals(BenchOutcome.ReadyRefused(listOf(BenchGate.PUMP_CONFIG), listOf(BenchPumpProblem.CALIBRATED_AT)),
            (res as BenchOutcome.ReadyRefused).copy(gates = res.gates.filter { it != BenchGate.NOT_READY && it != BenchGate.CLOCK }))
        assertEquals(Readiness.SERVICE_REQUIRED, (c.load() as BenchOutcome.Loaded).snapshot.status.readiness)
    }
}
