package cz.nowass.dispenser.ui.reservoir

import cz.nowass.dispenser.data.MockDispenserRepository
import cz.nowass.dispenser.domain.simulation.PredictionStatus
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.filterIsInstance
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.UnconfinedTestDispatcher
import kotlinx.coroutines.test.resetMain
import kotlinx.coroutines.test.runTest
import kotlinx.coroutines.test.setMain
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class ReservoirViewModelTest {
    @Before fun setUp() = Dispatchers.setMain(UnconfinedTestDispatcher())
    @After fun tearDown() = Dispatchers.resetMain()

    @Test
    fun activeSimulation_showsPrediction_andValidatesRefill() = runTest {
        val repo = MockDispenserRepository()
        val vm = ReservoirViewModel(repo)
        backgroundScope.launch { vm.uiState.collect {} }
        val s = vm.uiState.filterIsInstance<ReservoirUiState.Ready>().first()
        assertEquals(312_000L, s.reservoir!!.remainingUl)
        assertEquals(20, s.lowThresholdPercent)
        assertTrue(s.prediction!!.status == PredictionStatus.DEPLETES_WITHIN_HORIZON)

        assertEquals(RefillError.ABOVE_CAPACITY, vm.validateRefill("500,001"))
        assertEquals(RefillError.NOT_POSITIVE, vm.validateRefill("0"))
        assertEquals(RefillError.TOO_PRECISE, vm.validateRefill("12,3456"))
        assertEquals(RefillError.INVALID, vm.validateRefill("1e3"))
        assertNull(vm.validateRefill("500"))

        vm.refill("450")
        assertEquals(450_000L, repo.observeReservoir().first()!!.remainingUl)
        assertTrue(vm.refillDone.value)
    }

    @Test
    fun inactiveSimulation_unknownReservoir_noRefill() = runTest {
        val vm = ReservoirViewModel(MockDispenserRepository(simulationActive = false))
        backgroundScope.launch { vm.uiState.collect {} }
        val s = vm.uiState.filterIsInstance<ReservoirUiState.Ready>().first()
        assertNull(s.reservoir)
        assertNull(s.prediction)
        assertEquals(RefillError.NOT_ACTIVE, vm.validateRefill("100"))
    }
}
