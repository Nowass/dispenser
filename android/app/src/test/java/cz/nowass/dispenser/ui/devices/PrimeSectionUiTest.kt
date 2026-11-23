package cz.nowass.dispenser.ui.devices

import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertIsEnabled
import androidx.compose.ui.test.assertIsNotEnabled
import androidx.compose.ui.test.assertTextEquals
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.onAllNodesWithTag
import androidx.compose.ui.test.performClick
import cz.nowass.dispenser.data.device.PrimeSupport
import cz.nowass.dispenser.device.ops.PrimeStatusRecord
import cz.nowass.dispenser.device.ops.PrimeVerdict
import cz.nowass.dispenser.ui.theme.DispenserTheme
import org.junit.Assert.assertEquals
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

/** The "Zavodnění pumpy" section on the JVM: the visible states and the one button. */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], qualifiers = "w412dp-h915dp")
class PrimeSectionUiTest {
    @get:Rule val compose = createComposeRule()

    private var taps = 0
    private fun show(state: PrimeUiState) = compose.setContent { DispenserTheme { PrimeSection(state, onToggle = { taps++ }) } }
    private fun rec(running: Boolean, elapsed: Long, reason: Int = 0, verdict: Int = 0) = PrimeStatusRecord(verdict, running, elapsed, 30_000, 3_000, reason)

    @Test
    fun running_shows_elapsed_maximum_and_stop() {
        show(PrimeUiState(PrimeSupport.Supported, connected = true, phase = PrimePhase.RUNNING, last = rec(true, 12_345)))
        compose.onNodeWithText("Zavodnění pumpy").assertIsDisplayed()
        compose.onNodeWithTag(PRIME_STATE_TEST_TAG).assertTextEquals("Stav: Běží")
        compose.onNodeWithText("Doba běhu: 12 s (max. 30 s)").assertIsDisplayed()
        compose.onNodeWithTag(PRIME_BUTTON_TEST_TAG).assertIsEnabled().assertTextEquals("Zastavit").performClick()
        assertEquals(1, taps)
    }

    @Test
    fun stopped_by_the_limit_explains_and_reminds_about_the_reservoir() {
        show(PrimeUiState(PrimeSupport.Supported, connected = true, phase = PrimePhase.STOPPED, last = rec(false, 30_000, reason = 2)))
        compose.onNodeWithTag(PRIME_STATE_TEST_TAG).assertTextEquals("Stav: Zastaveno")
        compose.onNodeWithText("Zastaveno: dosažen limit 30 s – pro pokračování spusťte znovu.").assertIsDisplayed()
        compose.onNodeWithText("Zkontrolujte nastavení zásobníku – zavodnění neodečítá objem.").assertIsDisplayed()
        compose.onNodeWithTag(PRIME_BUTTON_TEST_TAG).assertIsEnabled().assertTextEquals("Spustit zavodnění")
    }

    @Test
    fun disconnected_is_unknown_with_a_disabled_button() {
        show(PrimeUiState(PrimeSupport.Supported, connected = false, phase = PrimePhase.UNKNOWN, last = rec(true, 5_000), failed = true))
        compose.onNodeWithTag(PRIME_STATE_TEST_TAG).assertTextEquals("Stav neznámý – zařízení odpojeno")
        compose.onNodeWithTag(PRIME_BUTTON_TEST_TAG).assertIsNotEnabled()
    }

    @Test
    fun pending_start_disables_the_button() {
        show(PrimeUiState(PrimeSupport.Supported, connected = true, phase = PrimePhase.STARTING))
        compose.onNodeWithTag(PRIME_STATE_TEST_TAG).assertTextEquals("Stav: spouštím… (čeká na potvrzení zařízení)")
        compose.onNodeWithTag(PRIME_BUTTON_TEST_TAG).assertIsNotEnabled()
    }

    @Test
    fun refusal_is_explained() {
        show(PrimeUiState(PrimeSupport.Supported, connected = true, phase = PrimePhase.STOPPED, last = rec(false, 0, verdict = 3), refusal = PrimeVerdict.DOSE_SOON))
        compose.onNodeWithText("Nelze spustit: do 15 minut je naplánovaná dávka.").assertIsDisplayed()
    }

    @Test
    fun old_firmware_gets_a_note_and_unknown_support_hides_the_section() {
        show(PrimeUiState(PrimeSupport.Unsupported, connected = true))
        compose.onNodeWithText("Zavodnění pumpy vyžaduje firmware 0.3.3 nebo novější. Nejprve aktualizujte firmware zařízení.").assertIsDisplayed()
        assertEquals(0, compose.onAllNodesWithTag(PRIME_BUTTON_TEST_TAG).fetchSemanticsNodes().size)
    }

    @Test
    fun not_known_yet_shows_nothing() {
        show(PrimeUiState(support = null))
        assertEquals(0, compose.onAllNodesWithTag(PRIME_SECTION_TEST_TAG).fetchSemanticsNodes().size)
    }
}
