package cz.nowass.dispenser.ui.devices

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import cz.nowass.dispenser.R
import cz.nowass.dispenser.data.device.FirmwareStatusOutcome
import cz.nowass.dispenser.data.device.FirmwareUpdateController
import cz.nowass.dispenser.data.device.StartUpdateOutcome
import cz.nowass.dispenser.device.ops.UpdateResult
import cz.nowass.dispenser.device.ops.UpdateStatusRecord
import cz.nowass.dispenser.device.ops.UpdateVerdict
import cz.nowass.dispenser.domain.DEFAULT_ZONE
import cz.nowass.dispenser.domain.update.FirmwareCheckResult
import cz.nowass.dispenser.domain.update.FirmwareOffer
import cz.nowass.dispenser.domain.update.UpdateCheckResult
import cz.nowass.dispenser.ui.Format
import cz.nowass.dispenser.ui.theme.Spacing
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

const val FIRMWARE_CHECK_TEST_TAG = "firmware_check"
const val FIRMWARE_UPDATE_TEST_TAG = "firmware_update"
const val FIRMWARE_BETA_TEST_TAG = "firmware_beta"
const val FIRMWARE_RELOAD_TEST_TAG = "firmware_reload"

data class FirmwareUiState(
    val connected: Boolean = false,
    val loading: Boolean = true,
    /** The last GET_UPDATE_STATUS outcome (null = not read yet). */
    val status: FirmwareStatusOutcome? = null,
    /** The last status the device reported in this screen. */
    val record: UpdateStatusRecord? = null,
    val beta: Boolean = false,
    val checking: Boolean = false,
    val check: FirmwareCheckResult? = null,
    /** The explicit confirmation is shown (START_UPDATE is never sent without it). */
    val confirming: Boolean = false,
    val starting: Boolean = false,
    val start: StartUpdateOutcome? = null,
) {
    val offer: FirmwareOffer? get() = (check as? FirmwareCheckResult.Available)?.offer
    val busy: Boolean get() = loading || checking || starting
    val accepted: Boolean get() = (start as? StartUpdateOutcome.Answered)?.verdict?.verdict == UpdateVerdict.ACCEPTED
    val canCheck: Boolean get() = record != null && !busy && !accepted
    val canUpdate: Boolean get() = connected && record?.capable == true && offer != null && !busy && !accepted
}

/** Device firmware and its owner-initiated OTA update (DEVICE mode); see [FirmwareUpdateController]. */
class FirmwareViewModel(private val controller: FirmwareUpdateController) : ViewModel() {
    private val _state = MutableStateFlow(FirmwareUiState())
    val state: StateFlow<FirmwareUiState> = _state.asStateFlow()

    init {
        load()
        viewModelScope.launch { controller.betaChannel.collect { b -> _state.update { it.copy(beta = b) } } }
        viewModelScope.launch {
            var was = false
            controller.connected.collect { up ->
                _state.update { it.copy(connected = up) }
                // a new connection after an accepted (or unanswered) update shows its result by a READ
                val s = _state.value
                if (up && !was && !s.busy && (s.record == null || s.accepted || s.start == StartUpdateOutcome.Uncertain)) load()
                was = up
            }
        }
    }

    fun load() {
        _state.update { it.copy(loading = true) }
        viewModelScope.launch {
            val r = controller.readStatus()
            _state.update { s ->
                when (r) {
                    // a fresh status replaces what an earlier check or update reported
                    is FirmwareStatusOutcome.Status -> s.copy(loading = false, status = r, record = r.record, check = null, start = null)
                    FirmwareStatusOutcome.Unsupported -> s.copy(loading = false, status = r, record = null, check = null)
                    else -> s.copy(loading = false, status = r)
                }
            }
        }
    }

    fun check() {
        val s = _state.value
        val running = s.record?.runningVersion ?: return
        if (!s.canCheck) return
        _state.update { it.copy(checking = true, check = null, start = null) }
        viewModelScope.launch {
            val r = controller.check(running, s.beta)
            _state.update { it.copy(checking = false, check = r) }
        }
    }

    /** The channel changes what may be offered: a previous result is dropped. */
    fun setBeta(enabled: Boolean) {
        _state.update { it.copy(beta = enabled, check = if (it.checking) it.check else null) }
        viewModelScope.launch { controller.setBetaChannel(enabled) }
    }

    fun requestUpdate() {
        if (_state.value.canUpdate) _state.update { it.copy(confirming = true) }
    }

    fun cancelUpdate() = _state.update { it.copy(confirming = false) }

