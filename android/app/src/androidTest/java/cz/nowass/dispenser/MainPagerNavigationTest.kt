package cz.nowass.dispenser

import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertIsNotSelected
import androidx.compose.ui.test.assertIsSelected
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.compose.ui.test.onAllNodesWithTag
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import androidx.compose.ui.test.performTouchInput
import androidx.compose.ui.test.swipeLeft
import androidx.compose.ui.test.swipeRight
import androidx.lifecycle.Lifecycle
import androidx.test.espresso.Espresso
import androidx.test.ext.junit.runners.AndroidJUnit4
import cz.nowass.dispenser.domain.DEFAULT_ZONE
import cz.nowass.dispenser.ui.Format
import cz.nowass.dispenser.ui.MAIN_PAGER_TEST_TAG
import java.time.YearMonth
import cz.nowass.dispenser.ui.TopLevelDestination
import cz.nowass.dispenser.ui.TopLevelDestination.CALENDAR
import cz.nowass.dispenser.ui.TopLevelDestination.DASHBOARD
import cz.nowass.dispenser.ui.TopLevelDestination.HISTORY
import cz.nowass.dispenser.ui.TopLevelDestination.SETTINGS
import cz.nowass.dispenser.ui.schedule.EDITOR_SAVE_TEST_TAG
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

/** Swipe navigation between the four main screens, synchronized with the dock. */
@RunWith(AndroidJUnit4::class)
class MainPagerNavigationTest {

    @get:Rule
    val rule = createAndroidComposeRule<MainActivity>()

    private fun str(id: Int) = rule.activity.getString(id)
    private fun pager() = rule.onNodeWithTag(MAIN_PAGER_TEST_TAG)
    private fun swipeToNext() = pager().performTouchInput { swipeLeft() }
    private fun swipeToPrevious() = pager().performTouchInput { swipeRight() }

    private fun assertActive(dest: TopLevelDestination) {
        rule.onNodeWithTag(dest.testTag).assertIsSelected()
        TopLevelDestination.entries.filter { it != dest }.forEach { rule.onNodeWithTag(it.testTag).assertIsNotSelected() }
    }

    @Test
    fun swipeLeftAndRight_visitAllFourScreens_andDockFollows() {
        assertActive(DASHBOARD)
        swipeToNext(); assertActive(CALENDAR)
        swipeToNext(); assertActive(HISTORY)
        swipeToNext(); assertActive(SETTINGS)
        swipeToNext(); assertActive(SETTINGS) // last page: no wrap-around
        swipeToPrevious(); assertActive(HISTORY)
        swipeToPrevious(); assertActive(CALENDAR)
        swipeToPrevious(); assertActive(DASHBOARD)
        swipeToPrevious(); assertActive(DASHBOARD) // first page
    }

    @Test
    fun dockSelection_movesThePager_andHeaderTitle() {
        for (dest in listOf(SETTINGS, CALENDAR, HISTORY, DASHBOARD)) {
            rule.onNodeWithTag(dest.testTag).performClick()
            assertActive(dest)
        }
        rule.onNodeWithTag(SETTINGS.testTag).performClick()
        rule.onNodeWithText(str(R.string.settings_mode)).assertIsDisplayed()
    }

    @Test
    fun calendarMonth_isPreservedAcrossTabSwitches() {
        rule.onNodeWithTag(CALENDAR.testTag).performClick()
        rule.onNodeWithContentDescription(str(R.string.calendar_next_month)).performClick()
        rule.onNodeWithContentDescription(str(R.string.calendar_next_month)).performClick()
        // The app uses the phone clock in Europe/Prague, so "two months ahead" is computable here.
        val expected = Format.monthYear(YearMonth.now(DEFAULT_ZONE).plusMonths(2))
        rule.onNodeWithText(expected).assertIsDisplayed()

        swipeToNext(); swipeToNext(); assertActive(SETTINGS)
        swipeToPrevious(); swipeToPrevious(); assertActive(CALENDAR)
        rule.onNodeWithText(expected).assertIsDisplayed() // not reset to the current month
    }

    @Test
    fun verticalScrolling_stillWorksInsidePages() {
        rule.onNodeWithTag(CALENDAR.testTag).performClick()
        rule.onNodeWithText(str(R.string.schedules_add)).performScrollTo().assertIsDisplayed()
        assertActive(CALENDAR)
    }

    @Test
    fun scheduleEditor_isNotSwipeable_andReturnsToCalendar() {
        rule.onNodeWithTag(CALENDAR.testTag).performClick()
        rule.onNodeWithText(str(R.string.schedules_add)).performScrollTo().performClick()
        rule.onNodeWithTag(EDITOR_SAVE_TEST_TAG).assertIsDisplayed()
        assertEquals(0, rule.onAllNodesWithTag(MAIN_PAGER_TEST_TAG).fetchSemanticsNodes().size)
        rule.onNodeWithTag(EDITOR_SAVE_TEST_TAG).performTouchInput { swipeLeft() }
        rule.onNodeWithTag(EDITOR_SAVE_TEST_TAG).assertIsDisplayed() // still in the editor
        rule.onNodeWithContentDescription(str(R.string.editor_close)).performClick()
        assertActive(CALENDAR)
    }

    @Test
    fun back_returnsToDashboard_thenLeavesTheApp() {
        rule.onNodeWithTag(HISTORY.testTag).performClick()
        Espresso.pressBack()
        assertActive(DASHBOARD)
        Espresso.pressBackUnconditionally()
        rule.waitUntil(5_000) { rule.activityRule.scenario.state == Lifecycle.State.DESTROYED }
        assertTrue(rule.activityRule.scenario.state == Lifecycle.State.DESTROYED)
    }
}
