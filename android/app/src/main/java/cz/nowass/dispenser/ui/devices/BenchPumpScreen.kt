package cz.nowass.dispenser.ui.devices

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Checkbox
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import cz.nowass.dispenser.R
import cz.nowass.dispenser.data.onboarding.BenchField
import cz.nowass.dispenser.data.onboarding.BenchFieldError
import cz.nowass.dispenser.data.onboarding.BenchGate
import cz.nowass.dispenser.data.onboarding.BenchOutcome
import cz.nowass.dispenser.data.onboarding.BenchPumpController
import cz.nowass.dispenser.data.onboarding.BenchPumpForm
import cz.nowass.dispenser.data.onboarding.BenchPumpProblem
import cz.nowass.dispenser.data.onboarding.BenchSnapshot
import cz.nowass.dispenser.ui.theme.Spacing
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

const val BENCH_PUMP_SAVE_TEST_TAG = "bench_pump_save"
const val BENCH_PUMP_READY_TEST_TAG = "bench_pump_ready"
const val BENCH_PUMP_NOT_READY_TEST_TAG = "bench_pump_not_ready"

data class BenchPumpUiState(
    val loading: Boolean = true,
    val snapshot: BenchSnapshot? = null,
    val form: BenchPumpForm = BenchPumpForm("", "", "", "", "", calibrateNow = false),
    val errors: Map<BenchField, BenchFieldError> = emptyMap(),
    /** Result of the last action (null = nothing to report). */
    val outcome: BenchOutcome? = null,
    val busy: Boolean = false,
    /** The READY confirmation dialog is shown (READY is never sent without it). */
    val confirmReady: Boolean = false,
)

/** BENCH ONLY (debug builds): pump configuration and readiness; see [BenchPumpController]. */
class BenchPumpViewModel(private val controller: BenchPumpController) : ViewModel() {
    private val _state = MutableStateFlow(BenchPumpUiState())
    val state: StateFlow<BenchPumpUiState> = _state.asStateFlow()

    init { load() }

    fun load() = act { controller.load() }

    fun edit(form: BenchPumpForm) = _state.update {
        it.copy(form = form, errors = controller.validate(form, it.snapshot?.config?.hwMaxRuntimeMs ?: 0))
    }

    fun save() {
        val s = _state.value
        val snap = s.snapshot ?: return
        if (s.busy) return
        act { controller.save(snap, s.form) }
    }

    /** Opens the confirmation; READY is sent only by [confirmReady]. */
    fun requestReady() = _state.update { it.copy(confirmReady = true) }
    fun cancelReady() = _state.update { it.copy(confirmReady = false) }
    fun confirmReady() {
        _state.update { it.copy(confirmReady = false) }
        act { controller.setReady(true) }
    }

    fun setNotReady() = act { controller.setReady(false) }

    private fun act(block: suspend () -> BenchOutcome) {
        _state.update { it.copy(busy = true) }
        viewModelScope.launch {
            val r = block()
            _state.update { s ->
                when (r) {
                    is BenchOutcome.Loaded -> {
                        val first = s.snapshot == null
                        val form = if (first) formOf(r.snapshot) else s.form
                        s.copy(loading = false, busy = false, snapshot = r.snapshot, form = form,
                            errors = controller.validate(form, r.snapshot.config.hwMaxRuntimeMs),
                            outcome = if (first) null else r)
                    }
                    else -> s.copy(loading = false, busy = false, outcome = r,
                        errors = if (r is BenchOutcome.Invalid) r.errors else s.errors)
                }
            }
        }
    }

    companion object {
        /** Current device values as the form's start (0 = unset is shown empty). */
        fun formOf(s: BenchSnapshot): BenchPumpForm {
            fun t(v: Long) = if (v > 0) v.toString() else ""
            fun ml(v: Long) = if (v > 0) cz.nowass.dispenser.ui.Format.mlInput(cz.nowass.dispenser.domain.Volume.microlitresToMl(v)) else ""
            val c = s.config
            return BenchPumpForm(t(c.pumpFlowUlPerS), ml(c.maxDoseUl), ml(c.maxDailyUl), t(c.maxRuntimeMs), t(c.minIntervalS), calibrateNow = false)
        }

        fun factory(controller: () -> BenchPumpController): ViewModelProvider.Factory = viewModelFactory {
            initializer { BenchPumpViewModel(controller()) }
        }
    }
}

