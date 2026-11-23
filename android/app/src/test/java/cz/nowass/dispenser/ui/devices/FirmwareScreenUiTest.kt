package cz.nowass.dispenser.ui.devices

import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertIsEnabled
import androidx.compose.ui.test.assertIsNotEnabled
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performScrollTo
import cz.nowass.dispenser.data.device.FirmwareStatusOutcome
import cz.nowass.dispenser.device.ops.UpdateStatusRecord
import cz.nowass.dispenser.domain.update.FirmwareCheckResult
import cz.nowass.dispenser.domain.update.FirmwareOffer
import cz.nowass.dispenser.ui.theme.DispenserTheme
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

/**
 * The firmware screen on the JVM: display and enabled state only (the confirmation dialog is not
 * driven: dialogs never reach idle under Robolectric here; the flow is covered by FirmwareViewModelTest).
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], qualifiers = "w412dp-h915dp")
class FirmwareScreenUiTest {
    @get:Rule val compose = createComposeRule()

    private val record = UpdateStatusRecord("0.3.0", capable = true, lastResultWire = 1, lastVersion = "0.3.0")
    private val offer = FirmwareOffer("0.3.1", "firmware-v0.3.1", 1_430_528, "0f".repeat(32), "Oprava čerpadla.", false, null)

    private fun show(state: FirmwareUiState) = compose.setContent {
        DispenserTheme { FirmwareScreen(state, onBack = {}, onReload = {}, onCheck = {}, onBeta = {}, onUpdate = {}) }
    }

    @Test
    fun connected_with_an_offer_shows_version_notes_and_an_enabled_update() {
        show(FirmwareUiState(connected = true, loading = false, status = FirmwareStatusOutcome.Status(record), record = record,
            check = FirmwareCheckResult.Available(offer)))
        compose.onNodeWithText("Nainstalovaná verze: 0.3.0").assertIsDisplayed()
        compose.onNodeWithText("Poslední aktualizace: verze 0.3.0 byla úspěšně nainstalována.").assertIsDisplayed()
        compose.onNodeWithText("K dispozici je firmware 0.3.1.").performScrollTo()
        compose.onNodeWithText("Oprava čerpadla.").performScrollTo()
        compose.onNodeWithTag(FIRMWARE_CHECK_TEST_TAG).performScrollTo().assertIsEnabled()
        compose.onNodeWithTag(FIRMWARE_BETA_TEST_TAG).performScrollTo().assertIsEnabled()
        compose.onNodeWithTag(FIRMWARE_UPDATE_TEST_TAG).performScrollTo().assertIsEnabled()
    }

    @Test
    fun disconnected_update_is_disabled() {
        show(FirmwareUiState(connected = false, loading = false, status = FirmwareStatusOutcome.NotConnected, record = record,
            check = FirmwareCheckResult.Available(offer)))
        compose.onNodeWithTag(FIRMWARE_UPDATE_TEST_TAG).performScrollTo().assertIsNotEnabled()
        compose.onNodeWithText("Aktualizace vyžaduje připojené zařízení.").performScrollTo()
    }

    @Test
    fun old_firmware_explains_the_usb_installation() {
        show(FirmwareUiState(connected = true, loading = false, status = FirmwareStatusOutcome.Unsupported))
        compose.onNodeWithText("Tento firmware nelze aktualizovat přes Bluetooth. Je potřeba jednou nainstalovat novější firmware přes USB.").assertIsDisplayed()
        compose.onNodeWithTag(FIRMWARE_RELOAD_TEST_TAG).assertIsEnabled()
    }
}
