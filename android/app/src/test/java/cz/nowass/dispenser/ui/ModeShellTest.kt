package cz.nowass.dispenser.ui

import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertIsNotEnabled
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.hasText
import androidx.compose.ui.test.isHeading
import androidx.compose.ui.test.assertContentDescriptionEquals
import androidx.compose.ui.test.assertCountEquals
import androidx.compose.ui.test.onAllNodesWithText
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import cz.nowass.dispenser.data.AppVersion
import cz.nowass.dispenser.data.InMemoryAppSettingsRepository
import cz.nowass.dispenser.data.MockDispenserRepository
import cz.nowass.dispenser.data.device.DeviceSchedules
import cz.nowass.dispenser.data.device.DeviceUiRepository
import cz.nowass.dispenser.data.device.FakeDeviceRepository
import cz.nowass.dispenser.data.device.Fresh
import cz.nowass.dispenser.domain.RecurrenceKind
import cz.nowass.dispenser.domain.ScheduleDraft
import cz.nowass.dispenser.domain.update.UpdateCheckResult
import cz.nowass.dispenser.ui.components.CONNECTION_CHIP_TEST_TAG
import cz.nowass.dispenser.ui.components.DEMO_BADGE_TEST_TAG
import cz.nowass.dispenser.ui.history.HISTORY_DEVICE_LIST_TEST_TAG
import cz.nowass.dispenser.ui.theme.DispenserTheme
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import java.time.Clock
import java.time.Instant
import java.time.LocalDate
import java.time.LocalTime
import java.time.ZoneOffset