@Composable
fun BenchPumpRoute(viewModel: BenchPumpViewModel, onBack: () -> Unit) {
    val state by viewModel.state.collectAsStateWithLifecycle()
    Column(Modifier.fillMaxSize()) {
        Row(Modifier.fillMaxWidth().heightIn(min = 64.dp).padding(horizontal = Spacing.xs), verticalAlignment = Alignment.CenterVertically) {
            IconButton(onClick = onBack) { Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = stringResource(R.string.reservoir_back)) }
            Text(stringResource(R.string.bench_title), style = MaterialTheme.typography.titleLarge, modifier = Modifier.weight(1f).semantics { heading() })
        }
        Column(
            Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(horizontal = Spacing.m, vertical = Spacing.s),
            verticalArrangement = Arrangement.spacedBy(Spacing.m),
        ) {
            Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.errorContainer)) {
                Text(stringResource(R.string.bench_warning), color = MaterialTheme.colorScheme.onErrorContainer, modifier = Modifier.padding(Spacing.m))
            }
            val snap = state.snapshot
            if (state.loading) {
                CircularProgressIndicator()
            } else if (snap == null) {
                Text(outcomeText(state.outcome) ?: stringResource(R.string.bench_not_connected))
                OutlinedButton(onClick = viewModel::load, modifier = Modifier.fillMaxWidth()) { Text(stringResource(R.string.bench_reload)) }
            } else {
                StatusCard(snap)
                FormCard(state, viewModel)
                if (snap.ready) {
                    OutlinedButton(onClick = viewModel::setNotReady, enabled = !state.busy, modifier = Modifier.fillMaxWidth().testTag(BENCH_PUMP_NOT_READY_TEST_TAG)) {
                        Text(stringResource(R.string.bench_set_not_ready))
                    }
                } else {
                    Button(onClick = viewModel::requestReady, enabled = !state.busy, modifier = Modifier.fillMaxWidth().testTag(BENCH_PUMP_READY_TEST_TAG)) {
                        Text(stringResource(R.string.bench_set_ready))
                    }
                }
                outcomeText(state.outcome)?.let { Text(it, style = MaterialTheme.typography.bodyMedium) }
                OutlinedButton(onClick = viewModel::load, enabled = !state.busy, modifier = Modifier.fillMaxWidth()) { Text(stringResource(R.string.bench_reload)) }
            }
        }
    }
    if (state.confirmReady) {
        AlertDialog(
            onDismissRequest = viewModel::cancelReady,
            title = { Text(stringResource(R.string.bench_ready_confirm_title)) },
            text = { Text(stringResource(R.string.bench_ready_confirm_text)) },
            confirmButton = { TextButton(onClick = viewModel::confirmReady) { Text(stringResource(R.string.bench_set_ready)) } },
            dismissButton = { TextButton(onClick = viewModel::cancelReady) { Text(stringResource(R.string.dialog_cancel)) } },
        )
    }
}

@Composable
private fun StatusCard(s: BenchSnapshot) = Card(Modifier.fillMaxWidth()) {
    Column(Modifier.padding(Spacing.m), verticalArrangement = Arrangement.spacedBy(Spacing.xs)) {
        Text(stringResource(if (s.ready) R.string.bench_state_ready else R.string.bench_state_not_ready), style = MaterialTheme.typography.titleMedium)
        Text(stringResource(R.string.bench_hw_max, s.config.hwMaxRuntimeMs))
        val gates = s.gates.filter { it != BenchGate.NOT_READY }
        if (gates.isEmpty() && s.problems.isEmpty()) {
            Text(stringResource(R.string.bench_gates_open))
        } else {
            Text(stringResource(R.string.bench_gates_closed), style = MaterialTheme.typography.labelLarge)
            gates.forEach { Text("• " + gateText(it)) }
            s.problems.forEach { Text("• " + problemText(it)) }
        }
    }
}

@Composable
private fun FormCard(state: BenchPumpUiState, vm: BenchPumpViewModel) = Card(Modifier.fillMaxWidth()) {
    val f = state.form
    val snap = state.snapshot!!
    Column(Modifier.padding(Spacing.m), verticalArrangement = Arrangement.spacedBy(Spacing.s)) {
        BenchNumberField(stringResource(R.string.bench_flow), f.flowUlPerS, state.errors[BenchField.FLOW], snap) { vm.edit(f.copy(flowUlPerS = it)) }
        BenchNumberField(stringResource(R.string.bench_max_dose), f.maxDoseMl, state.errors[BenchField.MAX_DOSE], snap, decimal = true) { vm.edit(f.copy(maxDoseMl = it)) }
        BenchNumberField(stringResource(R.string.bench_max_daily), f.maxDailyMl, state.errors[BenchField.MAX_DAILY], snap, decimal = true) { vm.edit(f.copy(maxDailyMl = it)) }
        BenchNumberField(stringResource(R.string.bench_max_runtime), f.maxRuntimeMs, state.errors[BenchField.MAX_RUNTIME], snap) { vm.edit(f.copy(maxRuntimeMs = it)) }
        BenchNumberField(stringResource(R.string.bench_min_interval), f.minIntervalS, state.errors[BenchField.MIN_INTERVAL], snap) { vm.edit(f.copy(minIntervalS = it)) }
        Row(verticalAlignment = Alignment.CenterVertically) {
            Checkbox(checked = f.calibrateNow, onCheckedChange = { vm.edit(f.copy(calibrateNow = it)) })
            Text(stringResource(if (snap.config.calibratedAt > 0) R.string.bench_calibrate_again else R.string.bench_calibrate_now))
        }
        if (snap.ready) Text(stringResource(R.string.bench_save_needs_not_ready), color = MaterialTheme.colorScheme.onSurfaceVariant)
        Button(
            onClick = vm::save,
            enabled = !state.busy && !snap.ready && state.errors.isEmpty(),
            modifier = Modifier.fillMaxWidth().testTag(BENCH_PUMP_SAVE_TEST_TAG),
        ) { Text(stringResource(R.string.bench_save)) }
    }
}

