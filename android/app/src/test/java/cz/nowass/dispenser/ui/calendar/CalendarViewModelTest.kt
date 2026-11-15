package cz.nowass.dispenser.ui.calendar

import androidx.lifecycle.SavedStateHandle
import cz.nowass.dispenser.data.MockDispenserRepository
import cz.nowass.dispenser.data.MockSeed
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.filterIsInstance
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.UnconfinedTestDispatcher
import kotlinx.coroutines.test.resetMain
import kotlinx.coroutines.test.runTest
import kotlinx.coroutines.test.setMain
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import java.time.DayOfWeek
import java.time.LocalDate
import java.time.LocalTime
import java.time.YearMonth

@OptIn(ExperimentalCoroutinesApi::class)
class CalendarViewModelTest {

    private val seed = MockSeed.default()
    private val today = LocalDate.of(2026, 11, 14) // seed device clock date

    @Before
    fun setUp() = Dispatchers.setMain(UnconfinedTestDispatcher())

    @After
    fun tearDown() = Dispatchers.resetMain()

    private fun build(selected: LocalDate?, mode: CalendarMode = CalendarMode.MONTH, device: cz.nowass.dispenser.domain.DeviceStatus = seed.device) =
        CalendarViewModel.buildState(device, seed.schedules, selected, mode)

    /** Keeps uiState subscribed (WhileSubscribed) and returns the latest Ready state. */
    private suspend fun TestScope.ready(vm: CalendarViewModel): CalendarUiState.Ready {
        backgroundScope.launch { vm.uiState.collect {} }
        return vm.uiState.filterIsInstance<CalendarUiState.Ready>().first()
    }

    @Test
    fun monthGrid_isSixMondayFirstWeeks_withAdjacentMonthDaysMarked() {
        val s = build(selected = null)
        assertEquals(today, s.selectedDate)
        assertEquals(YearMonth.of(2026, 11), s.visibleMonth)
        assertEquals(CalendarViewModel.MONTH_GRID_DAYS, s.days.size)
        assertEquals(LocalDate.of(2026, 10, 26), s.days.first().date)
        assertEquals(DayOfWeek.MONDAY, s.days.first().date.dayOfWeek)
        assertFalse(s.days.first().inVisibleMonth)
        val todayCell = s.days.single { it.isToday }
        assertTrue(todayCell.isSelected)
        assertEquals(1, s.days.count { it.isSelected })
    }

    @Test
    fun monthGrid_countsComeFromSchedules() {
        val s = build(selected = null)
        fun count(d: Int) = s.days.single { it.date == LocalDate.of(2026, 11, d) }.doseCount
        assertEquals(0, count(1))   // before seed start date
        assertEquals(2, count(16))  // Monday: daily + weekly
        assertEquals(2, count(19))  // Thursday
        assertEquals(1, count(17))
        assertEquals(15.0, s.days.single { it.date == LocalDate.of(2026, 11, 16) }.totalMl, 0.0)
    }

    @Test
    fun selectedDay_listsOccurrencesInOrder_withTotal() {
        val s = build(selected = LocalDate.of(2026, 11, 16))
        val occ = s.selectedDay.occurrences
        assertEquals(listOf("Ranní dávka", "Večerní doplnění"), occ.map { it.scheduleName })
        assertEquals(listOf(LocalTime.of(8, 0), LocalTime.of(19, 30)), occ.map { it.localTime })
        assertEquals(15.0, s.selectedDay.totalMl, 0.0)
        assertTrue(occ.none { it.isPast })
        assertEquals(occ.size, occ.map { it.key }.toSet().size)
    }

    @Test
    fun pastFlag_followsDeviceClock_notOutcome() {
        val at0900 = seed.device.copy(now = seed.device.now.plusSeconds(90 * 60))
        val s = build(selected = today, device = at0900)
        assertTrue(s.selectedDay.occurrences.single().isPast)
        assertTrue(build(selected = LocalDate.of(2026, 11, 13)).selectedDay.occurrences.all { it.isPast })
    }

    @Test
    fun emptyDay_hasNoOccurrences() {
        val s = build(selected = LocalDate.of(2026, 11, 1))
        assertTrue(s.selectedDay.occurrences.isEmpty())
        assertEquals(0.0, s.selectedDay.totalMl, 0.0)
    }

