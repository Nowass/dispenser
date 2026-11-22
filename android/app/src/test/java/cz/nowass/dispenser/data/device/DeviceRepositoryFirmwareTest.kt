package cz.nowass.dispenser.data.device

import cz.nowass.dispenser.crypto.NativeNoiseProvider
import cz.nowass.dispenser.crypto.SetupLabel
import cz.nowass.dispenser.crypto.TestNative
import cz.nowass.dispenser.crypto.credentials.CredentialStore
import cz.nowass.dispenser.crypto.credentials.FakeKeyWrapper
import cz.nowass.dispenser.device.fwsim.FirmwareGattPortFactory
import cz.nowass.dispenser.device.fwsim.FirmwareSim
import cz.nowass.dispenser.device.ops.ConfigRecord
import cz.nowass.dispenser.device.ops.DeviceErrorCode
import cz.nowass.dispenser.device.ops.LocalRefusal
import cz.nowass.dispenser.device.ops.ManagerSessionPort
import cz.nowass.dispenser.device.ops.MessageType as M
import cz.nowass.dispenser.device.ops.OpResult
import cz.nowass.dispenser.device.ops.Readiness
import cz.nowass.dispenser.device.session.DeviceSessionManager
import cz.nowass.dispenser.device.session.OpenRequest
import cz.nowass.dispenser.device.session.OpenResult
import cz.nowass.dispenser.device.session.SessionState
import cz.nowass.dispenser.device.session.SessionTimeouts
import cz.nowass.dispenser.device.transport.BluetoothReadiness
import cz.nowass.dispenser.device.transport.GattTransportFactory
import cz.nowass.dispenser.domain.RecurrenceKind
import cz.nowass.dispenser.domain.ScheduleDraft
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.time.DayOfWeek
import java.time.LocalDate
import java.time.LocalTime

/**
 * M1.10e.3 acceptance (decision E10): DeviceDispenserRepository → DeviceSessionManager → SecureSession
 * → FrameLink → GattTransport against the REAL firmware logic (libdsp_fwsim): every operation with
 * read-back, device refusals, reconnects, process death, a power cut during the pump and ambiguous
 * outcomes. Host interoperability only (not radio, target RNG or ESP32-C3 timing).
 */
class DeviceRepositoryFirmwareTest {
    @get:Rule val tmp = TemporaryFolder()

