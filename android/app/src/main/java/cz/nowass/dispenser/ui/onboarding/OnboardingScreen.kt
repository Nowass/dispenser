package cz.nowass.dispenser.ui.onboarding

import android.app.Activity
import android.content.Intent
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.IntentSenderRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import cz.nowass.dispenser.R
import cz.nowass.dispenser.data.onboarding.EnrollOutcome
import cz.nowass.dispenser.data.onboarding.EnrollPhase
import cz.nowass.dispenser.data.onboarding.InvitationEntry
import cz.nowass.dispenser.data.onboarding.LabelEntry
import cz.nowass.dispenser.data.onboarding.OnboardingPurpose
import cz.nowass.dispenser.data.onboarding.OnboardingReadiness
import cz.nowass.dispenser.data.onboarding.PhoneName
import cz.nowass.dispenser.ui.components.DetailTopBar
import cz.nowass.dispenser.ui.components.SecureWindow
import cz.nowass.dispenser.ui.theme.DispenserTheme
import cz.nowass.dispenser.ui.theme.Spacing

const val ONBOARDING_LABEL_FIELD_TEST_TAG = "onboarding_label_field"
const val ONBOARDING_SCAN_TEST_TAG = "onboarding_scan"
const val ONBOARDING_STEP_TEST_TAG = "onboarding_step_"

/** android.bluetooth.adapter.action.REQUEST_ENABLE (the UI layer does not import android.bluetooth). */
private const val ACTION_REQUEST_ENABLE = "android.bluetooth.adapter.action.REQUEST_ENABLE"

@Composable
fun OnboardingRoute(viewModel: OnboardingViewModel, isDemo: Boolean, onClose: () -> Unit) {
    SecureWindow()
    val state by viewModel.state.collectAsStateWithLifecycle()
    val dialog by viewModel.dialog.collectAsStateWithLifecycle()

    val permissions = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { viewModel.recheckReadiness() }
    val enableBluetooth = rememberLauncherForActivityResult(ActivityResultContracts.StartActivityForResult()) { viewModel.recheckReadiness() }
    val context = androidx.compose.ui.platform.LocalContext.current
    var cameraGranted by remember {
        mutableStateOf(androidx.core.content.ContextCompat.checkSelfPermission(context, CAMERA) == android.content.pm.PackageManager.PERMISSION_GRANTED)
    }
    var cameraDenied by remember { mutableStateOf(false) }
    // asked only when the user opens the scanner (never in DEMO, never at start)
    val camera = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        cameraGranted = granted
        cameraDenied = !granted
    }
    val association = rememberLauncherForActivityResult(ActivityResultContracts.StartIntentSenderForResult()) { r ->
        viewModel.onAssociationResult(if (r.resultCode == Activity.RESULT_OK) r.data else null)
    }
    LaunchedEffect(dialog) {
        dialog?.let {
            viewModel.dialogLaunched()
            association.launch(IntentSenderRequest.Builder(it).build())
        }
    }

    OnboardingScreen(
        state = state,
        isDemo = isDemo,
        onClose = onClose,
        actions = OnboardingActions(
            start = viewModel::start,
            requestPermissions = { permissions.launch(it.toTypedArray()) },
            enableBluetooth = { enableBluetooth.launch(Intent(ACTION_REQUEST_ENABLE)) },
            recheck = viewModel::recheckReadiness,
            submitLabel = viewModel::submitLabel,
            associate = viewModel::beginAssociation,
            enroll = viewModel::enroll,
            retry = viewModel::retry,
            confirmStaged = viewModel::confirmStaged,
            discardStaged = viewModel::discardStaged,
            finish = viewModel::finish,
            scanned = viewModel::onScanned,
            requestCamera = { camera.launch(CAMERA) },
        ),
        camera = CameraState(granted = cameraGranted, denied = cameraDenied),
    )
}

private const val CAMERA = "android.permission.CAMERA"

/** Camera permission as the onboarding screen knows it (asked only when the scanner is opened). */
data class CameraState(val granted: Boolean = false, val denied: Boolean = false)

