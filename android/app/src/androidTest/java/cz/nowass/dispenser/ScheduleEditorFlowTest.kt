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

/** UI flow tests for the schedule editor (mock backend). Not run in CI here due to emulator memory limits. */
@RunWith(AndroidJUnit4::class)
class ScheduleEditorFlowTest {

    @get:Rule
    val rule = createAndroidComposeRule<MainActivity>()

    private fun str(id: Int) = rule.activity.getString(id)

    private fun openCalendar() = rule.onNodeWithTag(TopLevelDestination.CALENDAR.testTag).performClick()

    @Test
    fun createSchedule_saveDisabledUntilValid_thenReturnsToCalendar() {
        openCalendar()
        rule.onNodeWithText(str(R.string.schedules_add)).performScrollTo().performClick()
        rule.onNodeWithTag(EDITOR_SAVE_TEST_TAG).assertIsDisplayed().assertIsNotEnabled()
        rule.onNodeWithTag(EDITOR_NAME_TEST_TAG).performTextInput("Testovací dávka")
        rule.onNodeWithTag(EDITOR_AMOUNT_TEST_TAG).performTextInput("7,5")
        rule.onNodeWithTag(EDITOR_SAVE_TEST_TAG).assertIsEnabled().performClick()
        // Back on the calendar (dock visible again) with the new schedule listed.
        rule.waitUntil(5_000) { rule.onAllNodesWithTag(TopLevelDestination.CALENDAR.testTag).fetchSemanticsNodes().isNotEmpty() }
        rule.waitUntil(5_000) { rule.onAllNodes(hasText("Testovací dávka")).fetchSemanticsNodes().isNotEmpty() }
        rule.onNodeWithText("Testovací dávka").performScrollTo().assertIsDisplayed()
    }

    @Test
    fun editSchedule_changesAmountInList() {
        openCalendar()
        rule.onNodeWithText("Večerní doplnění").performScrollTo().performClick()
        rule.onNodeWithTag(EDITOR_AMOUNT_TEST_TAG).performTextClearance()
        rule.onNodeWithTag(EDITOR_AMOUNT_TEST_TAG).performTextInput("6")
        rule.onNodeWithTag(EDITOR_SAVE_TEST_TAG).performClick()
        rule.waitUntil(5_000) { rule.onAllNodes(hasText("6 ml", substring = true)).fetchSemanticsNodes().isNotEmpty() }
    }

    @Test
    fun unsavedChanges_backAsksToDiscard_andKeepsStoredValue() {
        openCalendar()
        rule.onNodeWithText("Večerní doplnění").performScrollTo().performClick()
        rule.onNodeWithTag(EDITOR_AMOUNT_TEST_TAG).performTextClearance()
        rule.onNodeWithTag(EDITOR_AMOUNT_TEST_TAG).performTextInput("42")
        rule.activityRule.scenario.onActivity { it.onBackPressedDispatcher.onBackPressed() }
        rule.onNodeWithText(str(R.string.editor_discard_title)).assertIsDisplayed()
        rule.onNodeWithText(str(R.string.editor_discard_confirm)).performClick()
        rule.waitUntil(5_000) { rule.onAllNodesWithTag(TopLevelDestination.CALENDAR.testTag).fetchSemanticsNodes().isNotEmpty() }
        assertTrue(rule.onAllNodes(hasText("42 ml", substring = true)).fetchSemanticsNodes().isEmpty())
    }
}
