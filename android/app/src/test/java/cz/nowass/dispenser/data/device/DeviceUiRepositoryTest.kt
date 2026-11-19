package cz.nowass.dispenser.data.device

import cz.nowass.dispenser.data.CommandResult
import cz.nowass.dispenser.data.ScheduleWriteResult
import cz.nowass.dispenser.device.ops.DeviceError
import cz.nowass.dispenser.device.ops.DeviceErrorCode
import cz.nowass.dispenser.device.ops.DeviceValidation
import cz.nowass.dispenser.device.ops.LocalRefusal
import cz.nowass.dispenser.device.ops.OpResult
import cz.nowass.dispenser.device.session.SessionState
import cz.nowass.dispenser.domain.ConnectionState
import cz.nowass.dispenser.domain.DataSource
import cz.nowass.dispenser.domain.DeviceAccess
import cz.nowass.dispenser.domain.Freshness
import cz.nowass.dispenser.domain.RecurrenceKind
import cz.nowass.dispenser.domain.ScheduleDraft
import cz.nowass.dispenser.domain.SyncState
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.time.Clock
import java.time.Instant
import java.time.LocalDate
import java.time.LocalTime
import java.time.ZoneOffset

/** DEVICE-mode adapter (M1.10e.4a): freshness, connection and every outcome stay explicit. */
class DeviceUiRepositoryTest {
    private val now = Instant.parse("2026-11-19T10:00:00Z")
    private val readAt = Instant.parse("2026-11-19T09:30:00Z")
    private val fake = FakeDeviceRepository()
    private val repo = DeviceUiRepository(fake, Clock.fixed(now, ZoneOffset.UTC), clockTick = null)
    private val draft = ScheduleDraft("Ranní", 2.0, LocalDate.of(2027, 1, 4), LocalTime.of(8, 0), RecurrenceKind.DAILY)

    @Test
    fun never_read_is_unknown_and_nothing_is_invented() = runTest {
        val d = repo.observeDevice().first()
        assertEquals(DataSource.DEVICE, d.source)
        assertEquals(ConnectionState.DISCONNECTED, d.connection)
        assertEquals(Freshness.Unknown, d.freshness)
        assertNull(d.batteryPercent)
        assertFalse(d.simulationActive)
        assertEquals(now, d.now)
        assertNull(repo.observeReservoir().first())
        assertTrue(repo.observeSchedules().first().isEmpty())
        assertEquals(Freshness.Unknown, repo.observeScheduleFreshness().first())
    }

    @Test
    fun live_values_only_while_connected_cached_values_keep_their_read_time() = runTest {
        fake.authenticated()
        fake.statusF.value = Fresh.Live(FakeDeviceRepository.status(), readAt, 7)
        var d = repo.observeDevice().first()
        assertEquals(ConnectionState.CONNECTED, d.connection)
        assertEquals(Freshness.Live(readAt), d.freshness)
        assertEquals(80, d.batteryPercent)
        val r = repo.observeReservoir().first()!!
        assertEquals(200_000L, r.capacityUl)
        assertEquals(150_000L, r.remainingUl)
        assertEquals(readAt, r.updatedAt)

        // The session ends: the repository reports the same value as Cached; the adapter never says Live.
        fake.connection.value = SessionState.Idle
        fake.statusF.value = Fresh.Cached(FakeDeviceRepository.status(), readAt)
        d = repo.observeDevice().first()
        assertEquals(ConnectionState.DISCONNECTED, d.connection)
        assertEquals(Freshness.Cached(readAt), d.freshness)
        assertEquals(80, d.batteryPercent)
    }

    @Test
    fun connection_state_is_per_device() = runTest {
        fake.connection.value = SessionState.Authenticated("ffffffffffff", 3, 0)
        assertEquals(ConnectionState.DISCONNECTED, repo.observeDevice().first().connection)
        fake.connection.value = SessionState.Connecting(fake.deviceId)
        assertEquals(ConnectionState.CONNECTING, repo.observeDevice().first().connection)
        fake.connection.value = SessionState.Handshaking(fake.deviceId, "Session")
        assertEquals(ConnectionState.CONNECTING, repo.observeDevice().first().connection)
        fake.connection.value = SessionState.ConfirmingOnDevice(fake.deviceId, 30)
        assertEquals(ConnectionState.CONNECTING, repo.observeDevice().first().connection)
        fake.connection.value = SessionState.Handshaking("ffffffffffff", "Session")
        assertEquals(ConnectionState.DISCONNECTED, repo.observeDevice().first().connection)
    }