    @Test
    fun weekMode_showsMondayToSundayOfSelection() {
        val s = build(selected = LocalDate.of(2026, 11, 18), mode = CalendarMode.WEEK)
        assertEquals(7, s.days.size)
        assertEquals(LocalDate.of(2026, 11, 16), s.days.first().date)
        assertEquals(LocalDate.of(2026, 11, 22), s.days.last().date)
        assertTrue(s.days.all { it.inVisibleMonth })
        assertEquals(listOf(2, 1, 1, 2, 1, 1, 1), s.days.map { it.doseCount })
    }

    @Test
    fun navArgument_selectsDate_andInvalidArgumentFallsBackToToday() = runTest {
        val vm = CalendarViewModel(MockDispenserRepository(), SavedStateHandle(mapOf(CalendarViewModel.ARG_DATE to "2026-11-19")))
        assertEquals(LocalDate.of(2026, 11, 19), ready(vm).selectedDate)

        val bad = CalendarViewModel(MockDispenserRepository(), SavedStateHandle(mapOf(CalendarViewModel.ARG_DATE to "not-a-date")))
        assertEquals(today, ready(bad).selectedDate)
    }

    @Test
    fun shiftPeriod_movesByMonthOrWeek_andClampsMonthEnd() = runTest {
        val vm = CalendarViewModel(MockDispenserRepository(), SavedStateHandle(mapOf(CalendarViewModel.ARG_DATE to "2027-01-31")))
        ready(vm)
        vm.shiftPeriod(forward = true)
        assertEquals(LocalDate.of(2027, 2, 28), ready(vm).selectedDate)
        vm.setMode(CalendarMode.WEEK)
        vm.shiftPeriod(forward = false)
        assertEquals(LocalDate.of(2027, 2, 21), ready(vm).selectedDate)
        vm.goToToday()
        assertEquals(today, ready(vm).selectedDate)
    }

    @Test
    fun selectionAndMode_surviveRecreationViaSavedState() = runTest {
        val handle = SavedStateHandle()
        val first = CalendarViewModel(MockDispenserRepository(), handle)
        ready(first)
        first.selectDate(LocalDate.of(2026, 12, 3))
        first.setMode(CalendarMode.WEEK)

        val recreated = CalendarViewModel(MockDispenserRepository(), handle)
        val s = ready(recreated)
        assertEquals(LocalDate.of(2026, 12, 3), s.selectedDate)
        assertEquals(CalendarMode.WEEK, s.mode)
    }

    @Test
    fun setEnabled_showsDisabledScheduleInCalendar_andBumpsRevision() = runTest {
        val repo = MockDispenserRepository()
        val vm = CalendarViewModel(repo, SavedStateHandle(mapOf(CalendarViewModel.ARG_DATE to "2026-11-21")))
        val before = ready(vm)
        assertTrue(before.schedules.any { it.schedule.id == "demo-weekend" && !it.schedule.enabled })
        assertTrue(before.selectedDay.occurrences.none { it.scheduleId == "demo-weekend" })

        vm.setEnabled("demo-weekend", true)
        val after = ready(vm)
        val item = after.schedules.single { it.schedule.id == "demo-weekend" }
        assertTrue(item.schedule.enabled)
        assertEquals(3, item.schedule.revision)
        assertTrue(after.selectedDay.occurrences.any { it.scheduleId == "demo-weekend" })
        assertEquals(after.schedules.size, after.schedules.map { it.schedule.id }.toSet().size)
    }

    @Test
    fun monthGrid_leapFebruary_andYearBoundaryPaging() {
        val feb = build(selected = LocalDate.of(2028, 2, 29))
        assertEquals(YearMonth.of(2028, 2), feb.visibleMonth)
        assertEquals(29, feb.days.count { it.inVisibleMonth })
        assertEquals(LocalDate.of(2028, 1, 31), feb.days.first().date) // Monday before 1 Feb 2028
        val dec = build(selected = LocalDate.of(2026, 12, 31), mode = CalendarMode.WEEK)
        assertEquals(LocalDate.of(2026, 12, 28), dec.days.first().date)
        assertEquals(LocalDate.of(2027, 1, 3), dec.days.last().date)
    }
}
