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
import androidx.compose.ui.test.assertCountEquals
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
import cz.nowass.dispenser.ui.components.SecureWindow
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
    fun manifest_requests_only_the_permissions_of_these_steps() {
        val ctx = compose.activity
        val info = ctx.packageManager.getPackageInfo(ctx.packageName, PackageManager.GET_PERMISSIONS or PackageManager.GET_CONFIGURATIONS)
        val requested = info.requestedPermissions.orEmpty().toSet()
        assertTrue("android.permission.BLUETOOTH_CONNECT" in requested)
        assertTrue("android.permission.CAMERA (scanner, asked when opened)", "android.permission.CAMERA" in requested)
        for (p in listOf("android.permission.ACCESS_FINE_LOCATION", "android.permission.ACCESS_COARSE_LOCATION",
            "android.permission.ACCESS_BACKGROUND_LOCATION", "android.permission.BLUETOOTH_SCAN",
            // no telemetry transport (ML Kit was replaced by ZXing, M1.10e.4c)
            "android.permission.ACCESS_NETWORK_STATE", "android.permission.RECEIVE_BOOT_COMPLETED", "android.permission.WAKE_LOCK")) {
            assertFalse("$p is not requested (merged manifest)", p in requested)
        }
        val features = info.reqFeatures.orEmpty().associateBy(FeatureInfo::name)
        for (f in listOf("android.hardware.bluetooth_le", "android.software.companion_device_setup", "android.hardware.camera", "android.hardware.camera.autofocus")) {
            val fi = checkNotNull(features[f]) { "uses-feature $f" }
            assertEquals("$f must not be required (DEMO installs everywhere)", 0, fi.flags and FeatureInfo.FLAG_REQUIRED)
        }
    }

    @Test
    fun no_ml_kit_initializer_or_telemetry_component_is_merged() {
        val ctx = compose.activity
        val info = ctx.packageManager.getPackageInfo(ctx.packageName,
            PackageManager.GET_SERVICES or PackageManager.GET_PROVIDERS or PackageManager.GET_RECEIVERS or PackageManager.GET_ACTIVITIES)
        val names = listOfNotNull(info.services, info.providers, info.receivers, info.activities).flatMap { it.toList() }.map { it.name }
        val forbidden = listOf("com.google.mlkit", "com.google.android.datatransport", "com.google.firebase", "com.google.android.gms")
        assertTrue("merged components: $names", names.none { n -> forbidden.any { n.startsWith(it) } })
        // and no such library is on the runtime classpath
        for (cls in listOf("com.google.mlkit.vision.barcode.BarcodeScanning", "com.google.android.datatransport.runtime.TransportRuntime")) {
            assertTrue("$cls must not be present", runCatching { Class.forName(cls) }.isFailure)
        }
    }

    @Test
    fun the_invitation_screen_is_secure_and_shows_the_code() {
        compose.setContent {
            DispenserTheme {
                cz.nowass.dispenser.ui.devices.InviteScreen(
                    cz.nowass.dispenser.ui.devices.InviteUiState.Showing("DSP2I1:06GB3GPKWKTJ08924CJ2A9H750MJMASC5MQ2YC1H68SK8D9P6WW3JEHV7GYKWFXTSM7TH1WYMKEH0K215Y9Y39YB324ZTTP0GVYPM73NTS8RP69ZW0F4AV7JEYV6188", 1_795_000_600),
                    onBack = {}, onCreate = {}, onCancel = {},
                )
            }
        }
        compose.waitForIdle()
        assertTrue("FLAG_SECURE while the invitation is shown", secureFlag())
        compose.onNodeWithTag(cz.nowass.dispenser.ui.devices.INVITE_QR_TEST_TAG).assertExists()
    }

    @Test
    fun the_label_step_scans_after_asking_for_the_camera() {
        var asked = 0
        val scanned = mutableListOf<String>()
        var granted by mutableStateOf(false)
        compose.setContent {
            DispenserTheme {
                OnboardingScreen(
                    OnboardingUiState(OnboardingPurpose.CLAIM, step = OnboardingStep.LABEL), isDemo = true, onClose = {},
                    actions = cz.nowass.dispenser.ui.onboarding.OnboardingActions(requestCamera = { asked++ }, scanned = { scanned += it }),
                    camera = cz.nowass.dispenser.ui.onboarding.CameraState(granted = granted),
                    // a fake camera: emits the same code on every frame
                    scanner = { onScanned -> androidx.compose.material3.Button(onClick = { onScanned("DSP2:x"); onScanned("DSP2:x") }) { androidx.compose.material3.Text("FAKE_CAMERA") } },
                )
            }
        }
        assertTrue("FLAG_SECURE during scanning and entry", secureFlag())
        compose.onNodeWithTag(cz.nowass.dispenser.ui.onboarding.ONBOARDING_SCAN_TEST_TAG).performScrollTo().performClick()
        assertEquals("the camera permission is asked only when the scanner is opened", 1, asked)
        compose.onNodeWithText("FAKE_CAMERA").assertDoesNotExist()
        granted = true
        compose.onNodeWithText("FAKE_CAMERA").performScrollTo().performClick()
        assertEquals(listOf("DSP2:x", "DSP2:x"), scanned) // routing and de-duplication: OnboardingViewModelTest / QrResultGateTest
        // manual entry stays available
        compose.onNodeWithTag(ONBOARDING_LABEL_FIELD_TEST_TAG).performScrollTo().assertIsDisplayed()
    }

    @Test
    fun the_phone_list_offers_removal_of_other_phones_only() {
        var removed: cz.nowass.dispenser.data.onboarding.PhoneUi? = null
        val a = cz.nowass.dispenser.data.onboarding.PhoneUi(0, "A", 1, 2, isThisPhone = true)
        val b = cz.nowass.dispenser.data.onboarding.PhoneUi(1, "B", 1, 2, isThisPhone = false)
        compose.setContent {
            DispenserTheme {
                cz.nowass.dispenser.ui.devices.PhonesScreen(cz.nowass.dispenser.ui.devices.PhonesUiState(loading = false, phones = listOf(a, b)), {}, {}, { removed = it })
            }
        }
        compose.onAllNodes(androidx.compose.ui.test.hasText("Odebrat") and androidx.compose.ui.test.hasClickAction()).assertCountEquals(1)
        compose.onNodeWithText("Odebrat").performClick()
        compose.onNodeWithText("Odebrat telefon „B“?").assertIsDisplayed()
        compose.onAllNodes(androidx.compose.ui.test.hasText("Odebrat") and androidx.compose.ui.test.hasClickAction())
            .let { it[it.fetchSemanticsNodes().size - 1].performClick() }
        assertEquals(b, removed)
    }

    @Test
    fun settings_offer_the_device_administration_entries() {
        var phones = 0
        var invite = 0
        settings(SettingsDeviceAdmin(bleSupported = true, onPhones = { phones++ }, onInvite = { invite++ }), isDemo = false)
        compose.onNodeWithText("Telefony").performScrollTo().performClick()
        compose.onNodeWithText("Pozvat další telefon").performScrollTo().performClick()
        assertEquals(1, phones)
        assertEquals(1, invite)
    }

    @Test
    fun settings_offer_joining_by_invitation_in_demo() {
        var add = 0
        settings(SettingsDeviceAdmin(bleSupported = true, onAddByInvitation = { add++ }))
        compose.onNodeWithText("Připojit pomocí pozvánky").performScrollTo().performClick()
        assertEquals(1, add)
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

    @Test
    fun forget_failures_are_reported_in_settings() {
        var dismissed = 0
        settings(SettingsDeviceAdmin(bleSupported = true, forgetNotice = cz.nowass.dispenser.ui.settings.ForgetNotice.FAILED_CREDENTIALS,
            onDismissForgetNotice = { dismissed++ }), isDemo = false)
        compose.onNodeWithTag(cz.nowass.dispenser.ui.settings.SETTINGS_FORGET_NOTICE_TEST_TAG, useUnmergedTree = true)
            .assertTextEquals("Zapomenutí se nezdařilo: klíč nebo údaje o spárování se nepodařilo odstranit. Zkuste to znovu.")
        compose.onNodeWithText("OK").performScrollTo().performClick()
        assertEquals(1, dismissed)
    }
}
