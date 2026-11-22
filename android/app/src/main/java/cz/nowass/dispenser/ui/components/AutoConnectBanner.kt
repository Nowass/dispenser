package cz.nowass.dispenser.ui.components

import android.content.Intent
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import cz.nowass.dispenser.R
import cz.nowass.dispenser.data.device.AutoConnectStatus

const val AUTO_CONNECT_BANNER_TEST_TAG = "auto_connect_banner"
const val AUTO_CONNECT_ACTION_TEST_TAG = "auto_connect_action"

/** The automatic connection's state and the user's retry (DEVICE mode only; null in DEMO). */
class AutoConnectUi(
    val status: kotlinx.coroutines.flow.StateFlow<AutoConnectStatus>,
    val retry: () -> Unit,
)

/** Bluetooth "enable" request (BluetoothAdapter.ACTION_REQUEST_ENABLE, literal: the UI never imports android.bluetooth). */
private const val ACTION_REQUEST_ENABLE = "android.bluetooth.adapter.action.REQUEST_ENABLE"

/**
 * One line under the header while the app looks for the owned device or needs the user (press the
 * button, retry, enable Bluetooth, grant the permission). Nothing is shown when connected or idle (the
 * connection chip shows "Připojeno").
 */
@Composable
fun AutoConnectBanner(status: AutoConnectStatus, onRetry: () -> Unit, modifier: Modifier = Modifier) {
    val permissions = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { onRetry() }
    val enable = rememberLauncherForActivityResult(ActivityResultContracts.StartActivityForResult()) { onRetry() }
    val (text, action) = when (status) {
        AutoConnectStatus.Idle, AutoConnectStatus.Connected -> return
        is AutoConnectStatus.Searching -> stringResource(R.string.auto_connect_searching) to
            status.scanPermission.takeIf { it.isNotEmpty() }?.let { p -> stringResource(R.string.auto_connect_grant_scan) to { permissions.launch(p.toTypedArray()) } }
        AutoConnectStatus.Connecting -> stringResource(R.string.connection_connecting) to null
        AutoConnectStatus.Unavailable -> stringResource(R.string.auto_connect_unavailable) to (stringResource(R.string.auto_connect_retry) to onRetry)
        is AutoConnectStatus.Failed -> stringResource(if (status.authentication) R.string.auto_connect_failed_auth else R.string.auto_connect_failed) to
            (stringResource(R.string.auto_connect_retry) to onRetry)
        AutoConnectStatus.BluetoothOff -> stringResource(R.string.auto_connect_bluetooth_off) to
            (stringResource(R.string.auto_connect_enable) to { enable.launch(Intent(ACTION_REQUEST_ENABLE)) })
        is AutoConnectStatus.PermissionMissing -> stringResource(R.string.auto_connect_permission) to
            (stringResource(R.string.auto_connect_grant) to { permissions.launch(status.permissions.toTypedArray()) })
    }
    Surface(
        color = MaterialTheme.colorScheme.surfaceContainerHigh,
        modifier = modifier.fillMaxWidth().testTag(AUTO_CONNECT_BANNER_TEST_TAG),
    ) {
        Row(
            modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.SpaceBetween,
        ) {
            Text(text, style = MaterialTheme.typography.bodyMedium, modifier = Modifier.weight(1f).padding(vertical = 8.dp))
            if (action != null) {
                TextButton(onClick = action.second, modifier = Modifier.testTag(AUTO_CONNECT_ACTION_TEST_TAG)) { Text(action.first) }
            }
        }
    }
}
