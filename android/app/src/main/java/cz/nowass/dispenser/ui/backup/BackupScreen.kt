package cz.nowass.dispenser.ui.backup

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Checkbox
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import cz.nowass.dispenser.R
import cz.nowass.dispenser.data.backup.BackupError
import cz.nowass.dispenser.data.backup.RestorePlan
import cz.nowass.dispenser.data.backup.SimulationEffect
import cz.nowass.dispenser.domain.DEFAULT_ZONE
import cz.nowass.dispenser.ui.Format
import cz.nowass.dispenser.ui.components.DetailTopBar
import cz.nowass.dispenser.ui.czechPluralStringResource
import cz.nowass.dispenser.ui.theme.DispenserTheme
import cz.nowass.dispenser.ui.theme.Spacing

const val BACKUP_EXPORT_TEST_TAG = "backup_export"
const val BACKUP_IMPORT_TEST_TAG = "backup_import"
const val BACKUP_INCLUDE_SIM_TEST_TAG = "backup_include_sim"
const val BACKUP_MESSAGE_TEST_TAG = "backup_message"
const val BACKUP_CONFIRM_TEST_TAG = "backup_confirm"

/** Types offered by the open picker; some providers label .json files as text or binary. */
private val IMPORT_MIME_TYPES = arrayOf("application/json", "text/*", "application/octet-stream")

@Composable
fun BackupRoute(viewModel: BackupViewModel, isDemo: Boolean, onBack: () -> Unit) {
    val state by viewModel.uiState.collectAsStateWithLifecycle()
    // Storage Access Framework: the user picks the document; no storage permission is needed.
    val exportLauncher = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/json")) {
        viewModel.onExportDestination(it?.toString())
    }
    val importLauncher = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) {
        viewModel.onImportSource(it?.toString())
    }
    BackupScreen(
        state = state,
        isDemo = isDemo,
        onBack = onBack,
        onIncludeSimulationChange = viewModel::setIncludeSimulation,
        onExport = { exportLauncher.launch(viewModel.suggestedFileName()) },
        onImport = { importLauncher.launch(IMPORT_MIME_TYPES) },
        onConfirmRestore = viewModel::confirmRestore,
        onCancelRestore = viewModel::cancelRestore,
    )
}

@Composable
fun BackupScreen(
    state: BackupUiState,
    isDemo: Boolean,
    onBack: () -> Unit,
    onIncludeSimulationChange: (Boolean) -> Unit,
    onExport: () -> Unit,
    onImport: () -> Unit,
    onConfirmRestore: () -> Unit,
    onCancelRestore: () -> Unit,
) {
    Column(Modifier.fillMaxSize()) {
        DetailTopBar(stringResource(R.string.backup_title), isDemo, onBack)
        Column(
            Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(horizontal = Spacing.m, vertical = Spacing.s),
            verticalArrangement = Arrangement.spacedBy(Spacing.m),
        ) {
            Text(stringResource(R.string.backup_intro), style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)

            SectionCard(stringResource(R.string.backup_export_title)) {
                Row(
                    Modifier
                        .fillMaxWidth()
                        .testTag(BACKUP_INCLUDE_SIM_TEST_TAG)
                        .toggleable(
                            value = state.includeSimulation,
                            enabled = !state.busy,
                            role = Role.Checkbox,
                            onValueChange = onIncludeSimulationChange,
                        ),
                    verticalAlignment = Alignment.Top,
                ) {
                    Checkbox(checked = state.includeSimulation, onCheckedChange = null, enabled = !state.busy)
                    Column(Modifier.padding(start = Spacing.s)) {
                        Text(stringResource(R.string.backup_include_simulation), style = MaterialTheme.typography.bodyLarge)
                        Text(
                            stringResource(R.string.backup_include_simulation_hint),
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
                Button(onClick = onExport, enabled = !state.busy, modifier = Modifier.fillMaxWidth().testTag(BACKUP_EXPORT_TEST_TAG)) {
                    Text(stringResource(R.string.backup_export_button))
                }
                Text(stringResource(R.string.backup_export_privacy), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }

            SectionCard(stringResource(R.string.backup_restore_title)) {
                Text(stringResource(R.string.backup_restore_hint), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                OutlinedButton(onClick = onImport, enabled = !state.busy, modifier = Modifier.fillMaxWidth().testTag(BACKUP_IMPORT_TEST_TAG)) {
                    Text(stringResource(R.string.backup_restore_button))
                }
            }

            if (state.busy) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(Spacing.s)) {
                    CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.dp)
                    Text(stringResource(R.string.backup_busy), style = MaterialTheme.typography.bodyMedium)
                }
            }
            state.message?.let { MessageCard(it) }
            Spacer(Modifier.height(Spacing.m))
        }
    }

    state.pendingRestore?.let { ConfirmRestoreDialog(it, onConfirmRestore, onCancelRestore) }
}

@Composable
private fun SectionCard(title: String, content: @Composable () -> Unit) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = MaterialTheme.shapes.large,
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
    ) {
        Column(Modifier.padding(Spacing.m), verticalArrangement = Arrangement.spacedBy(Spacing.s)) {
            Text(
                title,
                style = MaterialTheme.typography.titleMedium,
                color = MaterialTheme.colorScheme.onSurface,
                modifier = Modifier.semantics { heading() },
            )
            content()
        }
    }
}