@Composable
private fun BenchNumberField(label: String, value: String, error: BenchFieldError?, snap: BenchSnapshot, decimal: Boolean = false, onChange: (String) -> Unit) {
    var touched by remember { mutableStateOf(false) }
    OutlinedTextField(
        value = value,
        onValueChange = { touched = true; onChange(it) },
        label = { Text(label) },
        singleLine = true,
        isError = error != null && (touched || value.isNotEmpty()),
        supportingText = error?.let { e -> { Text(fieldError(e, snap.config.hwMaxRuntimeMs)) } },
        keyboardOptions = KeyboardOptions(keyboardType = if (decimal) KeyboardType.Decimal else KeyboardType.Number),
        modifier = Modifier.fillMaxWidth(),
    )
}

@Composable
private fun fieldError(e: BenchFieldError, hwMax: Long): String = when (e) {
    BenchFieldError.MISSING -> stringResource(R.string.bench_err_missing)
    BenchFieldError.NOT_A_NUMBER -> stringResource(R.string.bench_err_number)
    BenchFieldError.NOT_POSITIVE -> stringResource(R.string.bench_err_positive)
    BenchFieldError.TOO_PRECISE -> stringResource(R.string.editor_err_amount_precision)
    BenchFieldError.TOO_LARGE -> stringResource(R.string.bench_err_too_large)
    BenchFieldError.ABOVE_HW_MAX -> stringResource(R.string.bench_err_above_hw, hwMax)
    BenchFieldError.NO_HW_MAX -> stringResource(R.string.bench_err_no_hw)
    BenchFieldError.DAILY_BELOW_DOSE -> stringResource(R.string.bench_err_daily_below_dose)
}

@Composable
private fun gateText(g: BenchGate): String = stringResource(
    when (g) {
        BenchGate.NOT_LOADED -> R.string.bench_gate_not_loaded
        BenchGate.DEGRADED -> R.string.bench_gate_degraded
        BenchGate.NOT_READY -> R.string.bench_gate_not_ready
        BenchGate.BLOCKED -> R.string.bench_gate_blocked
        BenchGate.REFILL_PENDING -> R.string.bench_gate_refill_pending
        BenchGate.MANUAL_ACTIVE -> R.string.bench_gate_manual_active
        BenchGate.CLOCK -> R.string.bench_gate_clock
        BenchGate.PUMP_CONFIG -> R.string.bench_gate_pump_config
        BenchGate.RESERVOIR_UNCONFIGURED -> R.string.bench_gate_reservoir
        BenchGate.CREDENTIALS -> R.string.bench_gate_credentials
    },
)

@Composable
private fun problemText(p: BenchPumpProblem): String = stringResource(
    when (p) {
        BenchPumpProblem.FLOW -> R.string.bench_problem_flow
        BenchPumpProblem.CALIBRATED_AT -> R.string.bench_problem_calibrated
        BenchPumpProblem.MAX_DOSE -> R.string.bench_problem_max_dose
        BenchPumpProblem.MAX_DAILY -> R.string.bench_problem_max_daily
        BenchPumpProblem.MAX_RUNTIME -> R.string.bench_problem_max_runtime
        BenchPumpProblem.MIN_INTERVAL -> R.string.bench_problem_min_interval
        BenchPumpProblem.HW_LIMIT -> R.string.bench_problem_hw_limit
    },
)

@Composable
private fun outcomeText(o: BenchOutcome?): String? = when (o) {
    null -> null
    is BenchOutcome.Loaded -> stringResource(R.string.bench_out_done) +
        (if (o.schedulesAboveMaxDose > 0) "\n" + stringResource(R.string.bench_out_schedules_above_max_dose, o.schedulesAboveMaxDose) else "")
    BenchOutcome.NotConnected -> stringResource(R.string.bench_not_connected)
    is BenchOutcome.Invalid -> stringResource(R.string.bench_out_invalid)
    BenchOutcome.MustBeNotReady -> stringResource(R.string.bench_save_needs_not_ready)
    is BenchOutcome.ReadyRefused -> stringResource(R.string.bench_out_ready_refused) +
        (o.gates.filter { it != BenchGate.NOT_READY }.map { "\n• " + gateText(it) } + o.problems.map { "\n• " + problemText(it) }).joinToString("")
    is BenchOutcome.Rejected -> stringResource(R.string.bench_out_rejected, o.code, o.detail)
    BenchOutcome.Uncertain -> stringResource(R.string.bench_out_uncertain)
    BenchOutcome.AwaitingVerification -> stringResource(R.string.bench_out_awaiting)
}
