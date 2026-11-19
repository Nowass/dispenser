package cz.nowass.dispenser.ui

import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertIsNotEnabled
import androidx.compose.ui.test.hasClickAction
import androidx.compose.ui.test.hasText
import androidx.compose.ui.test.isHeading
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import androidx.lifecycle.ViewModelProvider
import cz.nowass.dispenser.data.AppVersion
import cz.nowass.dispenser.data.InMemoryAppSettingsRepository
import cz.nowass.dispenser.data.MockDispenserRepository
import cz.nowass.dispenser.domain.update.UpdateCheckResult
import cz.nowass.dispenser.ui.components.DEMO_BADGE_TEST_TAG
import cz.nowass.dispenser.ui.settings.SETTINGS_DEVICE_CONNECT_TEST_TAG
import cz.nowass.dispenser.ui.theme.DispenserTheme
import kotlinx.coroutines.flow.flowOf
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

/**
 * v0.5.0-beta.2 decision A1: MainActivity passes no device administration when the build type's
 * `device_features_enabled` is off (release; DeviceFeaturesBuildTypeTest checks the flag per build type).
 * The shell then keeps the disabled "coming later" placeholder; with it on (debug) the pairing entries
 * appear. DEMO itself (badge, simulation, mode text) is the same either way.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], qualifiers = "w412dp-h915dp")
class DeviceFeatureVisibilityTest {
    @get:Rule val compose = createComposeRule()

    private val noUpdates = cz.nowass.dispenser.data.update.UpdateChecker { _, _ -> UpdateCheckResult.RepositoryUnavailable }
    private val unusedFactory: (cz.nowass.dispenser.data.onboarding.OnboardingPurpose) -> ViewModelProvider.Factory =
        { error("onboarding is not opened in this test") }

    private fun demoShell(deviceAdmin: DeviceAdminUi?) {
        val repo = MockDispenserRepository()
        compose.setContent {
            DispenserTheme {
                DispenserApp(
                    repo, repo.simulation, InMemoryAppSettingsRepository(), AppVersion("0.5.0", 5), isDemo = true, noUpdates, { null }, null,
                    deviceAdmin = deviceAdmin,
                )
            }
        }
        compose.onNodeWithTag(DEMO_BADGE_TEST_TAG).assertIsDisplayed()
        compose.onNodeWithTag(TopLevelDestination.SETTINGS.testTag).performClick()
        compose.onNode(hasText("SIMULACE") and isHeading()).performScrollTo().assertIsDisplayed()
        compose.onNodeWithText("Ukázkový režim (bez zařízení)").performScrollTo().assertIsDisplayed()
    }

    @Test
    fun without_device_features_demo_keeps_the_disabled_coming_later_placeholder() {
        demoShell(deviceAdmin = null)
        compose.onNodeWithText("Připojení přes Bluetooth bude dostupné později.").performScrollTo().assertIsDisplayed()
        compose.onNode(hasText("Připojit zařízení") and hasClickAction()).performScrollTo().assertIsNotEnabled()
        compose.onNodeWithTag(SETTINGS_DEVICE_CONNECT_TEST_TAG).assertDoesNotExist()
        compose.onNodeWithText("Připojit pomocí pozvánky").assertDoesNotExist()
        compose.onNodeWithText("Obnovit přístup k zařízení").assertDoesNotExist()
    }

    @Test
    fun with_device_features_demo_offers_pairing_invitation_and_recovery() {
        demoShell(DeviceAdminUi(bleSupported = true, selectedDeviceInDemo = flowOf(null), onboardingFactory = unusedFactory,
            switchToDevice = {}, switchToDemo = {}, forget = {}))
        compose.onNodeWithTag(SETTINGS_DEVICE_CONNECT_TEST_TAG).performScrollTo().assertIsDisplayed()
        compose.onNodeWithText("Připojit pomocí pozvánky").performScrollTo().assertIsDisplayed()
        compose.onNodeWithText("Obnovit přístup k zařízení").performScrollTo().assertIsDisplayed()
        compose.onNodeWithText("Připojení přes Bluetooth bude dostupné později.").assertDoesNotExist()
    }
}
