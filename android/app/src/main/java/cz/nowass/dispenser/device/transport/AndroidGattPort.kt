package cz.nowass.dispenser.device.transport

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.BluetoothStatusCodes
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import androidx.core.content.ContextCompat

/**
 * The only class that touches `android.bluetooth` GATT (docs/ANDROID_BLE_DESIGN.md 2.1, 4.1–4.2).
 * Thin mapping of [GattPort] onto `BluetoothGatt`, hiding the API 33 signature changes. Callers have
 * checked [AndroidBluetoothPrerequisites]; a permission revoked meanwhile surfaces as
 * SecurityException, handled by [GattTransport]. Runtime behavior is a hardware item (M1.10e.5).
 */
@SuppressLint("MissingPermission") // checked by AndroidBluetoothPrerequisites; SecurityException handled by GattTransport
class AndroidGattPort internal constructor(
    private val context: Context,
    private val adapter: BluetoothAdapter,
    private val address: String,
    private val sink: (GattCallback) -> Unit,
) : GattPort {
    @Volatile private var gatt: BluetoothGatt? = null
    @Volatile private var info: BluetoothGattCharacteristic? = null
    @Volatile private var request: BluetoothGattCharacteristic? = null
    @Volatile private var response: BluetoothGattCharacteristic? = null

    private val callback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            sink(GattCallback.ConnectionState(status == BluetoothGatt.GATT_SUCCESS && newState == BluetoothProfile.STATE_CONNECTED, status))
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            val s = g.getService(DispenserGatt.SERVICE)
            info = s?.getCharacteristic(DispenserGatt.INFO)
            request = s?.getCharacteristic(DispenserGatt.REQUEST)
            response = s?.getCharacteristic(DispenserGatt.RESPONSE)
            val complete = info?.let { it.properties and BluetoothGattCharacteristic.PROPERTY_READ != 0 } == true &&
                request?.let { it.properties and BluetoothGattCharacteristic.PROPERTY_WRITE != 0 } == true &&
                response?.let { it.properties and BluetoothGattCharacteristic.PROPERTY_NOTIFY != 0 && it.getDescriptor(DispenserGatt.CCCD) != null } == true
            sink(GattCallback.ServicesDiscovered(status, complete))
        }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) = sink(GattCallback.MtuChanged(mtu, status))

        override fun onCharacteristicRead(g: BluetoothGatt, c: BluetoothGattCharacteristic, value: ByteArray, status: Int) {
            if (c.uuid == DispenserGatt.INFO) sink(GattCallback.InfoRead(status, value.copyOf()))
        }

        @Deprecated("API < 33")
        override fun onCharacteristicRead(g: BluetoothGatt, c: BluetoothGattCharacteristic, status: Int) {
            @Suppress("DEPRECATION")
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU && c.uuid == DispenserGatt.INFO) {
                sink(GattCallback.InfoRead(status, c.value?.copyOf() ?: ByteArray(0)))
            }
        }

        override fun onDescriptorWrite(g: BluetoothGatt, d: BluetoothGattDescriptor, status: Int) {
            if (d.uuid == DispenserGatt.CCCD) sink(GattCallback.NotificationsEnabled(status))
        }

        override fun onCharacteristicWrite(g: BluetoothGatt, c: BluetoothGattCharacteristic, status: Int) {
            if (c.uuid == DispenserGatt.REQUEST) sink(GattCallback.RequestWritten(status))
        }

        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic, value: ByteArray) {
            if (c.uuid == DispenserGatt.RESPONSE) sink(GattCallback.Notification(value.copyOf()))
        }

        @Deprecated("API < 33")
        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic) {
            @Suppress("DEPRECATION")
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU && c.uuid == DispenserGatt.RESPONSE) {
                sink(GattCallback.Notification(c.value?.copyOf() ?: ByteArray(0)))
            }
        }
    }

    override fun connect() {
        val device: BluetoothDevice = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            adapter.getRemoteLeDevice(address, BluetoothDevice.ADDRESS_TYPE_PUBLIC)
        } else {
            adapter.getRemoteDevice(address)
        }
        gatt = device.connectGatt(context, false, callback, BluetoothDevice.TRANSPORT_LE)
            ?: run { sink(GattCallback.ConnectionState(false, GattTransport.NOT_STARTED)); null }
    }

    override fun discoverServices(): Boolean = gatt?.discoverServices() == true
    override fun requestMtu(mtu: Int): Boolean = gatt?.requestMtu(mtu) == true

    override fun requestHighPriority() {
        gatt?.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH)
    }

    override fun readInfo(): Boolean {
        val g = gatt ?: return false
        val c = info ?: return false
        return g.readCharacteristic(c)
    }

    override fun enableNotifications(): Boolean {
        val g = gatt ?: return false
        val c = response ?: return false
        val d = c.getDescriptor(DispenserGatt.CCCD) ?: return false
        if (!g.setCharacteristicNotification(c, true)) return false
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            g.writeDescriptor(d, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE) == BluetoothStatusCodes.SUCCESS
        } else {
            @Suppress("DEPRECATION")
            d.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
            @Suppress("DEPRECATION")
            g.writeDescriptor(d)
        }
    }

    override fun writeRequest(fragment: ByteArray): Boolean {
        val g = gatt ?: return false
        val c = request ?: return false
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            g.writeCharacteristic(c, fragment, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT) == BluetoothStatusCodes.SUCCESS
        } else {
            @Suppress("DEPRECATION")
            c.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
            @Suppress("DEPRECATION")
            c.value = fragment
            @Suppress("DEPRECATION")
            g.writeCharacteristic(c)
        }
    }

    override fun disconnect() {
        gatt?.disconnect()
    }

    override fun close() {
        gatt?.close()
        gatt = null
    }
}

