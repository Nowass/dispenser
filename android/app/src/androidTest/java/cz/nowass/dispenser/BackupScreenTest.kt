package cz.nowass.dispenser

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertIsOff
import androidx.compose.ui.test.assertIsOn
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import cz.nowass.dispenser.data.backup.BackupContent
import cz.nowass.dispenser.data.backup.RestorePlan
import cz.nowass.dispenser.data.backup.SimulationEffect
import cz.nowass.dispenser.ui.backup.BACKUP_CONFIRM_TEST_TAG
import cz.nowass.dispenser.ui.backup.BACKUP_EXPORT_TEST_TAG
import cz.nowass.dispenser.ui.backup.BACKUP_IMPORT_TEST_TAG
import cz.nowass.dispenser.ui.backup.BACKUP_INCLUDE_SIM_TEST_TAG
import cz.nowass.dispenser.ui.backup.BackupScreen
import cz.nowass.dispenser.ui.backup.BackupUiState
import cz.nowass.dispenser.ui.theme.DispenserTheme
import org.junit.Assert.assertEquals
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import java.time.Instant

@RunWith(AndroidJUnit4::class)
class BackupScreenTest {
    @get:Rule val rule = createComposeRule()
    private val ctx = InstrumentationRegistry.getInstrumentation().targetContext

    @Test
    fun checkboxDefaultsOff_buttonsInvokeCallbacks_confirmationExplainsReplacement() {
        val calls = mutableListOf<String>()
        val plan = RestorePlan(
            content = BackupContent(Instant.parse("2026-11-15T10:00:00Z"), "0.4.0 (4)", "Obývák", emptyList(), null),
            currentScheduleCount = 3, currentUnreadableCount = 0, currentDisplayName = null, currentHistoryCount = 5,
            simulationEffect = SimulationEffect.DEACTIVATED,
        )
        var state by mutableStateOf(BackupUiState())
        rule.setContent {
            DispenserTheme {
                BackupScreen(
                    state = state, isDemo = true, onBack = {},
                    onIncludeSimulationChange = { state = state.copy(includeSimulation = it) },
                    onExport = { calls += "export" }, onImport = { calls += "import" },
                    onConfirmRestore = { calls += "confirm" }, onCancelRestore = { calls += "cancel"; state = state.copy(pendingRestore = null) },
                )
            }
        }
        rule.onNodeWithTag(BACKUP_INCLUDE_SIM_TEST_TAG).assertIsOff().performClick().assertIsOn()
        rule.onNodeWithTag(BACKUP_EXPORT_TEST_TAG).performClick()
        rule.onNodeWithTag(BACKUP_IMPORT_TEST_TAG).performClick()
        assertEquals(listOf("export", "import"), calls)

        state = state.copy(pendingRestore = plan)
        rule.onNodeWithText(ctx.getString(R.string.backup_confirm_title)).assertIsDisplayed()
        rule.onNodeWithText(ctx.getString(R.string.backup_confirm_sim_deactivated)).assertIsDisplayed()
        rule.onNodeWithText(ctx.getString(R.string.dialog_cancel)).performClick()
        assertEquals("cancel", calls.last())

        state = state.copy(pendingRestore = plan)
        rule.onNodeWithTag(BACKUP_CONFIRM_TEST_TAG).performClick()
        assertEquals("confirm", calls.last())
    }
}
