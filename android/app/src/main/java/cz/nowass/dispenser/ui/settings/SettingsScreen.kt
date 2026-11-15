package cz.nowass.dispenser.ui.settings

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import cz.nowass.dispenser.R
import cz.nowass.dispenser.domain.DEFAULT_ZONE
import cz.nowass.dispenser.data.AppVersion
import cz.nowass.dispenser.data.DisplayNameRules
import cz.nowass.dispenser.ui.components.StatusTag
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.height
import androidx.compose.ui.platform.testTag
import cz.nowass.dispenser.ui.Format
import cz.nowass.dispenser.ui.components.DispenserDatePickerDialog
import cz.nowass.dispenser.ui.components.DispenserTimePickerDialog
import cz.nowass.dispenser.ui.czechPluralStringResource
import cz.nowass.dispenser.ui.theme.DispenserTheme
import cz.nowass.dispenser.ui.theme.Spacing
import java.time.LocalDate
import java.time.LocalDateTime
import java.time.LocalTime
import java.time.ZonedDateTime

@Composable
fun SettingsRoute(
    simulationViewModel: SimulationSettingsViewModel,
    appViewModel: AppSettingsViewModel,
    onOpenReservoir: () -> Unit,
    onOpenAbout: () -> Unit,
    modifier: Modifier = Modifier,
    onOpenBackup: (() -> Unit)? = null,
) {
    val state by simulationViewModel.uiState.collectAsStateWithLifecycle()
    val message by simulationViewModel.message.collectAsStateWithLifecycle()
    val app by appViewModel.uiState.collectAsStateWithLifecycle()
    SettingsScreen(
        app = app,
        simulation = state,
        message = message,
        appActions = AppSettingsActions(
            validateName = appViewModel::validateName,
            saveName = appViewModel::saveName,
            resetName = appViewModel::resetName,
            openReservoir = onOpenReservoir,
            openAbout = onOpenAbout,
            openBackup = onOpenBackup,
        ),
        actions = SimulationActions(
            start = simulationViewModel::start,
            plusHour = { simulationViewModel.advanceHours(1) },
            plusDay = { simulationViewModel.advanceDays(1) },
            advanceTo = { simulationViewModel.advanceTo(it, DEFAULT_ZONE) },
            outageUntil = { simulationViewModel.outageUntil(it, DEFAULT_ZONE) },
            reset = simulationViewModel::reset,
        ),
        modifier = modifier,
    )
}

data class SimulationActions(
    val start: (LocalDateTime, java.time.ZoneId, String, String) -> Unit = { _, _, _, _ -> },
    val plusHour: () -> Unit = {},
    val plusDay: () -> Unit = {},
    val advanceTo: (LocalDateTime) -> Unit = {},
    val outageUntil: (LocalDateTime) -> Unit = {},
    val reset: () -> Unit = {},
)

data class AppSettingsActions(
    val validateName: (String) -> DisplayNameRules.Error? = { DisplayNameRules.validate(it) },
    val saveName: (String) -> Unit = {},
    val resetName: () -> Unit = {},
    val openReservoir: () -> Unit = {},
    val openAbout: () -> Unit = {},
    /** Null hides the "Záloha a obnovení" entry. */
    val openBackup: (() -> Unit)? = null,
)

const val SETTINGS_ABOUT_ROW_TEST_TAG = "settings_about_row"
const val SETTINGS_BACKUP_ROW_TEST_TAG = "settings_backup_row"

const val SETTINGS_NAME_ROW_TEST_TAG = "settings_name_row"
const val SETTINGS_RESERVOIR_ROW_TEST_TAG = "settings_reservoir_row"

/**
 * Compact settings in four sections (A1.4a): Aplikace, Zásobník (read-only summary +
 * shortcut), Simulace (existing DEMO controls) and Zařízení (placeholder, never "connected").
 */
