package cz.nowass.dispenser

import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertIsNotEnabled
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import androidx.compose.ui.test.performTextReplacement
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import cz.nowass.dispenser.data.AppVersion
import cz.nowass.dispenser.data.DisplayNameRules
import cz.nowass.dispenser.data.MockSeed
import cz.nowass.dispenser.domain.DEFAULT_ZONE
import cz.nowass.dispenser.ui.settings.AppSettingsActions
import cz.nowass.dispenser.ui.settings.AppSettingsUiState
import cz.nowass.dispenser.ui.settings.SETTINGS_NAME_ROW_TEST_TAG
import cz.nowass.dispenser.ui.settings.SETTINGS_RESERVOIR_ROW_TEST_TAG
import cz.nowass.dispenser.ui.settings.SettingsScreen
import cz.nowass.dispenser.ui.settings.SimulationActions
import cz.nowass.dispenser.ui.settings.SimulationUiState
import cz.nowass.dispenser.ui.theme.DispenserTheme
import org.junit.Assert.assertEquals
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import java.time.ZonedDateTime

@RunWith(AndroidJUnit4::class)
class SettingsScreenTest {
    @get:Rule val rule = createComposeRule()
    private val ctx = InstrumentationRegistry.getInstrumentation().targetContext

    private fun show(saved: MutableList<String> = mutableListOf(), opened: MutableList<Unit> = mutableListOf()) {
        val now = ZonedDateTime.of(2026, 11, 20, 8, 0, 0, 0, DEFAULT_ZONE)
        rule.setContent {
            DispenserTheme {
                SettingsScreen(
                    app = AppSettingsUiState.Ready(null, AppVersion("0.4.0", 4), MockSeed.default().reservoir, simulationActive = true),
                    simulation = SimulationUiState.Active(now, now.minusDays(2)),
                    message = null,
                    appActions = AppSettingsActions(saveName = { saved += it }, openReservoir = { opened += Unit }),
                    actions = SimulationActions(),
                )
            }
        }
    }

    @Test
    fun sections_deviceNotConnected_simulationActiveTag() {
        show()
        rule.onNodeWithText(ctx.getString(R.string.settings_device_not_connected)).performScrollTo().assertIsDisplayed()
        rule.onNodeWithText(ctx.getString(R.string.settings_sim_active_tag)).performScrollTo().assertIsDisplayed()
        rule.onNodeWithText(ctx.getString(R.string.settings_device_connect)).performScrollTo().assertIsNotEnabled()
    }

    @Test
    fun renameDialog_blocksTooLongName_andSavesValidOne() {
        val saved = mutableListOf<String>()
        show(saved)
        rule.onNodeWithTag(SETTINGS_NAME_ROW_TEST_TAG).performClick()
        rule.onNodeWithText(ctx.getString(R.string.settings_name_label)).performTextReplacement("x".repeat(DisplayNameRules.MAX_LENGTH + 1))
        rule.onNodeWithText(ctx.getString(R.string.settings_name_err_too_long, DisplayNameRules.MAX_LENGTH)).assertIsDisplayed()
        rule.onNodeWithText(ctx.getString(R.string.editor_save)).assertIsNotEnabled()
        rule.onNodeWithText(ctx.getString(R.string.settings_name_label)).performTextReplacement("Obývák")
        rule.onNodeWithText(ctx.getString(R.string.editor_save)).performClick()
        assertEquals(listOf("Obývák"), saved)
    }

    @Test
    fun reservoirShortcut_invokesNavigation() {
        val opened = mutableListOf<Unit>()
        show(opened = opened)
        rule.onNodeWithTag(SETTINGS_RESERVOIR_ROW_TEST_TAG).performScrollTo().performClick()
        assertEquals(1, opened.size)
    }
}
