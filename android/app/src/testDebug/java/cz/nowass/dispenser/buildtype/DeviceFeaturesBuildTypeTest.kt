package cz.nowass.dispenser.buildtype

import androidx.test.core.app.ApplicationProvider
import cz.nowass.dispenser.DeviceFeatures
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

/** Runs in testDebugUnitTest against the merged debug resources: debug builds offer the BLE device features. */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35])
class DeviceFeaturesBuildTypeTest {
    @Test
    fun device_features_flag_of_this_build_type() {
        assertTrue("debug builds offer the BLE device features", DeviceFeatures.enabled(ApplicationProvider.getApplicationContext()))
    }

    @Test
    fun demo_switch_and_bench_texts_in_debug() {
        val ctx = ApplicationProvider.getApplicationContext<android.content.Context>()
        assertTrue("debug builds keep the switch back to DEMO", DeviceFeatures.demoSwitch(ctx))
        assertTrue("debug builds keep the BENCH pump screen texts", ctx.getString(cz.nowass.dispenser.R.string.settings_device_bench_pump).startsWith("BENCH"))
    }
}
