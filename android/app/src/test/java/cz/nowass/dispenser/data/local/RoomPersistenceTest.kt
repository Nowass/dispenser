package cz.nowass.dispenser.data.local

import android.content.Context
import androidx.test.core.app.ApplicationProvider
import cz.nowass.dispenser.data.LocalDispenserRepository
import cz.nowass.dispenser.data.MockSeed
import cz.nowass.dispenser.data.ScheduleWriteResult
import cz.nowass.dispenser.data.StoredSchedule
import cz.nowass.dispenser.domain.DEFAULT_ZONE
import cz.nowass.dispenser.domain.Recurrence
import cz.nowass.dispenser.domain.RecurrenceKind
import cz.nowass.dispenser.domain.Schedule
import cz.nowass.dispenser.domain.ScheduleDraft
import cz.nowass.dispenser.domain.SyncState
import cz.nowass.dispenser.domain.toDraft
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import java.time.Clock
import java.time.DayOfWeek
import java.time.LocalDate
import java.time.LocalTime

/** File-backed Room database: survives closing/reopening, like an app restart. */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35])
class RoomPersistenceTest {

    private val context: Context = ApplicationProvider.getApplicationContext()
    private val clock = Clock.fixed(MockSeed.START, DEFAULT_ZONE)
    private val name = "persistence-test.db"
    private val open = mutableListOf<DispenserDatabase>()

    @Before
    fun clean() { context.deleteDatabase(name) }

    @After
    fun close() { open.forEach { it.close() }; context.deleteDatabase(name) }

    private fun openDb() = DispenserDatabase.build(context, name).also { open += it }

    private fun repo(db: DispenserDatabase, invalid: MutableList<String>? = null) = LocalDispenserRepository(
        store = RoomScheduleStore(db.scheduleDao(), clock, onInvalidRow = { id, _ -> invalid?.add(id) }),
        clock = clock,
        clockTick = null,
    )

    private val daily = ScheduleDraft("Ranní", 10.0, LocalDate.of(2026, 11, 16), LocalTime.of(8, 0), RecurrenceKind.DAILY, interval = 2)
    private val weekly = ScheduleDraft(
        "Víkend", 12.345, LocalDate.of(2026, 11, 16), LocalTime.of(10, 30), RecurrenceKind.WEEKLY,
        weekdays = setOf(DayOfWeek.SATURDAY, DayOfWeek.SUNDAY), endDate = LocalDate.of(2027, 3, 31),
    )
    private val once = ScheduleDraft("Jednorázová", 0.5, LocalDate.of(2026, 12, 24), LocalTime.of(18, 0), RecurrenceKind.ONCE)

    @Test
    fun freshInstallation_isEmpty() = runBlocking {
        val r = repo(openDb())
        assertTrue(r.observeSchedules().first().isEmpty())
        assertTrue(r.observeScheduleSync().first().isEmpty())
    }

    @Test
    fun schedulesSurviveRestart_withRevisionsAndPendingState() = runBlocking {
        val db1 = openDb()
        val r1 = repo(db1)
        val a = (r1.createSchedule(daily) as ScheduleWriteResult.Success).schedule!!
        val b = (r1.createSchedule(weekly) as ScheduleWriteResult.Success).schedule!!
        val c = (r1.createSchedule(once) as ScheduleWriteResult.Success).schedule!!
        r1.updateSchedule(a.id, 1, a.toDraft().copy(localTime = LocalTime.of(7, 45)))
        r1.updateSchedule(b.id, 1, b.toDraft().copy(enabled = false))
        r1.deleteSchedule(c.id, 1)
        val before = r1.observeSchedules().first()
        db1.close()

        // "Restart": new database instance and repository on the same file.
        val r2 = repo(openDb())
        val after = r2.observeSchedules().first()
        assertEquals(before.sortedBy { it.id }, after.sortedBy { it.id })
        assertEquals(2, after.size)
        assertTrue(after.none { it.id == c.id })
        val restoredA = after.single { it.id == a.id }
        assertEquals(2, restoredA.revision)
        assertEquals(LocalTime.of(7, 45), restoredA.localTime)
        val restoredB = after.single { it.id == b.id }
        assertEquals(false, restoredB.enabled)
        assertEquals(12.345, restoredB.amountMl, 0.0)
        assertEquals(Recurrence.Weekly(setOf(DayOfWeek.SATURDAY, DayOfWeek.SUNDAY)), restoredB.recurrence)
        assertEquals(LocalDate.of(2027, 3, 31), restoredB.endDate)
        assertEquals(setOf(SyncState.PENDING), r2.observeScheduleSync().first().values.toSet())

        // Optimistic revision survives restart too.
        assertEquals(ScheduleWriteResult.Conflict, r2.updateSchedule(a.id, 1, restoredA.toDraft()))
    }

