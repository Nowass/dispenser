package cz.nowass.dispenser.ui

import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertTextEquals
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.onAllNodesWithText
import androidx.compose.ui.test.assertCountEquals
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import androidx.compose.ui.test.hasText
import androidx.compose.ui.test.hasTestTag
import cz.nowass.dispenser.data.AppVersion
import cz.nowass.dispenser.data.InMemoryAppSettingsRepository
import cz.nowass.dispenser.data.MockDispenserRepository
import cz.nowass.dispenser.data.device.DeviceUiRepository
import cz.nowass.dispenser.data.device.FakeDeviceRepository
import cz.nowass.dispenser.data.device.Fresh
import cz.nowass.dispenser.device.ops.BatteryRecord
import cz.nowass.dispenser.device.ops.BatteryState
import cz.nowass.dispenser.domain.update.UpdateCheckResult
import cz.nowass.dispenser.ui.dashboard.BATTERY_CARD_TEST_TAG
import cz.nowass.dispenser.ui.dashboard.BATTERY_MEASURED_AT_TEST_TAG
import cz.nowass.dispenser.ui.dashboard.BATTERY_REASON_TEST_TAG
import cz.nowass.dispenser.ui.dashboard.BATTERY_REFRESHING_TEST_TAG
import cz.nowass.dispenser.ui.dashboard.BATTERY_REFRESH_MESSAGE_TEST_TAG
import cz.nowass.dispenser.ui.dashboard.BATTERY_VALUE_TEST_TAG
import cz.nowass.dispenser.ui.dashboard.BATTERY_VOLTAGE_TEST_TAG
import cz.nowass.dispenser.ui.theme.DispenserTheme
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import java.time.Clock
import java.time.Instant
import java.time.ZoneOffset

