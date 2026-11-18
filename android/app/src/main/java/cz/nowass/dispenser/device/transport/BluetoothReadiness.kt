package cz.nowass.dispenser.device.transport

/** Whether Bluetooth can be used now (docs/ANDROID_BLE_DESIGN.md 4.1). */
sealed interface BluetoothReadiness {
    data object Ready : BluetoothReadiness
    /** No Bluetooth LE hardware (the app still runs in DEMO mode). */
    data object Unsupported : BluetoothReadiness
    data object BluetoothOff : BluetoothReadiness
    /** Runtime permissions to request first (API 31+: BLUETOOTH_CONNECT). */
    data class PermissionMissing(val permissions: List<String>) : BluetoothReadiness
}

fun interface BluetoothPrerequisites {
    fun check(): BluetoothReadiness
}