data class OnboardingActions(
    val start: () -> Unit = {},
    val requestPermissions: (List<String>) -> Unit = {},
    val enableBluetooth: () -> Unit = {},
    val recheck: () -> Unit = {},
    val submitLabel: (String) -> Boolean = { false },
    val associate: () -> Unit = {},
    val enroll: (String) -> Unit = {},
    val retry: () -> Unit = {},
    val confirmStaged: () -> Unit = {},
    val discardStaged: () -> Unit = {},
    val finish: () -> Unit = {},
    /** A QR text from the scanner (routed by the ViewModel to the current step only). */
    val scanned: (String) -> Unit = {},
    val requestCamera: () -> Unit = {},
)

@Composable
fun OnboardingScreen(
    state: OnboardingUiState,
    isDemo: Boolean,
    onClose: () -> Unit,
    actions: OnboardingActions,
    camera: CameraState = CameraState(),
    /** Renders the camera scanner; replaced in previews and tests (no camera there). */
    scanner: @Composable (onScanned: (String) -> Unit) -> Unit = { onScanned ->
        cz.nowass.dispenser.scanner.QrScanner(onScanned, Modifier.fillMaxWidth().heightIn(min = 280.dp), acceptedPrefixes = listOf("DSP2"))
    },
) {
    SecureWindow() // setup code, scanned invitation and camera preview are never captured
    val recovery = state.purpose == OnboardingPurpose.RECOVERY
    Column(Modifier.fillMaxSize()) {
        DetailTopBar(
            stringResource(
                when (state.purpose) {
                    OnboardingPurpose.RECOVERY -> R.string.onboarding_title_recovery
                    OnboardingPurpose.ADD -> R.string.onboarding_title_add
                    OnboardingPurpose.CLAIM -> R.string.onboarding_title
                },
            ),
            isDemo, onClose,
        )
        Column(
            Modifier.fillMaxSize().verticalScroll(rememberScrollState()).imePadding().padding(horizontal = Spacing.m, vertical = Spacing.s)
                .testTag(ONBOARDING_STEP_TEST_TAG + state.step.name),
            verticalArrangement = Arrangement.spacedBy(Spacing.m),
        ) {
            when (state.step) {
                OnboardingStep.INTRO -> Intro(state.purpose, actions)
                OnboardingStep.READINESS -> Readiness(state.readiness, actions)
                OnboardingStep.INVITATION -> InvitationStep(state.invitationProblem, camera, actions, scanner)
                OnboardingStep.LABEL -> Label(state, camera, actions, scanner)
                OnboardingStep.ASSOCIATE -> Associate(state, recovery, actions)
                OnboardingStep.NAME -> Name(recovery, actions)
                OnboardingStep.WORKING -> Working(state.phase, recovery)
                OnboardingStep.DONE -> Done(state.deviceId, actions)
                OnboardingStep.FAILED -> Failed(state.failure, state.failedWithStaged, onClose, actions)
                OnboardingStep.UNCONFIRMED -> Unconfirmed(state.deviceId, actions)
            }
        }
    }
}

@Composable
private fun Section(content: @Composable ColumnScope.() -> Unit) {
    Card(
        shape = MaterialTheme.shapes.large,
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
        modifier = Modifier.fillMaxWidth(),
    ) { Column(Modifier.padding(Spacing.m), verticalArrangement = Arrangement.spacedBy(Spacing.s), content = content) }
}

@Composable
private fun Body(text: String) = Text(text, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)

@Composable
private fun Primary(text: String, enabled: Boolean = true, onClick: () -> Unit) =
    Button(onClick = onClick, enabled = enabled, shape = MaterialTheme.shapes.medium, modifier = Modifier.fillMaxWidth().heightIn(min = 52.dp)) {
        Text(text, style = MaterialTheme.typography.titleMedium)
    }

@Composable
private fun Problem(text: String) = Text(
    text,
    style = MaterialTheme.typography.bodyMedium,
    color = MaterialTheme.colorScheme.error,
    modifier = Modifier.semantics { liveRegion = LiveRegionMode.Polite },
)

@Composable
private fun Intro(purpose: OnboardingPurpose, actions: OnboardingActions) {
    Section {
        Text(stringResource(R.string.onboarding_intro_title), style = MaterialTheme.typography.titleMedium)
        if (purpose == OnboardingPurpose.ADD) Body(stringResource(R.string.onboarding_intro_invitation))
        Body(stringResource(R.string.onboarding_intro_label))
        Body(stringResource(if (purpose == OnboardingPurpose.RECOVERY) R.string.onboarding_intro_window_recovery else R.string.onboarding_intro_window))
        Body(stringResource(R.string.onboarding_intro_bluetooth))
    }
    Primary(stringResource(R.string.onboarding_continue), onClick = actions.start)
}

