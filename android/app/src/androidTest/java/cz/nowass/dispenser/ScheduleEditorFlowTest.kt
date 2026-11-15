package cz.nowass.dispenser

import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertIsEnabled
import androidx.compose.ui.test.assertIsNotEnabled
import androidx.compose.ui.test.onAllNodesWithTag
import org.junit.Assert.assertTrue
import androidx.compose.ui.test.hasText
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import androidx.compose.ui.test.performTextClearance
import androidx.compose.ui.test.performTextInput
import androidx.test.ext.junit.runners.AndroidJUnit4
import cz.nowass.dispenser.ui.TopLevelDestination
import cz.nowass.dispenser.ui.schedule.EDITOR_AMOUNT_TEST_TAG
import cz.nowass.dispenser.ui.schedule.EDITOR_NAME_TEST_TAG
import cz.nowass.dispenser.ui.schedule.EDITOR_SAVE_TEST_TAG
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

/** UI flow tests for the schedule editor against the real local (Room) database. Need an emulator. */
@RunWith(AndroidJUnit4::class)
class ScheduleEditorFlowTest {

    @get:Rule
    val rule = createAndroidComposeRule<MainActivity>()

    private fun str(id: Int) = rule.activity.getString(id)

    private fun openCalendar() = rule.onNodeWithTag(TopLevelDestination.CALENDAR.testTag).performClick()

    /** The app database persists between runs, so every test uses its own unique schedule. */
    private fun unique(prefix: String) = "$prefix ${java.util.UUID.randomUUID().toString().take(6)}"

    private fun waitForCalendar() =
        rule.waitUntil(5_000) { rule.onAllNodesWithTag(TopLevelDestination.CALENDAR.testTag).fetchSemanticsNodes().isNotEmpty() }

    private fun createSchedule(name: String, amount: String) {
        openCalendar()
        rule.onNodeWithText(str(R.string.schedules_add)).performScrollTo().performClick()
        rule.onNodeWithTag(EDITOR_NAME_TEST_TAG).performTextInput(name)
        rule.onNodeWithTag(EDITOR_AMOUNT_TEST_TAG).performTextInput(amount)
        rule.onNodeWithTag(EDITOR_SAVE_TEST_TAG).performClick()
        waitForCalendar()
        rule.waitUntil(5_000) { rule.onAllNodes(hasText(name)).fetchSemanticsNodes().isNotEmpty() }
    }

    @Test
    fun createSchedule_saveDisabledUntilValid_thenSavedOnPhone() {
        val name = unique("Nová")
        openCalendar()
        rule.onNodeWithText(str(R.string.schedules_add)).performScrollTo().performClick()
        rule.onNodeWithTag(EDITOR_SAVE_TEST_TAG).assertIsDisplayed().assertIsNotEnabled()
        rule.onNodeWithTag(EDITOR_NAME_TEST_TAG).performTextInput(name)
        rule.onNodeWithTag(EDITOR_AMOUNT_TEST_TAG).performTextInput("7,5")
        rule.onNodeWithTag(EDITOR_SAVE_TEST_TAG).assertIsEnabled().performClick()
        waitForCalendar()
        // Confirmation never claims a device sync.
        rule.waitUntil(5_000) { rule.onAllNodes(hasText(str(R.string.editor_saved_snackbar))).fetchSemanticsNodes().isNotEmpty() }
        rule.onNodeWithText(name).performScrollTo().assertIsDisplayed()
    }

    @Test
    fun editSchedule_changesAmountInList() {
        val name = unique("Úprava")
        createSchedule(name, "5")
        rule.onNodeWithText(name).performScrollTo().performClick()
        rule.onNodeWithTag(EDITOR_AMOUNT_TEST_TAG).performTextClearance()
        rule.onNodeWithTag(EDITOR_AMOUNT_TEST_TAG).performTextInput("6,25")
        rule.onNodeWithTag(EDITOR_SAVE_TEST_TAG).performClick()
        waitForCalendar()
        rule.waitUntil(5_000) { rule.onAllNodes(hasText("6,25 ml", substring = true)).fetchSemanticsNodes().isNotEmpty() }
    }

    @Test
    fun unsavedChanges_backAsksToDiscard_andKeepsStoredValue() {
        val name = unique("Zahodit")
        createSchedule(name, "3")
        rule.onNodeWithText(name).performScrollTo().performClick()
        rule.onNodeWithTag(EDITOR_AMOUNT_TEST_TAG).performTextClearance()
        rule.onNodeWithTag(EDITOR_AMOUNT_TEST_TAG).performTextInput("42,125")
        rule.activityRule.scenario.onActivity { it.onBackPressedDispatcher.onBackPressed() }
        rule.onNodeWithText(str(R.string.editor_discard_title)).assertIsDisplayed()
        rule.onNodeWithText(str(R.string.editor_discard_confirm)).performClick()
        waitForCalendar()
        assertTrue(rule.onAllNodes(hasText("42,125 ml", substring = true)).fetchSemanticsNodes().isEmpty())
    }
}
