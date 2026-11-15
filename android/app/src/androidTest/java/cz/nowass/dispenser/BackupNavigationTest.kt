package cz.nowass.dispenser

import androidx.compose.ui.test.assertCountEquals
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.compose.ui.test.onAllNodesWithText
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import androidx.test.espresso.Espresso
import androidx.test.ext.junit.runners.AndroidJUnit4
import cz.nowass.dispenser.ui.TopLevelDestination
import cz.nowass.dispenser.ui.backup.BACKUP_EXPORT_TEST_TAG
import cz.nowass.dispenser.ui.settings.SETTINGS_BACKUP_ROW_TEST_TAG
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

/** Settings -> Záloha a obnovení: single detail header, system back returns to Settings. */
@RunWith(AndroidJUnit4::class)
class BackupNavigationTest {
    @get:Rule val rule = createAndroidComposeRule<MainActivity>()
    private fun str(id: Int) = rule.activity.getString(id)

    @Test
    fun backupScreen_hasOnlyItsOwnHeader_andBackReturnsToSettings() {
        rule.onNodeWithTag(TopLevelDestination.SETTINGS.testTag).performClick()
        rule.onNodeWithTag(SETTINGS_BACKUP_ROW_TEST_TAG).performScrollTo().performClick()
        rule.onNodeWithTag(BACKUP_EXPORT_TEST_TAG).assertIsDisplayed()
        // The shell's "Nastavení" tab header and the navigation dock are hidden.
        rule.onAllNodesWithText(str(R.string.nav_settings)).assertCountEquals(0)
        rule.onAllNodesWithText(str(R.string.backup_title)).assertCountEquals(1)
        Espresso.pressBack()
        rule.onNodeWithTag(SETTINGS_BACKUP_ROW_TEST_TAG).assertIsDisplayed()
    }
}
