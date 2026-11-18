package cz.nowass.dispenser.device.transport

import android.app.Application
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothGattService
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import androidx.test.core.app.ApplicationProvider
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.Shadows.shadowOf
import org.robolectric.annotation.Config
import java.util.concurrent.CopyOnWriteArrayList

/**
 * [AndroidGattPort] mapping on the Android framework (Robolectric SDK 35, i.e. the API 33+ calls):
 * immediate start results vs completion callbacks for writes and the CCCD subscription.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35])
class AndroidGattPortTest {
    private val app: Application = ApplicationProvider.getApplicationContext()
    private val events = CopyOnWriteArrayList<GattCallback>()
    private lateinit var port: AndroidGattPort
    private lateinit var gatt: BluetoothGatt
    private lateinit var request: BluetoothGattCharacteristic
    private lateinit var response: BluetoothGattCharacteristic
    private lateinit var cccd: BluetoothGattDescriptor

    private fun service(requestWritable: Boolean): BluetoothGattService {
        val s = BluetoothGattService(DispenserGatt.SERVICE, BluetoothGattService.SERVICE_TYPE_PRIMARY)
        s.addCharacteristic(BluetoothGattCharacteristic(DispenserGatt.INFO, BluetoothGattCharacteristic.PROPERTY_READ, BluetoothGattCharacteristic.PERMISSION_READ))
        request = BluetoothGattCharacteristic(DispenserGatt.REQUEST,
            if (requestWritable) BluetoothGattCharacteristic.PROPERTY_WRITE else BluetoothGattCharacteristic.PROPERTY_READ,
            BluetoothGattCharacteristic.PERMISSION_WRITE)
        s.addCharacteristic(request)
        response = BluetoothGattCharacteristic(DispenserGatt.RESPONSE, BluetoothGattCharacteristic.PROPERTY_NOTIFY, BluetoothGattCharacteristic.PERMISSION_READ)
        cccd = BluetoothGattDescriptor(DispenserGatt.CCCD, BluetoothGattDescriptor.PERMISSION_WRITE)
        response.addDescriptor(cccd)
        s.addCharacteristic(response)
        return s
    }

    private fun open(requestWritable: Boolean = true) {
        val adapter = app.getSystemService(BluetoothManager::class.java).adapter
        shadowOf(adapter).setEnabled(true)
        port = AndroidGattPort(app, adapter, "A0:B1:C2:D3:E4:F5") { events += it }
        port.connect()
        gatt = checkNotNull(portGatt())
        shadowOf(gatt).addDiscoverableService(service(requestWritable))
        shadowOf(gatt).getGattCallback().onConnectionStateChange(gatt, BluetoothGatt.GATT_SUCCESS, BluetoothProfile.STATE_CONNECTED)
        assertTrue(port.discoverServices())
        shadowOf(gatt).getGattCallback().onServicesDiscovered(gatt, BluetoothGatt.GATT_SUCCESS)
        assertEquals(GattCallback.ServicesDiscovered(BluetoothGatt.GATT_SUCCESS, requestWritable), events.last())
        events.clear()
    }

    private fun portGatt(): BluetoothGatt? = AndroidGattPort::class.java.getDeclaredField("gatt").apply { isAccessible = true }.get(port) as BluetoothGatt?

    @Before
    fun clear() = events.clear()

    @Test
    fun write_immediate_failure_returns_false() {
        open(requestWritable = false) // the stack refuses the write at once (not BluetoothStatusCodes.SUCCESS)
        assertFalse(port.writeRequest(byteArrayOf(0xC0.toByte(), 1)))
        assertTrue(events.none { it is GattCallback.RequestWritten })
    }

    @Test
    fun write_completion_is_the_callback_not_the_start_result() {
        open()
        // Robolectric completes successful writes synchronously with status 0 through the callback
        assertTrue(port.writeRequest(byteArrayOf(0xC0.toByte(), 1)))
        assertEquals(listOf<GattCallback>(GattCallback.RequestWritten(BluetoothGatt.GATT_SUCCESS)), events.toList())
        assertArrayEquals(byteArrayOf(0xC0.toByte(), 1), shadowOf(gatt).getLatestWrittenBytes())
        // a later completion failure (onCharacteristicWrite status != 0) is delivered as such
        events.clear()
        shadowOf(gatt).getGattCallback().onCharacteristicWrite(gatt, request, 133)
        assertEquals(listOf<GattCallback>(GattCallback.RequestWritten(133)), events.toList())
    }

    @Test
    fun subscription_is_the_cccd_write_and_its_callback() {
        open()
        assertFalse(port.enableNotifications()) // the stack refuses the local enable: subscription not started
        shadowOf(gatt).allowCharacteristicNotification(response)
        assertTrue(port.enableNotifications())
        assertArrayEquals(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE, cccd.value) // the CCCD write carries 0x0001
        // the only completion report is onDescriptorWrite (Robolectric delivers it synchronously with
        // status 0); a missing callback is a subscription timeout (GattTransportTest)
        assertEquals(listOf<GattCallback>(GattCallback.NotificationsEnabled(BluetoothGatt.GATT_SUCCESS)), events.toList())
        shadowOf(gatt).getGattCallback().onDescriptorWrite(gatt, cccd, 5)
        assertEquals(GattCallback.NotificationsEnabled(5), events.last())
        shadowOf(gatt).getGattCallback().onDescriptorWrite(gatt, cccd, BluetoothGatt.GATT_SUCCESS)
        assertEquals(GattCallback.NotificationsEnabled(BluetoothGatt.GATT_SUCCESS), events.last())
    }

    @Test
    fun negotiated_mtu_and_notifications_are_reported_as_received() {
        open()
        shadowOf(gatt).getGattCallback().onMtuChanged(gatt, 185, BluetoothGatt.GATT_SUCCESS)
        assertEquals(GattCallback.MtuChanged(185, BluetoothGatt.GATT_SUCCESS), events.last())
        shadowOf(gatt).getGattCallback().onCharacteristicChanged(gatt, response, byteArrayOf(0xC0.toByte(), 7))
        assertArrayEquals(byteArrayOf(0xC0.toByte(), 7), (events.last() as GattCallback.Notification).value)
        port.close()
    }
}
