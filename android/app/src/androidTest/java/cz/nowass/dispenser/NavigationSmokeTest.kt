package cz.nowass.dispenser

import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertIsSelected
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.test.ext.junit.runners.AndroidJUnit4
import cz.nowass.dispenser.ui.TopLevelDestination
import cz.nowass.dispenser.ui.components.DEMO_BADGE_TEST_TAG
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class NavigationSmokeTest {

    @get:Rule
    val rule = createAndroidComposeRule<MainActivity>()

    @Test
    fun dashboardShowsSeededDataAndDemoBadge() {
        rule.onNodeWithTag(DEMO_BADGE_TEST_TAG).assertIsDisplayed()
        rule.onNodeWithText(rule.activity.getString(R.string.dashboard_next_dose)).assertIsDisplayed()
        rule.onNodeWithText("Ranní dávka", substring = true).assertIsDisplayed()
    }

    @Test
    fun bottomNavigationReachesAllScreens_andDemoBadgePersists() {
        rule.onNodeWithTag(TopLevelDestination.DASHBOARD.testTag).assertIsSelected()
        listOf(
            TopLevelDestination.CALENDAR,
            TopLevelDestination.HISTORY,
            TopLevelDestination.SETTINGS,
            TopLevelDestination.DASHBOARD,
        ).forEach { dest ->
            rule.onNodeWithTag(dest.testTag).performClick()
            rule.onNodeWithTag(dest.testTag).assertIsSelected()
            rule.onNodeWithTag(DEMO_BADGE_TEST_TAG).assertIsDisplayed()
        }
    }
}
