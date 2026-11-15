package cz.nowass.dispenser

import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import cz.nowass.dispenser.data.MockSeed
import cz.nowass.dispenser.domain.simulation.ReservoirPredictor
import cz.nowass.dispenser.domain.simulation.ReservoirStatus
import cz.nowass.dispenser.ui.history.HistoryScreen
import cz.nowass.dispenser.ui.history.HistoryUiState
import cz.nowass.dispenser.ui.history.HistoryViewModel
import cz.nowass.dispenser.ui.reservoir.RESERVOIR_REFILL_TEST_TAG
import cz.nowass.dispenser.ui.reservoir.ReservoirScreen
import cz.nowass.dispenser.ui.reservoir.ReservoirUiState
import cz.nowass.dispenser.ui.theme.DispenserTheme
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

/** State-driven checks of the A1.3b screens (no repository). */
@RunWith(AndroidJUnit4::class)
class SimulationScreensTest {
    @get:Rule val rule = createComposeRule()
    private val ctx = InstrumentationRegistry.getInstrumentation().targetContext
    private val seed = MockSeed.default()

    @Test
    fun history_showsSimulationNote_andEvents() {
        rule.setContent { DispenserTheme { HistoryScreen(HistoryUiState.Ready(HistoryViewModel.group(seed.events, seed.device.zone), seed.device.zone)) } }
        rule.onNodeWithText(ctx.getString(R.string.history_simulated_note)).assertIsDisplayed()
        rule.onNodeWithText(ctx.getString(R.string.history_type_skipped)).assertIsDisplayed()
    }

    @Test
    fun reservoir_refillDialog_opens() {
        val state = ReservoirUiState.Ready(seed.reservoir, ReservoirStatus.NORMAL, 20, ReservoirPredictor().predict(seed.reservoir, seed.device.now, seed.schedules), seed.device.zone)
        rule.setContent { DispenserTheme { ReservoirScreen(state, true, {}, { null }, {}) } }
        rule.onNodeWithTag(RESERVOIR_REFILL_TEST_TAG).performClick()
        rule.onNodeWithText(ctx.getString(R.string.refill_dialog_title)).assertIsDisplayed()
    }

    @Test
    fun reservoir_unknownWithoutSimulation() {
        val state = ReservoirUiState.Ready(null, null, 20, null, seed.device.zone)
        rule.setContent { DispenserTheme { ReservoirScreen(state, true, {}, { null }, {}) } }
        rule.onNodeWithText(ctx.getString(R.string.reservoir_simulation_inactive)).assertIsDisplayed()
    }
}
