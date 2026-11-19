package cz.nowass.dispenser.data.device

import cz.nowass.dispenser.device.ops.DeviceErrorCode
import cz.nowass.dispenser.device.ops.DeviceValidation
import cz.nowass.dispenser.device.ops.LocalRefusal
import cz.nowass.dispenser.device.ops.MessageType as M
import cz.nowass.dispenser.device.ops.OpResult
import cz.nowass.dispenser.device.ops.Readiness
import cz.nowass.dispenser.device.session.SessionFailure
import cz.nowass.dispenser.device.transport.CloseReason
import cz.nowass.dispenser.domain.RecurrenceKind
import cz.nowass.dispenser.domain.ScheduleDraft
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.async
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.time.DayOfWeek
import java.time.LocalDate
import java.time.LocalTime
import java.time.ZoneId

/**
 * DeviceDispenserRepository against a scripted device (FakeDevicePort): freshness (D2), the
 * manager-owned lifecycle (D3), the manual-dose crash boundaries (D4), REFILL (D5), SET_TIME (D6)
 * and the journal resolution of every journaled operation (11.4, table 5.1).
 */
class DeviceRepositoryTest {
    private val dev = FakeDevicePort()
    private val journal = InMemoryPendingOpsJournal()
    private val cache = InMemoryDeviceCacheStore()
    private val seq = TestSeqAllocator()
    private var time: PhoneTimeSource = UnavailablePhoneTimeSource
    private var ids = 0

    private fun TestScope.repo(scope: CoroutineScope = backgroundScope) = DeviceDispenserRepository(
        dev.deviceId, { "A0:B1:C2:D3:E4:F5" }, dev, journal, cache, seq, { time.now() }, scope,
        nowMs = { 1_795_000_000_000L }, newScheduleId = { "sched-${++ids}" },
    )

    private suspend fun DeviceDispenserRepository.connected(): ConnectOutcome.Connected = connect() as ConnectOutcome.Connected

    private fun draft(name: String = "Ráno", ml: Double = 2.0) = ScheduleDraft(name, ml, LocalDate.of(2026, 12, 1), LocalTime.of(8, 0),
        RecurrenceKind.WEEKLY, 1, setOf(DayOfWeek.MONDAY, DayOfWeek.FRIDAY))

    private suspend fun rows() = journal.list(dev.deviceId)

    // ---- freshness (D2) ---------------------------------------------------------------------

    @Test
    fun live_values_belong_to_the_current_generation_and_become_cached_on_disconnect() = runTest {
        val r = repo()
        assertEquals(Fresh.Unknown, r.status().first())
        r.connected()
        val live = r.status().first()
        assertTrue("$live", live is Fresh.Live && live.generation == 1L)
        dev.close() // disconnect
        assertTrue(r.status().first() is Fresh.Cached)
        r.connected() // generation 2: the generation-1 value is cached until read again
        dev.hooks[M.GET_STATUS] = { } // (connect already re-read it)
        assertEquals(2L, (r.status().first() as Fresh.Live).generation)
        dev.close()
        dev.generation = 9
        dev.open("x", cz.nowass.dispenser.device.session.OpenRequest.Session(dev.deviceId)) // another session, no read yet
        assertTrue(r.status().first() is Fresh.Cached)
    }

    @Test
    fun cache_survives_process_death_but_is_never_live() = runTest {
        repo().connected()
        dev.close()
        val r2 = repo() // process death: a new repository over the same durable cache
        runCurrent()
        val c = r2.status().first()
        assertTrue("$c", c is Fresh.Cached && c.value.remainingUl == 150_000L)
        dev.open("x", cz.nowass.dispenser.device.session.OpenRequest.Session(dev.deviceId)) // connected, nothing read in this session
        assertTrue(r2.status().first() is Fresh.Cached)
    }

    @Test
    fun cached_status_never_authorizes_a_dose_or_refill() = runTest {
        repo().connected()
        dev.close()
        val r = repo()
        runCurrent()
        assertTrue(r.status().first() is Fresh.Cached)
        assertEquals(OpResult.NotSent("not connected"), r.manualDose(1000))
        assertEquals(OpResult.NotSent("not connected"), r.refill(100))
        assertEquals(0, seq.calls)
        assertTrue(rows().isEmpty())
    }

