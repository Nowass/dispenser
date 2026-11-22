package cz.nowass.dispenser.buildtype

import androidx.test.core.app.ApplicationProvider
import cz.nowass.dispenser.DeviceFeatures
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

/** Runs in testReleaseUnitTest against the merged release resources: the daily-use app (2026-11-22). */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35])
class DeviceFeaturesBuildTypeTest {
    private val ctx: android.content.Context get() = ApplicationProvider.getApplicationContext()

    @Test
    fun device_features_flag_of_this_build_type() {
        assertTrue("the daily-use release offers the BLE device features", DeviceFeatures.enabled(ctx))
    }

    @Test
    fun no_demo_switch_in_release() {
        assertFalse("switching an owned device's app back to DEMO is a debug tool", DeviceFeatures.demoSwitch(ctx))
    }

    @Test
    fun no_bench_or_debug_wording_on_the_pump_screen() {
        val R = cz.nowass.dispenser.R.string::class.java
        for (name in listOf("settings_device_bench_pump", "bench_title", "bench_warning", "bench_calibrate_now", "bench_calibrate_again", "bench_ready_confirm_text", "bench_not_connected", "app_name")) {
            val text = ctx.getString(R.getField(name).getInt(null))
            for (word in listOf("BENCH", "Bench", "bench", "debug", "DEBUG", "GPIO", "RESET")) {
                assertFalse("$name contains \"$word\": $text", text.contains(word))
            }
        }
    }
}
