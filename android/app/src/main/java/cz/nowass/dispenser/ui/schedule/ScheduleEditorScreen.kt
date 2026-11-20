package cz.nowass.dispenser.ui.schedule

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.BasicAlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DatePicker
import androidx.compose.material3.DatePickerDialog
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilledTonalIconButton
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FilterChipDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TimePicker
import androidx.compose.material3.rememberDatePickerState
import androidx.compose.material3.rememberTimePickerState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.onClick
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.toggleableState
import androidx.compose.ui.state.ToggleableState
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import cz.nowass.dispenser.R
import cz.nowass.dispenser.domain.RecurrenceKind
import cz.nowass.dispenser.domain.SyncState
import cz.nowass.dispenser.ui.Format
import cz.nowass.dispenser.ui.components.DEMO_BADGE_TEST_TAG
import cz.nowass.dispenser.ui.components.DemoBadge
import cz.nowass.dispenser.ui.components.StatusTag
import cz.nowass.dispenser.ui.components.DispenserDatePickerDialog
import cz.nowass.dispenser.ui.components.DispenserTimePickerDialog
import cz.nowass.dispenser.ui.theme.DispenserTheme
import cz.nowass.dispenser.ui.theme.Spacing
import java.time.DayOfWeek
import java.time.Instant
import java.time.LocalDate
import java.time.LocalTime
import java.time.ZoneOffset

const val EDITOR_SAVE_TEST_TAG = "editor_save"
const val EDITOR_NAME_TEST_TAG = "editor_name"
const val EDITOR_AMOUNT_TEST_TAG = "editor_amount"

/** Callbacks from the editor screen to its ViewModel. */
data class ScheduleEditorActions(
    val onName: (String) -> Unit = {},
    val onAmount: (String) -> Unit = {},
    val onStartDate: (LocalDate) -> Unit = {},
    val onTime: (LocalTime) -> Unit = {},
    val onEnabled: (Boolean) -> Unit = {},
    val onKind: (RecurrenceKind) -> Unit = {},
    val onIntervalDelta: (Int) -> Unit = {},
    val onToggleWeekday: (DayOfWeek) -> Unit = {},
    val onHasEndDate: (Boolean) -> Unit = {},
    val onEndDate: (LocalDate) -> Unit = {},
    val onSave: () -> Unit = {},
    val onDelete: () -> Unit = {},
    val onReload: () -> Unit = {},
)

@Composable
fun ScheduleEditorRoute(
    viewModel: ScheduleEditorViewModel,
    isDemo: Boolean,
    /** Called with the result after a successful write, or null when closed without writing. */
    onClose: (EditorResult?) -> Unit,
) {
    val state by viewModel.uiState.collectAsStateWithLifecycle()
    val closeResult by viewModel.closeResult.collectAsStateWithLifecycle()
    LaunchedEffect(closeResult) { closeResult?.let(onClose) }
    ScheduleEditorScreen(
        state = state,
        isDemo = isDemo,
        onClose = { onClose(null) },
        actions = ScheduleEditorActions(
            onName = viewModel::setName,
            onAmount = viewModel::setAmountText,
            onStartDate = viewModel::setStartDate,
            onTime = viewModel::setTime,
            onEnabled = viewModel::setEnabled,
            onKind = viewModel::setKind,
            onIntervalDelta = viewModel::changeInterval,
            onToggleWeekday = viewModel::toggleWeekday,
            onHasEndDate = viewModel::setHasEndDate,
            onEndDate = viewModel::setEndDate,
            onSave = viewModel::save,
            onDelete = viewModel::delete,
            onReload = viewModel::reloadStored,
        ),
    )
}