    // ---- manual dose: D4 crash boundaries -----------------------------------------------------

    @Test
    fun manual_dose_order_seq_then_journal_then_send_and_resolution_by_get_result() = runTest {
        val r = repo()
        r.connected()
        dev.manualFloor = 41
        val order = mutableListOf<String>()
        dev.hooks[M.MANUAL_DOSE] = { order += "sent seq=${rows().single().seq} high=${seq.high}" }
        val res = r.manualDose(1_000)
        assertTrue("$res", res is OpResult.Uncertain && res.journalId != null) // suspended for the pump: never a failure
        assertEquals(listOf("sent seq=41 high=41"), order) // seq durable and row durable BEFORE the request
        assertEquals(PendingState.PREPARED, rows().single().state)
        // another dose is refused while this one is unresolved (no stacking)
        r.connected()
        val row = rows().single()
        assertEquals(PendingState.RESOLVED, row.state) // resolved at session start, before anything else
        assertEquals(Resolution.DONE, row.resolution)
        assertEquals(1_000L, row.resolutionDetail)
        assertEquals(1, dev.pumpRuns)
        assertEquals(OpResult.Ok(Unit), r.acknowledge(row.id))
        assertTrue(rows().isEmpty())
    }

    @Test
    fun seq_persistence_failure_prevents_transmission() = runTest {
        val r = repo()
        r.connected()
        seq.fail = true
        val res = r.manualDose(1_000)
        assertTrue("$res", res is OpResult.Refused && res.reason is LocalRefusal.PersistenceFailed)
        assertEquals(0, dev.count(M.MANUAL_DOSE))
        assertTrue(rows().isEmpty())
    }

    @Test
    fun journal_persistence_failure_prevents_transmission_and_the_seq_is_never_reused() = runTest {
        val r = repo()
        r.connected()
        journal.failInsert = true
        val res = r.manualDose(1_000)
        assertTrue("$res", res is OpResult.Refused && res.reason is LocalRefusal.PersistenceFailed)
        assertEquals(0, dev.count(M.MANUAL_DOSE))
        assertEquals(1L, seq.high) // allocated (durably) and burned
        journal.failInsert = false
        r.manualDose(1_000)
        assertEquals(2L, rows().single().seq) // the next dose takes a new number; 1 is never reused
        assertEquals(2L, dev.received.single { it.first == M.MANUAL_DOSE }.second["seq"])
    }

    @Test
    fun crash_after_seq_before_journal_skips_the_number() = runTest {
        seq.next(dev.deviceId, 1) // seq 1 durably allocated, then the process died (no row)
        val r = repo()
        r.connected()
        r.manualDose(500)
        assertEquals(2L, rows().single().seq)
    }

    @Test
    fun crash_after_journal_before_send_is_resolved_not_decided_and_resent_with_the_same_seq() = runTest {
        // the state a crash between "row committed" and "request sent" leaves behind
        val s = seq.next(dev.deviceId, 1)
        journal.insert(PendingOp(deviceId = dev.deviceId, kind = PendingKind.MANUAL_DOSE, createdAtMs = 0, seq = s, volumeUl = 750,
            payload = cz.nowass.dispenser.ble.protocol.BleProtocol.encodeBody(M.MANUAL_DOSE, 0, mapOf("seq" to s, "volume_ul" to 750L))))
        val r = repo() // restarted process
        r.connected()
        val row = rows().single()
        assertEquals(PendingState.AWAITING_USER, row.state) // NOT_DECIDED: never executed
        assertEquals(Resolution.NOT_DECIDED, row.resolution)
        assertEquals(0, dev.pumpRuns)
        // no new dose while it is unresolved; no new seq ever allocated for it
        assertEquals(OpResult.Refused(LocalRefusal.Unresolved(row.id)), r.manualDose(750))
        val callsBefore = seq.calls
        assertTrue(r.resendManualDose(row.id) is OpResult.Uncertain)
        assertEquals(callsBefore, seq.calls)
        assertEquals(s, dev.received.last { it.first == M.MANUAL_DOSE }.second["seq"]) // the ORIGINAL seq
        r.connected()
        assertEquals(Resolution.DONE, rows().single().resolution)
        assertEquals(1, dev.pumpRuns)
    }

