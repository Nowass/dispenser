package cz.nowass.dispenser.data.device

import cz.nowass.dispenser.device.ops.ConfigRecord
import cz.nowass.dispenser.device.ops.LocalRefusal
import cz.nowass.dispenser.device.ops.MessageType as M
import cz.nowass.dispenser.device.ops.OpResult
import cz.nowass.dispenser.domain.RecurrenceKind
import cz.nowass.dispenser.domain.ScheduleDraft
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.async
import kotlinx.coroutines.launch
import kotlinx.coroutines.flow.Flow
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

/**
 * Review of M1.10e.3 (findings 1 and 2): the pending-operation guard under concurrent callers, and
 * journal resolution that cannot complete. Interleavings are forced deterministically: [GatedJournal]
 * parks the first caller right after its guard read (the window between "no unresolved row" and the
 * insert of its own row), while a second caller runs.
 */
class DeviceRepositoryConcurrencyTest {
    private val dev = FakeDevicePort()
    private val journal = GatedJournal(InMemoryPendingOpsJournal())
    private val cache = InMemoryDeviceCacheStore()
    private val seq = TestSeqAllocator()
    private var ids = 0

    /** Delegating journal: can park the next guard read after its snapshot, or fail reads/writes. */
    class GatedJournal(val inner: InMemoryPendingOpsJournal) : PendingOpsJournal {
        private var gate: CompletableDeferred<Unit>? = null
        @Volatile var failList = false
        var failUpdate: Boolean
            get() = inner.failUpdate
            set(v) { inner.failUpdate = v }

        /** Completed when a caller is parked at the gate (for tests on real threads). */
        @Volatile var parked = CompletableDeferred<Unit>()

        fun parkNextList(): CompletableDeferred<Unit> {
            parked = CompletableDeferred()
            return CompletableDeferred<Unit>().also { gate = it }
        }

        override fun observe(deviceId: String): Flow<List<PendingOp>> = inner.observe(deviceId)
        override suspend fun list(deviceId: String): List<PendingOp> {
            if (failList) throw java.io.IOException("injected journal read failure")
            val snapshot = inner.list(deviceId)
            gate?.let { g -> gate = null; parked.complete(Unit); g.await() }
            return snapshot
        }
        override suspend fun insert(op: PendingOp): Long = inner.insert(op)
        override suspend fun update(op: PendingOp) = inner.update(op)
        override suspend fun delete(id: Long) = inner.delete(id)
    }

    private fun TestScope.repo(scope: CoroutineScope = backgroundScope) = DeviceDispenserRepository(
        dev.deviceId, { "A0:B1:C2:D3:E4:F5" }, dev, journal, cache, seq, { null }, scope,
        nowMs = { 1_795_000_000_000L }, newScheduleId = { "sched-${++ids}" },
    )

    private fun draft(name: String) = ScheduleDraft(name, 2.0, LocalDate.of(2026, 12, 1), LocalTime.of(8, 0),
        RecurrenceKind.WEEKLY, 1, setOf(DayOfWeek.MONDAY))

    private fun refusedSameOperation(r: OpResult<*>) =
        r is OpResult.Refused && (r.reason is LocalRefusal.InProgress || r.reason is LocalRefusal.Unresolved)

    /**
     * Runs [first] until it is parked right after its guard read, then [second] to completion, then
     * releases [first]. Returns both results.
     */
    private suspend fun <A, B> TestScope.interleave(first: suspend () -> A, second: suspend () -> B): Pair<A, B> {
        val gate = journal.parkNextList()
        val a = async { first() }
        runCurrent() // first: guard read done, parked before inserting its row
        val b = async { second() }
        runCurrent()
        gate.complete(Unit)
        return a.await() to b.await()
    }

    // ---- finding 2: concurrent mutations -----------------------------------------------------