/** Creates [AndroidGattPort]s. */
class AndroidGattPortFactory(private val context: Context) : GattPortFactory {
    override fun create(address: String, sink: (GattCallback) -> Unit): GattPort {
        val adapter = context.getSystemService(BluetoothManager::class.java)?.adapter
            ?: throw IllegalStateException("no Bluetooth adapter")
        return AndroidGattPort(context.applicationContext, adapter, address, sink)
    }
}

/** Bluetooth readiness on this phone (docs/ANDROID_BLE_DESIGN.md 4.1). Never asks the user for anything. */
class AndroidBluetoothPrerequisites internal constructor(
    private val context: Context,
    private val sdkInt: Int,
) : BluetoothPrerequisites {
    constructor(context: Context) : this(context, Build.VERSION.SDK_INT)

    override fun check(): BluetoothReadiness {
        if (!context.packageManager.hasSystemFeature(PackageManager.FEATURE_BLUETOOTH_LE)) return BluetoothReadiness.Unsupported
        val adapter = context.getSystemService(BluetoothManager::class.java)?.adapter ?: return BluetoothReadiness.Unsupported
        if (sdkInt >= Build.VERSION_CODES.S) {
            val missing = listOf(BLUETOOTH_CONNECT).filter {
                ContextCompat.checkSelfPermission(context, it) != PackageManager.PERMISSION_GRANTED
            }
            if (missing.isNotEmpty()) return BluetoothReadiness.PermissionMissing(missing)
        }
        return try {
            if (adapter.isEnabled) BluetoothReadiness.Ready else BluetoothReadiness.BluetoothOff
        } catch (e: SecurityException) {
            BluetoothReadiness.PermissionMissing(listOf(BLUETOOTH_CONNECT))
        }
    }
}

/** android.Manifest.permission.BLUETOOTH_CONNECT (API 31+), as a literal for the API 26-30 build. */
const val BLUETOOTH_CONNECT = "android.permission.BLUETOOTH_CONNECT"