@Composable
fun SettingsScreen(
    app: AppSettingsUiState,
    simulation: SimulationUiState,
    message: SimulationMessage?,
    appActions: AppSettingsActions,
    actions: SimulationActions,
    modifier: Modifier = Modifier,
) {
    val itemColors = ListItemDefaults.colors(
        containerColor = MaterialTheme.colorScheme.surface,
        supportingColor = MaterialTheme.colorScheme.onSurfaceVariant,
    )
    var showRename by rememberSaveable { mutableStateOf(false) }
    val ready = app as? AppSettingsUiState.Ready
    val defaultName = stringResource(R.string.dashboard_title)

    Column(
        modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(horizontal = Spacing.m, vertical = Spacing.s),
        verticalArrangement = Arrangement.spacedBy(Spacing.s),
    ) {
        SectionLabel(stringResource(R.string.settings_section_app))
        SurfaceCard {
            Column {
                ListItem(
                    headlineContent = { Text(stringResource(R.string.settings_name)) },
                    supportingContent = {
                        Text(ready?.customName ?: stringResource(R.string.settings_name_default_suffix, defaultName))
                    },
                    colors = itemColors,
                    modifier = Modifier
                        .testTag(SETTINGS_NAME_ROW_TEST_TAG)
                        .clickable(enabled = ready != null, onClickLabel = stringResource(R.string.settings_name_dialog_title)) { showRename = true },
                )
                HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                ListItem(
                    headlineContent = { Text(stringResource(R.string.settings_mode)) },
                    supportingContent = { Text(stringResource(R.string.settings_mode_demo)) },
                    colors = itemColors,
                )
                HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                ListItem(
                    headlineContent = { Text(stringResource(R.string.settings_timezone)) },
                    supportingContent = { Text(DEFAULT_ZONE.id) },
                    colors = itemColors,
                )
                HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                ListItem(
                    headlineContent = { Text(stringResource(R.string.settings_version)) },
                    supportingContent = { Text(ready?.let { stringResource(R.string.settings_version_value, it.version.name, it.version.code.toInt()) } ?: "") },
                    colors = itemColors,
                )
                appActions.openBackup?.let { openBackup ->
                    HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                    ListItem(
                        headlineContent = { Text(stringResource(R.string.settings_backup)) },
                        supportingContent = { Text(stringResource(R.string.settings_backup_hint)) },
                        colors = itemColors,
                        modifier = Modifier.testTag(SETTINGS_BACKUP_ROW_TEST_TAG).clickable(onClick = openBackup),
                    )
                }
                HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                ListItem(
                    headlineContent = { Text(stringResource(R.string.settings_about)) },
                    colors = itemColors,
                    modifier = Modifier.testTag(SETTINGS_ABOUT_ROW_TEST_TAG).clickable(onClick = appActions.openAbout),
                )
            }
        }

        SectionLabel(stringResource(R.string.settings_section_reservoir))
        SurfaceCard {
            Column {
                val r = ready?.reservoir
                if (r == null) {
                    ListItem(
                        headlineContent = { Text(stringResource(R.string.reservoir_unknown)) },
                        supportingContent = { Text(stringResource(R.string.reservoir_simulation_inactive)) },
                        colors = itemColors,
                    )
                } else {
                    ListItem(
                        headlineContent = { Text(stringResource(R.string.settings_reservoir_capacity)) },
                        supportingContent = { Text(stringResource(R.string.unit_ml, Format.ml(r.capacityUl / 1000.0))) },
                        colors = itemColors,
                    )
                    HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                    ListItem(
                        headlineContent = { Text(stringResource(R.string.settings_reservoir_estimate)) },
                        supportingContent = {
                            Text(stringResource(R.string.unit_ml, Format.ml(r.remainingUl / 1000.0)) + " · " + stringResource(R.string.dashboard_estimate_label) + " · " + stringResource(R.string.dashboard_simulated_label))
                        },
                        colors = itemColors,
                    )
                }
                HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                ListItem(
                    headlineContent = { Text(stringResource(R.string.settings_reservoir_manage), color = MaterialTheme.colorScheme.primary) },
                    colors = itemColors,
                    modifier = Modifier.testTag(SETTINGS_RESERVOIR_ROW_TEST_TAG).clickable(onClick = appActions.openReservoir),
                )
            }
        }

        SectionLabel(stringResource(R.string.settings_section_simulation))
        SimulationCard(simulation, message, actions)

        SectionLabel(stringResource(R.string.settings_device))
        SurfaceCard {
            ListItem(
                headlineContent = { Text(stringResource(R.string.settings_device_not_connected)) },
                supportingContent = {
                    Column {
                        // Placeholder only: nothing here may suggest a working physical connection.
                        Text(stringResource(R.string.settings_device_coming_later))
                        OutlinedButton(onClick = {}, enabled = false, modifier = Modifier.padding(top = Spacing.s)) {
                            Text(stringResource(R.string.settings_device_connect))
                        }
                    }
                },
                colors = itemColors,
            )
        }
        Spacer(Modifier.height(Spacing.m))
    }

    if (showRename && ready != null) {
        RenameDialog(
            initial = ready.customName ?: "",
            defaultName = defaultName,
            validate = appActions.validateName,
            onDismiss = { showRename = false },
            onSave = { appActions.saveName(it); showRename = false },
            onReset = { appActions.resetName(); showRename = false },
        )
    }
}

