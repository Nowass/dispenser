package cz.nowass.dispenser.data

import cz.nowass.dispenser.domain.RecurrenceKind
import cz.nowass.dispenser.domain.ScheduleDraft
import cz.nowass.dispenser.domain.SyncState
import cz.nowass.dispenser.domain.ValidationError
import cz.nowass.dispenser.domain.toDraft
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.async
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.test.advanceTimeBy
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.time.LocalDate
import java.time.LocalTime
import kotlin.time.Duration
import kotlin.time.Duration.Companion.milliseconds

@OptIn(ExperimentalCoroutinesApi::class)
class MockDispenserRepositoryTest {

    private val draft = ScheduleDraft(
        name = "Test",
        amountMl = 7.5,
        startDate = LocalDate.of(2026, 11, 20),
        localTime = LocalTime.of(9, 15),
        kind = RecurrenceKind.DAILY,
    )

    private fun repo(ids: List<String> = listOf("n1", "n2", "n3"), delay: Duration = Duration.ZERO): MockDispenserRepository {
        val it = ids.iterator()
        return MockDispenserRepository(idGenerator = { it.next() }, simulatedAckDelay = delay)
    }

    @Test
    fun create_assignsIdAndRevision1_andEmits() = runTest {
        val r = repo()
        val result = r.createSchedule(draft) as ScheduleWriteResult.Success
        assertEquals("n1", result.schedule!!.id)
        assertEquals(1, result.schedule!!.revision)
        assertTrue(r.observeSchedules().first().any { it.id == "n1" && it.amountMl == 7.5 })
        assertEquals(SyncState.SIMULATED_ACK, r.observeScheduleSync().first()["n1"])
    }

    @Test
    fun create_isPendingUntilSimulatedAck() = runTest {
        val r = repo(delay = 1000.milliseconds)
        val call = async { r.createSchedule(draft) }
        advanceTimeBy(500)
        assertEquals(SyncState.PENDING, r.observeScheduleSync().first()["n1"])
        call.await()
        assertEquals(SyncState.SIMULATED_ACK, r.observeScheduleSync().first()["n1"])
    }

    @Test
    fun create_skipsCollidingGeneratedId() = runTest {
        val r = repo(ids = listOf("demo-morning", "fresh"))
        val result = r.createSchedule(draft) as ScheduleWriteResult.Success
        assertEquals("fresh", result.schedule!!.id)
        assertEquals(1, r.observeSchedules().first().count { it.id == "demo-morning" })
    }

    @Test
    fun invalidDraft_isRejected_withoutChanges() = runTest {
        val r = repo()
        val before = r.observeSchedules().first()
        val result = r.createSchedule(draft.copy(amountMl = 0.0))
        assertEquals(ScheduleWriteResult.Invalid(setOf(ValidationError.AMOUNT_NOT_POSITIVE)), result)
        assertEquals(before, r.observeSchedules().first())
    }

    @Test
    fun update_checksRevision_andIncrementsIt() = runTest {
        val r = repo()
        val morning = r.observeSchedules().first().single { it.id == "demo-morning" }
        val ok = r.updateSchedule(morning.id, morning.revision, morning.toDraft().copy(amountMl = 12.0)) as ScheduleWriteResult.Success
        assertEquals(morning.revision + 1, ok.schedule!!.revision)
        assertEquals(12.0, ok.schedule!!.amountMl, 0.0)
        assertEquals(morning.startDate, ok.schedule!!.startDate)

        // Stale writer still holding the old revision is rejected.
        val stale = r.updateSchedule(morning.id, morning.revision, morning.toDraft().copy(amountMl = 99.0))
        assertEquals(ScheduleWriteResult.Conflict, stale)
        assertEquals(12.0, r.observeSchedules().first().single { it.id == morning.id }.amountMl, 0.0)
    }

    @Test
    fun update_unknownId_isNotFound() = runTest {
        assertEquals(ScheduleWriteResult.NotFound, repo().updateSchedule("nope", 1, draft))
    }

    @Test
    fun update_doesNotDuplicateSchedules() = runTest {
        val r = repo()
        val count = r.observeSchedules().first().size
        val s = r.observeSchedules().first().first()
        r.updateSchedule(s.id, s.revision, s.toDraft().copy(enabled = !s.enabled))
        assertEquals(count, r.observeSchedules().first().size)
        assertEquals(count, r.observeSchedules().first().map { it.id }.toSet().size)
    }

    @Test
    fun delete_checksRevision_removesSchedule_keepsHistory() = runTest {
        val r = repo()
        val events = r.observeEvents().first()
        val morning = r.observeSchedules().first().single { it.id == "demo-morning" }

        assertEquals(ScheduleWriteResult.Conflict, r.deleteSchedule(morning.id, morning.revision + 5))
        val ok = r.deleteSchedule(morning.id, morning.revision) as ScheduleWriteResult.Success
        assertNull(ok.schedule)
        assertTrue(r.observeSchedules().first().none { it.id == morning.id })
        assertTrue(morning.id !in r.observeScheduleSync().first())
        assertEquals(events, r.observeEvents().first())
        assertEquals(ScheduleWriteResult.NotFound, r.deleteSchedule(morning.id, morning.revision))
    }
}
