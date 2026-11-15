package cz.nowass.dispenser.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import cz.nowass.dispenser.R
import cz.nowass.dispenser.data.backup.BlockReason
import cz.nowass.dispenser.data.backup.RecoveryNotice
import cz.nowass.dispenser.data.backup.StorageState
import cz.nowass.dispenser.ui.theme.Spacing

const val STORAGE_BLOCKED_TEST_TAG = "storage_blocked"

/**
 * Startup gate (R1): [content] (which creates repositories and ViewModels) is composed only
 * when local storage is [StorageState.Ready]. While an interrupted restore is rolled back a
 * progress screen is shown; if recovery fails the app stays on a blocked screen.
 */
@Composable
fun StorageGate(
    state: StorageState,
    onRetry: () -> Unit,
    onAcknowledgeNotice: () -> Unit,
    content: @Composable () -> Unit,
) {
    when (state) {
        is StorageState.Ready -> {
            content()
            if (state.notice == RecoveryNotice.INTERRUPTED_RESTORE_ROLLED_BACK) {
                AlertDialog(
                    onDismissRequest = onAcknowledgeNotice,
                    title = { Text(stringResource(R.string.storage_recovered_title)) },
                    text = { Text(stringResource(R.string.storage_recovered_text)) },
                    confirmButton = { TextButton(onClick = onAcknowledgeNotice) { Text(stringResource(R.string.dialog_ok)) } },
                )
            }
        }
        StorageState.Recovering -> Box(
            Modifier.fillMaxSize().background(MaterialTheme.colorScheme.background).safeDrawingPadding(),
            contentAlignment = Alignment.Center,
        ) {
            Column(horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(Spacing.m)) {
                CircularProgressIndicator()
                Text(stringResource(R.string.storage_recovering), color = MaterialTheme.colorScheme.onBackground)
            }
        }
        is StorageState.Blocked -> Box(
            Modifier.fillMaxSize().background(MaterialTheme.colorScheme.background).safeDrawingPadding().padding(Spacing.l).testTag(STORAGE_BLOCKED_TEST_TAG),
            contentAlignment = Alignment.Center,
        ) {
            Column(verticalArrangement = Arrangement.spacedBy(Spacing.m)) {
                Text(stringResource(R.string.storage_blocked_title), style = MaterialTheme.typography.headlineSmall, color = MaterialTheme.colorScheme.error)
                Text(stringResource(R.string.storage_blocked_text), color = MaterialTheme.colorScheme.onBackground)
                Text(
                    stringResource(
                        when (state.reason) {
                            BlockReason.SNAPSHOT_UNAVAILABLE -> R.string.storage_blocked_snapshot
                            BlockReason.ROLLBACK_FAILED -> R.string.storage_blocked_rollback
                        },
                    ),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Button(onClick = onRetry) { Text(stringResource(R.string.storage_retry)) }
            }
        }
    }
}
