package cz.nowass.dispenser.ui.dashboard

import cz.nowass.dispenser.data.MockDispenserRepository
import cz.nowass.dispenser.data.MockSeed
import cz.nowass.dispenser.domain.DEFAULT_ZONE
import cz.nowass.dispenser.domain.DataSource
import cz.nowass.dispenser.ui.components.StatusTone
import java.time.LocalDate
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.filterIsInstance
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
import java.time.ZonedDateTime

@OptIn(ExperimentalCoroutinesApi::class)
class DashboardViewModelTest {

    @Before
    fun setUp() = Dispatchers.setMain(UnconfinedTestDispatcher())

    @After
    fun tearDown() = Dispatchers.resetMain()

    @Test
    fun seededMock_producesExpectedDashboard() = runTest {
        val vm = DashboardViewModel(MockDispenserRepository())

        val state = vm.uiState.filterIsInstance<DashboardUiState.Ready>().first()

        assertTrue(state.isDemo)
        // Seed clock is Sat 14 Nov 2026 07:30 -> daily 08:00 schedule is next.
        assertEquals("Ranní dávka", state.nextDose?.scheduleName)
        assertEquals(10.0, state.nextDose!!.amountMl, 0.0)
        assertEquals(ZonedDateTime.of(2026, 11, 14, 8, 0, 0, 0, DEFAULT_ZONE), state.nextDose.at)
        assertEquals(312.0, state.reservoir!!.estimatedRemainingMl, 0.0)
        assertEquals(312f / 500f, state.reservoir!!.fraction, 1e-6f)
        assertTrue(state.reservoir!!.coversNextDose)
        assertEquals(78, state.battery.percent)
        assertTrue(state.battery.isSimulated)
    }

    @Test
    fun reservoirBelowNextDose_isFlaggedAsNotCovering() {
        val seed = MockSeed.default()
        val low = seed.reservoir.copy(remainingUl = 9500) // next dose is 10 ml
        val state = DashboardViewModel.toUiState(seed.device, seed.schedules, low)
        assertFalse(state.reservoir!!.coversNextDose)
    }

    @Test
    fun reservoirExactlyEqualToNextDose_covers() {
        val seed = MockSeed.default()
        val exact = seed.reservoir.copy(remainingUl = 10000)
        assertTrue(DashboardViewModel.toUiState(seed.device, seed.schedules, exact).reservoir!!.coversNextDose)
    }

    @Test
    fun noEnabledSchedules_noNextDose_andReservoirCovers() {
        val seed = MockSeed.default()
        val state = DashboardViewModel.toUiState(
            seed.device,
            seed.schedules.map { it.copy(enabled = false) },
            seed.reservoir.copy(remainingUl = 0),
        )
        assertEquals(null, state.nextDose)
        assertTrue(state.reservoir!!.coversNextDose)
    }

    @Test
    fun mockSeed_isEntirelyTaggedDemo() {
        val seed = MockSeed.default()
        assertEquals(DataSource.DEMO, seed.device.source)
        assertTrue(seed.events.all { it.origin == cz.nowass.dispenser.domain.simulation.EventOrigin.SIMULATION })
    }

    // ---- A1.1.1 presentation mapping ----

    @Test
    fun weekPreview_seed_countsRealUpcomingOccurrences() {
        val seed = MockSeed.default()
        val week = DashboardViewModel.toUiState(seed.device, seed.schedules, seed.reservoir).week

        assertEquals(7, week.size)
        assertEquals(LocalDate.of(2026, 11, 14), week.first().date)
        assertTrue(week.first().isToday)
        assertTrue(week.drop(1).none { it.isToday })
        // Daily 08:00 every day; Mon 16 + Thu 19 add the 19:30 weekly dose; disabled schedule ignored.
        assertEquals(listOf(1, 1, 2, 1, 1, 2, 1), week.map { it.doseCount })
        assertEquals(80.0, week.sumOf { it.totalMl }, 0.0)
        assertEquals(15.0, week[2].totalMl, 0.0)
    }

    @Test
    fun weekPreview_excludesOccurrencesAtOrBeforeNow() {
        val seed = MockSeed.default()
        // Exactly at today's 08:00: that occurrence is no longer upcoming.
        val atEight = seed.device.copy(now = seed.device.now.plusSeconds(30 * 60))
        val week = DashboardViewModel.weekPreview(seed.schedules, atEight)
        assertEquals(0, week.first().doseCount)
    }

    @Test
    fun weekPreview_noEnabledSchedules_isEmptyButHasSevenDays() {
        val seed = MockSeed.default()
        val week = DashboardViewModel.weekPreview(seed.schedules.map { it.copy(enabled = false) }, seed.device)
        assertEquals(7, week.size)
        assertTrue(week.all { it.doseCount == 0 && it.totalMl == 0.0 })
    }

    @Test
    fun reservoirTone_normalLowBlocked() {
        val seed = MockSeed.default()
        fun tone(ml: Double) = DashboardViewModel.toUiState(
            seed.device, seed.schedules, seed.reservoir.copy(remainingUl = Math.round(ml * 1000)),
        ).reservoir!!.tone
        assertEquals(StatusTone.Normal, tone(312.0))
        assertEquals(StatusTone.Normal, tone(100.0)) // exactly 20 % is not low
        assertEquals(StatusTone.Low, tone(60.0))
        assertEquals(StatusTone.Blocked, tone(9.9)) // below the 10 ml next dose
    }

    @Test
    fun batteryTone_unknownWhenNull() {
        val seed = MockSeed.default()
        val unknown = DashboardViewModel.toUiState(seed.device.copy(batteryPercent = null), seed.schedules, seed.reservoir)
        assertEquals(null, unknown.battery.percent)
        assertEquals(StatusTone.Unknown, unknown.battery.tone)
        assertEquals(StatusTone.Normal, DashboardViewModel.toUiState(seed.device, seed.schedules, seed.reservoir).battery.tone)
    }

    @Test
    fun unknownReservoir_whenNoSimulation_isNull_neverInvented() {
        val seed = MockSeed.default()
        assertEquals(null, DashboardViewModel.toUiState(seed.device, seed.schedules, reservoir = null).reservoir)
    }

    @Test
    fun blockedReservoir_isBlockedTone_evenWithEnoughVolume() {
        val seed = MockSeed.default()
        val blocked = seed.reservoir.copy(blocked = true)
        assertEquals(StatusTone.Blocked, DashboardViewModel.toUiState(seed.device, seed.schedules, blocked).reservoir!!.tone)
        assertFalse(DashboardViewModel.toUiState(seed.device, seed.schedules, blocked).reservoir!!.coversNextDose)
    }
}