/**
 * The app shell in both modes (M1.10e.4a), on the JVM (Robolectric). DEMO keeps its badge and the
 * Simulace section; DEVICE shows the connection chip instead, cached values with their time, no
 * simulation anywhere, no device history, and online-only editing.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], qualifiers = "w412dp-h915dp")
class ModeShellTest {
    @get:Rule val compose = createComposeRule()

    private val noUpdates = cz.nowass.dispenser.data.update.UpdateChecker { _, _ -> UpdateCheckResult.RepositoryUnavailable }

    @Test
    fun demo_mode_is_unchanged_badge_and_simulation() {
        val repo = MockDispenserRepository()
        compose.setContent {
            DispenserTheme {
                DispenserApp(repo, repo.simulation, InMemoryAppSettingsRepository(), AppVersion("0.5.0", 5), isDemo = true, noUpdates, { null }, null)
            }
        }
        compose.onNodeWithTag(DEMO_BADGE_TEST_TAG).assertIsDisplayed()
        compose.onNodeWithTag(CONNECTION_CHIP_TEST_TAG).assertDoesNotExist()
        compose.onNodeWithTag(TopLevelDestination.SETTINGS.testTag).performClick()
        compose.onNode(hasText("SIMULACE") and isHeading()).performScrollTo().assertIsDisplayed()
        compose.onNodeWithText("Ukázkový režim (bez zařízení)").performScrollTo().assertIsDisplayed()
    }

    @Test
    fun device_mode_shows_connection_and_cached_freshness_never_simulation() {
        // 09:30 UTC = 10:30 in Prague (the schedule zone the UI formats in).
        val readAt = Instant.parse("2026-11-19T09:30:00Z")
        val fake = FakeDeviceRepository()
        val draft = ScheduleDraft("Ranní", 2.0, LocalDate.of(2026, 11, 19), LocalTime.of(18, 0), RecurrenceKind.DAILY)
        fake.statusF.value = Fresh.Cached(FakeDeviceRepository.status(), readAt)
        fake.schedulesF.value = Fresh.Cached(DeviceSchedules(1, listOf(FakeDeviceRepository.schedule(draft, "id-1", 1))), readAt)
        val repo = DeviceUiRepository(fake, Clock.fixed(Instant.parse("2026-11-19T10:00:00Z"), ZoneOffset.UTC), clockTick = null)
        compose.setContent {
            DispenserTheme {
                DispenserApp(repo, simulation = null, InMemoryAppSettingsRepository(), AppVersion("0.5.0", 5), isDemo = false, noUpdates, { null }, null)
            }
        }
        compose.onNodeWithTag(DEMO_BADGE_TEST_TAG).assertDoesNotExist()
        compose.onNodeWithTag(CONNECTION_CHIP_TEST_TAG).assertIsDisplayed()
        compose.onNodeWithTag(CONNECTION_CHIP_TEST_TAG).assertContentDescriptionEquals("Zařízení: Nepřipojeno")
        compose.onNodeWithText("simulace").assertDoesNotExist()
        // Reservoir and battery cards: one device status record, cached, with its time.
        compose.onAllNodesWithText("uloženo 10:30").assertCountEquals(2)
        compose.onNodeWithText("živě").assertDoesNotExist()

        compose.onNodeWithTag(TopLevelDestination.CALENDAR.testTag).performClick()
        compose.onNodeWithText("Rozvrhy ze zařízení · uloženo 10:30").performScrollTo().assertIsDisplayed()
        compose.onNodeWithText("Nový rozvrh").performScrollTo().assertIsNotEnabled()
        compose.onNodeWithText("Změny rozvrhů vyžadují připojené zařízení.").performScrollTo().assertIsDisplayed()

        compose.onNodeWithTag(TopLevelDestination.HISTORY.testTag).performClick()
        // Phase C: the device's own log; disconnected, nothing is read by itself or invented
        compose.onNodeWithTag(HISTORY_DEVICE_LIST_TEST_TAG).assertIsDisplayed()
        compose.onNodeWithText("Zařízení není připojeno. Historii lze načíst jen ze zařízení.").assertIsDisplayed()
        compose.onNodeWithText("Načíst ze zařízení").assertIsDisplayed()
        compose.onNodeWithText("Simulované události – nejde o činnost skutečného čerpadla.").assertDoesNotExist()

        compose.onNodeWithTag(TopLevelDestination.SETTINGS.testTag).performClick()
        compose.onNodeWithText("SIMULACE").assertDoesNotExist()
        compose.onNodeWithText("Zařízení (Bluetooth)").performScrollTo().assertIsDisplayed()
        compose.onNodeWithTag(cz.nowass.dispenser.ui.settings.SETTINGS_CONNECTION_CHIP_TEST_TAG, useUnmergedTree = true).assertContentDescriptionEquals("Zařízení: Nepřipojeno")
    }

    @Test
    fun device_mode_recovery_required_shows_unresolved_operations_and_disables_operations() {
        val fake = FakeDeviceRepository()
        fake.statusF.value = Fresh.Cached(FakeDeviceRepository.status(), Instant.parse("2026-11-19T09:30:00Z"))
        fake.pendingF.value = listOf(
            cz.nowass.dispenser.data.device.PendingOp(id = 1, deviceId = fake.deviceId, kind = cz.nowass.dispenser.data.device.PendingKind.MANUAL_DOSE, createdAtMs = 1, seq = 3, volumeUl = 1000),
        )
        val access = kotlinx.coroutines.flow.MutableStateFlow(cz.nowass.dispenser.domain.DeviceAccess.KEY_MISSING)
        val repo = DeviceUiRepository(fake, Clock.fixed(Instant.parse("2026-11-19T10:00:00Z"), ZoneOffset.UTC), clockTick = null, access = access)
        compose.setContent {
            DispenserTheme {
                DispenserApp(repo, simulation = null, InMemoryAppSettingsRepository(), AppVersion("0.5.0", 5), isDemo = false, noUpdates, { null }, null)
            }
        }
        compose.onNodeWithTag(CONNECTION_CHIP_TEST_TAG).assertContentDescriptionEquals("Zařízení: Vyžaduje obnovu")
        compose.onNodeWithTag(DEMO_BADGE_TEST_TAG).assertDoesNotExist()
        compose.onAllNodesWithText("uloženo 10:30").assertCountEquals(2)
        compose.onNodeWithTag(TopLevelDestination.CALENDAR.testTag).performClick()
        compose.onNodeWithText("Nový rozvrh").performScrollTo().assertIsNotEnabled()
        compose.onNodeWithTag(TopLevelDestination.SETTINGS.testTag).performClick()
        compose.onNodeWithText("Přístup k zařízení je třeba obnovit").performScrollTo().assertIsDisplayed()
        compose.onNodeWithTag(cz.nowass.dispenser.ui.settings.SETTINGS_DEVICE_RECOVERY_TEST_TAG, useUnmergedTree = true).assertExists()
        compose.onNodeWithTag(cz.nowass.dispenser.ui.settings.SETTINGS_DEVICE_UNRESOLVED_TEST_TAG, useUnmergedTree = true).assertExists()
        compose.onNodeWithText("SIMULACE").assertDoesNotExist()
    }
}