    @Test
    fun a_status_without_usable_capacity_has_no_reservoir_estimate() = runTest {
        fake.statusF.value = Fresh.Cached(FakeDeviceRepository.status(capacityUl = 0, remainingUl = 0), readAt)
        assertNull(repo.observeReservoir().first())
        fake.statusF.value = Fresh.Cached(FakeDeviceRepository.status(capacityUl = 100, remainingUl = 101), readAt)
        assertNull(repo.observeReservoir().first())
    }

    @Test
    fun schedules_are_the_device_set_device_confirmed_with_freshness() = runTest {
        val s = FakeDeviceRepository.schedule(draft, "id-1", 3)
        fake.schedulesF.value = Fresh.Cached(DeviceSchedules(9, listOf(s), unreadable = 2), readAt)
        assertEquals(listOf(s.schedule), repo.observeSchedules().first())
        assertEquals(mapOf("id-1" to SyncState.DEVICE_APPLIED), repo.observeScheduleSync().first())
        assertEquals(2, repo.observeUnreadableScheduleCount().first())
        assertEquals(Freshness.Cached(readAt), repo.observeScheduleFreshness().first())
    }

    @Test
    fun no_device_history_and_writes_need_a_connection() = runTest {
        assertFalse(repo.historyAvailable)
        assertTrue(repo.writesRequireConnection)
        assertTrue(repo.observeHistory().first().isEmpty())
    }

    @Test
    fun schedule_write_outcomes_stay_explicit() = runTest {
        suspend fun check(op: OpResult<ScheduleWrite>, expected: ScheduleWriteResult) {
            fake.writeOutcome = op
            assertEquals(op.toString(), expected, repo.updateSchedule("id-1", 3, draft))
            assertEquals(op.toString(), expected, repo.createSchedule(draft))
        }
        // Uncertain is never a failure, and never Conflict/NotFound (which would invite a retry or a reload).
        check(OpResult.Uncertain("lost reply", journalId = 4), ScheduleWriteResult.Uncertain)
        check(OpResult.NotSent("not connected"), ScheduleWriteResult.NotSent)
        check(OpResult.Rejected(DeviceError(DeviceErrorCode.CONFLICT, 0, 0)), ScheduleWriteResult.Conflict)
        check(OpResult.Rejected(DeviceError(DeviceErrorCode.NOT_FOUND, 0, 0)), ScheduleWriteResult.NotFound)
        check(OpResult.Rejected(DeviceError(DeviceErrorCode.LIMIT, 0, 0)), ScheduleWriteResult.DeviceRejected(DeviceErrorCode.LIMIT.wire))
        check(OpResult.Refused(LocalRefusal.Unresolved(4)), ScheduleWriteResult.AwaitingVerification)
        check(OpResult.Refused(LocalRefusal.InProgress), ScheduleWriteResult.AwaitingVerification)
        check(OpResult.Refused(LocalRefusal.Invalid(setOf(DeviceValidation.NAME_TOO_LONG))), ScheduleWriteResult.DeviceLimit(setOf("NAME_TOO_LONG")))
        check(OpResult.Refused(LocalRefusal.SessionChanged), ScheduleWriteResult.NotSent)
        check(OpResult.Refused(LocalRefusal.PersistenceFailed("journal")), ScheduleWriteResult.NotSent)

        fake.writeOutcome = OpResult.Uncertain("lost reply", journalId = 5)
        assertEquals(ScheduleWriteResult.Uncertain, repo.deleteSchedule("id-1", 3))
    }

