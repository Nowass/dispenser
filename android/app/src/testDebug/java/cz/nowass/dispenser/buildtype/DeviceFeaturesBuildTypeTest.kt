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
    fun bench_tools_in_debug() {
        assertTrue("debug builds offer the BENCH ONLY pump configuration", DeviceFeatures.benchTools(ApplicationProvider.getApplicationContext()))
    }
}