    /** START_UPDATE once, after the confirmation; further taps are ignored while it runs. */
    fun confirmUpdate() {
        val s = _state.value
        val offer = s.offer
        _state.update { it.copy(confirming = false) }
        if (offer == null || !s.canUpdate) return
        _state.update { it.copy(starting = true, start = null) }
        viewModelScope.launch {
            val r = controller.start(offer)
            _state.update { it.copy(starting = false, start = r) }
        }
    }

    companion object {
        fun factory(controller: () -> FirmwareUpdateController): ViewModelProvider.Factory = viewModelFactory {
            initializer { FirmwareViewModel(controller()) }
        }
    }
}

@Composable
fun FirmwareRoute(viewModel: FirmwareViewModel, onBack: () -> Unit) {
    val state by viewModel.state.collectAsStateWithLifecycle()
    FirmwareScreen(
        state, onBack = onBack, onReload = viewModel::load, onCheck = viewModel::check, onBeta = viewModel::setBeta,
        onUpdate = viewModel::requestUpdate,
    )
    val offer = state.offer
    if (state.confirming && offer != null) {
        AlertDialog(
            onDismissRequest = viewModel::cancelUpdate,
            title = { Text(stringResource(R.string.firmware_confirm_title, offer.version)) },
            text = { Text(stringResource(R.string.firmware_confirm_text)) },
            confirmButton = { TextButton(onClick = viewModel::confirmUpdate) { Text(stringResource(R.string.firmware_update)) } },
            dismissButton = { TextButton(onClick = viewModel::cancelUpdate) { Text(stringResource(R.string.dialog_cancel)) } },
        )
    }
}