    @Test
    fun concurrent_manual_doses_never_both_reach_the_device() = runTest {
        dev.doseClosesLink = false // the device answers (DONE) and keeps the link: the worst case
        val r = repo()
        assertTrue(r.connect() is ConnectOutcome.Connected)
        val (a, b) = interleave({ r.manualDose(1_000) }, { r.manualDose(1_000) })
        assertEquals("pump runs ($a / $b)", 1, dev.pumpRuns)
        assertEquals(1, dev.count(M.MANUAL_DOSE))
        assertTrue("one confirmed: $a / $b", (a is OpResult.Ok) xor (b is OpResult.Ok))
        assertTrue("the other refused as the same operation: $a / $b", refusedSameOperation(a) || refusedSameOperation(b))
        assertEquals("one seq allocated", 1, seq.calls)
    }

    @Test
    fun concurrent_refills_send_one_refill() = runTest {
        val r = repo()
        assertTrue(r.connect() is ConnectOutcome.Connected)
        val (a, b) = interleave({ r.refill(100_000) }, { r.refill(120_000) })
        assertEquals("refills ($a / $b)", 1, dev.refills)
        assertEquals(1, dev.count(M.REFILL))
        assertTrue("$a / $b", refusedSameOperation(a) || refusedSameOperation(b))
    }

    @Test
    fun concurrent_config_writes_send_one_set_config() = runTest {
        val r = repo()
        assertTrue(r.connect() is ConnectOutcome.Connected)
        val cfg = (r.refreshConfig() as OpResult.Ok).value
        val (a, b) = interleave(
            { r.setConfig(cfg.revision, ConfigRecord.F_MAX_DOSE, cfg.copy(maxDoseUl = 900)) },
            { r.setConfig(cfg.revision, ConfigRecord.F_MAX_DOSE, cfg.copy(maxDoseUl = 800)) },
        )
        assertEquals("SET_CONFIG frames ($a / $b)", 1, dev.count(M.SET_CONFIG))
        assertTrue("$a / $b", refusedSameOperation(a) || refusedSameOperation(b))
    }

    @Test
    fun concurrent_writes_of_the_same_schedule_send_one_write() = runTest {
        val r = repo()
        assertTrue(r.connect() is ConnectOutcome.Connected)
        val id = (r.createSchedule(draft("A")) as OpResult.Ok).value.id
        val (a, b) = interleave({ r.updateSchedule(id, 1, draft("B")) }, { r.deleteSchedule(id, 1) })
        assertEquals("schedule writes ($a / $b)", 1, dev.count(M.UPDATE_SCHEDULE) + dev.count(M.DELETE_SCHEDULE))
        assertTrue("$a / $b", refusedSameOperation(a) || refusedSameOperation(b))
        // A different schedule is a different target: not blocked.
        val other = (r.createSchedule(draft("C")) as OpResult.Ok).value.id
        val (c, d) = interleave({ r.updateSchedule(id, 2, draft("D")) }, { r.updateSchedule(other, 1, draft("E")) })
        assertTrue("$c", c is OpResult.Ok || c is OpResult.Rejected) // revision may have moved; it was sent
        assertTrue("$d", d is OpResult.Ok)
    }

    @Test
    fun an_operation_in_progress_does_not_block_other_kinds_or_reads() = runTest {
        val r = repo()
        assertTrue(r.connect() is ConnectOutcome.Connected)
        val (a, b) = interleave({ r.refill(100_000) }, { r.refreshStatus() })
        assertTrue("$a", a is OpResult.Ok)
        assertTrue("$b", b is OpResult.Ok)
        val cfg = (r.refreshConfig() as OpResult.Ok).value
        val (c, d) = interleave({ r.refill(90_000) }, { r.setConfig(cfg.revision, ConfigRecord.F_MAX_DOSE, cfg.copy(maxDoseUl = 700)) })
        assertTrue("$c", c is OpResult.Ok)
        assertTrue("$d", d is OpResult.Ok)
    }