    @Test
    fun revisionConflict_isEnforcedByDatabase_acrossRepositories() = runBlocking {
        val db = openDb()
        val writer1 = repo(db)
        val writer2 = repo(db) // separate repository = separate in-process lock
        val s = (writer1.createSchedule(daily) as ScheduleWriteResult.Success).schedule!!
        assertTrue(writer1.updateSchedule(s.id, 1, s.toDraft().copy(amountMl = 11.0)) is ScheduleWriteResult.Success)
        assertEquals(ScheduleWriteResult.Conflict, writer2.updateSchedule(s.id, 1, s.toDraft().copy(amountMl = 99.0)))
        assertEquals(11.0, writer2.observeSchedules().first().single().amountMl, 0.0)
    }

    @Test
    fun amountIsStoredAsIntegerMicrolitres() = runBlocking {
        val db = openDb()
        val s = (repo(db).createSchedule(weekly) as ScheduleWriteResult.Success).schedule!!
        assertEquals(12_345L, db.scheduleDao().get(s.id)!!.amountMicrolitres)
    }

    @Test
    fun corruptedRows_areSkippedAndReported_validRowsStillLoad() = runBlocking {
        val db = openDb()
        val dao = db.scheduleDao()
        val valid = Schedule("ok", 1, "Platný", true, 5.0, LocalDate.of(2026, 11, 16), LocalTime.of(8, 0), Recurrence.Daily())
        val good = ScheduleEntityMapper.toEntity(StoredSchedule(valid, SyncState.PENDING), 0)
        dao.insert(good)
        dao.insert(good.copy(id = "bad-date", startDate = "not-a-date"))
        dao.insert(good.copy(id = "bad-kind", recurrenceKind = "MONTHLY"))
        dao.insert(good.copy(id = "bad-amount", amountMicrolitres = -1))
        dao.insert(good.copy(id = "bad-weekly", recurrenceKind = "WEEKLY", weekdaysMask = 0))

        val invalid = mutableListOf<String>()
        val loaded = repo(db, invalid).observeSchedules().first()
        assertEquals(listOf(valid), loaded)
        assertEquals(setOf("bad-date", "bad-kind", "bad-amount", "bad-weekly"), invalid.toSet())
    }

    @Test
    fun corruptedRows_areCountedForTheUser_keptUntouched_andLoggedOnce() = runBlocking {
        val db = openDb()
        val dao = db.scheduleDao()
        val valid = Schedule("ok", 1, "Platný", true, 5.0, LocalDate.of(2026, 11, 16), LocalTime.of(8, 0), Recurrence.Daily())
        val good = ScheduleEntityMapper.toEntity(StoredSchedule(valid, SyncState.PENDING), 0)
        dao.insert(good)
        val bad1 = good.copy(id = "bad-1", startDate = "not-a-date")
        val bad2 = good.copy(id = "bad-2", recurrenceKind = "MONTHLY")
        dao.insert(bad1)
        dao.insert(bad2)

        val reported = mutableListOf<String>()
        val r = repo(db, reported)
        assertEquals(2, r.observeUnreadableScheduleCount().first())
        assertEquals(listOf(valid), r.observeSchedules().first())

        // A later write re-emits but does not re-log the same corrupted rows, and never touches them.
        r.createSchedule(daily)
        assertEquals(2, r.observeUnreadableScheduleCount().first())
        assertEquals(listOf("bad-1", "bad-2"), reported.sorted())
        assertEquals(bad1, dao.get("bad-1"))
        assertEquals(bad2, dao.get("bad-2"))
    }
}
