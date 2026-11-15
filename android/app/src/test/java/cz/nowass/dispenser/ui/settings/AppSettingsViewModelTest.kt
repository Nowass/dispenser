package cz.nowass.dispenser.ui.settings

import cz.nowass.dispenser.data.AppVersion
import cz.nowass.dispenser.data.DisplayNameRules
import cz.nowass.dispenser.data.InMemoryAppSettingsRepository
import cz.nowass.dispenser.data.MockDispenserRepository
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
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class AppSettingsViewModelTest {
    @Before fun setUp() = Dispatchers.setMain(UnconfinedTestDispatcher())
    @After fun tearDown() = Dispatchers.resetMain()

    private val version = AppVersion("0.4.0", 4)

    @Test
    fun state_reflectsNameVersionReservoirAndSimulation() = runTest {
        val settings = InMemoryAppSettingsRepository()
        val vm = AppSettingsViewModel(settings, MockDispenserRepository(), version)
        backgroundScope.launch { vm.uiState.collect {} }
        val s = vm.uiState.filterIsInstance<AppSettingsUiState.Ready>().first()
        assertNull(s.customName)
        assertEquals(version, s.version)
        assertEquals(500_000L, s.reservoir!!.capacityUl)
        assertTrue(s.simulationActive)

        vm.saveName("  Obývák 120 l ")
        assertEquals("Obývák 120 l", vm.uiState.filterIsInstance<AppSettingsUiState.Ready>().first { it.customName != null }.customName)

        assertEquals(DisplayNameRules.Error.TOO_LONG, vm.validateName("x".repeat(31)))
        vm.saveName("x".repeat(31)) // ignored
        assertEquals("Obývák 120 l", (vm.uiState.value as AppSettingsUiState.Ready).customName)

        vm.resetName()
        assertNull(vm.uiState.filterIsInstance<AppSettingsUiState.Ready>().first { it.customName == null }.customName)
    }

    @Test
    fun withoutSimulation_reservoirUnknown_notActive() = runTest {
        val vm = AppSettingsViewModel(InMemoryAppSettingsRepository(), MockDispenserRepository(simulationActive = false), version)
        backgroundScope.launch { vm.uiState.collect {} }
        val s = vm.uiState.filterIsInstance<AppSettingsUiState.Ready>().first()
        assertNull(s.reservoir)
        assertFalse(s.simulationActive)
    }
}