@Composable
fun ScheduleEditorScreen(
    state: ScheduleEditorUiState,
    isDemo: Boolean,
    onClose: () -> Unit,
    actions: ScheduleEditorActions,
) {
    var showDiscard by rememberSaveable { mutableStateOf(false) }
    val editing = state as? ScheduleEditorUiState.Editing
    val dirty = editing?.isDirty == true
    val saving = editing?.isSaving == true
    val requestClose = {
        when {
            saving -> Unit // a save is in flight: stay until it completes (no lost/duplicate changes)
            dirty -> showDiscard = true
            else -> onClose()
        }
    }

    // While saving, back is consumed and ignored; with unsaved changes it asks first.
    BackHandler(enabled = dirty || saving) { requestClose() }

    // System navigation inset is already applied (and consumed) by the app shell; imePadding adds
    // only the keyboard excess, so the pinned Save bar sits above the keyboard and the gesture bar.
    Column(Modifier.fillMaxSize().imePadding()) {
        EditorTopBar(
            title = stringResource(if (editing?.isNew == false) R.string.editor_title_edit else R.string.editor_title_new),
            isDemo = isDemo,
            closeEnabled = !saving,
            onClose = requestClose,
        )
        when (state) {
            ScheduleEditorUiState.Loading -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                CircularProgressIndicator()
            }
            ScheduleEditorUiState.NotFound -> Column(Modifier.padding(Spacing.m), verticalArrangement = Arrangement.spacedBy(Spacing.m)) {
                Text(stringResource(R.string.editor_not_found), style = MaterialTheme.typography.bodyLarge)
                OutlinedButton(onClick = onClose) { Text(stringResource(R.string.editor_back)) }
            }
            is ScheduleEditorUiState.Editing -> {
                // Floating overlay: the form fills the area and scrolls behind the Save button.
                val density = LocalDensity.current
                var floatingHeight by remember { mutableStateOf(0.dp) }
                Box(Modifier.weight(1f).fillMaxWidth()) {
                    EditorBody(
                        state = state,
                        actions = actions,
                        modifier = Modifier.fillMaxSize(),
                        // Lets the last fields scroll completely above the floating button.
                        bottomContentPadding = floatingHeight,
                    )
                    FloatingSaveButton(
                        state = state,
                        onSave = actions.onSave,
                        modifier = Modifier
                            .align(Alignment.BottomCenter)
                            .onSizeChanged { floatingHeight = with(density) { it.height.toDp() } },
                    )
                }
            }
        }
    }

    if (showDiscard) {
        AlertDialog(
            onDismissRequest = { showDiscard = false },
            title = { Text(stringResource(R.string.editor_discard_title)) },
            text = { Text(stringResource(R.string.editor_discard_text)) },
            confirmButton = {
                TextButton(onClick = { showDiscard = false; onClose() }) {
                    Text(stringResource(R.string.editor_discard_confirm), color = MaterialTheme.colorScheme.error)
                }
            },
            dismissButton = { TextButton(onClick = { showDiscard = false }) { Text(stringResource(R.string.editor_discard_keep)) } },
        )
    }
}

@Composable
private fun EditorTopBar(
    title: String,
    isDemo: Boolean,
    closeEnabled: Boolean,
    onClose: () -> Unit,
) {
    Row(
        Modifier
            .fillMaxWidth()
            .heightIn(min = 64.dp)
            .padding(horizontal = Spacing.xs),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(Spacing.xs),
    ) {
        IconButton(onClick = onClose, enabled = closeEnabled) {
            Icon(Icons.Filled.Close, contentDescription = stringResource(R.string.editor_close))
        }
        Text(
            title,
            style = MaterialTheme.typography.titleLarge,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.weight(1f).semantics { heading() },
        )
        if (isDemo) DemoBadge(Modifier.padding(end = Spacing.s).testTag(DEMO_BADGE_TEST_TAG))
    }
}

/** User-facing message for a field error (shared by field feedback and the Save bar). */
private fun EditorFieldError.messageRes(): Int = when (this) {
    EditorFieldError.AMOUNT_MISSING -> R.string.editor_err_amount_missing
    EditorFieldError.AMOUNT_INVALID -> R.string.editor_err_amount_invalid
    EditorFieldError.AMOUNT_NOT_POSITIVE -> R.string.editor_err_amount_positive
    EditorFieldError.AMOUNT_TOO_PRECISE -> R.string.editor_err_amount_precision
    EditorFieldError.AMOUNT_OUT_OF_RANGE -> R.string.editor_err_amount_range
    EditorFieldError.TIME_NONEXISTENT -> R.string.editor_err_time_nonexistent
    EditorFieldError.INTERVAL_TOO_SMALL -> R.string.editor_err_interval
    EditorFieldError.NO_WEEKDAYS -> R.string.editor_err_weekdays
    EditorFieldError.END_BEFORE_START -> R.string.editor_err_end_before_start
}

