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
fun SettingsRoute(viewModel: SimulationSettingsViewModel, modifier: Modifier = Modifier) {
    val state by viewModel.uiState.collectAsStateWithLifecycle()
    val message by viewModel.message.collectAsStateWithLifecycle()
    SettingsScreen(
        simulation = state,
        message = message,
        actions = SimulationActions(
            start = viewModel::start,
            plusHour = { viewModel.advanceHours(1) },
            plusDay = { viewModel.advanceDays(1) },
            advanceTo = { viewModel.advanceTo(it, DEFAULT_ZONE) },
            outageUntil = { viewModel.outageUntil(it, DEFAULT_ZONE) },
            reset = viewModel::reset,
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

/** Static settings (demo mode, timezone, future device) plus the DEMO simulation controls (D10). */
@Composable
fun SettingsScreen(
    simulation: SimulationUiState,
    message: SimulationMessage?,
    actions: SimulationActions,
    modifier: Modifier = Modifier,
) {
    val itemColors = ListItemDefaults.colors(
        containerColor = MaterialTheme.colorScheme.surface,
        supportingColor = MaterialTheme.colorScheme.onSurfaceVariant,
    )
    Column(
        modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(horizontal = Spacing.m, vertical = Spacing.s),
        verticalArrangement = Arrangement.spacedBy(Spacing.m),
    ) {
        SurfaceCard {
            Column {
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
                    headlineContent = { Text(stringResource(R.string.settings_device)) },
                    supportingContent = {
                        Column {
                            Text(stringResource(R.string.settings_device_coming_later))
                            OutlinedButton(onClick = {}, enabled = false, modifier = Modifier.padding(top = Spacing.s)) {
                                Text(stringResource(R.string.settings_device_connect))
                            }
                        }
                    },
                    colors = itemColors,
                )
            }
        }
        SimulationCard(simulation, message, actions)
    }
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
            Text(stringResource(R.string.settings_simulation_title), style = MaterialTheme.typography.titleMedium, modifier = Modifier.semantics { heading() })
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
    DispenserTheme { SettingsScreen(SimulationUiState.Active(now, now.minusDays(4)), SimulationMessage.Processed(3), SimulationActions()) }
}

@Preview(name = "Settings inactive", widthDp = 320, heightDp = 900, fontScale = 1.3f, showBackground = true, backgroundColor = 0xFF111820)
@Composable
private fun SettingsInactivePreview() {
    val now = ZonedDateTime.of(LocalDate.of(2026, 11, 15), LocalTime.of(12, 0), DEFAULT_ZONE)
    DispenserTheme { SettingsScreen(SimulationUiState.Inactive(now), null, SimulationActions()) }
}
