package cz.nowass.dispenser.data

import cz.nowass.dispenser.domain.Recurrence
import cz.nowass.dispenser.domain.RecurrenceKind
import cz.nowass.dispenser.domain.ScheduleDraft
import cz.nowass.dispenser.domain.SyncState
import cz.nowass.dispenser.domain.ValidationError
import cz.nowass.dispenser.domain.toDraft
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.time.DayOfWeek
import java.time.LocalDate
import java.time.LocalTime

/**
 * Behaviour every [ScheduleStore]-backed repository must satisfy. Subclasses provide
 * the store (in-memory, Room). Uses runBlocking because Room emits on its own threads.
 */
abstract class ScheduleRepositoryContract {

    protected abstract fun newRepository(ids: List<String> = listOf("n1", "n2", "n3")): LocalDispenserRepository

    protected val draft = ScheduleDraft(
        name = "Test",
        amountMl = 7.5,
        startDate = LocalDate.of(2026, 11, 20),
        localTime = LocalTime.of(9, 15),
        kind = RecurrenceKind.WEEKLY,
        interval = 2,
        weekdays = setOf(DayOfWeek.MONDAY, DayOfWeek.FRIDAY),
        endDate = LocalDate.of(2027, 2, 1),
    )

    private suspend fun LocalDispenserRepository.schedules() = observeSchedules().first()

    @Test
    fun create_assignsIdRevision1_andStoresPending() = runBlocking {
        val r = newRepository()
        val created = (r.createSchedule(draft) as ScheduleWriteResult.Success).schedule!!
        assertEquals("n1", created.id)
        assertEquals(1, created.revision)
        assertEquals(listOf(created), r.schedules())
        assertEquals(mapOf("n1" to SyncState.PENDING), r.observeScheduleSync().first())
        assertEquals(Recurrence.Weekly(setOf(DayOfWeek.MONDAY, DayOfWeek.FRIDAY), 2), created.recurrence)
    }

    @Test
    fun create_retriesOnIdCollision() = runBlocking {
        val r = newRepository(ids = listOf("dup", "dup", "fresh"))
        r.createSchedule(draft)
        val second = (r.createSchedule(draft) as ScheduleWriteResult.Success).schedule!!
        assertEquals("fresh", second.id)
        assertEquals(2, r.schedules().size)
    }

    @Test
    fun invalidDraft_isRejected_withoutChanges() = runBlocking {
        val r = newRepository()
        assertEquals(ScheduleWriteResult.Invalid(setOf(ValidationError.AMOUNT_NOT_POSITIVE)), r.createSchedule(draft.copy(amountMl = 0.0)))
        assertEquals(ScheduleWriteResult.Invalid(setOf(ValidationError.AMOUNT_TOO_PRECISE)), r.createSchedule(draft.copy(amountMl = 1.2345)))
        assertTrue(r.schedules().isEmpty())
    }

    @Test
    fun update_checksRevision_incrementsIt_staysPending() = runBlocking {
        val r = newRepository()
        val s = (r.createSchedule(draft) as ScheduleWriteResult.Success).schedule!!
        val ok = (r.updateSchedule(s.id, 1, s.toDraft().copy(amountMl = 12.345, localTime = LocalTime.of(20, 0))) as ScheduleWriteResult.Success).schedule!!
        assertEquals(2, ok.revision)
        assertEquals(listOf(ok), r.schedules())
        assertEquals(12.345, r.schedules().single().amountMl, 0.0)
        assertEquals(SyncState.PENDING, r.observeScheduleSync().first()[s.id])

        assertEquals(ScheduleWriteResult.Conflict, r.updateSchedule(s.id, 1, s.toDraft().copy(amountMl = 99.0)))
        assertEquals(12.345, r.schedules().single().amountMl, 0.0)
        assertEquals(ScheduleWriteResult.NotFound, r.updateSchedule("missing", 1, draft))
    }

    @Test
    fun enableDisable_togglesFlag_bumpsRevision_noDuplicates() = runBlocking {
        val r = newRepository()
        val s = (r.createSchedule(draft) as ScheduleWriteResult.Success).schedule!!
        r.updateSchedule(s.id, 1, s.toDraft().copy(enabled = false))
        assertEquals(false, r.schedules().single().enabled)
        r.updateSchedule(s.id, 2, r.schedules().single().toDraft().copy(enabled = true))
        val back = r.schedules().single()
        assertEquals(true, back.enabled)
        assertEquals(3, back.revision)
        assertEquals(s.copy(revision = 3), back) // nothing else changed
    }

    @Test
    fun delete_checksRevision_removesSchedule() = runBlocking {
        val r = newRepository()
        val s = (r.createSchedule(draft) as ScheduleWriteResult.Success).schedule!!
        assertEquals(ScheduleWriteResult.Conflict, r.deleteSchedule(s.id, 7))
        val ok = r.deleteSchedule(s.id, 1) as ScheduleWriteResult.Success
        assertNull(ok.schedule)
        assertTrue(r.schedules().isEmpty())
        assertTrue(r.observeScheduleSync().first().isEmpty())
        assertEquals(ScheduleWriteResult.NotFound, r.deleteSchedule(s.id, 1))
    }

    @Test
    fun onceAtNonexistentDstTime_isRejectedOnCreateAndUpdate() = runBlocking {
        val r = newRepository()
        val gap = draft.copy(kind = RecurrenceKind.ONCE, startDate = LocalDate.of(2027, 3, 28), localTime = LocalTime.of(2, 15), endDate = null)
        assertEquals(ScheduleWriteResult.Invalid(setOf(ValidationError.TIME_NONEXISTENT)), r.createSchedule(gap))
        val s = (r.createSchedule(draft) as ScheduleWriteResult.Success).schedule!!
        assertEquals(ScheduleWriteResult.Invalid(setOf(ValidationError.TIME_NONEXISTENT)), r.updateSchedule(s.id, 1, gap))
        assertEquals(listOf(s), r.schedules())
    }

    @Test
    fun noUnreadableRecords_forValidData() = runBlocking {
        val r = newRepository()
        r.createSchedule(draft)
        assertEquals(0, r.observeUnreadableScheduleCount().first())
    }
}