    @Test
    fun dose_dropped_before_decryption_then_process_death_then_same_seq_once() = runTest {
        val r1 = repo()
        r1.connected()
        dev.faults[M.MANUAL_DOSE] = FakeDevicePort.Fault.DROP_BEFORE
        val res = r1.manualDose(600)
        assertTrue(res is OpResult.Uncertain)
        val original = rows().single().seq!!
        val r2 = repo() // process death
        r2.connected()
        assertEquals(PendingState.AWAITING_USER, rows().single().state)
        r2.resendManualDose(rows().single().id)
        r2.connected()
        assertEquals(Resolution.DONE, rows().single().resolution)
        assertEquals(1, dev.pumpRuns)
        assertEquals(listOf(original, original), dev.received.filter { it.first == M.MANUAL_DOSE }.map { it.second["seq"] })
    }

    @Test
    fun discarding_a_not_decided_dose_is_a_user_action_only() = runTest {
        val r = repo()
        r.connected()
        dev.faults[M.MANUAL_DOSE] = FakeDevicePort.Fault.DROP_BEFORE
        r.manualDose(600)
        val id = rows().single().id
        assertTrue(r.discardManualDose(id) is OpResult.Refused) // PREPARED: may have reached the device
        r.connected()
        assertEquals(OpResult.Ok(Unit), r.discardManualDose(id)) // NOT_DECIDED: the user may discard
        assertTrue(rows().isEmpty())
    }

    @Test
    fun dose_answers_settle_the_row() = runTest {
        val r = repo()
        r.connected()
        dev.doseClosesLink = false
        assertTrue(r.manualDose(100) is OpResult.Ok) // an answer arrived: decided, row removed
        assertTrue(rows().isEmpty())
        dev.rejects[M.MANUAL_DOSE] = Triple(8, 2, 5000) // LIMIT: durably refused
        assertEquals(DeviceErrorCode.LIMIT, (r.manualDose(100) as OpResult.Rejected).error.code)
        assertTrue(rows().isEmpty())
        dev.rejects[M.MANUAL_DOSE] = Triple(13, 0, 0) // STORAGE: undecided, the row stays for GET_RESULT
        assertEquals(DeviceErrorCode.STORAGE, (r.manualDose(100) as OpResult.Rejected).error.code)
        assertEquals(PendingState.PREPARED, rows().single().state)
    }

    // ---- D3: cancellation --------------------------------------------------------------------

    @Test
    fun cancelled_caller_does_not_discard_the_outcome_or_the_journal() = runTest {
        val r = repo()
        r.connected()
        val gate = CompletableDeferred<Unit>()
        dev.hooks[M.REFILL] = { gate.await() } // the request is in flight
        val caller = async { r.refill(120_000) }
        runCurrent()
        assertEquals(PendingKind.REFILL, rows().single().kind) // journaled before sending
        caller.cancel()
        runCurrent()
        gate.complete(Unit) // the device answers after the caller gave up
        runCurrent()
        assertTrue(rows().isEmpty()) // the outcome was still processed: row settled
        assertEquals(120_000L, (r.status().first() as Fresh.Live).value.remainingUl) // and the status updated
        assertEquals(1, dev.refills)
    }

    // ---- D5: REFILL --------------------------------------------------------------------------

    @Test
    fun refill_uses_the_capacity_read_live_in_the_same_session() = runTest {
        val r = repo()
        r.connected()
        dev.capacityUl = 300_000 // changed on the device since the cached read
        val res = r.refill(250_000)
        assertTrue("$res", res is OpResult.Ok)
        assertEquals(300_000L, dev.received.last { it.first == M.REFILL }.second["capacity_ul"])
        assertTrue(r.refill(400_000).let { it is OpResult.Refused && it.reason == LocalRefusal.Invalid(setOf(DeviceValidation.REMAINING_ABOVE_CAPACITY)) })
    }

