package cz.nowass.dispenser.ui

import android.content.Context
import androidx.lifecycle.SavedStateHandle
import androidx.room.Room
import androidx.test.core.app.ApplicationProvider
import cz.nowass.dispenser.data.LocalDispenserRepository
import cz.nowass.dispenser.data.MockSeed
import cz.nowass.dispenser.data.ScheduleWriteResult
import cz.nowass.dispenser.data.local.DispenserDatabase
import cz.nowass.dispenser.data.local.RoomScheduleStore
import cz.nowass.dispenser.domain.DEFAULT_ZONE
import cz.nowass.dispenser.domain.RecurrenceKind
import cz.nowass.dispenser.domain.ScheduleDraft
import cz.nowass.dispenser.domain.toDraft
import cz.nowass.dispenser.ui.calendar.CalendarUiState
import cz.nowass.dispenser.ui.calendar.CalendarViewModel
import cz.nowass.dispenser.ui.dashboard.DashboardUiState
import cz.nowass.dispenser.ui.dashboard.DashboardViewModel
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.test.UnconfinedTestDispatcher
import kotlinx.coroutines.test.resetMain
import kotlinx.coroutines.test.setMain
import kotlinx.coroutines.withTimeout
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import java.time.Clock
import java.time.LocalDate
import java.time.LocalTime

/** Calendar and dashboard ViewModels react to writes persisted in Room. */
@OptIn(ExperimentalCoroutinesApi::class)
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35])
class RoomBackedViewModelTest {

    private lateinit var db: DispenserDatabase
    private lateinit var repo: LocalDispenserRepository
    private val collectors = CoroutineScope(Dispatchers.Default)

    @Before
    fun setUp() {
        Dispatchers.setMain(UnconfinedTestDispatcher())
        db = Room.inMemoryDatabaseBuilder(ApplicationProvider.getApplicationContext<Context>(), DispenserDatabase::class.java).build()
        val clock = Clock.fixed(MockSeed.START, DEFAULT_ZONE) // Sat 14 Nov 2026 07:30
        repo = LocalDispenserRepository(RoomScheduleStore(db.scheduleDao(), clock), clock, clockTick = null)
    }

    @After
    fun tearDown() {
        collectors.cancel()
        db.close()
        Dispatchers.resetMain()
    }

    private suspend fun <T> StateFlow<T>.await(predicate: (T) -> Boolean): T {
        collectors.launch { collect {} } // keep WhileSubscribed sharing active
        return withTimeout(5_000) { first(predicate) }
    }

    private val monday = LocalDate.of(2026, 11, 16)
    private val draft = ScheduleDraft("Ranní", 10.0, LocalDate.of(2026, 11, 14), LocalTime.of(8, 0), RecurrenceKind.DAILY)

    @Test
    fun calendarAndDashboard_followCreateEditDisableDelete() = runBlocking {
        val calendar = CalendarViewModel(repo, SavedStateHandle(mapOf(CalendarViewModel.ARG_DATE to monday.toString())))
        val dashboard = DashboardViewModel(repo)

        // Empty database: no occurrences, no next dose.
        val empty = calendar.uiState.await { it is CalendarUiState.Ready } as CalendarUiState.Ready
        assertTrue(empty.selectedDay.occurrences.isEmpty() && empty.schedules.isEmpty())
        assertEquals(null, (dashboard.uiState.await { it is DashboardUiState.Ready } as DashboardUiState.Ready).nextDose)

        val s = (repo.createSchedule(draft) as ScheduleWriteResult.Success).schedule!!
        calendar.uiState.await { (it as? CalendarUiState.Ready)?.selectedDay?.occurrences?.size == 1 }
        val withDose = dashboard.uiState.await { (it as? DashboardUiState.Ready)?.nextDose != null } as DashboardUiState.Ready
        assertEquals("Ranní", withDose.nextDose!!.scheduleName)
        assertEquals(LocalTime.of(8, 0), withDose.nextDose.at.toLocalTime())

        // Edit time -> calendar shows the new time.
        repo.updateSchedule(s.id, 1, s.toDraft().copy(localTime = LocalTime.of(21, 15)))
        calendar.uiState.await {
            (it as? CalendarUiState.Ready)?.selectedDay?.occurrences?.singleOrNull()?.localTime == LocalTime.of(21, 15)
        }

        // Disable -> no occurrences, still listed (disabled) in schedule management.
        val current = repo.observeSchedules().first().single()
        repo.updateSchedule(s.id, current.revision, current.toDraft().copy(enabled = false))
        val disabled = calendar.uiState.await {
            (it as? CalendarUiState.Ready)?.let { r -> r.selectedDay.occurrences.isEmpty() && r.schedules.singleOrNull()?.schedule?.enabled == false } == true
        } as CalendarUiState.Ready
        assertEquals(s.id, disabled.schedules.single().schedule.id)
        dashboard.uiState.await { (it as? DashboardUiState.Ready)?.nextDose == null }

        // Delete -> gone everywhere.
        repo.deleteSchedule(s.id, current.revision + 1)
        calendar.uiState.await { (it as? CalendarUiState.Ready)?.schedules?.isEmpty() == true }
        Unit
    }
}
