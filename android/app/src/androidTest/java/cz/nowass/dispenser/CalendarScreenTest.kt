package cz.nowass.dispenser

import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performScrollTo
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import cz.nowass.dispenser.data.MockSeed
import cz.nowass.dispenser.ui.calendar.CalendarMode
import cz.nowass.dispenser.ui.calendar.CalendarScreen
import cz.nowass.dispenser.ui.calendar.CalendarViewModel
import cz.nowass.dispenser.ui.theme.DispenserTheme
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class CalendarScreenTest {

    @get:Rule
    val rule = createComposeRule()

    private val ctx = InstrumentationRegistry.getInstrumentation().targetContext

    @Test
    fun unreadableSchedules_showNonBlockingWarning_validOnesStillListed() {
        val seed = MockSeed.default()
        val state = CalendarViewModel.buildState(seed.device, seed.schedules, null, CalendarMode.MONTH, unreadableScheduleCount = 2)
        rule.setContent { DispenserTheme { CalendarScreen(state, {}, {}, {}, {}) } }
        // Detail sentence (no plural): plural forms are chosen by Czech rules, not the device locale.
        rule.onNodeWithText(ctx.getString(R.string.schedules_unreadable_detail))
            .performScrollTo().assertIsDisplayed()
        rule.onNodeWithText("Ranní dávka").performScrollTo().assertIsDisplayed()
    }
}
