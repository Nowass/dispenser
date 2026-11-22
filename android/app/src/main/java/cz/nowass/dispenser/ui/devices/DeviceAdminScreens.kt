package cz.nowass.dispenser.ui.devices

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
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
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import cz.nowass.dispenser.R
import cz.nowass.dispenser.data.onboarding.AdminFailure
import cz.nowass.dispenser.data.onboarding.PhoneName
import cz.nowass.dispenser.data.onboarding.PhoneUi
import cz.nowass.dispenser.data.onboarding.RemoveOutcome
import cz.nowass.dispenser.domain.DEFAULT_ZONE
import cz.nowass.dispenser.ui.Format
import cz.nowass.dispenser.ui.components.DetailTopBar
import cz.nowass.dispenser.ui.components.QrCodeImage
import cz.nowass.dispenser.ui.components.SecureWindow
import cz.nowass.dispenser.ui.components.StatusTag
import java.time.Instant

const val INVITE_QR_TEST_TAG = "invite_qr"
const val INVITE_COPY_TEST_TAG = "invite_copy"
const val PHONES_LIST_TEST_TAG = "phones_list"

@Composable
private fun Section(content: @Composable ColumnScope.() -> Unit) {
    Card(
        shape = MaterialTheme.shapes.large,
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
        modifier = Modifier.fillMaxWidth(),
    ) { Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp), content = content) }
}

@Composable
private fun Body(text: String) = Text(text, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)

@Composable
private fun Problem(text: String) = Text(
    text, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.error,
    modifier = Modifier.semantics { liveRegion = LiveRegionMode.Polite },
)

private fun failureText(f: AdminFailure): Int = when (f) {
    AdminFailure.NOT_CONNECTED -> R.string.admin_err_not_connected
    AdminFailure.CREDENTIAL -> R.string.admin_err_credential
    AdminFailure.DEVICE_REFUSED -> R.string.admin_err_refused
    AdminFailure.UNCERTAIN -> R.string.admin_err_uncertain
    AdminFailure.INVALID -> R.string.admin_err_invalid
}

@Composable
fun InviteRoute(viewModel: InviteViewModel, onBack: () -> Unit) {
    val state by viewModel.state.collectAsStateWithLifecycle()
    InviteScreen(state, onBack, viewModel::create, viewModel::cancel)
}