    @Test
    fun refill_aborts_when_the_session_changes_after_the_capacity_read() = runTest {
        val r = repo()
        r.connected()
        dev.after[M.GET_STATUS] = { dev.close() } // the capacity is answered, then the link drops before REFILL
        assertEquals(OpResult.Refused(LocalRefusal.SessionChanged), r.refill(100_000))
        assertEquals(0, dev.count(M.REFILL))
        assertTrue(rows().isEmpty())
    }

    @Test
    fun uncertain_refill_is_never_repeated_and_is_read_back() = runTest {
        val r = repo()
        r.connected()
        dev.faults[M.REFILL] = FakeDevicePort.Fault.EXECUTE_THEN_LOSE
        val res = r.refill(110_000)
        assertTrue(res is OpResult.Uncertain)
        r.connected()
        val row = rows().single()
        assertEquals(Resolution.READ_BACK, row.resolution)
        assertEquals(110_000L, row.resolutionDetail)
        assertEquals(1, dev.count(M.REFILL)) // never re-sent
        assertEquals(1, dev.refills)
    }

    @Test
    fun a_second_refill_is_refused_while_one_is_unresolved() = runTest {
        val r = repo()
        r.connected()
        dev.faults[M.REFILL] = FakeDevicePort.Fault.DROP_BEFORE
        r.refill(110_000)
        val id = rows().single().id
        dev.openFailure = SessionFailure.Transport(CloseReason.Disconnected(8)) // device out of reach
        r.connect()
        dev.openFailure = null
        dev.open("x", cz.nowass.dispenser.device.session.OpenRequest.Session(dev.deviceId)) // a session without resolution
        assertEquals(OpResult.Refused(LocalRefusal.Unresolved(id)), r.refill(110_000))
    }

    // ---- schedules ---------------------------------------------------------------------------

    @Test
    fun schedule_writes_are_read_back_and_mapped() = runTest {
        val r = repo()
        r.connected()
        val c = r.createSchedule(draft()) as OpResult.Ok
        assertEquals("sched-1", c.value.id)
        val list = (r.schedules().first() as Fresh.Live).value
        assertEquals(listOf("Ráno"), list.schedules.map { it.schedule.name })
        assertEquals(setOf(DayOfWeek.MONDAY, DayOfWeek.FRIDAY), (list.schedules[0].schedule.recurrence as cz.nowass.dispenser.domain.Recurrence.Weekly).weekdays)
        assertEquals(2_000L, list.schedules[0].record.amountUl)
        val u = r.updateSchedule("sched-1", 1, draft(ml = 3.5)) as OpResult.Ok
        assertEquals(2L, u.value.revision)
        assertEquals(DeviceErrorCode.CONFLICT, (r.updateSchedule("sched-1", 1, draft()) as OpResult.Rejected).error.code)
        assertTrue(r.deleteSchedule("sched-1", 2) is OpResult.Ok)
        assertTrue((r.schedules().first() as Fresh.Live).value.schedules.isEmpty())
        assertTrue(rows().isEmpty())
    }

    @Test
    fun device_limits_are_checked_before_sending() = runTest {
        val r = repo()
        r.connected()
        val bad = draft(name = "x".repeat(33)).copy(zone = ZoneId.of("UTC"), startDate = LocalDate.of(2101, 1, 1), endDate = LocalDate.of(2102, 1, 1))
        val res = r.createSchedule(bad) as OpResult.Refused
        assertEquals(setOf(DeviceValidation.NAME_TOO_LONG, DeviceValidation.ZONE_NOT_SUPPORTED, DeviceValidation.DATE_OUT_OF_RANGE),
            (res.reason as LocalRefusal.Invalid).problems)
        val big = r.createSchedule(draft(ml = 4_294_967.296)) as OpResult.Refused
        assertEquals(setOf(DeviceValidation.AMOUNT_ABOVE_U32), (big.reason as LocalRefusal.Invalid).problems)
        assertEquals(0, dev.count(M.CREATE_SCHEDULE))
    }