@Composable
private fun Readiness(readiness: OnboardingReadiness?, actions: OnboardingActions) {
    Section {
        when (readiness) {
            is OnboardingReadiness.PermissionMissing -> {
                Body(stringResource(R.string.onboarding_permission_needed))
                Primary(stringResource(R.string.onboarding_permission_grant)) { actions.requestPermissions(readiness.permissions) }
                Body(stringResource(R.string.onboarding_permission_settings_hint))
            }
            OnboardingReadiness.BluetoothOff -> {
                Body(stringResource(R.string.onboarding_bluetooth_off))
                Primary(stringResource(R.string.onboarding_bluetooth_enable), onClick = actions.enableBluetooth)
            }
            OnboardingReadiness.Unsupported -> Body(stringResource(R.string.onboarding_unsupported))
            OnboardingReadiness.Ready, null -> Unit
        }
        TextButton(onClick = actions.recheck) { Text(stringResource(R.string.onboarding_recheck)) }
    }
}

/** The camera scanner, opened by the user; asks for the camera permission first. */
@Composable
private fun ScannerArea(
    camera: CameraState,
    actions: OnboardingActions,
    scanner: @Composable (onScanned: (String) -> Unit) -> Unit,
    deniedText: Int,
    open: Boolean,
    onOpen: () -> Unit,
) {
    when {
        !open -> OutlinedButton(onClick = { onOpen(); if (!camera.granted) actions.requestCamera() }, modifier = Modifier.fillMaxWidth().testTag(ONBOARDING_SCAN_TEST_TAG)) {
            Text(stringResource(R.string.onboarding_scan_open))
        }
        camera.granted -> scanner(actions.scanned)
        camera.denied -> Problem(stringResource(deniedText))
        else -> Body(stringResource(R.string.onboarding_scan_permission))
    }
}

@Composable
private fun InvitationStep(
    problem: InvitationEntry.Problem?,
    camera: CameraState,
    actions: OnboardingActions,
    scanner: @Composable (onScanned: (String) -> Unit) -> Unit,
) {
    var open by remember { mutableStateOf(false) }
    Section {
        Text(stringResource(R.string.onboarding_invitation_title), style = MaterialTheme.typography.titleMedium)
        Body(stringResource(R.string.onboarding_invitation_hint))
        problem?.let {
            Problem(
                stringResource(
                    when (it) {
                        InvitationEntry.Problem.NOT_INVITATION -> R.string.onboarding_invitation_err_not_invitation
                        InvitationEntry.Problem.CORRUPT -> R.string.onboarding_invitation_err_corrupt
                        InvitationEntry.Problem.UNSUPPORTED -> R.string.onboarding_invitation_err_unsupported
                        InvitationEntry.Problem.EXPIRED -> R.string.onboarding_invitation_err_expired
                    },
                ),
            )
        }
        // a rejected scan opens a fresh scanner (a new gate) on the next attempt
        ScannerArea(camera, actions, scanner, R.string.onboarding_scan_denied_invitation, open && problem == null) { open = true }
    }
}