    @Test
    fun a_confirmed_write_returns_the_device_read_back() = runTest {
        val readBack = FakeDeviceRepository.schedule(draft.copy(name = "Ranní (zařízení)"), "id-1", 4)
        fake.schedulesF.value = Fresh.Live(DeviceSchedules(10, listOf(readBack)), readAt, 7)
        fake.writeOutcome = OpResult.Ok(ScheduleWrite("id-1", 4, 10))
        assertEquals(ScheduleWriteResult.Success(readBack.schedule), repo.updateSchedule("id-1", 3, draft))
        // Not in the re-read set (yet): built from the draft with the device's revision.
        fake.writeOutcome = OpResult.Ok(ScheduleWrite("id-2", 1, 11))
        val created = repo.createSchedule(draft) as ScheduleWriteResult.Success
        assertEquals("id-2", created.schedule!!.id)
        assertEquals(1, created.schedule!!.revision)
        // A confirmed delete has no schedule (the editor reports "deleted").
        fake.writeOutcome = OpResult.Ok(ScheduleWrite("id-1", null, 12))
        assertEquals(ScheduleWriteResult.Success(null), repo.deleteSchedule("id-1", 4))
    }

    @Test
    fun recovery_required_keeps_values_and_unresolved_operations_visible_and_blocks_every_mutation() = runTest {
        for (lost in listOf(DeviceAccess.KEY_MISSING, DeviceAccess.RECORD_UNREADABLE, DeviceAccess.RECORD_MISSING)) {
            val f = FakeDeviceRepository()
            val access = kotlinx.coroutines.flow.MutableStateFlow(lost)
            val r = DeviceUiRepository(f, Clock.fixed(now, ZoneOffset.UTC), clockTick = null, access = access)
            f.authenticated() // even a (stale) session never makes it look connected
            f.statusF.value = Fresh.Cached(FakeDeviceRepository.status(), readAt)
            f.pendingF.value = listOf(
                PendingOp(id = 1, deviceId = f.deviceId, kind = PendingKind.MANUAL_DOSE, createdAtMs = 1, seq = 5, volumeUl = 1000),
                PendingOp(id = 2, deviceId = f.deviceId, kind = PendingKind.REFILL, state = PendingState.RESOLVED, createdAtMs = 2),
            )
            f.writeOutcome = OpResult.Ok(ScheduleWrite("x", 1, 1))
            f.refillOutcome = OpResult.Ok(FakeDeviceRepository.status())
            val d = r.observeDevice().first()
            assertEquals("$lost", ConnectionState.RECOVERY_REQUIRED, d.connection)
            assertEquals(lost, d.access)
            assertEquals("unresolved rows stay visible", 1, d.unresolvedOperations)
            assertEquals(Freshness.Cached(readAt), d.freshness)
            assertEquals(80, d.batteryPercent)
            assertEquals(ScheduleWriteResult.NotSent, r.createSchedule(draft))
            assertEquals(ScheduleWriteResult.NotSent, r.updateSchedule("id-1", 3, draft))
            assertEquals(ScheduleWriteResult.NotSent, r.deleteSchedule("id-1", 3))
            assertEquals(CommandResult.NotSent, r.refillReservoir(100_000))
            assertTrue("nothing reached the device repository: ${f.calls}", f.calls.isEmpty())
            // Credential usable again (e.g. after re-enrollment): normal state.
            access.value = DeviceAccess.OK
            assertEquals(ConnectionState.CONNECTED, r.observeDevice().first().connection)
        }
    }

    @Test
    fun refill_outcomes_stay_explicit() = runTest {
        suspend fun check(op: OpResult<cz.nowass.dispenser.device.ops.StatusRecord>, expected: CommandResult) {
            fake.refillOutcome = op
            assertEquals(op.toString(), expected, repo.refillReservoir(100_000))
        }
        check(OpResult.Ok(FakeDeviceRepository.status()), CommandResult.Done(0))
        check(OpResult.Uncertain("lost reply", 3), CommandResult.Uncertain)
        check(OpResult.NotSent("not connected"), CommandResult.NotSent)
        check(OpResult.Refused(LocalRefusal.Unresolved(3)), CommandResult.AwaitingVerification)
        check(OpResult.Refused(LocalRefusal.InProgress), CommandResult.AwaitingVerification)
        check(OpResult.Refused(LocalRefusal.SessionChanged), CommandResult.NotSent)
        check(OpResult.Refused(LocalRefusal.Invalid(setOf(DeviceValidation.REMAINING_ABOVE_CAPACITY))), CommandResult.NotAccepted)
        check(OpResult.Rejected(DeviceError(DeviceErrorCode.REFUSED, 0, 0)), CommandResult.DeviceRejected(DeviceErrorCode.REFUSED.wire))
        assertEquals(8, fake.calls.count { it == "refill 100000" })
    }
}