    @Test
    fun uncertain_schedule_writes_are_resolved_by_read_back() = runTest {
        val r = repo()
        r.connected()
        dev.faults[M.CREATE_SCHEDULE] = FakeDevicePort.Fault.EXECUTE_THEN_LOSE
        assertTrue(r.createSchedule(draft()) is OpResult.Uncertain)
        r.connected()
        assertEquals(Resolution.APPLIED, rows().single().resolution)
        r.acknowledge(rows().single().id)

        dev.faults[M.UPDATE_SCHEDULE] = FakeDevicePort.Fault.DROP_BEFORE
        assertTrue(r.updateSchedule("sched-1", 1, draft(ml = 9.0)) is OpResult.Uncertain)
        assertEquals(OpResult.Refused(LocalRefusal.Unresolved(rows().single().id)),
            run { dev.open("x", cz.nowass.dispenser.device.session.OpenRequest.Session(dev.deviceId)); r.deleteSchedule("sched-1", 1) })
        dev.close()
        r.connected()
        assertEquals(Resolution.NOT_APPLIED, rows().single().resolution)
        r.acknowledge(rows().single().id)

        dev.faults[M.DELETE_SCHEDULE] = FakeDevicePort.Fault.EXECUTE_THEN_LOSE
        assertTrue(r.deleteSchedule("sched-1", 1) is OpResult.Uncertain)
        r.connected()
        assertEquals(Resolution.APPLIED, rows().single().resolution)
        r.acknowledge(rows().single().id)

        // changed elsewhere: another phone edited it before the read-back
        r.createSchedule(draft(name = "B"))
        dev.faults[M.UPDATE_SCHEDULE] = FakeDevicePort.Fault.DROP_BEFORE
        r.updateSchedule("sched-2", 1, draft(name = "B2"))
        dev.schedules["sched-2"] = dev.schedules["sched-2"]!! + ("revision" to 5L)
        r.connected()
        assertEquals(Resolution.CHANGED_ELSEWHERE, rows().single().resolution)
    }

    // ---- config, readiness, phones -----------------------------------------------------------

    @Test
    fun config_readiness_and_phone_writes() = runTest {
        val r = repo()
        r.connected()
        val cfg = (r.refreshConfig() as OpResult.Ok).value
        assertEquals(OpResult.Ok(2L), r.setConfig(cfg.revision, cz.nowass.dispenser.device.ops.ConfigRecord.F_MAX_DOSE, cfg.copy(maxDoseUl = 4000)))
        assertEquals(4000L, (r.config().first() as Fresh.Live).value.maxDoseUl) // read back
        assertEquals(DeviceErrorCode.CONFLICT, (r.setConfig(cfg.revision, cz.nowass.dispenser.device.ops.ConfigRecord.F_MAX_DOSE, cfg) as OpResult.Rejected).error.code)
        assertTrue(r.setConfig(2, 0, cfg) is OpResult.Refused)
        assertEquals(Readiness.SERVICE_REQUIRED, (r.setReadiness(Readiness.SERVICE_REQUIRED) as OpResult.Ok).value.readiness)
        dev.phones.add(FakeDevicePort.phone(2, "B", false))
        assertEquals(2, (r.refreshPhones() as OpResult.Ok).value.size)
        assertTrue(r.removePhone(2) is OpResult.Ok)
        assertEquals(1, (r.phones().first() as Fresh.Live).value.size)
        assertTrue(r.removePhone(4) is OpResult.Refused)
        assertTrue(rows().isEmpty())
    }

    @Test
    fun uncertain_config_readiness_and_phone_writes_are_resolved() = runTest {
        val r = repo()
        r.connected()
        val cfg = (r.refreshConfig() as OpResult.Ok).value
        dev.faults[M.SET_CONFIG] = FakeDevicePort.Fault.EXECUTE_THEN_LOSE
        r.setConfig(cfg.revision, cz.nowass.dispenser.device.ops.ConfigRecord.F_LOW_STOCK, cfg.copy(lowStockPercent = 30))
        r.connected()
        assertEquals(Resolution.APPLIED, rows().single().resolution)
        r.acknowledge(rows().single().id)
        dev.faults[M.SET_READINESS] = FakeDevicePort.Fault.DROP_BEFORE
        r.setReadiness(Readiness.SERVICE_REQUIRED)
        r.connected()
        assertEquals(Resolution.NOT_APPLIED, rows().single().resolution)
        r.acknowledge(rows().single().id)
        dev.phones.add(FakeDevicePort.phone(1, "B", false))
        dev.faults[M.PHONE_REMOVE] = FakeDevicePort.Fault.EXECUTE_THEN_LOSE
        r.removePhone(1)
        r.connected()
        assertEquals(Resolution.APPLIED, rows().single().resolution)
    }