    @Test
    fun the_reservation_is_released_after_every_outcome() = runTest {
        val r = repo()
        assertTrue(r.connect() is ConnectOutcome.Connected)
        dev.rejects[M.REFILL] = Triple(7, 0, 0) // REFUSED
        assertTrue(r.refill(100_000) is OpResult.Rejected)
        assertTrue("a refused refill releases the reservation", r.refill(100_000) is OpResult.Ok)
        dev.faults[M.REFILL] = FakeDevicePort.Fault.NOT_SENT
        assertTrue(r.refill(100_000) is OpResult.NotSent)
        assertTrue(r.refill(110_000) is OpResult.Ok)
        assertEquals(2, dev.refills)
    }

    // ---- finding 1: incomplete journal resolution --------------------------------------------

    @Test
    fun resolution_that_cannot_write_reports_incomplete_and_keeps_the_guard() = runTest {
        val r = repo()
        assertTrue(r.connect() is ConnectOutcome.Connected)
        dev.faults[M.REFILL] = FakeDevicePort.Fault.EXECUTE_THEN_LOSE
        assertTrue(r.refill(100_000) is OpResult.Uncertain) // row kept, session closed
        journal.failUpdate = true
        val c = r.connect() as ConnectOutcome.Connected
        assertFalse("resolution could not record its result", c.resolutionComplete)
        assertEquals(1, c.unresolved)
        // the unresolved REFILL row still blocks a second REFILL; reads stay available
        assertTrue(refusedSameOperation(r.refill(100_000)))
        assertTrue(r.refreshStatus() is OpResult.Ok)
        assertEquals(1, dev.refills)
        // journal writable again: connect() in the SAME session retries the resolution
        journal.failUpdate = false
        val again = r.connect() as ConnectOutcome.Connected
        assertTrue(again.resolutionComplete)
        assertEquals(0, again.unresolved)
        assertTrue(r.refill(120_000) is OpResult.Ok)
    }

    @Test
    fun an_unreadable_journal_fails_every_mutation_closed_but_allows_reads() = runTest {
        val r = repo()
        journal.failList = true
        val c = r.connect() as ConnectOutcome.Connected
        assertFalse(c.resolutionComplete)
        assertEquals("unknown", -1, c.unresolved)
        val before = dev.received.size
        for (res in listOf(r.manualDose(1_000), r.refill(100_000), r.updateSchedule("x", 1, draft("X")), r.deleteSchedule("x", 1))) {
            assertTrue("$res", res is OpResult.Refused && res.reason is LocalRefusal.PersistenceFailed)
        }
        val cfg = ConfigRecord(1, 200_000, 150_000, 20, 24, 0, 1000, 0, 5000, 20000, 10000, 1, 30000)
        assertTrue(r.setConfig(1, ConfigRecord.F_MAX_DOSE, cfg).let { it is OpResult.Refused && it.reason is LocalRefusal.PersistenceFailed })
        assertEquals("no mutation frame sent", before, dev.received.size)
        assertTrue(r.refreshStatus() is OpResult.Ok)
    }

    @Test
    fun an_interrupted_resolution_keeps_the_dose_guard_and_never_re_issues() = runTest {
        val r = repo()
        assertTrue(r.connect() is ConnectOutcome.Connected)
        dev.faults[M.MANUAL_DOSE] = FakeDevicePort.Fault.DROP_BEFORE // not executed, outcome unknown to the phone
        assertTrue(r.manualDose(1_000) is OpResult.Uncertain)
        // next session: GET_RESULT is lost, so the resolution stops early
        dev.faults[M.GET_RESULT] = FakeDevicePort.Fault.DROP_BEFORE
        val c = r.connect() as ConnectOutcome.Connected
        assertFalse(c.resolutionComplete)
        assertEquals(1, c.unresolved)
        assertTrue("${r.manualDose(1_000)}", r.manualDose(1_000) !is OpResult.Ok)
        // and the next one resolves it (NOT_DECIDED -> awaiting the user), never re-sending by itself
        val c2 = r.connect() as ConnectOutcome.Connected
        assertTrue(c2.resolutionComplete)
        assertEquals(1, c2.unresolved)
        assertTrue(refusedSameOperation(r.manualDose(1_000)))
        assertEquals(0, dev.pumpRuns)
        assertEquals(1, dev.count(M.MANUAL_DOSE))
    }