@Composable
private fun Label(
    state: OnboardingUiState,
    camera: CameraState,
    actions: OnboardingActions,
    scanner: @Composable (onScanned: (String) -> Unit) -> Unit,
) {
    val problem = state.labelProblem
    var scanning by remember { mutableStateOf(false) }
    // The setup code is held only in this non-saveable state (never in the saved instance state) and
    // cleared as soon as it was handed over; the keyboard is told not to learn or suggest it.
    var text by remember { mutableStateOf("") }
    Section {
        Text(stringResource(R.string.onboarding_label_title), style = MaterialTheme.typography.titleMedium)
        if (state.purpose == OnboardingPurpose.ADD) {
            state.deviceId?.let { Body(stringResource(R.string.onboarding_invitation_accepted, it)) }
            if (state.invitationClockWarning) Body(stringResource(R.string.onboarding_invitation_clock_warning))
        }
        ScannerArea(camera, actions, scanner, R.string.onboarding_scan_denied_label, scanning && problem == null) { scanning = true }
        Body(stringResource(R.string.onboarding_label_hint))
        OutlinedTextField(
            value = text,
            onValueChange = { text = it },
            label = { Text(stringResource(R.string.onboarding_label_field)) },
            singleLine = true,
            isError = problem != null,
            keyboardOptions = KeyboardOptions(
                capitalization = KeyboardCapitalization.Characters,
                autoCorrectEnabled = false,
                keyboardType = KeyboardType.Password,
                imeAction = ImeAction.Done,
            ),
            modifier = Modifier.fillMaxWidth().testTag(ONBOARDING_LABEL_FIELD_TEST_TAG),
        )
        problem?.let {
            Problem(
                stringResource(
                    when (it) {
                        LabelEntry.Problem.FORMAT -> R.string.onboarding_label_err_format
                        LabelEntry.Problem.DEVICE_ID -> R.string.onboarding_label_err_device
                        LabelEntry.Problem.CODE -> R.string.onboarding_label_err_code
                        LabelEntry.Problem.CHECK -> R.string.onboarding_label_err_check
                        LabelEntry.Problem.OTHER_DEVICE -> R.string.onboarding_label_err_other_device
                    },
                ),
            )
        }
    }
    Primary(stringResource(R.string.onboarding_continue), enabled = text.isNotBlank()) {
        if (actions.submitLabel(text)) text = ""
    }
}

@Composable
private fun Associate(state: OnboardingUiState, recovery: Boolean, actions: OnboardingActions) {
    Section {
        Text(stringResource(R.string.onboarding_associate_title), style = MaterialTheme.typography.titleMedium)
        state.deviceId?.let { Body(stringResource(R.string.onboarding_device_id, it)) }
        Body(stringResource(if (recovery) R.string.onboarding_associate_hint_recovery else R.string.onboarding_associate_hint))
        state.associationProblem?.let {
            Problem(
                stringResource(
                    when (it) {
                        AssociationProblem.CANCELLED -> R.string.onboarding_associate_cancelled
                        AssociationProblem.UNSUPPORTED -> R.string.onboarding_unsupported
                        AssociationProblem.FAILED -> R.string.onboarding_associate_failed
                    },
                ),
            )
        }
    }
    Primary(stringResource(R.string.onboarding_associate_search), enabled = !state.busy, onClick = actions.associate)
}

@Composable
private fun Name(recovery: Boolean, actions: OnboardingActions) {
    var name by rememberSaveable { mutableStateOf(defaultPhoneName()) }
    val valid = PhoneName.normalize(name) != null
    Section {
        Text(stringResource(R.string.onboarding_name_title), style = MaterialTheme.typography.titleMedium)
        Body(stringResource(R.string.onboarding_name_hint))
        OutlinedTextField(
            value = name,
            onValueChange = { name = it },
            label = { Text(stringResource(R.string.onboarding_name_field)) },
            singleLine = true,
            isError = !valid,
            supportingText = { if (!valid) Text(stringResource(R.string.onboarding_name_invalid)) },
            modifier = Modifier.fillMaxWidth(),
        )
        if (recovery) Body(stringResource(R.string.onboarding_recovery_warning))
    }
    Primary(stringResource(if (recovery) R.string.onboarding_recover else R.string.onboarding_claim), enabled = valid) { actions.enroll(name) }
}

private fun defaultPhoneName(): String =
    (android.os.Build.MODEL ?: "").let { m -> PhoneName.normalize(m) ?: PhoneName.normalize(m.take(PhoneName.MAX_BYTES / 2)) } ?: "Telefon"

@Composable
private fun Working(phase: EnrollPhase?, recovery: Boolean) {
    Section {
        CircularProgressIndicator()
        Text(
            when (phase) {
                is EnrollPhase.ConfirmOnDevice -> stringResource(R.string.onboarding_phase_confirm, phase.seconds)
                EnrollPhase.Handshaking -> stringResource(R.string.onboarding_phase_handshake)
                EnrollPhase.Connecting, null -> stringResource(R.string.onboarding_phase_connecting)
            },
            style = MaterialTheme.typography.titleMedium,
            color = if (phase is EnrollPhase.ConfirmOnDevice) DispenserTheme.extendedColors.warning else MaterialTheme.colorScheme.onSurface,
            modifier = Modifier.semantics { liveRegion = LiveRegionMode.Polite },
        )
        if (recovery && phase !is EnrollPhase.ConfirmOnDevice) Body(stringResource(R.string.onboarding_phase_recovery_hint))
    }
}