/**
 * The dashboard battery card of the normal app (DEVICE mode) on the JVM: the measured voltage, the
 * percentage marked as an estimate, explicit unavailable states (never 0 V or 0 %), no charging
 * state, and DEMO unchanged.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], qualifiers = "w412dp-h915dp")
class BatteryCardTest {
    @get:Rule val compose = createComposeRule()

    private val noUpdates = cz.nowass.dispenser.data.update.UpdateChecker { _, _ -> UpdateCheckResult.RepositoryUnavailable }
    private val now = Instant.parse("2026-11-19T10:00:00Z")

    private fun device(battery: Fresh<BatteryRecord>, fake: FakeDeviceRepository = FakeDeviceRepository()) {
        fake.authenticated()
        fake.statusF.value = Fresh.Live(FakeDeviceRepository.status().copy(batteryPercent = null), now, 7)
        fake.batteryF.value = battery
        val repo = DeviceUiRepository(fake, Clock.fixed(now, ZoneOffset.UTC), clockTick = null)
        compose.setContent {
            DispenserTheme {
                DispenserApp(repo, simulation = null, InMemoryAppSettingsRepository(), AppVersion("0.5.0", 5), isDemo = false, noUpdates, { null }, null)
            }
        }
    }

    @Test
    fun measured_voltage_with_an_estimated_percentage() {
        device(Fresh.Live(BatteryRecord(BatteryState.VALID, 3970, 5), now.minusSeconds(5), 7))
        compose.onNodeWithTag(BATTERY_VALUE_TEST_TAG, useUnmergedTree = true).performScrollTo().assertTextEquals("≈ 73 %")
        compose.onNodeWithTag(BATTERY_VOLTAGE_TEST_TAG, useUnmergedTree = true).assertTextEquals("Naměřeno 3,97 V")
        compose.onNodeWithText("Procenta jsou přibližný odhad z napětí.").assertIsDisplayed()
        compose.onNodeWithTag(BATTERY_REASON_TEST_TAG, useUnmergedTree = true).assertDoesNotExist()
        compose.onAllNodesWithText("Nabíj", substring = true).assertCountEquals(0) // no charging state
    }

    @Test
    fun cached_reading_keeps_its_measurement_time() {
        // 09:45 UTC = 10:45 in Prague
        device(Fresh.Cached(BatteryRecord(BatteryState.VALID, 3800, 0), Instant.parse("2026-11-19T09:45:00Z")))
        compose.onNodeWithTag(BATTERY_VALUE_TEST_TAG, useUnmergedTree = true).performScrollTo().assertTextEquals("≈ 40 %")
        compose.onNode(hasText("uloženo 10:45")).assertIsDisplayed()
    }

    @Test
    fun unavailable_states_are_explicit() {
        device(Fresh.Live(BatteryRecord(BatteryState.UNSUPPORTED, null, 0), now, 7))
        compose.onNodeWithTag(BATTERY_VALUE_TEST_TAG, useUnmergedTree = true).performScrollTo().assertTextEquals("Nedostupné")
        compose.onNodeWithTag(BATTERY_REASON_TEST_TAG, useUnmergedTree = true)
            .assertTextEquals("Firmware zařízení měření baterie zatím nepodporuje.")
        compose.onNodeWithTag(BATTERY_VOLTAGE_TEST_TAG, useUnmergedTree = true).assertDoesNotExist()
        compose.onAllNodesWithText("0 %").assertCountEquals(0)
        compose.onAllNodesWithText("0,00", substring = true).assertCountEquals(0)
    }

    @Test
    fun measurement_failure_is_explained() {
        device(Fresh.Live(BatteryRecord(BatteryState.ADC_FAILED, null, 0), now, 7))
        compose.onNodeWithTag(BATTERY_REASON_TEST_TAG, useUnmergedTree = true).performScrollTo()
            .assertTextEquals("Měření napětí v zařízení selhalo.")
    }

    @Test
    fun never_read_is_unknown() {
        device(Fresh.Unknown)
        compose.onNodeWithTag(BATTERY_VALUE_TEST_TAG, useUnmergedTree = true).performScrollTo().assertTextEquals("Neznámá")
    }

    @Test
    fun demo_card_is_unchanged() {
        val repo = MockDispenserRepository()
        compose.setContent {
            DispenserTheme {
                DispenserApp(repo, repo.simulation, InMemoryAppSettingsRepository(), AppVersion("0.5.0", 5), isDemo = true, noUpdates, { null }, null)
            }
        }
        compose.onNodeWithTag(BATTERY_VALUE_TEST_TAG, useUnmergedTree = true).performScrollTo().assertTextEquals("78 %")
        compose.onNodeWithText("Simulovaná hodnota, nejde o měření.").assertIsDisplayed()
        compose.onNodeWithTag(BATTERY_VOLTAGE_TEST_TAG, useUnmergedTree = true).assertDoesNotExist()
    }

    // ---- manual refresh: tap the card ------------------------------------------------------------

    @Test
    fun tap_measures_again_and_updates_voltage_estimate_and_time_together() {
        val fake = FakeDeviceRepository()
        val gate = kotlinx.coroutines.CompletableDeferred<Unit>()
        // 09:40:00 UTC = 10:40:00 in Prague; the refreshed sample at 09:59:58 = 10:59:58
        device(Fresh.Live(BatteryRecord(BatteryState.VALID, 4050, 0), Instant.parse("2026-11-19T09:40:00Z"), 7), fake)
        fake.batteryRefresh = {
            gate.await()
            val r = BatteryRecord(BatteryState.VALID, 4160, 2)
            fake.batteryF.value = Fresh.Live(r, Instant.parse("2026-11-19T09:59:58Z"), 7)
            cz.nowass.dispenser.device.ops.OpResult.Ok(r)
        }
        compose.onNodeWithTag(BATTERY_MEASURED_AT_TEST_TAG, useUnmergedTree = true).performScrollTo()
            .assertTextEquals("Změřeno dnes v 10:40:00")
        compose.onNodeWithTag(BATTERY_CARD_TEST_TAG).performClick()
        compose.onNodeWithTag(BATTERY_REFRESHING_TEST_TAG, useUnmergedTree = true).assertExists()
        compose.onNodeWithTag(BATTERY_CARD_TEST_TAG).performClick() // ignored while measuring
        compose.onNodeWithTag(BATTERY_VOLTAGE_TEST_TAG, useUnmergedTree = true).assertTextEquals("Naměřeno 4,05\u00A0V")
        gate.complete(Unit)
        compose.waitForIdle()
        compose.onNodeWithTag(BATTERY_VOLTAGE_TEST_TAG, useUnmergedTree = true).assertTextEquals("Naměřeno 4,16\u00A0V")
        compose.onNodeWithTag(BATTERY_VALUE_TEST_TAG, useUnmergedTree = true).assertTextEquals("≈ 96 %")
        compose.onNodeWithTag(BATTERY_MEASURED_AT_TEST_TAG, useUnmergedTree = true).assertTextEquals("Změřeno dnes v 10:59:58")
        compose.onNodeWithTag(BATTERY_REFRESHING_TEST_TAG, useUnmergedTree = true).assertDoesNotExist()
        compose.onNodeWithTag(BATTERY_REFRESH_MESSAGE_TEST_TAG, useUnmergedTree = true).assertDoesNotExist()
        org.junit.Assert.assertEquals(1, fake.batteryRefreshCalls)
        org.junit.Assert.assertEquals(0, fake.batteryRefreshOverlaps)
    }

    @Test
    fun failed_tap_keeps_the_old_value_and_time_and_says_so() {
        val fake = FakeDeviceRepository()
        device(Fresh.Live(BatteryRecord(BatteryState.VALID, 4050, 0), Instant.parse("2026-11-19T09:40:00Z"), 7), fake)
        fake.batteryRefresh = { cz.nowass.dispenser.device.ops.OpResult.Uncertain("timeout") }
        compose.onNodeWithTag(BATTERY_CARD_TEST_TAG).performScrollTo().performClick()
        compose.onNodeWithTag(BATTERY_REFRESH_MESSAGE_TEST_TAG, useUnmergedTree = true)
            .assertTextEquals("Nové měření se nezdařilo. Zobrazená hodnota je starší.")
        compose.onNodeWithTag(BATTERY_VOLTAGE_TEST_TAG, useUnmergedTree = true).assertTextEquals("Naměřeno 4,05\u00A0V")
        compose.onNodeWithTag(BATTERY_MEASURED_AT_TEST_TAG, useUnmergedTree = true).assertTextEquals("Změřeno dnes v 10:40:00")
    }

    @Test
    fun tap_while_disconnected_explains_that_the_device_must_be_awake() {
        val fake = FakeDeviceRepository()
        device(Fresh.Cached(BatteryRecord(BatteryState.VALID, 4050, 0), Instant.parse("2026-11-19T09:40:00Z")), fake)
        fake.batteryRefresh = { cz.nowass.dispenser.device.ops.OpResult.NotSent("not connected") }
        compose.onNodeWithTag(BATTERY_CARD_TEST_TAG).performScrollTo().performClick()
        compose.onNodeWithTag(BATTERY_REFRESH_MESSAGE_TEST_TAG, useUnmergedTree = true)
            .assertTextEquals("Nelze změřit: dávkovač není připojený. Probuďte ho tlačítkem a počkejte na připojení.")
        compose.onNodeWithTag(BATTERY_MEASURED_AT_TEST_TAG, useUnmergedTree = true).assertTextEquals("Změřeno dnes v 10:40:00")
    }
}
