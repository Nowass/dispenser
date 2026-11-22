package cz.nowass.dispenser.ui.reservoir

import androidx.compose.ui.test.assertIsEnabled
import androidx.compose.ui.test.assertIsNotEnabled
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performScrollTo
import cz.nowass.dispenser.data.device.DeviceUiRepository
import cz.nowass.dispenser.data.device.FakeDeviceRepository
import cz.nowass.dispenser.data.device.Fresh
import cz.nowass.dispenser.device.ops.ConfigRecord
import cz.nowass.dispenser.ui.theme.DispenserTheme
import org.junit.Assert.assertEquals
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import java.time.Clock
import java.time.Instant
import java.time.ZoneOffset

/** The LOW-threshold setting of the reservoir screen (DEVICE mode, normal app) on the JVM. */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], qualifiers = "w412dp-h915dp")
class ReservoirThresholdUiTest {
    @get:Rule val compose = createComposeRule()
    private val now = Instant.parse("2026-11-22T20:00:00Z")
    private fun cfg(rev: Long, low: Int) = ConfigRecord(rev, 500_000, 480_000, low, 24, 7, 1000, 0, 5000, 20000, 10000, 1, 30000)

    private fun screen(fake: FakeDeviceRepository) {
        val vm = ReservoirViewModel(DeviceUiRepository(fake, Clock.fixed(now, ZoneOffset.UTC), clockTick = null))
        compose.setContent { DispenserTheme { ReservoirRoute(vm, isDemo = false, onBack = {}) } }
    }

    /**
     * Connected: the device's own value and an enabled change button. (Typing into the dialog is not
     * driven here: the dialog never reaches idle under Robolectric; parsing, single flight and the
     * outcomes are covered by DeviceModeViewModelsTest, the firmware effect by
     * DeviceRepositoryFirmwareTest.low_threshold_change_opens_and_closes_….)
     */
    @Test
    fun connected_shows_the_device_threshold_and_offers_the_change() {
        val fake = FakeDeviceRepository()
        fake.authenticated(7)
        fake.statusF.value = Fresh.Live(FakeDeviceRepository.status(capacityUl = 500_000, remainingUl = 480_000), now, 7)
        fake.configF.value = Fresh.Live(cfg(34, 20), now, 7)
        screen(fake)
        compose.onNodeWithText("Zařízení upozorní, když odhadovaná zásoba klesne pod 20 % kapacity.").performScrollTo()
        compose.onNodeWithTag(RESERVOIR_THRESHOLD_EDIT_TEST_TAG).performScrollTo().assertIsEnabled()
        assertEquals(1, fake.refreshConfigCalls) // the live value is read when the screen opens
    }

    @Test
    fun disconnected_threshold_change_is_not_offered() {
        val fake = FakeDeviceRepository() // never connected
        fake.statusF.value = Fresh.Cached(FakeDeviceRepository.status(capacityUl = 500_000, remainingUl = 480_000), now)
        screen(fake)
        compose.onNodeWithTag(RESERVOIR_THRESHOLD_EDIT_TEST_TAG).performScrollTo().assertIsNotEnabled()
        compose.onNodeWithText("Změna meze vyžaduje připojené zařízení.").performScrollTo()
    }
}
