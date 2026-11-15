package cz.nowass.dispenser.ui.calendar

import androidx.lifecycle.SavedStateHandle
import cz.nowass.dispenser.data.LocalDispenserRepository
import cz.nowass.dispenser.data.MockDispenserRepository
import cz.nowass.dispenser.data.MockSeed
import cz.nowass.dispenser.data.ScheduleStore
import cz.nowass.dispenser.data.StoreSnapshot
import cz.nowass.dispenser.data.StoreWriteOutcome
import cz.nowass.dispenser.data.StoredSchedule
import cz.nowass.dispenser.domain.DEFAULT_ZONE
import cz.nowass.dispenser.domain.SyncState
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.filterIsInstance
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.flowOf
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.UnconfinedTestDispatcher
import kotlinx.coroutines.test.resetMain
import kotlinx.coroutines.test.runTest
import kotlinx.coroutines.test.setMain
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Before
import org.junit.Test
import java.time.Clock

@OptIn(ExperimentalCoroutinesApi::class)
class CalendarUnreadableTest {

    @Before fun setUp() = Dispatchers.setMain(UnconfinedTestDispatcher())
    @After fun tearDown() = Dispatchers.resetMain()

    /** Store reporting corrupted records alongside one valid schedule. */
    private class CorruptedStore(private val valid: StoredSchedule, private val unreadable: Int) : ScheduleStore {
        override fun observe(): Flow<StoreSnapshot> = flowOf(StoreSnapshot(listOf(valid), unreadable))
        override suspend fun insert(stored: StoredSchedule) = false
        override suspend fun replace(stored: StoredSchedule, expectedRevision: Int) = StoreWriteOutcome.NOT_FOUND
        override suspend fun delete(id: String, expectedRevision: Int) = StoreWriteOutcome.NOT_FOUND
    }

    @Test
    fun unreadableCount_reachesCalendarState_validSchedulesStillShown() = runTest {
        val seed = MockSeed.default()
        val repo = LocalDispenserRepository(
            CorruptedStore(StoredSchedule(seed.schedules.first(), SyncState.PENDING), unreadable = 2),
            Clock.fixed(seed.device.now, DEFAULT_ZONE),
            clockTick = null,
        )
        val vm = CalendarViewModel(repo, SavedStateHandle())
        backgroundScope.launch { vm.uiState.collect {} }
        val s = vm.uiState.filterIsInstance<CalendarUiState.Ready>().first()
        assertEquals(2, s.unreadableScheduleCount)
        assertEquals(1, s.schedules.size)
    }

    @Test
    fun healthyStore_reportsZero() = runTest {
        val vm = CalendarViewModel(MockDispenserRepository(), SavedStateHandle())
        backgroundScope.launch { vm.uiState.collect {} }
        assertEquals(0, vm.uiState.filterIsInstance<CalendarUiState.Ready>().first().unreadableScheduleCount)
    }
}