@Composable
private fun SectionLabel(text: String) {
    Text(
        text.uppercase(),
        style = MaterialTheme.typography.labelSmall,
        color = MaterialTheme.colorScheme.primary,
        modifier = Modifier.padding(start = Spacing.xs, top = Spacing.s).semantics { heading() },
    )
}

@Composable
private fun RenameDialog(
    initial: String,
    defaultName: String,
    validate: (String) -> DisplayNameRules.Error?,
    onDismiss: () -> Unit,
    onSave: (String) -> Unit,
    onReset: () -> Unit,
) {
    var text by rememberSaveable { mutableStateOf(initial) }
    val error = validate(text)
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.settings_name_dialog_title)) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(Spacing.xs)) {
                OutlinedTextField(
                    value = text,
                    onValueChange = { text = it },
                    label = { Text(stringResource(R.string.settings_name_label)) },
                    placeholder = { Text(defaultName) },
                    singleLine = true,
                    isError = error != null,
                    supportingText = {
                        Text(
                            if (error != null) stringResource(R.string.settings_name_err_too_long, DisplayNameRules.MAX_LENGTH)
                            else stringResource(R.string.settings_name_counter, DisplayNameRules.normalize(text)?.length ?: 0, DisplayNameRules.MAX_LENGTH),
                        )
                    },
                    modifier = Modifier.fillMaxWidth(),
                )
                Text(stringResource(R.string.settings_name_hint, defaultName), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                TextButton(onClick = onReset) { Text(stringResource(R.string.settings_name_reset)) }
            }
        },
        confirmButton = { TextButton(onClick = { onSave(text) }, enabled = error == null) { Text(stringResource(R.string.editor_save)) } },
        dismissButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.dialog_cancel)) } },
    )
}

@Composable
private fun SurfaceCard(content: @Composable () -> Unit) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = MaterialTheme.shapes.large,
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
    ) { content() }
}