    private val address = "A0:B1:C2:D3:E4:F5"
    private val deviceId = "a0b1c2d3e4f5"
    private val setup = ByteArray(16) { (0x30 + it).toByte() }
    private lateinit var noise: NativeNoiseProvider
    private lateinit var scope: CoroutineScope
    private val ports = FirmwareGattPortFactory()
    private val journal = InMemoryPendingOpsJournal() // durable across "process deaths" below (Room: DeviceDatabaseTest)
    private val cache = InMemoryDeviceCacheStore()
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
        assertEquals("pump oracle: run without a durable decision, or a duplicate manual run", 0, d[5])
        assertEquals("GPIO pump signal active outside a run, or not off/held at the end of a wake", 0, d[7])
        assertEquals("every pump run went through the GPIO driver", d[4], d[8])
        scope.cancel()
    }

    private fun hexBytes(h: String) = ByteArray(h.length / 2) { h.substring(2 * it, 2 * it + 2).toInt(16).toByte() }

    /** A phone process: credential store, session manager and repository (a new one = a process restart). */
    private inner class Phone(timeouts: SessionTimeouts = SessionTimeouts(), time: PhoneTimeSource = trustedTime) {
        val store = CredentialStore(credDir, wrapper, noise)
        val manager = DeviceSessionManager(GattTransportFactory(ports, { BluetoothReadiness.Ready }, scope), noise, store, scope, timeouts)
        val repo = DeviceDispenserRepository(deviceId, { address }, ManagerSessionPort(manager), journal, cache,
            CredentialSeqAllocator(store), time, scope)

        suspend fun claim() {
            val r = (SetupLabel.parse(SetupLabel.format(deviceId, setup)) as SetupLabel.Result.Ok).label.use { manager.open(address, OpenRequest.Claim(it, "A")) }
            assertTrue("claim: $r", r is OpenResult.Opened)
        }

        suspend fun connected() {
            val c = repo.connect()
            assertTrue("connect: $c", c is ConnectOutcome.Connected)
        }

        suspend fun idle() = withTimeout(10_000) { manager.state.first { it == SessionState.Idle } }
    }

    private fun wake() {
        assertTrue(FirmwareSim.buttonWake(false))
        assertTrue(FirmwareSim.waitAdvertising(5_000))
    }

    private suspend fun rows() = journal.list(deviceId)

    private fun draft(name: String, ml: Double = 2.0, hour: Int = 8) =
        ScheduleDraft(name, ml, LocalDate.of(2027, 1, 4), LocalTime.of(hour, 0), RecurrenceKind.WEEKLY, 1, setOf(DayOfWeek.MONDAY))

    /** Time, reservoir capacity, refill and readiness through the repository itself. */
    private suspend fun DeviceDispenserRepository.prepareForDosing() {
        // 0 ACCEPTED, or 3 IGNORED_NTP_PREFERRED when the device clock is already synchronized
        assertTrue("time", (setTime() as OpResult.Ok).value.decision in setOf(0, 3))
        val st = (refreshStatus() as OpResult.Ok).value
        if (st.capacityUl == 0L) setReadiness(Readiness.SERVICE_REQUIRED).let { assertTrue("readiness: $it", it is OpResult.Ok) }
        if (st.capacityUl == 0L) {
            val cfg = (refreshConfig() as OpResult.Ok).value
            setConfig(cfg.revision, ConfigRecord.F_CAPACITY, cfg.copy(reservoirCapacityUl = 200_000)).let { assertTrue("capacity: $it", it is OpResult.Ok) }
        }
        val cap = (refreshStatus() as OpResult.Ok).value.capacityUl
        refill(cap).let { assertTrue("refill: $it (status $st)", it is OpResult.Ok) }
        setReadiness(Readiness.READY).let { assertTrue("ready: $it", it is OpResult.Ok) }
    }

    @Test
    fun every_operation_with_read_back_and_device_refusals() = runBlocking {
        val p = Phone()
        wake()
        p.claim()
        val r = p.repo
        p.connected()
        assertTrue(r.status().first() is Fresh.Live)
        r.prepareForDosing()
        val st = (r.status().first() as Fresh.Live).value
        assertEquals(st.capacityUl, st.remainingUl) // filled to the live capacity
        assertEquals(Readiness.READY, st.readiness)
        // schedules: create, read back, update, stale update, delete
        val c = r.createSchedule(draft("Ráno")) as OpResult.Ok
        val list = (r.schedules().first() as Fresh.Live).value
        assertEquals(listOf("Ráno"), list.schedules.map { it.schedule.name })
        assertEquals(c.value.id, list.schedules.single().record.id)
        assertEquals(2L, (r.updateSchedule(c.value.id, 1, draft("Ráno", 3.0)) as OpResult.Ok).value.revision)
        assertEquals(3_000L, (r.schedules().first() as Fresh.Live).value.schedules.single().record.amountUl)
        assertEquals(DeviceErrorCode.CONFLICT, (r.updateSchedule(c.value.id, 1, draft("x")) as OpResult.Rejected).error.code)
        // above the per-dose limit: refused by the device, nothing stored
        val cfg = (r.refreshConfig() as OpResult.Ok).value
        val limit = r.createSchedule(draft("Velká", ml = (cfg.maxDoseUl + 1) / 1000.0)) as OpResult.Rejected
        assertEquals(DeviceErrorCode.LIMIT, limit.error.code)
        assertTrue(r.deleteSchedule(c.value.id, 2) is OpResult.Ok)
        assertTrue((r.schedules().first() as Fresh.Live).value.schedules.isEmpty())
        // configuration: limits only in SERVICE_REQUIRED (REFUSED detail 1 while READY)
        val cfg2 = (r.refreshConfig() as OpResult.Ok).value
        val refused = r.setConfig(cfg2.revision, ConfigRecord.F_MAX_DOSE, cfg2.copy(maxDoseUl = cfg2.maxDoseUl - 1)) as OpResult.Rejected
        assertEquals(DeviceErrorCode.REFUSED, refused.error.code)
        assertEquals(1L, refused.error.detail)
        assertTrue(r.setReadiness(Readiness.SERVICE_REQUIRED) is OpResult.Ok)
        val cfg3 = (r.refreshConfig() as OpResult.Ok).value
        assertTrue(r.setConfig(cfg3.revision, ConfigRecord.F_MAX_DOSE, cfg3.copy(maxDoseUl = cfg3.maxDoseUl - 1)) is OpResult.Ok)
        assertEquals(cfg3.maxDoseUl - 1, (r.config().first() as Fresh.Live).value.maxDoseUl) // read back
        assertTrue(r.setReadiness(Readiness.READY) is OpResult.Ok)
        // REFILL above the live capacity is refused locally; never sent
        val refillsBefore = ports.sent(M.REFILL)
        assertTrue(r.refill(st.capacityUl + 1).let { it is OpResult.Refused })
        assertEquals(refillsBefore, ports.sent(M.REFILL))
        // phones, invites, diagnostics, rejections
        assertEquals(listOf(true), (r.refreshPhones() as OpResult.Ok).value.map { it.isSelf })
        val inv = (r.createInvite("B") as OpResult.Ok).value
        assertEquals(16, inv.token.size)
        inv.token.fill(0)
        assertTrue(r.cancelInvite() is OpResult.Ok)
        assertTrue(r.refreshDiagnostics() is OpResult.Ok)
        assertTrue(r.ackRejections() is OpResult.Ok)
        assertTrue(rows().isEmpty()) // every write had a definitive answer
        r.disconnect(graceful = true)
        assertTrue(FirmwareSim.joinWake(20_000))
        assertTrue(r.status().first() is Fresh.Cached) // after the session: cached, never live
    }

    /**
     * D2 with the real manager: once the session's close() returns, a value read in it is no longer
     * Live — not for a new reader, and an existing reader gets the Cached value — for a close by
     * the app, a disconnect by the app and a close of the session itself.
     */
    @Test
    fun battery_telemetry_against_the_real_firmware() = runBlocking {
        val p = Phone()
        wake()
        p.claim()
        val r = p.repo
        p.connected()
        // the simulator reports version.txt (>= 0.2.0) and has no battery port: the real dispatcher
        // answers GET_BATTERY with NOT_MEASURED (no voltage), and the session stays usable
        val b = r.battery().first()
        assertTrue("$b", b is Fresh.Live)
        assertEquals(cz.nowass.dispenser.device.ops.BatteryState.NOT_MEASURED, b.valueOrNull?.state)
        assertEquals(null, b.valueOrNull?.millivolts)
        assertTrue(r.refreshStatus() is OpResult.Ok)
        assertTrue(r.battery().first() is Fresh.Live)
        assertTrue(r.status().first() is Fresh.Live)
        assertTrue(r.refreshConfig() is OpResult.Ok)
    }

    @Test
    fun low_threshold_change_opens_and_closes_the_reservoir_low_episode_on_the_real_firmware() = runBlocking {
        val p = Phone()
        wake()
        p.claim()
        val r = p.repo
        p.connected()
        r.prepareForDosing()
        val cap = (r.refreshStatus() as OpResult.Ok).value.capacityUl
        assertTrue(r.refill(cap / 2) is OpResult.Ok) // 50 % remaining
        val ui = DeviceUiRepository(r, java.time.Clock.systemUTC(), clockTick = null)
        var st = (r.refreshStatus() as OpResult.Ok).value
        assertEquals(0, st.pendingNotes)
        val before = (r.refreshConfig() as OpResult.Ok).value
        // above the current level: the firmware queues exactly one RESERVOIR_LOW (WARNING)
        assertEquals(cz.nowass.dispenser.data.CommandResult.Done(newEvents = 0), ui.setLowStockPercent(60))
        val after = (r.config().first() as Fresh.Live).value
        assertEquals(60, after.lowStockPercent)
        assertEquals(before.reservoirRemainingUl, after.reservoirRemainingUl) // no refill, no estimate change
        assertEquals(before.reservoirCapacityUl, after.reservoirCapacityUl)
        st = (r.refreshStatus() as OpResult.Ok).value
        assertEquals(1, st.pendingNotes)
        assertEquals(1, st.highestSeverity) // WARNING
        assertEquals(Readiness.READY, st.readiness)
        // the same value again: nothing new
        ui.setLowStockPercent(60)
        assertEquals(1, (r.refreshStatus() as OpResult.Ok).value.pendingNotes)
        // restored (50 % >= 20 % + 5): the episode closes and the undelivered event is withdrawn
        assertEquals(cz.nowass.dispenser.data.CommandResult.Done(newEvents = 0), ui.setLowStockPercent(20))
        st = (r.refreshStatus() as OpResult.Ok).value
        assertEquals(0, st.pendingNotes)
        assertEquals(Readiness.READY, st.readiness)
        assertEquals(20, (r.config().first() as Fresh.Live).value.lowStockPercent)
    }

    @Test
    fun values_are_never_live_after_the_session_closes() = runBlocking {
        val p = Phone()
        wake()
        p.claim()
        val r = p.repo
        repeat(3) { way ->
            p.connected()
            assertTrue(r.refreshStatus() is OpResult.Ok)
            val live = r.status().first() as Fresh.Live
            val latest = java.util.concurrent.atomic.AtomicReference<Fresh<*>>()
            val watcher = scope.launch { r.status().collect { latest.set(it) } }
            withTimeout(5_000) { while (latest.get() !is Fresh.Live) kotlinx.coroutines.delay(1) }
            val s = p.manager.session!!
            when (way) {
                0 -> s.close("closed by the test")
                1 -> p.manager.end(graceful = false)
                else -> r.disconnect(graceful = false)
            }
            assertFalse(s.isOpen)
            assertEquals("way $way", SessionState.Idle, p.manager.state.value)
            val now = r.status().first()
            assertTrue("way $way: $now", now is Fresh.Cached)
            assertEquals(live.value, (now as Fresh.Cached).value)
            withTimeout(5_000) { while (latest.get() !is Fresh.Cached) kotlinx.coroutines.delay(1) }
            watcher.cancel()
            assertTrue(FirmwareSim.waitAdvertising(5_000)) // the device advertises again for the next way
        }
    }

    @Test
    fun set_time_without_a_defensible_uncertainty_is_never_sent() = runBlocking {
        val p = Phone(time = UnavailablePhoneTimeSource)
        wake()
        p.claim()
        p.connected()
        assertEquals(OpResult.Refused(LocalRefusal.TimeUntrusted), p.repo.setTime())
        assertEquals(0, ports.sent(M.SET_TIME))
    }

    @Test
    fun manual_dose_is_uncertain_then_resolved_done_at_the_next_session() = runBlocking {
        val p = Phone()
        wake()
        p.claim()
        p.connected()
        p.repo.prepareForDosing()
        val pumps = FirmwareSim.pumpRuns()
        val r = p.repo.manualDose(1_000)
        assertTrue("$r", r is OpResult.Uncertain) // the device suspended BLE for the pump
        p.idle()
        p.connected() // resolution runs first
        val row = rows().single()
        assertEquals(Resolution.DONE, row.resolution)
        assertEquals(1_000L, row.resolutionDetail)
        assertEquals(pumps + 1, FirmwareSim.pumpRuns())
        assertEquals(OpResult.Ok(Unit), p.repo.acknowledge(row.id))
        assertEquals(row.seq!! + 1, (p.repo.refreshStatus() as OpResult.Ok).value.manualFloor)
    }

    @Test
    fun dose_dropped_before_decryption_is_not_decided_and_resent_with_the_same_seq_once() = runBlocking {
        val p = Phone(SessionTimeouts(requestMs = 400))
        wake()
        p.claim()
        p.connected()
        p.repo.prepareForDosing()
        val pumps = FirmwareSim.pumpRuns()
        ports.dropWriteType = M.MANUAL_DOSE
        assertTrue(p.repo.manualDose(1_000) is OpResult.Uncertain) // the retransmission met the nonce desync
        p.idle()
        p.connected()
        val row = rows().single()
        assertEquals(PendingState.AWAITING_USER, row.state) // NOT_DECIDED
        assertEquals(pumps, FirmwareSim.pumpRuns())
        assertEquals(OpResult.Refused(LocalRefusal.Unresolved(row.id)), p.repo.manualDose(1_000)) // no stacking, no new seq
        assertTrue(p.repo.resendManualDose(row.id) is OpResult.Uncertain)
        p.idle()
        p.connected()
        val done = rows().single()
        assertEquals(Resolution.DONE, done.resolution)
        assertEquals(row.seq, done.seq) // the original seq
        assertEquals(pumps + 1, FirmwareSim.pumpRuns()) // exactly once
        assertEquals(row.seq!! + 1, (p.repo.refreshStatus() as OpResult.Ok).value.manualFloor)
    }

    @Test
    fun process_death_after_the_dose_is_resolved_by_a_new_process() = runBlocking {
        val p1 = Phone()
        wake()
        p1.claim()
        p1.connected()
        p1.repo.prepareForDosing()
        val pumps = FirmwareSim.pumpRuns()
        assertTrue(p1.repo.manualDose(500) is OpResult.Uncertain)
        p1.idle()
        val p2 = Phone() // new process: new credential store over the same files, new manager and repository
        p2.connected()
        assertEquals(Resolution.DONE, rows().single().resolution)
        assertEquals(pumps + 1, FirmwareSim.pumpRuns())
        // the seq high-water mark survived with the credential: the next dose takes a higher number
        p2.repo.acknowledge(rows().single().id)
        val prev = (p2.repo.refreshStatus() as OpResult.Ok).value.manualFloor
        assertTrue(p2.repo.manualDose(500) is OpResult.Uncertain)
        assertEquals(prev, rows().single().seq)
    }

    @Test
    fun power_cut_during_the_pump_is_resolved_as_interrupted() = runBlocking {
        val p = Phone()
        wake()
        p.claim()
        p.connected()
        p.repo.prepareForDosing()
        p.repo.disconnect(graceful = true)
        assertTrue(FirmwareSim.joinWake(20_000))
        assertTrue(FirmwareSim.armPumpPowerCut())
        wake()
        p.connected()
        assertTrue(p.repo.manualDose(1_000) is OpResult.Uncertain)
        assertTrue(FirmwareSim.joinWake(20_000)) // the power went during the pump run
        assertTrue(FirmwareSim.powerCutHappened())
        wake() // power back: the next boot reports the interrupted run
        p.connected()
        val row = rows().single()
        assertEquals(Resolution.INTERRUPTED, row.resolution) // "the dose may be partial": never retried automatically
        assertEquals(PendingState.RESOLVED, row.state)
    }

    /** The whole device history, newest first, through GET_HISTORY paging. */
    private suspend fun DeviceDispenserRepository.allHistory(): List<cz.nowass.dispenser.device.ops.HistoryEventRecord> {
        val out = mutableListOf<cz.nowass.dispenser.device.ops.HistoryEventRecord>()
        var before = 0L
        while (true) {
            val page = (history(before) as OpResult.Ok).value
            if (page.events.isEmpty()) break
            out += page.events
            before = page.events.last().seq
        }
        return out
    }

    /** Phase C, firmware in the loop: dose -> history -> reconnect / reboot / new process -> same history. */
    @Test
    fun history_survives_reconnect_reboot_and_process_restart() = runBlocking {
        val p = Phone()
        wake()
        p.claim()
        p.connected()
        p.repo.prepareForDosing()
        assertTrue(p.repo.manualDose(1_000) is OpResult.Uncertain) // BLE suspended for the pump
        p.idle()
        p.connected()
        val h = p.repo.allHistory()
        val kinds = h.map { it.kind }
        assertEquals(cz.nowass.dispenser.device.ops.HistoryEventKind.DOSE_MANUAL, kinds.first())
        assertTrue("reservoir events: $kinds", cz.nowass.dispenser.device.ops.HistoryEventKind.RESERVOIR_REFILL in kinds)
        val dose = h.first()
        assertEquals(cz.nowass.dispenser.device.ops.HistoryResult.OK, dose.result)
        assertEquals(1_000L, dose.volumeUl)
        assertTrue("the GPIO bench build marks its doses", dose.pumpBenchGpio)
        assertTrue(dose.clockTrusted)
        assertEquals(dose.reservoirBeforeUl!! - 1_000, dose.reservoirAfterUl)
        assertEquals(h.map { it.seq }.sortedDescending(), h.map { it.seq })
        assertEquals(h.size, h.map { it.seq }.toSet().size)
        // reconnect in the same wake
        p.repo.disconnect(graceful = false)
        p.idle()
        p.connected()
        assertEquals(h, p.repo.allHistory())
        // a new wake (reboot of the firmware world) and a new phone process
        p.repo.disconnect(graceful = true)
        assertTrue(FirmwareSim.joinWake(20_000))
        wake()
        val p2 = Phone()
        p2.connected()
        assertEquals(h, p2.repo.allHistory())
    }

    @Test
    fun power_cut_during_the_pump_is_logged_once_as_interrupted() = runBlocking {
        val p = Phone()
        wake()
        p.claim()
        p.connected()
        p.repo.prepareForDosing()
        p.repo.disconnect(graceful = true)
        assertTrue(FirmwareSim.joinWake(20_000))
        assertTrue(FirmwareSim.armPumpPowerCut())
        wake()
        p.connected()
        assertTrue(p.repo.manualDose(1_000) is OpResult.Uncertain)
        assertTrue(FirmwareSim.joinWake(20_000))
        assertTrue(FirmwareSim.powerCutHappened())
        wake()
        p.connected()
        val h1 = p.repo.allHistory()
        assertEquals(cz.nowass.dispenser.device.ops.HistoryResult.INTERRUPTED, h1.first().result)
        assertNull("reservoir change unknown after an interruption", h1.first().reservoirBeforeUl)
        // another boot never logs it again
        p.repo.disconnect(graceful = true)
        assertTrue(FirmwareSim.joinWake(20_000))
        wake()
        p.connected()
        val h2 = p.repo.allHistory()
        assertEquals(h1, h2)
        assertEquals(1, h2.count { it.result == cz.nowass.dispenser.device.ops.HistoryResult.INTERRUPTED })
    }

    /** Phase C review: a page read, then a new event and a reconnect, then the next page below the cursor. */
    @Test
    fun history_paging_across_a_new_event_and_a_reconnect() = runBlocking {
        val p = Phone()
        wake()
        p.claim()
        p.connected()
        p.repo.prepareForDosing()
        repeat(3) {
            assertTrue(p.repo.manualDose(500) is OpResult.Uncertain)
            p.idle()
            FirmwareSim.advance(120_000) // past the minimum interval
            p.connected()
        }
        val before = p.repo.allHistory()
        assertTrue("events: ${before.size}", before.size >= 5)
        val first = (p.repo.history(0, max = 2) as OpResult.Ok).value
        assertEquals(before.take(2), first.events)
        // repeated requests return the same identities
        assertEquals(first, (p.repo.history(0, max = 2) as OpResult.Ok).value)
        // a new event (dose -> BLE suspended -> reconnect) between two pages
        assertTrue(p.repo.manualDose(500) is OpResult.Uncertain)
        p.idle()
        p.connected()
        val rest = mutableListOf<cz.nowass.dispenser.device.ops.HistoryEventRecord>()
        var cursor = first.events.last().seq
        while (true) {
            val page = (p.repo.history(cursor) as OpResult.Ok).value
            if (page.events.isEmpty()) break
            rest += page.events
            cursor = page.events.last().seq
        }
        assertEquals("below the cursor: exactly the old events, no duplicate, no new one", before.drop(2), rest)
        val after = p.repo.allHistory()
        assertEquals(before.size + 1, after.size)
        assertTrue(after.first().seq > before.first().seq)
        assertEquals(before, after.drop(1))
    }

    @Test
    fun history_needs_the_authenticated_session_and_validates_its_arguments() = runBlocking {
        val p = Phone()
        assertTrue(p.repo.history(0) is OpResult.NotSent) // no session: nothing sent
        wake()
        p.claim()
        p.connected()
        val sent = ports.sent(M.GET_HISTORY)
        assertTrue(p.repo.history(0, max = 5) is OpResult.Refused)
        assertTrue(p.repo.history(-1) is OpResult.Refused)
        assertTrue(p.repo.history(0x1_0000_0000L) is OpResult.Refused)
        assertEquals(sent, ports.sent(M.GET_HISTORY))
        val page = (p.repo.history(0) as OpResult.Ok).value
        assertTrue(page.events.isEmpty() || page.newestSeq >= page.events.first().seq)
    }

    @Test
    fun lost_refill_response_is_read_back_and_never_resent() = runBlocking {
        val p = Phone(SessionTimeouts(requestMs = 400))
        wake()
        p.claim()
        p.connected()
        p.repo.prepareForDosing()
        ports.dropFrameType = M.REFILL // the answer is lost on an open link
        val sentBefore = ports.sent(M.REFILL)
        assertTrue(p.repo.refill(80_000) is OpResult.Uncertain)
        p.idle()
        p.connected()
        val row = rows().single()
        assertEquals(Resolution.READ_BACK, row.resolution)
        assertEquals(80_000L, row.resolutionDetail) // applied once (the retransmission came from the cache)
        assertEquals(sentBefore + 2, ports.sent(M.REFILL)) // original + one same-txn retransmission; nothing after
    }

    @Test
    fun lost_schedule_create_response_is_resolved_applied() = runBlocking {
        val p = Phone(SessionTimeouts(requestMs = 400))
        wake()
        p.claim()
        p.connected()
        ports.dropFrameType = M.CREATE_SCHEDULE
        val r = p.repo.createSchedule(draft("Večer", hour = 20))
        assertTrue("$r", r is OpResult.Uncertain)
        p.idle()
        p.connected()
        assertEquals(Resolution.APPLIED, rows().single().resolution)
        assertEquals(listOf("Večer"), (p.repo.refreshSchedules() as OpResult.Ok).value.schedules.map { it.schedule.name })
    }

    /**
     * A fresh MANUAL_DOSE suspends BLE before the device decides it (10.4, Q8), so even a refusal
     * arrives as an unknown outcome; GET_RESULT at the next session reports it REFUSED (durable:
     * that seq never runs later). No pump run, and the next dose is not blocked afterwards.
     */
    @Test
    fun dose_above_the_per_dose_limit_is_resolved_refused_at_the_next_session() = runBlocking {
        val p = Phone()
        wake()
        p.claim()
        p.connected()
        p.repo.prepareForDosing()
        val pumps = FirmwareSim.pumpRuns()
        val max = (p.repo.refreshConfig() as OpResult.Ok).value.maxDoseUl
        val r = p.repo.manualDose(max + 1)
        assertTrue("$r", r is OpResult.Uncertain) // never reported as a failure
        p.idle()
        p.connected()
        val row = rows().single()
        assertEquals(Resolution.REFUSED, row.resolution)
        assertEquals(pumps, FirmwareSim.pumpRuns())
        p.repo.acknowledge(row.id)
        assertTrue(p.repo.manualDose(1_000) is OpResult.Uncertain) // the next dose is allowed again
    }
}
