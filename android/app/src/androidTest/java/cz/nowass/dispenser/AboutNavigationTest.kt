package cz.nowass.dispenser

import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.compose.ui.test.onAllNodesWithText
import androidx.compose.ui.test.onFirst
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import androidx.test.espresso.Espresso
import androidx.test.ext.junit.runners.AndroidJUnit4
import cz.nowass.dispenser.ui.TopLevelDestination
import cz.nowass.dispenser.ui.about.ABOUT_APP_LICENSE_TEST_TAG
import cz.nowass.dispenser.ui.about.ABOUT_LICENSES_TEST_TAG
import cz.nowass.dispenser.ui.about.ABOUT_WHATS_NEW_TEST_TAG
import cz.nowass.dispenser.ui.settings.SETTINGS_ABOUT_ROW_TEST_TAG
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

/** Settings -> About and its sub-screens (no network involved). */
@RunWith(AndroidJUnit4::class)
class AboutNavigationTest {
    @get:Rule val rule = createAndroidComposeRule<MainActivity>()
    private fun str(id: Int) = rule.activity.getString(id)

    private fun openAbout() {
        rule.onNodeWithTag(TopLevelDestination.SETTINGS.testTag).performClick()
        rule.onNodeWithTag(SETTINGS_ABOUT_ROW_TEST_TAG).performScrollTo().performClick()
        rule.onNodeWithText(str(R.string.about_app_license)).performScrollTo().assertIsDisplayed()
    }

    @Test
    fun about_showsVersionFromPackage_andApacheLicense() {
        openAbout()
        val version = rule.activity.packageManager.getPackageInfo(rule.activity.packageName, 0).versionName!!
        rule.onNodeWithText(str(R.string.about_version).replace("%1\$s", version)).assertIsDisplayed()
    }

    @Test
    fun whatsNew_andLicenses_open_andBackReturns() {
        openAbout()
        rule.onNodeWithTag(ABOUT_WHATS_NEW_TEST_TAG).performClick()
        rule.onNodeWithText(str(R.string.changelog_current)).assertIsDisplayed()
        Espresso.pressBack()
        rule.onNodeWithTag(ABOUT_LICENSES_TEST_TAG).performScrollTo().performClick()
        rule.onNodeWithText(str(R.string.licenses_texts_title)).assertIsDisplayed()
        Espresso.pressBack()
        Espresso.pressBack()
        rule.onNodeWithTag(TopLevelDestination.SETTINGS.testTag).assertIsDisplayed()
    }

    @Test
    fun appLicense_opensBundledApacheText() {
        openAbout()
        rule.onNodeWithTag(ABOUT_APP_LICENSE_TEST_TAG).performScrollTo().performClick()
        // Title bar and the license body both contain the name.
        rule.onAllNodesWithText("Apache License", substring = true).onFirst().assertIsDisplayed()
        Espresso.pressBack()
        rule.onNodeWithTag(ABOUT_APP_LICENSE_TEST_TAG).performScrollTo().assertIsDisplayed()
    }
}