@Composable
private fun Done(deviceId: String?, actions: OnboardingActions) {
    Section {
        Text(stringResource(R.string.onboarding_done_title), style = MaterialTheme.typography.titleMedium)
        deviceId?.let { Body(stringResource(R.string.onboarding_device_id, it)) }
        Body(stringResource(R.string.onboarding_done_hint))
    }
    Primary(stringResource(R.string.onboarding_done_continue), onClick = actions.finish)
}

@Composable
private fun Failed(reason: EnrollOutcome.Reason?, staged: Boolean, onClose: () -> Unit, actions: OnboardingActions) {
    Section {
        Text(stringResource(R.string.onboarding_failed_title), style = MaterialTheme.typography.titleMedium)
        Problem(stringResource(failureText(reason)))
        if (staged) Body(stringResource(R.string.onboarding_failed_staged))
    }
    Primary(stringResource(R.string.onboarding_retry), onClick = actions.retry)
    OutlinedButton(onClick = onClose, modifier = Modifier.fillMaxWidth()) { Text(stringResource(R.string.dialog_cancel)) }
}

private fun failureText(reason: EnrollOutcome.Reason?): Int = when (reason) {
    EnrollOutcome.Reason.NOT_READY -> R.string.onboarding_err_not_ready
    EnrollOutcome.Reason.CONNECTION -> R.string.onboarding_err_connection
    EnrollOutcome.Reason.WRONG_DEVICE -> R.string.onboarding_err_wrong_device
    EnrollOutcome.Reason.ALREADY_CLAIMED -> R.string.onboarding_err_already_claimed
    EnrollOutcome.Reason.NOT_CLAIMED -> R.string.onboarding_err_not_claimed
    EnrollOutcome.Reason.RECOVERY_NOT_ARMED -> R.string.onboarding_err_recovery_not_armed
    EnrollOutcome.Reason.HANDSHAKE -> R.string.onboarding_err_handshake
    EnrollOutcome.Reason.DEVICE_REFUSED -> R.string.onboarding_err_refused
    EnrollOutcome.Reason.TIMEOUT -> R.string.onboarding_err_timeout
    EnrollOutcome.Reason.BUSY -> R.string.onboarding_err_busy
    EnrollOutcome.Reason.DEVICE_KEY_CHANGED -> R.string.onboarding_err_key_changed
    EnrollOutcome.Reason.NOTHING_STAGED -> R.string.onboarding_err_nothing_staged
    EnrollOutcome.Reason.INCOMPLETE -> R.string.onboarding_err_incomplete
    EnrollOutcome.Reason.STAGED_PENDING -> R.string.onboarding_err_staged_pending
    EnrollOutcome.Reason.PROTOCOL, null -> R.string.onboarding_err_protocol
}

@Composable
private fun Unconfirmed(deviceId: String?, actions: OnboardingActions) {
    var askDiscard by remember { mutableStateOf(false) }
    Section {
        Text(stringResource(R.string.onboarding_unconfirmed_title), style = MaterialTheme.typography.titleMedium)
        deviceId?.let { Body(stringResource(R.string.onboarding_device_id, it)) }
        Body(stringResource(R.string.onboarding_unconfirmed_hint))
    }
    Primary(stringResource(R.string.onboarding_unconfirmed_check), onClick = actions.confirmStaged)
    OutlinedButton(onClick = { askDiscard = true }, modifier = Modifier.fillMaxWidth()) {
        Text(stringResource(R.string.onboarding_unconfirmed_discard), color = MaterialTheme.colorScheme.error)
    }
    if (askDiscard) {
        AlertDialog(
            onDismissRequest = { askDiscard = false },
            title = { Text(stringResource(R.string.onboarding_unconfirmed_discard_title)) },
            text = { Text(stringResource(R.string.onboarding_unconfirmed_discard_text)) },
            confirmButton = { TextButton(onClick = { askDiscard = false; actions.discardStaged() }) { Text(stringResource(R.string.onboarding_unconfirmed_discard)) } },
            dismissButton = { TextButton(onClick = { askDiscard = false }) { Text(stringResource(R.string.dialog_cancel)) } },
        )
    }
}