/** The owner's invitation (FLAG_SECURE: the QR code carries the single-use invitation token). */
@Composable
fun InviteScreen(state: InviteUiState, onBack: () -> Unit, onCreate: (String) -> Unit, onCancel: () -> Unit) {
    SecureWindow()
    Column(Modifier.fillMaxSize()) {
        DetailTopBar(stringResource(R.string.invite_title), isDemo = false, onBack = onBack)
        Column(
            Modifier.fillMaxSize().verticalScroll(rememberScrollState()).imePadding().padding(horizontal = 16.dp, vertical = 8.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            when (state) {
                is InviteUiState.Form -> {
                    var name by rememberSaveable { mutableStateOf("") }
                    val valid = name.isBlank() || PhoneName.normalize(name) != null
                    Section {
                        Body(stringResource(R.string.invite_hint))
                        OutlinedTextField(
                            value = name, onValueChange = { name = it }, singleLine = true, isError = !valid,
                            label = { Text(stringResource(R.string.invite_name_field)) },
                            supportingText = { if (!valid) Text(stringResource(R.string.onboarding_name_invalid)) },
                            modifier = Modifier.fillMaxWidth(),
                        )
                        state.failure?.let { Problem(stringResource(failureText(it))) }
                        if (state.expired) Body(stringResource(R.string.invite_expired))
                        if (state.cancelled) Body(stringResource(R.string.invite_cancelled))
                        if (state.cancelFailed) Problem(stringResource(R.string.invite_cancel_failed))
                    }
                    Button(onClick = { onCreate(name) }, enabled = valid, modifier = Modifier.fillMaxWidth().heightIn(min = 52.dp)) {
                        Text(stringResource(R.string.invite_create), style = MaterialTheme.typography.titleMedium)
                    }
                }
                InviteUiState.Working -> Section {
                    CircularProgressIndicator()
                    Body(stringResource(R.string.invite_working))
                }
                is InviteUiState.Showing -> {
                    Section {
                        QrCodeImage(state.qrText, stringResource(R.string.invite_qr_description), Modifier.fillMaxWidth().testTag(INVITE_QR_TEST_TAG))
                        Body(stringResource(R.string.invite_show_hint))
                        val at = Instant.ofEpochSecond(state.expiresUtc).atZone(DEFAULT_ZONE)
                        Text(stringResource(R.string.invite_expires, Format.time(at)), style = MaterialTheme.typography.titleMedium)
                    }
                    // same-phone hand-over to the other app variant (its own Keystore identity; a QR cannot be scanned on this phone)
                    val context = androidx.compose.ui.platform.LocalContext.current
                    var copied by androidx.compose.runtime.saveable.rememberSaveable { androidx.compose.runtime.mutableStateOf(false) }
                    val clipLabel = stringResource(R.string.invite_copy_label)
                    OutlinedButton(
                        onClick = { cz.nowass.dispenser.ui.components.SensitiveClipboard.copyInvitation(context, clipLabel, state.qrText); copied = true },
                        modifier = Modifier.fillMaxWidth().testTag(INVITE_COPY_TEST_TAG),
                    ) { Text(stringResource(R.string.invite_copy)) }
                    if (copied) Body(stringResource(R.string.invite_copied))
                    OutlinedButton(onClick = onCancel, modifier = Modifier.fillMaxWidth()) {
                        Text(stringResource(R.string.invite_cancel), color = MaterialTheme.colorScheme.error)
                    }
                }
            }
        }
    }
}

@Composable
fun PhonesRoute(viewModel: PhonesViewModel, onBack: () -> Unit) {
    val state by viewModel.state.collectAsStateWithLifecycle()
    PhonesScreen(state, onBack, viewModel::load, viewModel::remove)
}

@Composable
fun PhonesScreen(state: PhonesUiState, onBack: () -> Unit, onReload: () -> Unit, onRemove: (PhoneUi) -> Unit) {
    var confirm by remember { mutableStateOf<PhoneUi?>(null) }
    Column(Modifier.fillMaxSize()) {
        DetailTopBar(stringResource(R.string.phones_title), isDemo = false, onBack = onBack)
        Column(
            Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(horizontal = 16.dp, vertical = 8.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            if (state.loading) Section { CircularProgressIndicator(); Body(stringResource(R.string.phones_loading)) }
            state.failure?.let { f ->
                Section {
                    Problem(stringResource(failureText(f)))
                    TextButton(onClick = onReload) { Text(stringResource(R.string.phones_reload)) }
                }
            }
            state.removal?.let { r ->
                val text = when (r) {
                    RemoveOutcome.REMOVED -> R.string.phones_removed
                    RemoveOutcome.UNCERTAIN -> R.string.phones_remove_uncertain
                    RemoveOutcome.NOT_SENT -> R.string.phones_remove_not_sent
                    RemoveOutcome.AWAITING_VERIFICATION -> R.string.phones_remove_awaiting
                    RemoveOutcome.REJECTED -> R.string.phones_remove_rejected
                    RemoveOutcome.NOT_ALLOWED -> R.string.phones_self_note
                }
                Section { if (r == RemoveOutcome.REMOVED) Body(stringResource(text)) else Problem(stringResource(text)) }
            }
            if (!state.loading && state.failure == null) {
                Card(
                    shape = MaterialTheme.shapes.large,
                    colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
                    modifier = Modifier.fillMaxWidth().testTag(PHONES_LIST_TEST_TAG),
                ) {
                    state.phones.forEachIndexed { i, p ->
                        if (i > 0) HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                        ListItem(
                            headlineContent = { Text(p.name.ifBlank { stringResource(R.string.phones_slot, p.slot) }) },
                            supportingContent = {
                                Column {
                                    Text(stringResource(R.string.phones_slot, p.slot))
                                    if (p.isThisPhone) StatusTag(stringResource(R.string.phones_this_phone))
                                }
                            },
                            trailingContent = {
                                if (!p.isThisPhone) TextButton(onClick = { confirm = p }, enabled = !state.busy) {
                                    Text(stringResource(R.string.phones_remove), color = MaterialTheme.colorScheme.error)
                                }
                            },
                            colors = ListItemDefaults.colors(containerColor = MaterialTheme.colorScheme.surface),
                        )
                    }
                }
                Body(stringResource(R.string.phones_self_note))
            }
        }
    }
    confirm?.let { p ->
        AlertDialog(
            onDismissRequest = { confirm = null },
            title = { Text(stringResource(R.string.phones_remove_title, p.name)) },
            text = { Text(stringResource(R.string.phones_remove_text)) },
            confirmButton = { TextButton(onClick = { confirm = null; onRemove(p) }) { Text(stringResource(R.string.phones_remove), color = MaterialTheme.colorScheme.error) } },
            dismissButton = { TextButton(onClick = { confirm = null }) { Text(stringResource(R.string.dialog_cancel)) } },
        )
    }
}