@Composable
private fun MessageCard(message: BackupMessage) {
    val isError = message !is BackupMessage.Exported && message !is BackupMessage.Restored
    val lines = buildList {
        when (message) {
            is BackupMessage.Exported -> {
                add(czechPluralStringResource(R.plurals.backup_export_done, message.schedules, message.schedules))
                message.historyEvents?.let { add(czechPluralStringResource(R.plurals.backup_export_history, it, it)) }
                if (message.unreadableSkipped > 0) {
                    add(czechPluralStringResource(R.plurals.backup_export_unreadable, message.unreadableSkipped, message.unreadableSkipped))
                }
            }
            BackupMessage.ExportFailed -> add(stringResource(R.string.backup_export_failed))
            BackupMessage.ReadFailed -> { add(stringResource(R.string.backup_read_failed)); add(stringResource(R.string.backup_nothing_changed)) }
            is BackupMessage.Invalid -> {
                add(stringResource(R.string.backup_invalid_title))
                add(errorText(message.error))
                message.error.path?.let { add(stringResource(R.string.backup_err_location, it)) }
                add(stringResource(R.string.backup_nothing_changed))
            }
            is BackupMessage.Restored -> {
                add(czechPluralStringResource(R.plurals.backup_restore_done, message.schedules, message.schedules))
                when (message.effect) {
                    SimulationEffect.DEACTIVATED -> add(stringResource(R.string.backup_restore_done_sim_stopped))
                    SimulationEffect.REPLACED -> add(stringResource(R.string.backup_restore_done_sim_replaced))
                    else -> Unit
                }
            }
            BackupMessage.RolledBack -> add(stringResource(R.string.backup_restore_rolled_back))
            is BackupMessage.NotStarted -> {
                add(message.error?.let { errorText(it) } ?: stringResource(R.string.backup_restore_not_started))
                add(stringResource(R.string.backup_nothing_changed))
            }
        }
    }
    Card(
        modifier = Modifier.fillMaxWidth().testTag(BACKUP_MESSAGE_TEST_TAG).semantics { liveRegion = LiveRegionMode.Polite },
        shape = MaterialTheme.shapes.large,
        colors = CardDefaults.cardColors(
            containerColor = if (isError) MaterialTheme.colorScheme.errorContainer else MaterialTheme.colorScheme.surface,
        ),
    ) {
        Column(Modifier.padding(Spacing.m), verticalArrangement = Arrangement.spacedBy(Spacing.xs)) {
            lines.forEach {
                Text(
                    it,
                    style = MaterialTheme.typography.bodyMedium,
                    color = if (isError) MaterialTheme.colorScheme.onErrorContainer else MaterialTheme.colorScheme.onSurface,
                )
            }
        }
    }
}