/**
 * Standalone floating Save button, overlaid on the scrolling form (no bar, panel or
 * divider). 16 dp side margins, 16 dp above the system navigation area (that inset is
 * already applied by the app shell; the keyboard inset by the editor column). When Save
 * is disabled, the reason floats above it as a small rounded hint.
 */
@Composable
private fun FloatingSaveButton(state: ScheduleEditorUiState.Editing, onSave: () -> Unit, modifier: Modifier = Modifier) {
    val firstError = EditorFieldError.entries.firstOrNull { it in state.blockingErrors }
    val reason: String? = when {
        state.isSaving || state.deletedMeanwhile -> null
        firstError != null -> stringResource(firstError.messageRes())
        state.needsConnection -> stringResource(R.string.editor_needs_connection)
        !state.isNew && !state.isDirty -> stringResource(R.string.editor_save_disabled_unchanged)
        else -> null
    }
    // Red only once it is a real field error; on a fresh form it is a neutral hint.
    val reasonIsError = firstError != null && firstError in state.visibleErrors
    Column(
        modifier
            .fillMaxWidth()
            .padding(start = Spacing.m, end = Spacing.m, bottom = Spacing.m),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(Spacing.s),
    ) {
        if (reason != null) {
            // Small rounded hint (not a bar) so it stays legible over scrolling content.
            Surface(
                shape = MaterialTheme.shapes.large,
                color = MaterialTheme.colorScheme.surfaceContainerHighest,
                shadowElevation = 4.dp,
            ) {
                Text(
                    reason,
                    style = MaterialTheme.typography.bodySmall,
                    color = if (reasonIsError) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier
                        .padding(horizontal = Spacing.m, vertical = 6.dp)
                        .semantics { liveRegion = LiveRegionMode.Polite },
                )
            }
        }
        Button(
            onClick = onSave,
            enabled = state.canSave,
            shape = MaterialTheme.shapes.medium,
            colors = ButtonDefaults.buttonColors(
                containerColor = MaterialTheme.colorScheme.primary,
                contentColor = MaterialTheme.colorScheme.onPrimary,
                // Opaque disabled colors: default disabled container is translucent and would
                // let form text show through the floating button.
                disabledContainerColor = MaterialTheme.colorScheme.surfaceContainerHighest,
                disabledContentColor = MaterialTheme.colorScheme.onSurfaceVariant,
            ),
            elevation = ButtonDefaults.buttonElevation(defaultElevation = 6.dp, pressedElevation = 2.dp, disabledElevation = 2.dp),
            modifier = Modifier.fillMaxWidth().heightIn(min = 52.dp).testTag(EDITOR_SAVE_TEST_TAG),
        ) {
            if (state.isSaving) {
                CircularProgressIndicator(
                    Modifier.size(20.dp),
                    strokeWidth = 2.dp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Spacer(Modifier.size(Spacing.s))
                Text(stringResource(R.string.editor_saving))
            } else {
                Text(stringResource(R.string.editor_save), style = MaterialTheme.typography.titleMedium)
            }
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun EditorBody(
    state: ScheduleEditorUiState.Editing,
    actions: ScheduleEditorActions,
    modifier: Modifier = Modifier,
    bottomContentPadding: Dp = 0.dp,
) {
    val f = state.form
    val errors = state.visibleErrors
    val focus = LocalFocusManager.current
    var showStartPicker by rememberSaveable { mutableStateOf(false) }
    var showEndPicker by rememberSaveable { mutableStateOf(false) }
    var showTimePicker by rememberSaveable { mutableStateOf(false) }
    var showDelete by rememberSaveable { mutableStateOf(false) }

    Column(
        modifier
            .fillMaxWidth()
            .verticalScroll(rememberScrollState())
            .padding(horizontal = Spacing.m, vertical = Spacing.s),
        verticalArrangement = Arrangement.spacedBy(Spacing.m),
    ) {
        StatusLine(state)
        if (state.showOncePerDayRule) {
            Text(
                stringResource(R.string.editor_once_per_day_rule),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.testTag(EDITOR_ONCE_PER_DAY_TEST_TAG),
            )
        }

        if (state.conflict) {
            Banner(stringResource(R.string.editor_conflict), actionLabel = stringResource(R.string.editor_conflict_reload), onAction = actions.onReload)
        }
        if (state.deletedMeanwhile) Banner(stringResource(R.string.editor_deleted_meanwhile))
        state.notice?.let { Banner(stringResource(it.text)) }

        SectionCard(stringResource(R.string.editor_section_dose)) {
            OutlinedTextField(
                value = f.name,
                onValueChange = actions.onName,
                label = { Text(stringResource(R.string.editor_name_label)) },
                placeholder = { Text(stringResource(R.string.editor_name_placeholder)) },
                singleLine = true,
                keyboardOptions = KeyboardOptions(capitalization = KeyboardCapitalization.Sentences, imeAction = ImeAction.Next),
                modifier = Modifier.fillMaxWidth().testTag(EDITOR_NAME_TEST_TAG),
            )
            val amountError = listOf(
                EditorFieldError.AMOUNT_MISSING,
                EditorFieldError.AMOUNT_INVALID,
                EditorFieldError.AMOUNT_NOT_POSITIVE,
                EditorFieldError.AMOUNT_TOO_PRECISE,
                EditorFieldError.AMOUNT_OUT_OF_RANGE,
            ).firstOrNull { it in errors }?.messageRes()
            OutlinedTextField(
                value = f.amountText,
                onValueChange = actions.onAmount,
                label = { Text(stringResource(R.string.editor_amount_label)) },
                suffix = { Text(stringResource(R.string.editor_amount_suffix)) },
                isError = amountError != null,
                supportingText = amountError?.let { { Text(stringResource(it)) } },
                singleLine = true,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal, imeAction = ImeAction.Done),
                keyboardActions = KeyboardActions(onDone = { focus.clearFocus() }),
                modifier = Modifier.fillMaxWidth().testTag(EDITOR_AMOUNT_TEST_TAG),
            )
            SwitchRow(
                label = stringResource(R.string.editor_enabled),
                checked = f.enabled,
                onCheckedChange = actions.onEnabled,
                note = if (!f.enabled) stringResource(R.string.editor_enabled_off_note) else null,
            )
        }

        SectionCard(stringResource(R.string.editor_section_time)) {
            PickerField(
                label = stringResource(if (f.kind == RecurrenceKind.ONCE) R.string.editor_date_once else R.string.editor_start_date),
                value = Format.date(f.startDate),
                onClick = { showStartPicker = true },
            )
            PickerField(
                label = stringResource(R.string.editor_time),
                value = Format.time(f.time),
                onClick = { showTimePicker = true },
                isError = EditorFieldError.TIME_NONEXISTENT in errors,
            )
            if (EditorFieldError.TIME_NONEXISTENT in errors) ErrorText(stringResource(R.string.editor_err_time_nonexistent))
            Text(
                stringResource(R.string.editor_timezone, state.zoneId),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }

        SectionCard(stringResource(R.string.editor_section_repeat)) {
            val kinds = RecurrenceKind.entries
            SingleChoiceSegmentedButtonRow(Modifier.fillMaxWidth()) {
                kinds.forEachIndexed { i, kind ->
                    SegmentedButton(
                        selected = f.kind == kind,
                        onClick = { actions.onKind(kind) },
                        shape = SegmentedButtonDefaults.itemShape(i, kinds.size),
                        icon = {}, // no checkmark: keeps labels untruncated at large font sizes
                        colors = SegmentedButtonDefaults.colors(
                            activeContainerColor = MaterialTheme.colorScheme.primaryContainer,
                            activeContentColor = MaterialTheme.colorScheme.onPrimaryContainer,
                            inactiveContainerColor = MaterialTheme.colorScheme.surface,
                            inactiveContentColor = MaterialTheme.colorScheme.onSurfaceVariant,
                        ),
                    ) {
                        Text(
                            stringResource(
                                when (kind) {
                                    RecurrenceKind.ONCE -> R.string.repeat_once
                                    RecurrenceKind.DAILY -> R.string.repeat_daily
                                    RecurrenceKind.WEEKLY -> R.string.repeat_weekly
                                },
                            ),
                            maxLines = 1,
                        )
                    }
                }
            }

            if (f.kind != RecurrenceKind.ONCE) {
                IntervalStepper(
                    text = if (f.kind == RecurrenceKind.DAILY) {
                        cz.nowass.dispenser.ui.czechPluralStringResource(R.plurals.every_n_days, f.interval, f.interval)
                    } else {
                        cz.nowass.dispenser.ui.czechPluralStringResource(R.plurals.every_n_weeks, f.interval, f.interval)
                    },
                    canDecrease = f.interval > 1,
                    onDelta = actions.onIntervalDelta,
                )
                if (EditorFieldError.INTERVAL_TOO_SMALL in errors) ErrorText(stringResource(R.string.editor_err_interval))
            }

            if (f.kind == RecurrenceKind.WEEKLY) {
                Text(stringResource(R.string.editor_weekdays), style = MaterialTheme.typography.labelLarge)
                FlowRow(horizontalArrangement = Arrangement.spacedBy(Spacing.xs)) {
                    WEEKDAYS_MONDAY_FIRST.forEach { day ->
                        FilterChip(
                            selected = day in f.weekdays,
                            onClick = { actions.onToggleWeekday(day) },
                            label = { Text(weekdayShort(day)) },
                            colors = FilterChipDefaults.filterChipColors(
                                selectedContainerColor = MaterialTheme.colorScheme.primaryContainer,
                                selectedLabelColor = MaterialTheme.colorScheme.onPrimaryContainer,
                            ),
                        )
                    }
                }
                if (EditorFieldError.NO_WEEKDAYS in errors) ErrorText(stringResource(R.string.editor_err_weekdays))
            }

            if (f.kind != RecurrenceKind.ONCE) {
                HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                SwitchRow(
                    label = stringResource(R.string.editor_end_date_toggle),
                    checked = f.hasEndDate,
                    onCheckedChange = actions.onHasEndDate,
                )
                if (f.hasEndDate && f.endDate != null) {
                    PickerField(
                        label = stringResource(R.string.editor_end_date),
                        value = Format.date(f.endDate),
                        onClick = { showEndPicker = true },
                        isError = EditorFieldError.END_BEFORE_START in errors,
                    )
                    Text(
                        stringResource(R.string.editor_end_inclusive_note),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    if (EditorFieldError.END_BEFORE_START in errors) ErrorText(stringResource(R.string.editor_err_end_before_start))
                }
            }
        }

        PreviewCard(state)

        if (!state.isNew) {
            OutlinedButton(
                onClick = { showDelete = true },
                enabled = !state.isSaving && !state.needsConnection,
                border = BorderStroke(1.dp, MaterialTheme.colorScheme.error.copy(alpha = 0.7f)),
                modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp),
            ) {
                Icon(Icons.Filled.Delete, contentDescription = null, tint = MaterialTheme.colorScheme.error)
                Spacer(Modifier.size(Spacing.s))
                Text(stringResource(R.string.editor_delete), color = MaterialTheme.colorScheme.error)
            }
        }
        // Space under the last field = floating button area (measured) + breathing room.
        Spacer(Modifier.height(bottomContentPadding + Spacing.m))
    }

    if (showStartPicker) {
        DispenserDatePickerDialog(initial = f.startDate, onDismiss = { showStartPicker = false }) { actions.onStartDate(it); showStartPicker = false }
    }
    if (showEndPicker) {
        DispenserDatePickerDialog(initial = f.endDate ?: f.startDate, onDismiss = { showEndPicker = false }) { actions.onEndDate(it); showEndPicker = false }
    }
    if (showTimePicker) {
        DispenserTimePickerDialog(initial = f.time, onDismiss = { showTimePicker = false }) { actions.onTime(it); showTimePicker = false }
    }
    if (showDelete) {
        AlertDialog(
            onDismissRequest = { showDelete = false },
            title = { Text(stringResource(R.string.editor_delete_title)) },
            text = { Text(stringResource(R.string.editor_delete_text, scheduleDisplayName(f.name))) },
            confirmButton = {
                TextButton(onClick = { showDelete = false; actions.onDelete() }) {
                    Text(stringResource(R.string.editor_delete_confirm), color = MaterialTheme.colorScheme.error)
                }
            },
            dismissButton = { TextButton(onClick = { showDelete = false }) { Text(stringResource(R.string.dialog_cancel)) } },
        )
    }
}

const val EDITOR_ONCE_PER_DAY_TEST_TAG = "editor_once_per_day"

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun StatusLine(state: ScheduleEditorUiState.Editing) {
    if (state.isNew && !state.isDirty) return
    // Unchanged: "Uloženo v telefonu · Revize N" describes what is on screen.
    // Changed: the phone status is explicitly attributed to the saved version, so
    // "Neuložené změny" and the stored state can never read as contradictory.
    FlowRow(horizontalArrangement = Arrangement.spacedBy(Spacing.xs), verticalArrangement = Arrangement.spacedBy(Spacing.xs)) {
        val sync = state.storedSync
        val revision = state.baseRevision
        if (state.isDirty) {
            StatusTag(stringResource(R.string.editor_unsaved))
            if (sync != null && revision != null) {
                StatusTag(
                    stringResource(
                        if (sync == SyncState.DEVICE_APPLIED) R.string.editor_saved_version_device else R.string.editor_saved_version_phone,
                        revision,
                    ),
                )
            }
        } else {
            sync?.let { StatusTag(syncStateText(it)) }
            revision?.let { StatusTag(stringResource(R.string.editor_revision, it)) }
        }
    }
}

@Composable
private fun Banner(text: String, actionLabel: String? = null, onAction: () -> Unit = {}) {
    Surface(
        color = MaterialTheme.colorScheme.errorContainer,
        contentColor = MaterialTheme.colorScheme.onErrorContainer,
        shape = MaterialTheme.shapes.medium,
        modifier = Modifier.fillMaxWidth(),
    ) {
        Column(Modifier.padding(Spacing.m), verticalArrangement = Arrangement.spacedBy(Spacing.s)) {
            Text(text, style = MaterialTheme.typography.bodyMedium)
            if (actionLabel != null) TextButton(onClick = onAction) { Text(actionLabel) }
        }
    }
}

@Composable
private fun SectionCard(title: String, content: @Composable ColumnScope.() -> Unit) {
    Card(
        shape = MaterialTheme.shapes.large,
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
        modifier = Modifier.fillMaxWidth(),
    ) {
        Column(Modifier.padding(Spacing.m), verticalArrangement = Arrangement.spacedBy(Spacing.m)) {
            Text(title, style = MaterialTheme.typography.titleMedium, modifier = Modifier.semantics { heading() })
            content()
        }
    }
}

/** Read-only field that opens a picker. Whole field is one 56 dp button. */
@Composable
private fun PickerField(label: String, value: String, onClick: () -> Unit, isError: Boolean = false) {
    val a11y = stringResource(R.string.editor_change_a11y, label, value)
    val outline = if (isError) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.outline
    Surface(
        shape = MaterialTheme.shapes.extraSmall,
        border = BorderStroke(1.dp, outline),
        color = MaterialTheme.colorScheme.surface,
        modifier = Modifier
            .fillMaxWidth()
            .heightIn(min = 56.dp)
            .clickable(onClick = onClick)
            .clearAndSetSemantics {
                contentDescription = a11y
                role = Role.Button
                onClick { onClick(); true }
            },
    ) {
        Column(Modifier.padding(horizontal = Spacing.m, vertical = Spacing.s)) {
            Text(label, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            Text(value, style = MaterialTheme.typography.bodyLarge)
        }
    }
}

@Composable
private fun SwitchRow(label: String, checked: Boolean, onCheckedChange: (Boolean) -> Unit, note: String? = null) {
    Row(
        Modifier
            .fillMaxWidth()
            .heightIn(min = 48.dp)
            .clickable { onCheckedChange(!checked) }
            .semantics(mergeDescendants = true) {
                role = Role.Switch
                toggleableState = ToggleableState(checked)
            },
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(label, style = MaterialTheme.typography.bodyLarge)
            if (note != null) {
                Text(note, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
        Switch(checked = checked, onCheckedChange = null)
    }
}

@Composable
private fun IntervalStepper(text: String, canDecrease: Boolean, onDelta: (Int) -> Unit) {
    val decrease = stringResource(R.string.editor_interval_decrease)
    val increase = stringResource(R.string.editor_interval_increase)
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(Spacing.s)) {
        FilledTonalIconButton(
            onClick = { onDelta(-1) },
            enabled = canDecrease,
            modifier = Modifier.semantics { contentDescription = decrease },
        ) {
            Text("−", style = MaterialTheme.typography.titleLarge, modifier = Modifier.clearAndSetSemantics {})
        }
        Text(
            text,
            style = MaterialTheme.typography.bodyLarge,
            modifier = Modifier.weight(1f).semantics { liveRegion = LiveRegionMode.Polite },
        )
        FilledTonalIconButton(
            onClick = { onDelta(1) },
            modifier = Modifier.semantics { contentDescription = increase },
        ) {
            Text("+", style = MaterialTheme.typography.titleLarge, modifier = Modifier.clearAndSetSemantics {})
        }
    }
}

@Composable
private fun ErrorText(text: String) {
    Text(text, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.error)
}

@Composable
private fun PreviewCard(state: ScheduleEditorUiState.Editing) {
    SectionCard(stringResource(R.string.editor_section_preview)) {
        when (val p = state.preview) {
            PreviewUi.Invalid -> Text(stringResource(R.string.editor_preview_invalid), color = MaterialTheme.colorScheme.onSurfaceVariant)
            PreviewUi.Empty -> Text(stringResource(R.string.editor_preview_empty), color = MaterialTheme.colorScheme.onSurfaceVariant)
            is PreviewUi.Items -> Column {
                if (!state.form.enabled) {
                    Text(
                        stringResource(R.string.editor_preview_disabled),
                        style = MaterialTheme.typography.bodySmall,
                        color = cz.nowass.dispenser.ui.theme.DispenserTheme.extendedColors.warning,
                        modifier = Modifier.padding(bottom = Spacing.s),
                    )
                }
                p.items.forEachIndexed { i, item ->
                    if (i > 0) HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                    Row(
                        Modifier.fillMaxWidth().heightIn(min = 44.dp).semantics(mergeDescendants = true) {},
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Text(Format.fullDateTitle(item.date), style = MaterialTheme.typography.bodyLarge, modifier = Modifier.weight(1f))
                        Text(Format.time(item.time), style = MaterialTheme.typography.titleMedium, color = MaterialTheme.colorScheme.primary)
                    }
                }
            }
        }
        Text(
            stringResource(R.string.editor_preview_note),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

// ---- Previews ----

private fun previewEditing(kind: RecurrenceKind, errors: Set<EditorFieldError> = emptySet()) = ScheduleEditorUiState.Editing(
    isNew = false,
    form = EditorForm(
        name = "Večerní doplnění pro velké akvárium v obývacím pokoji",
        amountText = "12,5",
        startDate = LocalDate.of(2026, 11, 16),
        kind = kind,
        interval = 2,
        weekdays = setOf(DayOfWeek.MONDAY, DayOfWeek.THURSDAY),
        hasEndDate = true,
        endDate = LocalDate.of(2026, 12, 31),
    ),
    visibleErrors = errors,
    blockingErrors = errors,
    canSave = errors.isEmpty(),
    preview = PreviewUi.Items(
        (0 until 5).map { PreviewItem("p$it", LocalDate.of(2026, 11, 16).plusDays(it * 3L), LocalTime.of(8, 0)) },
    ),
    zoneId = "Europe/Prague",
    baseRevision = 2,
    storedSync = SyncState.PENDING,
    isDirty = true,
    isSaving = false,
    conflict = false,
    deletedMeanwhile = false,
)

@Preview(name = "Editor weekly 412", widthDp = 412, heightDp = 1500, showBackground = true, backgroundColor = 0xFF111820)
@Composable
private fun EditorWeeklyPreview() {
    DispenserTheme { ScheduleEditorScreen(previewEditing(RecurrenceKind.WEEKLY), isDemo = true, onClose = {}, actions = ScheduleEditorActions()) }
}

@Preview(name = "Editor errors 320, large font", widthDp = 320, heightDp = 1500, fontScale = 1.3f, showBackground = true, backgroundColor = 0xFF111820)
@Composable
private fun EditorErrorsPreview() {
    DispenserTheme {
        ScheduleEditorScreen(
            previewEditing(RecurrenceKind.WEEKLY, setOf(EditorFieldError.AMOUNT_INVALID, EditorFieldError.NO_WEEKDAYS, EditorFieldError.END_BEFORE_START))
                .copy(form = previewEditing(RecurrenceKind.WEEKLY).form.copy(amountText = "abc", weekdays = emptySet()), preview = PreviewUi.Invalid),
            isDemo = true,
            onClose = {},
            actions = ScheduleEditorActions(),
        )
    }
}