    @Test
    fun a_refused_read_back_leaves_the_resolution_incomplete() = runTest {
        val r = repo()
        assertTrue(r.connect() is ConnectOutcome.Connected)
        dev.faults[M.MANUAL_DOSE] = FakeDevicePort.Fault.DROP_BEFORE
        assertTrue(r.manualDose(1_000) is OpResult.Uncertain)
        dev.rejects[M.GET_RESULT] = Triple(6, 0, 0) // BUSY: the device answered, but not with a result
        val c = r.connect() as ConnectOutcome.Connected
        assertFalse("a PREPARED row is left", c.resolutionComplete)
        assertEquals(1, c.unresolved)
        assertTrue(refusedSameOperation(r.manualDose(1_000)))
        val c2 = r.connect() as ConnectOutcome.Connected
        assertTrue(c2.resolutionComplete)
        assertEquals(0, dev.pumpRuns)
    }

    // ---- review of 4d7e3b2: caller cancelled while the manager-owned request continues ------

    /** Holds the next request of [type] inside the device until the returned gate completes. */
    private fun holdNext(type: Int): Pair<CompletableDeferred<Unit>, CompletableDeferred<Unit>> {
        val reached = CompletableDeferred<Unit>()
        val gate = CompletableDeferred<Unit>()
        dev.hooks[type] = { reached.complete(Unit); gate.await() }
        return reached to gate
    }

    private suspend fun unresolvedRows(kind: PendingKind) = journal.inner.list(dev.deviceId).filter { it.unresolved && it.kind == kind }

    @Test
    fun refill_cancelled_caller_keeps_the_guard_until_the_outcome_is_settled() = runTest {
        val r = repo()
        assertTrue(r.connect() is ConnectOutcome.Connected)
        val (reached, gate) = holdNext(M.REFILL)
        val caller = launch { r.refill(100_000) }
        runCurrent()
        assertTrue("the REFILL request is in flight", reached.isCompleted)
        val row = unresolvedRows(PendingKind.REFILL).single() // durable before sending
        caller.cancel()
        runCurrent()
        assertTrue(caller.isCancelled)

        // the caller is gone, the request is not: a second REFILL is refused by the journal, nothing sent
        val sent = dev.received.size
        assertEquals(OpResult.Refused(LocalRefusal.Unresolved(row.id)), r.refill(120_000))
        assertEquals(sent, dev.received.size)
        assertEquals(0, dev.refills)

        // the original continues, is executed once and settled by its own outcome (Ok: row removed)
        gate.complete(Unit)
        runCurrent()
        assertEquals(1, dev.refills)
        assertEquals(100_000L, dev.remainingUl)
        assertTrue(unresolvedRows(PendingKind.REFILL).isEmpty())
        // only now a new REFILL is allowed
        assertTrue(r.refill(130_000) is OpResult.Ok)
        assertEquals(2, dev.refills)
    }

