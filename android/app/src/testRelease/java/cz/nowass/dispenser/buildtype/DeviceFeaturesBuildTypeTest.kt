package cz.nowass.dispenser.buildtype

import androidx.test.core.app.ApplicationProvider
import cz.nowass.dispenser.DeviceFeatures
import org.junit.Assert.assertFalse
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

/** Runs in testReleaseUnitTest against the merged release resources: release builds hide the BLE device features (v0.5.0-beta.2 decision A1). */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35])
class DeviceFeaturesBuildTypeTest {
    @Test
    fun device_features_flag_of_this_build_type() {
        assertFalse("release builds hide the BLE device features (v0.5.0-beta.2 decision A1)", DeviceFeatures.enabled(ApplicationProvider.getApplicationContext()))
    }
}
