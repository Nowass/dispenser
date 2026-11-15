package cz.nowass.dispenser.ui.settings

import cz.nowass.dispenser.data.MockDispenserRepository
import cz.nowass.dispenser.domain.DEFAULT_ZONE
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
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import java.time.LocalDateTime

@OptIn(ExperimentalCoroutinesApi::class)
class SimulationSettingsViewModelTest {
    @Before fun setUp() = Dispatchers.setMain(UnconfinedTestDispatcher())
    @After fun tearDown() = Dispatchers.resetMain()

    @Test
    fun start_validatesInput_thenRuns_andCommandsReportResults() = runTest {
        val repo = MockDispenserRepository(simulationActive = false)
        val vm = SimulationSettingsViewModel(repo.simulation, repo)
        backgroundScope.launch { vm.uiState.collect {} }
        assertTrue(vm.uiState.filterIsInstance<SimulationUiState.Inactive>().first().phoneNow.second == 0)

        val start = LocalDateTime.of(2026, 11, 20, 7, 0)
        vm.start(start, DEFAULT_ZONE, "0", "0")
        assertEquals(SimulationMessage.Failed(SimulationFailure.INVALID_CAPACITY), vm.message.value)
        vm.start(start, DEFAULT_ZONE, "500", "600")
        assertEquals(SimulationMessage.Failed(SimulationFailure.INVALID_VOLUME), vm.message.value)
        vm.start(start, DEFAULT_ZONE, "500", "0") // empty reservoir is a valid start
        val active = vm.uiState.filterIsInstance<SimulationUiState.Active>().first()
        assertEquals(start, active.clock.toLocalDateTime())

        vm.advanceTo(start.minusHours(1), DEFAULT_ZONE)
        assertEquals(SimulationMessage.Failed(SimulationFailure.TARGET_BEFORE_CLOCK), vm.message.value)
        vm.advanceDays(1) // two fixture schedules due, both blocked (empty reservoir)
        assertTrue(vm.message.value is SimulationMessage.Processed)
        vm.advanceDays(400)
        assertEquals(SimulationMessage.Failed(SimulationFailure.RANGE_TOO_LONG), vm.message.value)

        vm.reset()
        vm.uiState.filterIsInstance<SimulationUiState.Inactive>().first()
        assertEquals(null, vm.message.value)
    }
}