    @Test
    fun refill_cancelled_caller_with_an_uncertain_outcome_blocks_until_read_back() = runTest {
        val r = repo()
        assertTrue(r.connect() is ConnectOutcome.Connected)
        val (reached, gate) = holdNext(M.REFILL)
        dev.faults[M.REFILL] = FakeDevicePort.Fault.EXECUTE_THEN_LOSE // executed, the answer is lost
        val caller = launch { r.refill(100_000) }
        runCurrent()
        assertTrue(reached.isCompleted)
        caller.cancel()
        runCurrent()
        gate.complete(Unit)
        runCurrent()
        assertEquals(1, dev.refills)
        val row = unresolvedRows(PendingKind.REFILL).single()
        assertEquals("kept for read-back", PendingState.PREPARED, row.state)
        // the link closed with the lost answer; the next session resolves it before a new REFILL is allowed
        assertTrue(r.refill(120_000).let { it is OpResult.NotSent || it is OpResult.Refused })
        val c = r.connect() as ConnectOutcome.Connected
        assertTrue(c.resolutionComplete)
        val resolved = journal.inner.list(dev.deviceId).single { it.id == row.id }
        assertEquals(PendingState.RESOLVED, resolved.state)
        assertEquals(Resolution.READ_BACK, resolved.resolution)
        assertTrue(r.refill(120_000) is OpResult.Ok)
        assertEquals("never re-sent automatically", 2, dev.refills)
    }

    @Test
    fun manual_dose_cancelled_caller_device_closes_link_resolved_by_get_result() = runTest {
        val r = repo()
        assertTrue(r.connect() is ConnectOutcome.Connected)
        val (reached, gate) = holdNext(M.MANUAL_DOSE)
        val caller = launch { r.manualDose(1_000) }
        runCurrent()
        assertTrue("the MANUAL_DOSE request is in flight", reached.isCompleted)
        val row = unresolvedRows(PendingKind.MANUAL_DOSE).single()
        caller.cancel()
        runCurrent()
        assertTrue(caller.isCancelled)

        val sent = dev.received.size
        val seqCalls = seq.calls
        assertEquals(OpResult.Refused(LocalRefusal.Unresolved(row.id)), r.manualDose(1_000))
        assertEquals("nothing sent, not even the status read", sent, dev.received.size)
        assertEquals("no seq allocated", seqCalls, seq.calls)

        // the original dose runs once; the device suspends BLE and closes the link (normal path)
        gate.complete(Unit)
        runCurrent()
        assertEquals(1, dev.pumpRuns)
        assertEquals(PendingState.PREPARED, journal.inner.list(dev.deviceId).single { it.id == row.id }.state)
        assertTrue(r.manualDose(1_000).let { it is OpResult.NotSent || it is OpResult.Refused }) // no session / unresolved
        // the next session settles it by GET_RESULT{seq} (DONE); only then is a new dose possible, with a new seq
        assertTrue((r.connect() as ConnectOutcome.Connected).resolutionComplete)
        val settled = journal.inner.list(dev.deviceId).single { it.id == row.id }
        assertEquals(PendingState.RESOLVED, settled.state)
        assertEquals(Resolution.DONE, settled.resolution)
        dev.doseClosesLink = false
        val next = r.manualDose(1_000)
        assertTrue("$next", next is OpResult.Ok)
        assertEquals(2, dev.pumpRuns)
        assertTrue("a fresh seq", dev.received.filter { it.first == M.MANUAL_DOSE }.map { it.second["seq"] as Long }.distinct().size == 2)
    }

    @Test
    fun manual_dose_cancelled_caller_device_answers_settled_in_the_same_session() = runTest {
        dev.doseClosesLink = false
        val r = repo()
        assertTrue(r.connect() is ConnectOutcome.Connected)
        val (reached, gate) = holdNext(M.MANUAL_DOSE)
        val caller = launch { r.manualDose(1_000) }
        runCurrent()
        assertTrue(reached.isCompleted)
        val row = unresolvedRows(PendingKind.MANUAL_DOSE).single()
        caller.cancel()
        runCurrent()
        assertEquals(OpResult.Refused(LocalRefusal.Unresolved(row.id)), r.manualDose(1_000))
        gate.complete(Unit)
        runCurrent()
        assertEquals(1, dev.pumpRuns)
        assertTrue("decided and answered: row removed", journal.inner.list(dev.deviceId).none { it.id == row.id })
        assertTrue(r.manualDose(1_000) is OpResult.Ok)
        assertEquals(2, dev.pumpRuns)
    }
}
