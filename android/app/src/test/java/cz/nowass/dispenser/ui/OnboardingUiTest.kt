package cz.nowass.dispenser.ui

import android.content.pm.FeatureInfo
import android.content.pm.PackageManager
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertTextEquals
import androidx.compose.ui.test.junit4.StateRestorationTester
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import androidx.compose.ui.test.performTextInput
import cz.nowass.dispenser.data.AppVersion
import cz.nowass.dispenser.data.onboarding.EnrollOutcome
import cz.nowass.dispenser.data.onboarding.OnboardingPurpose
import cz.nowass.dispenser.ui.onboarding.ONBOARDING_LABEL_FIELD_TEST_TAG
import cz.nowass.dispenser.ui.onboarding.OnboardingScreen
import cz.nowass.dispenser.ui.onboarding.OnboardingStep
import cz.nowass.dispenser.ui.onboarding.OnboardingUiState
import cz.nowass.dispenser.ui.onboarding.SecureWindow
import cz.nowass.dispenser.ui.settings.AppSettingsUiState
import cz.nowass.dispenser.ui.settings.AppSettingsActions
import cz.nowass.dispenser.ui.settings.SETTINGS_DEVICE_CONNECT_TEST_TAG
import cz.nowass.dispenser.ui.settings.SETTINGS_DEVICE_FORGET_TEST_TAG
import cz.nowass.dispenser.ui.settings.SettingsDeviceAdmin
import cz.nowass.dispenser.ui.settings.SettingsScreen
import cz.nowass.dispenser.ui.settings.SimulationActions
import cz.nowass.dispenser.ui.theme.DispenserTheme
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import java.io.File
import javax.xml.parsers.DocumentBuilderFactory