    // ---- time (D6), not connected, mapping ---------------------------------------------------

    @Test
    fun set_time_fails_closed_without_a_defensible_uncertainty() = runTest {
        val r = repo()
        r.connected()
        assertEquals(OpResult.Refused(LocalRefusal.TimeUntrusted), r.setTime())
        time = PhoneTimeSource { PhoneTime(1_795_000_000_000L, 2_001) }
        assertEquals(OpResult.Refused(LocalRefusal.TimeUntrusted), r.setTime())
        time = PhoneTimeSource { PhoneTime(1_795_000_000_000L, -1) }
        assertEquals(OpResult.Refused(LocalRefusal.TimeUntrusted), r.setTime())
        assertEquals(0, dev.count(M.SET_TIME))
        time = PhoneTimeSource { PhoneTime(1_795_000_000_000L, 2_000) }
        assertTrue((r.setTime() as OpResult.Ok).value.accepted)
        assertEquals(2_000L, dev.received.single { it.first == M.SET_TIME }.second["uncertainty_ms"])
    }

    @Test
    fun every_operation_without_a_session_is_not_sent() = runTest {
        val r = repo()
        val all: List<suspend () -> OpResult<*>> = listOf(
            { r.refreshStatus() }, { r.refreshDiagnostics() }, { r.ackRejections() }, { r.refreshConfig() },
            { r.setConfig(1, 1, dev.config) }, { r.refreshSchedules() }, { r.createSchedule(draft()) },
            { r.updateSchedule("a", 1, draft()) }, { r.deleteSchedule("a", 1) }, { r.manualDose(1) }, { r.refill(1) },
            { r.setReadiness(Readiness.READY) }, { r.setTime() }, { r.refreshPhones() }, { r.removePhone(1) },
            { r.createInvite("x") }, { r.cancelInvite() }, { r.cancelRecovery() },
        )
        for (op in all) assertEquals(OpResult.NotSent("not connected"), op())
        assertTrue(dev.received.isEmpty())
        assertTrue(rows().isEmpty())
    }

    @Test
    fun provably_unsent_writes_leave_no_row_and_reads_map_results() = runTest {
        val r = repo()
        r.connected()
        dev.faults[M.REFILL] = FakeDevicePort.Fault.NOT_SENT
        assertTrue(r.refill(1) is OpResult.NotSent)
        assertTrue(rows().isEmpty())
        dev.rejects[M.REFILL] = Triple(7, 2, 0) // REFUSED (refill pending): decided, row removed
        assertEquals(cz.nowass.dispenser.device.ops.DeviceError(DeviceErrorCode.REFUSED, 2, 0), (r.refill(1) as OpResult.Rejected).error)
        assertTrue(rows().isEmpty())
        dev.rejects[M.REFILL] = Triple(13, 0, 0) // STORAGE: read back decides
        r.refill(1)
        assertEquals(PendingKind.REFILL, rows().single().kind)
        assertTrue((r.refreshDiagnostics() as OpResult.Ok).value.notifyStorageOk)
        val inv = (r.createInvite("B") as OpResult.Ok).value
        assertEquals(600, inv.expiresS)
        assertTrue(r.createInvite("x".repeat(25)) is OpResult.Refused)
        assertEquals(OpResult.Ok(Unit), r.ackRejections())
    }

    @Test
    fun connect_failure_and_missing_address() = runTest {
        dev.openFailure = SessionFailure.HandshakeFailed
        assertEquals(ConnectOutcome.Failed(SessionFailure.HandshakeFailed), repo().connect())
        val noAddr = DeviceDispenserRepository(dev.deviceId, { null }, dev, journal, cache, seq, UnavailablePhoneTimeSource, backgroundScope)
        assertEquals(ConnectOutcome.NoAddress, noAddr.connect())
        assertFalse(dev.state.value is cz.nowass.dispenser.device.session.SessionState.Authenticated)
    }
}