@Composable
fun FirmwareScreen(
    state: FirmwareUiState,
    onBack: () -> Unit,
    onReload: () -> Unit,
    onCheck: () -> Unit,
    onBeta: (Boolean) -> Unit,
    onUpdate: () -> Unit,
) {
    Column(Modifier.fillMaxSize()) {
        Row(Modifier.fillMaxWidth().heightIn(min = 64.dp).padding(horizontal = Spacing.xs), verticalAlignment = Alignment.CenterVertically) {
            IconButton(onClick = onBack) { Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = stringResource(R.string.reservoir_back)) }
            Text(stringResource(R.string.firmware_title), style = MaterialTheme.typography.titleLarge, modifier = Modifier.weight(1f).semantics { heading() })
        }
        Column(
            Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(horizontal = Spacing.m, vertical = Spacing.s),
            verticalArrangement = Arrangement.spacedBy(Spacing.m),
        ) {
            Card(Modifier.fillMaxWidth()) {
                Column(Modifier.padding(Spacing.m), verticalArrangement = Arrangement.spacedBy(Spacing.xs)) {
                    val record = state.record
                    when {
                        state.loading && record == null -> CircularProgressIndicator()
                        state.status == FirmwareStatusOutcome.Unsupported -> Text(stringResource(R.string.firmware_unsupported))
                        record != null -> {
                            Text(stringResource(R.string.firmware_installed, record.runningVersion), style = MaterialTheme.typography.titleMedium)
                            lastResultText(record)?.let { Text(it) }
                            if (!record.capable) Text(stringResource(R.string.firmware_not_capable), color = MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                        else -> Text(stringResource(if (state.status == FirmwareStatusOutcome.Failed) R.string.firmware_status_failed else R.string.firmware_not_connected))
                    }
                    if (record != null && state.status != null && state.status !is FirmwareStatusOutcome.Status) {
                        Text(stringResource(R.string.firmware_not_connected), color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                    OutlinedButton(onClick = onReload, enabled = !state.busy, modifier = Modifier.fillMaxWidth().testTag(FIRMWARE_RELOAD_TEST_TAG)) {
                        Text(stringResource(R.string.firmware_reload))
                    }
                }
            }
            if (state.record != null) {
                Card(Modifier.fillMaxWidth()) {
                    Column(Modifier.padding(Spacing.m), verticalArrangement = Arrangement.spacedBy(Spacing.s)) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Column(Modifier.weight(1f)) {
                                Text(stringResource(R.string.firmware_beta))
                                Text(stringResource(R.string.firmware_beta_note), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                            }
                            Switch(checked = state.beta, onCheckedChange = onBeta, enabled = !state.checking, modifier = Modifier.testTag(FIRMWARE_BETA_TEST_TAG))
                        }
                        Button(onClick = onCheck, enabled = state.canCheck, modifier = Modifier.fillMaxWidth().testTag(FIRMWARE_CHECK_TEST_TAG)) {
                            Text(stringResource(R.string.firmware_check))
                        }
                        if (state.checking) Text(stringResource(R.string.update_checking))
                        state.check?.let { checkText(it) }?.let { Text(it) }
                        state.offer?.let { offer ->
                            offer.notes?.let { notes ->
                                Text(stringResource(R.string.firmware_notes_title), style = MaterialTheme.typography.labelLarge)
                                Text(notes, style = MaterialTheme.typography.bodyMedium)
                            }
                            if (!state.connected) Text(stringResource(R.string.firmware_needs_connection), color = MaterialTheme.colorScheme.onSurfaceVariant)
                            Button(onClick = onUpdate, enabled = state.canUpdate, modifier = Modifier.fillMaxWidth().testTag(FIRMWARE_UPDATE_TEST_TAG)) {
                                Text(stringResource(R.string.firmware_update))
                            }
                        }
                        if (state.starting) CircularProgressIndicator()
                        state.start?.let { Text(startText(it), style = MaterialTheme.typography.bodyMedium) }
                    }
                }
            }
        }
    }
}

@Composable
private fun lastResultText(r: UpdateStatusRecord): String? {
    val v = r.lastVersion.ifEmpty { "?" }
    return when (r.lastResult) {
        UpdateResult.NONE -> null
        UpdateResult.INSTALLED -> stringResource(R.string.firmware_last_installed, v)
        UpdateResult.ROLLED_BACK -> stringResource(R.string.firmware_last_rolled_back, v)
        UpdateResult.WIFI_FAILED -> stringResource(R.string.firmware_last_wifi, v)
        UpdateResult.DOWNLOAD_FAILED -> stringResource(R.string.firmware_last_download, v)
        UpdateResult.IMAGE_REJECTED -> stringResource(R.string.firmware_last_image, v)
        UpdateResult.FLASH_FAILED -> stringResource(R.string.firmware_last_flash, v)
        UpdateResult.CANCELLED -> stringResource(R.string.firmware_last_cancelled, v)
        UpdateResult.UNKNOWN -> stringResource(R.string.firmware_last_unknown, v, r.lastResultWire)
    }
}

@Composable
private fun checkText(r: FirmwareCheckResult): String = when (r) {
    is FirmwareCheckResult.Available ->
        stringResource(if (r.offer.isPreRelease) R.string.firmware_available_beta else R.string.firmware_available, r.offer.version)
    is FirmwareCheckResult.UpToDate -> buildList {
        add(stringResource(R.string.firmware_up_to_date))
        r.incompatibleNewer?.let { add(stringResource(R.string.firmware_incompatible, it)) }
        if (r.rejectedNewer > 0) add(stringResource(R.string.firmware_rejected))
    }.joinToString("\n")
    FirmwareCheckResult.UnknownRunningVersion -> stringResource(R.string.firmware_unknown_running)
    is FirmwareCheckResult.Failed -> when (val f = r.reason) {
        UpdateCheckResult.RepositoryUnavailable -> stringResource(R.string.update_repo_unavailable)
        UpdateCheckResult.Offline -> stringResource(R.string.update_offline)
        is UpdateCheckResult.RateLimited ->
            f.resetAt?.let { stringResource(R.string.update_rate_limited_until, Format.time(it.atZone(DEFAULT_ZONE))) } ?: stringResource(R.string.update_rate_limited)
        is UpdateCheckResult.HttpError -> stringResource(R.string.update_http_error, f.code)
        else -> stringResource(R.string.update_invalid)
    }
}

@Composable
private fun startText(o: StartUpdateOutcome): String = when (o) {
    is StartUpdateOutcome.Answered -> when (o.verdict.verdict) {
        UpdateVerdict.ACCEPTED -> stringResource(R.string.firmware_verdict_accepted)
        UpdateVerdict.NOT_CAPABLE -> stringResource(R.string.firmware_verdict_not_capable)
        UpdateVerdict.NOT_NEWER -> stringResource(R.string.firmware_verdict_not_newer)
        UpdateVerdict.INVALID_REQUEST -> stringResource(R.string.firmware_verdict_invalid)
        UpdateVerdict.DOSE_SOON -> stringResource(R.string.firmware_verdict_dose_soon)
        UpdateVerdict.BATTERY -> stringResource(R.string.firmware_verdict_battery)
        UpdateVerdict.NO_WIFI -> stringResource(R.string.firmware_verdict_no_wifi)
        UpdateVerdict.BUSY -> stringResource(R.string.firmware_verdict_busy)
        UpdateVerdict.UNKNOWN -> stringResource(R.string.firmware_verdict_unknown, o.verdict.wire)
    }
    StartUpdateOutcome.Uncertain -> stringResource(R.string.firmware_start_uncertain)
    StartUpdateOutcome.NotConnected -> stringResource(R.string.firmware_start_not_connected)
    StartUpdateOutcome.Unsupported -> stringResource(R.string.firmware_unsupported)
    StartUpdateOutcome.NotSent -> stringResource(R.string.firmware_start_not_sent)
    is StartUpdateOutcome.Rejected -> stringResource(R.string.firmware_start_rejected, o.code)
}