/** M1.10e.4b UI and manifest rules, on the JVM (Robolectric). */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], qualifiers = "w412dp-h915dp")
class OnboardingUiTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()

    private fun secureFlag() = compose.activity.window.attributes.flags and WindowManager.LayoutParams.FLAG_SECURE != 0

    @Test
    fun the_onboarding_window_is_secure_only_while_shown() {
        var shown by mutableStateOf(true)
        compose.setContent { if (shown) SecureWindow() }
        compose.waitForIdle()
        assertTrue("FLAG_SECURE while the onboarding is shown", secureFlag())
        shown = false
        compose.waitForIdle()
        assertFalse("cleared when it is left", secureFlag())
    }

    @Test
    fun the_setup_code_is_not_kept_in_saved_instance_state() {
        val restoration = StateRestorationTester(compose)
        restoration.setContent {
            DispenserTheme {
                OnboardingScreen(OnboardingUiState(OnboardingPurpose.CLAIM, step = OnboardingStep.LABEL), isDemo = true, onClose = {},
                    actions = cz.nowass.dispenser.ui.onboarding.OnboardingActions())
            }
        }
        compose.onNodeWithTag(ONBOARDING_LABEL_FIELD_TEST_TAG).performTextInput("DSP2:a0b1c2d3e4f5:SECRET")
        compose.onNodeWithTag(ONBOARDING_LABEL_FIELD_TEST_TAG).assertTextEquals("Kód zařízení", "DSP2:a0b1c2d3e4f5:SECRET")
        restoration.emulateSavedInstanceStateRestore()
        compose.onNodeWithTag(ONBOARDING_LABEL_FIELD_TEST_TAG).assertTextEquals("Kód zařízení", "")
    }

    @Test
    fun a_failure_with_a_staged_key_explains_the_required_decision() {
        compose.setContent {
            DispenserTheme {
                OnboardingScreen(
                    OnboardingUiState(OnboardingPurpose.CLAIM, step = OnboardingStep.FAILED, failure = EnrollOutcome.Reason.HANDSHAKE, failedWithStaged = true),
                    isDemo = true, onClose = {}, actions = cz.nowass.dispenser.ui.onboarding.OnboardingActions(),
                )
            }
        }
        compose.onNodeWithText("Ověření selhalo – zkontrolujte kód ze štítku.").assertIsDisplayed()
        compose.onNodeWithText("Telefon si ponechal nepotvrzený klíč", substring = true).assertIsDisplayed()
    }

    private fun settings(admin: SettingsDeviceAdmin, isDemo: Boolean = true) {
        compose.setContent {
            DispenserTheme {
                SettingsScreen(
                    app = AppSettingsUiState.Ready(null, AppVersion("0.5.0", 5), null, simulationActive = false, isDemo = isDemo),
                    simulation = null, message = null, appActions = AppSettingsActions(), actions = SimulationActions(), deviceAdmin = admin,
                )
            }
        }
    }

    @Test
    fun settings_offer_pairing_and_recovery_in_demo() {
        var connect = 0
        var recover = 0
        settings(SettingsDeviceAdmin(bleSupported = true, onConnect = { connect++ }, onRecover = { recover++ }))
        compose.onNodeWithTag(SETTINGS_DEVICE_CONNECT_TEST_TAG).performScrollTo().performClick()
        compose.onNodeWithText("Obnovit přístup k zařízení").performScrollTo().performClick()
        assertEquals(1, connect)
        assertEquals(1, recover)
    }

    @Test
    fun settings_without_bluetooth_le_offer_no_pairing() {
        settings(SettingsDeviceAdmin(bleSupported = false))
        compose.onNodeWithText("Tento telefon nepodporuje Bluetooth LE.").performScrollTo().assertIsDisplayed()
        compose.onNodeWithTag(SETTINGS_DEVICE_CONNECT_TEST_TAG).assertDoesNotExist()
    }

    @Test
    fun settings_switch_back_to_a_paired_device() {
        var switched = 0
        settings(SettingsDeviceAdmin(bleSupported = true, selectedDeviceInDemo = "a0b1c2d3e4f5", onSwitchToDevice = { switched++ }))
        compose.onNodeWithText("Spárované zařízení: a0b1c2d3e4f5").performScrollTo().assertIsDisplayed()
        compose.onNodeWithText("Přepnout na zařízení").performScrollTo().performClick()
        assertEquals(1, switched)
    }

    @Test
    fun forgetting_explains_that_the_device_is_not_revoked() {
        var forgot = 0
        settings(SettingsDeviceAdmin(bleSupported = true, onForget = { forgot++ }), isDemo = false)
        compose.onNodeWithTag(SETTINGS_DEVICE_FORGET_TEST_TAG).performScrollTo().performClick()
        compose.onNodeWithText("pamatovat jako oprávněný", substring = true).assertIsDisplayed()
        compose.onNodeWithText("Nevyřešené operace zůstanou uložené", substring = true).assertIsDisplayed()
        assertEquals(0, forgot)
        // the dialog's confirm button (the last clickable "Zapomenout zařízení"; the first opened the dialog)
        compose.onAllNodes(androidx.compose.ui.test.hasText("Zapomenout zařízení") and androidx.compose.ui.test.hasClickAction())
            .let { nodes -> nodes[nodes.fetchSemanticsNodes().size - 1].performClick() }
        assertEquals(1, forgot)
    }

    // ---- manifest (review condition 6: compatibility across the supported API levels) --------

    @Test
    fun manifest_requests_only_the_bluetooth_permissions_of_this_step() {
        val ctx = compose.activity
        val info = ctx.packageManager.getPackageInfo(ctx.packageName, PackageManager.GET_PERMISSIONS or PackageManager.GET_CONFIGURATIONS)
        val requested = info.requestedPermissions.orEmpty().toSet()
        assertTrue("android.permission.BLUETOOTH_CONNECT" in requested)
        for (p in listOf("android.permission.ACCESS_FINE_LOCATION", "android.permission.ACCESS_COARSE_LOCATION",
            "android.permission.ACCESS_BACKGROUND_LOCATION", "android.permission.BLUETOOTH_SCAN", "android.permission.CAMERA")) {
            assertFalse("$p is not requested in M1.10e.4b (merged manifest)", p in requested)
        }
        val features = info.reqFeatures.orEmpty().associateBy(FeatureInfo::name)
        for (f in listOf("android.hardware.bluetooth_le", "android.software.companion_device_setup")) {
            val fi = checkNotNull(features[f]) { "uses-feature $f" }
            assertEquals("$f must not be required (DEMO installs everywhere)", 0, fi.flags and FeatureInfo.FLAG_REQUIRED)
        }
    }

    @Test
    fun legacy_bluetooth_permissions_end_at_api_30() {
        val manifest = File(checkNotNull(System.getProperty("dsp.source.dir")), "../AndroidManifest.xml")
        val doc = DocumentBuilderFactory.newInstance().apply { isNamespaceAware = true }.newDocumentBuilder().parse(manifest)
        val ns = "http://schemas.android.com/apk/res/android"
        val perms = doc.getElementsByTagName("uses-permission").let { l -> (0 until l.length).map { l.item(it) as org.w3c.dom.Element } }
        fun attr(name: String, a: String) = perms.single { it.getAttributeNS(ns, "name") == name }.getAttributeNS(ns, a)
        assertEquals("30", attr("android.permission.BLUETOOTH", "maxSdkVersion"))
        assertEquals("30", attr("android.permission.BLUETOOTH_ADMIN", "maxSdkVersion"))
        assertEquals("", attr("android.permission.BLUETOOTH_CONNECT", "maxSdkVersion"))
    }

    @Test
    fun only_the_onboarding_ever_asks_for_a_runtime_permission() {
        val src = File(checkNotNull(System.getProperty("dsp.source.dir")))
        val askers = src.walk().filter { it.isFile && it.extension == "kt" }
            .filter { f -> listOf("RequestPermission()", "RequestMultiplePermissions()", "requestPermissions(").any { it in f.readText() } }
            .map { it.relativeTo(src).path }.toList()
        assertEquals(listOf("cz/nowass/dispenser/ui/onboarding/OnboardingScreen.kt"), askers)
    }
}
