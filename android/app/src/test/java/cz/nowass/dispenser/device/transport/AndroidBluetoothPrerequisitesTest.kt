package cz.nowass.dispenser.device.transport

import android.Manifest
import android.app.Application
import android.bluetooth.BluetoothManager
import android.content.pm.PackageManager
import androidx.test.core.app.ApplicationProvider
import org.junit.Assert.assertEquals
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.Shadows.shadowOf
import org.robolectric.annotation.Config

/** [AndroidBluetoothPrerequisites] on the Android framework (Robolectric SDK 35; the API 26-30 path via injected sdkInt). */
@RunWith(RobolectricTestRunner::class)
class AndroidBluetoothPrerequisitesTest {
    private val app: Application = ApplicationProvider.getApplicationContext()
    private val adapter get() = app.getSystemService(BluetoothManager::class.java).adapter

    private fun setFeature(on: Boolean) = shadowOf(app.packageManager).setSystemFeature(PackageManager.FEATURE_BLUETOOTH_LE, on)

    @Test
    @Config(sdk = [35])
    fun api31plus_permission_bluetooth_state_and_feature() {
        val p = AndroidBluetoothPrerequisites(app)
        setFeature(false)
        assertEquals(BluetoothReadiness.Unsupported, p.check())
        setFeature(true)
        shadowOf(app).denyPermissions(Manifest.permission.BLUETOOTH_CONNECT)
        assertEquals(BluetoothReadiness.PermissionMissing(listOf(Manifest.permission.BLUETOOTH_CONNECT)), p.check())
        shadowOf(app).grantPermissions(Manifest.permission.BLUETOOTH_CONNECT)
        shadowOf(adapter).setEnabled(false)
        assertEquals(BluetoothReadiness.BluetoothOff, p.check())
        shadowOf(adapter).setEnabled(true)
        assertEquals(BluetoothReadiness.Ready, p.check())
    }

    /** API 26-30: no runtime Bluetooth permission (BLUETOOTH is install-time); only the adapter state counts. */
    @Test
    @Config(sdk = [35])
    fun api30_path_needs_no_runtime_permission() {
        setFeature(true)
        shadowOf(app).denyPermissions(Manifest.permission.BLUETOOTH_CONNECT) // does not exist below API 31
        shadowOf(adapter).setEnabled(true)
        assertEquals(BluetoothReadiness.Ready, AndroidBluetoothPrerequisites(app, 30).check())
        shadowOf(adapter).setEnabled(false)
        assertEquals(BluetoothReadiness.BluetoothOff, AndroidBluetoothPrerequisites(app, 30).check())
    }
}