private enum class PickerTarget { START, ADVANCE, OUTAGE }

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun SimulationCard(state: SimulationUiState, message: SimulationMessage?, actions: SimulationActions) {
    var pickerTarget by rememberSaveable { mutableStateOf<PickerTarget?>(null) }
    var pickedDate by rememberSaveable { mutableStateOf<LocalDate?>(null) }
    var showReset by rememberSaveable { mutableStateOf(false) }
    var startAt by rememberSaveable { mutableStateOf<LocalDateTime?>(null) }
    var capacity by rememberSaveable { mutableStateOf("500") }
    var remaining by rememberSaveable { mutableStateOf("500") }

    SurfaceCard {
        Column(Modifier.padding(Spacing.m), verticalArrangement = Arrangement.spacedBy(Spacing.s)) {
            androidx.compose.foundation.layout.Row(
                verticalAlignment = androidx.compose.ui.Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(Spacing.s),
            ) {
                Text(stringResource(R.string.settings_simulation_title), style = MaterialTheme.typography.titleMedium, modifier = Modifier.weight(1f, fill = false).semantics { heading() })
                if (state is SimulationUiState.Active) StatusTag(stringResource(R.string.settings_sim_active_tag))
            }
            when (state) {
                SimulationUiState.Loading -> Unit
                is SimulationUiState.Inactive -> {
                    val start = startAt ?: state.phoneNow.toLocalDateTime()
                    Text(stringResource(R.string.settings_sim_inactive), style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    FilledTonalButton(onClick = { pickerTarget = PickerTarget.START }, modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp)) {
                        Text(stringResource(R.string.settings_sim_start_time) + ": " + Format.date(start.toLocalDate()) + " " + Format.time(start.toLocalTime()))
                    }
                    MlField(stringResource(R.string.settings_sim_capacity), capacity) { capacity = it }
                    MlField(stringResource(R.string.settings_sim_remaining), remaining) { remaining = it }
                    Button(
                        onClick = { actions.start(start, DEFAULT_ZONE, capacity, remaining) },
                        shape = MaterialTheme.shapes.medium,
                        colors = ButtonDefaults.buttonColors(containerColor = MaterialTheme.colorScheme.primary, contentColor = MaterialTheme.colorScheme.onPrimary),
                        modifier = Modifier.fillMaxWidth().heightIn(min = 52.dp),
                    ) { Text(stringResource(R.string.settings_sim_start), style = MaterialTheme.typography.titleMedium) }
                }
                is SimulationUiState.Active -> {
                    Text(stringResource(R.string.settings_sim_clock), style = MaterialTheme.typography.labelLarge, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    Text(simulatedClockText(state.clock), style = MaterialTheme.typography.headlineSmall, color = DispenserTheme.extendedColors.warning)
                    Text(stringResource(R.string.settings_sim_active_note), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    FlowRow(horizontalArrangement = Arrangement.spacedBy(Spacing.s), verticalArrangement = Arrangement.spacedBy(Spacing.s)) {
                        FilledTonalButton(onClick = actions.plusHour) { Text(stringResource(R.string.settings_sim_plus_hour)) }
                        FilledTonalButton(onClick = actions.plusDay) { Text(stringResource(R.string.settings_sim_plus_day)) }
                        FilledTonalButton(onClick = { pickerTarget = PickerTarget.ADVANCE }) { Text(stringResource(R.string.settings_sim_advance_to)) }
                        OutlinedButton(onClick = { pickerTarget = PickerTarget.OUTAGE }) { Text(stringResource(R.string.settings_sim_outage)) }
                    }
                    TextButton(onClick = { showReset = true }, modifier = Modifier.heightIn(min = 48.dp)) {
                        Text(stringResource(R.string.settings_sim_reset), color = MaterialTheme.colorScheme.error)
                    }
                }
            }
            message?.let { MessageText(it) }
        }
    }

    // Date, then time; result goes to the chosen action.
    val target = pickerTarget
    val defaultDateTime = when (state) {
        is SimulationUiState.Active -> state.clock.toLocalDateTime()
        is SimulationUiState.Inactive -> startAt ?: state.phoneNow.toLocalDateTime()
        SimulationUiState.Loading -> null
    }
    if (target != null && defaultDateTime != null) {
        val date = pickedDate
        if (date == null) {
            DispenserDatePickerDialog(initial = defaultDateTime.toLocalDate(), onDismiss = { pickerTarget = null }) { pickedDate = it }
        } else {
            DispenserTimePickerDialog(initial = defaultDateTime.toLocalTime(), onDismiss = { pickerTarget = null; pickedDate = null }) { time ->
                val chosen = LocalDateTime.of(date, time)
                when (target) {
                    PickerTarget.START -> startAt = chosen
                    PickerTarget.ADVANCE -> actions.advanceTo(chosen)
                    PickerTarget.OUTAGE -> actions.outageUntil(chosen)
                }
                pickerTarget = null
                pickedDate = null
            }
        }
    }

    if (showReset) {
        AlertDialog(
            onDismissRequest = { showReset = false },
            title = { Text(stringResource(R.string.settings_sim_reset_title)) },
            text = { Text(stringResource(R.string.settings_sim_reset_text)) },
            confirmButton = {
                TextButton(onClick = { showReset = false; actions.reset() }) {
                    Text(stringResource(R.string.settings_sim_reset_confirm), color = MaterialTheme.colorScheme.error)
                }
            },
            dismissButton = { TextButton(onClick = { showReset = false }) { Text(stringResource(R.string.dialog_cancel)) } },
        )
    }
}

@Composable
private fun MlField(label: String, value: String, onChange: (String) -> Unit) {
    OutlinedTextField(
        value = value,
        onValueChange = onChange,
        label = { Text(label) },
        suffix = { Text(stringResource(R.string.editor_amount_suffix)) },
        singleLine = true,
        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
        modifier = Modifier.fillMaxWidth(),
    )
}

@Composable
private fun MessageText(message: SimulationMessage) {
    val (text, isError) = when (message) {
        is SimulationMessage.Processed -> czechPluralStringResource(R.plurals.settings_sim_events_processed, message.events, message.events) to false
        is SimulationMessage.Failed -> stringResource(
            when (message.reason) {
                SimulationFailure.TARGET_BEFORE_CLOCK -> R.string.settings_sim_err_target_before
                SimulationFailure.RANGE_TOO_LONG -> R.string.settings_sim_err_range
                SimulationFailure.TOO_MANY_OCCURRENCES -> R.string.settings_sim_err_too_many
                SimulationFailure.INVALID_CAPACITY -> R.string.settings_sim_err_capacity
                SimulationFailure.INVALID_VOLUME -> R.string.settings_sim_err_volume
                SimulationFailure.NOT_ACTIVE -> R.string.settings_sim_err_not_active
                SimulationFailure.ALREADY_ACTIVE -> R.string.settings_sim_err_already_active
            },
        ) to true
    }
    Text(
        text,
        style = MaterialTheme.typography.bodySmall,
        color = if (isError) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurfaceVariant,
        modifier = Modifier.semantics { liveRegion = LiveRegionMode.Polite },
    )
}

/** e.g. "18. 11. 2026 08:00". */
fun simulatedClockText(at: ZonedDateTime): String = Format.date(at.toLocalDate()) + " " + Format.time(at)

@Preview(name = "Settings active", widthDp = 412, heightDp = 900, showBackground = true, backgroundColor = 0xFF111820)
@Composable
private fun SettingsActivePreview() {
    val now = ZonedDateTime.of(LocalDate.of(2026, 11, 18), LocalTime.of(8, 0), DEFAULT_ZONE)
    DispenserTheme {
        SettingsScreen(
            AppSettingsUiState.Ready("Obývák 120 l", AppVersion("0.4.0", 4), cz.nowass.dispenser.data.MockSeed.default().reservoir, simulationActive = true),
            SimulationUiState.Active(now, now.minusDays(4)), SimulationMessage.Processed(3), AppSettingsActions(), SimulationActions(),
        )
    }
}

@Preview(name = "Settings inactive", widthDp = 320, heightDp = 900, fontScale = 1.3f, showBackground = true, backgroundColor = 0xFF111820)
@Composable
private fun SettingsInactivePreview() {
    val now = ZonedDateTime.of(LocalDate.of(2026, 11, 15), LocalTime.of(12, 0), DEFAULT_ZONE)
    DispenserTheme {
        SettingsScreen(AppSettingsUiState.Ready(null, AppVersion("0.4.0", 4), null, false), SimulationUiState.Inactive(now), null, AppSettingsActions(), SimulationActions())
    }
}