@Composable
private fun errorText(error: BackupError): String = stringResource(
    when (error.kind) {
        BackupError.Kind.EMPTY -> R.string.backup_err_empty
        BackupError.Kind.TOO_LARGE -> R.string.backup_err_too_large
        BackupError.Kind.NOT_JSON -> R.string.backup_err_not_json
        BackupError.Kind.NOT_A_BACKUP -> R.string.backup_err_not_a_backup
        BackupError.Kind.UNSUPPORTED_VERSION -> R.string.backup_err_unsupported_version
        BackupError.Kind.MISSING_FIELD -> R.string.backup_err_missing_field
        BackupError.Kind.UNKNOWN_FIELD -> R.string.backup_err_unknown_field
        BackupError.Kind.WRONG_TYPE -> R.string.backup_err_wrong_type
        BackupError.Kind.INVALID_VALUE -> R.string.backup_err_invalid_value
        BackupError.Kind.DUPLICATE_ID -> R.string.backup_err_duplicate_id
        BackupError.Kind.INVALID_SCHEDULE -> R.string.backup_err_invalid_schedule
        BackupError.Kind.INCONSISTENT_SIMULATION -> R.string.backup_err_inconsistent_simulation
        BackupError.Kind.TOO_MANY_ITEMS -> R.string.backup_err_too_many_items
        BackupError.Kind.ID_COLLISION_UNREADABLE -> R.string.backup_err_collision_unreadable
        BackupError.Kind.READ_FAILED -> R.string.backup_read_failed
    },
)

@Composable
private fun ConfirmRestoreDialog(plan: RestorePlan, onConfirm: () -> Unit, onDismiss: () -> Unit) {
    val defaultName = stringResource(R.string.dashboard_title)
    val c = plan.content
    val exportedAt = c.exportedAt.atZone(DEFAULT_ZONE)
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.backup_confirm_title)) },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(Spacing.s)) {
                Text(czechPluralStringResource(R.plurals.backup_confirm_schedules, c.schedules.size, c.schedules.size))
                Text(stringResource(R.string.backup_confirm_name, c.displayName ?: defaultName))
                Text(
                    stringResource(R.string.backup_confirm_exported, Format.date(exportedAt.toLocalDate()) + " " + Format.time(exportedAt), c.applicationVersion),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Text(stringResource(R.string.backup_confirm_replace, plan.currentScheduleCount, plan.currentDisplayName ?: defaultName))
                Text(
                    stringResource(
                        when (plan.simulationEffect) {
                            SimulationEffect.REPLACED -> R.string.backup_confirm_sim_replaced
                            SimulationEffect.DEACTIVATED -> R.string.backup_confirm_sim_deactivated
                            SimulationEffect.CONTINUES -> R.string.backup_confirm_sim_continues
                            SimulationEffect.NONE -> R.string.backup_confirm_sim_none
                        },
                    ),
                )
                if (plan.currentUnreadableCount > 0) {
                    Text(czechPluralStringResource(R.plurals.backup_confirm_unreadable, plan.currentUnreadableCount, plan.currentUnreadableCount))
                }
                Text(stringResource(R.string.backup_confirm_pending), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        },
        confirmButton = {
            TextButton(onClick = onConfirm, modifier = Modifier.testTag(BACKUP_CONFIRM_TEST_TAG)) {
                Text(stringResource(R.string.backup_confirm_action), color = MaterialTheme.colorScheme.error)
            }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.dialog_cancel)) } },
    )
}

@Preview(showBackground = true, backgroundColor = 0xFF0B1418)
@Composable
private fun BackupScreenPreview() {
    DispenserTheme {
        BackupScreen(
            state = BackupUiState(message = BackupMessage.Exported(3, null, 0)),
            isDemo = true, onBack = {}, onIncludeSimulationChange = {}, onExport = {}, onImport = {},
            onConfirmRestore = {}, onCancelRestore = {},
        )
    }
}
