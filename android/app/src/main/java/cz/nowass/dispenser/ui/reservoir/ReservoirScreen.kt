package cz.nowass.dispenser.ui.reservoir

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import cz.nowass.dispenser.R
import cz.nowass.dispenser.data.MockSeed
import cz.nowass.dispenser.domain.Volume
import cz.nowass.dispenser.domain.simulation.PredictionStatus
import cz.nowass.dispenser.domain.simulation.ReservoirPredictor
import cz.nowass.dispenser.domain.simulation.ReservoirStatus
import cz.nowass.dispenser.ui.Format
import cz.nowass.dispenser.ui.components.DEMO_BADGE_TEST_TAG
import cz.nowass.dispenser.ui.components.DemoBadge
import cz.nowass.dispenser.ui.components.MetricProgress
import cz.nowass.dispenser.ui.components.StatusTag
import cz.nowass.dispenser.ui.components.StatusTone
import cz.nowass.dispenser.ui.components.accent
import cz.nowass.dispenser.ui.czechPluralStringResource
import cz.nowass.dispenser.ui.theme.DispenserTheme
import cz.nowass.dispenser.ui.theme.Spacing
import kotlin.math.roundToInt

const val RESERVOIR_REFILL_TEST_TAG = "reservoir_refill"

@Composable
fun ReservoirRoute(viewModel: ReservoirViewModel, isDemo: Boolean, onBack: () -> Unit) {
    val state by viewModel.uiState.collectAsStateWithLifecycle()
    val refillDone by viewModel.refillDone.collectAsStateWithLifecycle()
    ReservoirScreen(
        state, isDemo, onBack, viewModel::validateRefill, viewModel::refill,
        refillConfirmed = refillDone,
        onRefillOpened = viewModel::consumeRefillDone,
    )
}

@Composable
fun ReservoirScreen(
    state: ReservoirUiState,
    isDemo: Boolean,
    onBack: () -> Unit,
    validateRefill: (String) -> RefillError?,
    onRefill: (String) -> Unit,
    refillConfirmed: Boolean = false,
    onRefillOpened: () -> Unit = {},
) {
    var showRefill by rememberSaveable { mutableStateOf(false) }
    Column(Modifier.fillMaxSize()) {
        Row(
            Modifier.fillMaxWidth().heightIn(min = 64.dp).padding(horizontal = Spacing.xs),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(Spacing.xs),
        ) {
            IconButton(onClick = onBack) {
                Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = stringResource(R.string.reservoir_back))
            }
            Text(
                stringResource(R.string.reservoir_title),
                style = MaterialTheme.typography.titleLarge,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.weight(1f).semantics { heading() },
            )
            if (isDemo) DemoBadge(Modifier.padding(end = Spacing.s).testTag(DEMO_BADGE_TEST_TAG))
        }
        when (state) {
            ReservoirUiState.Loading -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { CircularProgressIndicator() }
            is ReservoirUiState.Ready -> Column(
                Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(horizontal = Spacing.m, vertical = Spacing.s),
                verticalArrangement = Arrangement.spacedBy(Spacing.m),
            ) {
                LevelCard(state)
                if (state.reservoir != null) {
                    PredictionCard(state)
                    Button(
                        onClick = { onRefillOpened(); showRefill = true },
                        shape = MaterialTheme.shapes.medium,
                        colors = ButtonDefaults.buttonColors(containerColor = MaterialTheme.colorScheme.primary, contentColor = MaterialTheme.colorScheme.onPrimary),
                        modifier = Modifier.fillMaxWidth().heightIn(min = 52.dp).testTag(RESERVOIR_REFILL_TEST_TAG),
                    ) { Text(stringResource(R.string.reservoir_refill), style = MaterialTheme.typography.titleMedium) }
                    if (refillConfirmed) {
                        Text(
                            stringResource(R.string.refill_done),
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.primary,
                            modifier = Modifier.semantics { liveRegion = LiveRegionMode.Polite },
                        )
                    }
                }
            }
        }
    }
    val ready = state as? ReservoirUiState.Ready
    val reservoir = ready?.reservoir
    if (showRefill && reservoir != null) {
        RefillDialog(
            capacityText = Format.mlInput(Volume.microlitresToMl(reservoir.capacityUl)),
            capacityLabel = stringResource(R.string.unit_ml, Format.ml(Volume.microlitresToMl(reservoir.capacityUl))),
            validate = validateRefill,
            onDismiss = { showRefill = false },
            onConfirm = { onRefill(it); showRefill = false },
        )
    }
}

@Composable
private fun SectionCard(content: @Composable ColumnScope.() -> Unit) {
    Card(
        shape = MaterialTheme.shapes.large,
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
        modifier = Modifier.fillMaxWidth(),
    ) { Column(Modifier.padding(Spacing.m), verticalArrangement = Arrangement.spacedBy(Spacing.s), content = content) }
}

private fun ml(ul: Long): Double = ul / Volume.MICROLITRES_PER_ML.toDouble()

@Composable
private fun LevelCard(state: ReservoirUiState.Ready) = SectionCard {
    val r = state.reservoir
    Row(horizontalArrangement = Arrangement.spacedBy(Spacing.xs)) {
        StatusTag(stringResource(R.string.dashboard_estimate_label))
        StatusTag(stringResource(R.string.dashboard_simulated_label))
    }
    if (r == null) {
        Text(stringResource(R.string.reservoir_unknown), style = MaterialTheme.typography.headlineMedium)
        Text(stringResource(R.string.reservoir_simulation_inactive), style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        return@SectionCard
    }
    val tone = when (state.status) {
        ReservoirStatus.BLOCKED -> StatusTone.Blocked
        ReservoirStatus.LOW -> StatusTone.Low
        else -> StatusTone.Normal
    }
    val accent = tone.accent()
    Text(stringResource(R.string.unit_ml, Format.ml(ml(r.remainingUl))), style = MaterialTheme.typography.displaySmall)
    Text(
        stringResource(R.string.dashboard_reservoir_capacity, stringResource(R.string.unit_ml, Format.ml(ml(r.capacityUl)))),
        style = MaterialTheme.typography.bodyMedium,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
    val fraction = r.remainingUl.toFloat() / r.capacityUl
    MetricProgress(fraction, stringResource(R.string.dashboard_reservoir_a11y, (fraction * 100).roundToInt()), accent)
    Text(
        stringResource(
            when (state.status) {
                ReservoirStatus.BLOCKED -> R.string.reservoir_status_blocked
                ReservoirStatus.LOW -> R.string.reservoir_status_low
                else -> R.string.reservoir_status_normal
            },
        ),
        style = MaterialTheme.typography.titleMedium,
        color = if (tone == StatusTone.Normal) MaterialTheme.colorScheme.onSurface else accent,
    )
    Text(stringResource(R.string.reservoir_low_threshold, state.lowThresholdPercent), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
    Text(stringResource(R.string.dashboard_reservoir_estimate_note), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
}

@Composable
private fun PredictionCard(state: ReservoirUiState.Ready) = SectionCard {
    val p = state.prediction ?: return@SectionCard
    Text(stringResource(R.string.reservoir_prediction_title), style = MaterialTheme.typography.titleMedium, modifier = Modifier.semantics { heading() })
    val body: @Composable () -> Unit = {
        when (p.status) {
            PredictionStatus.DEPLETES_WITHIN_HORIZON, PredictionStatus.ALL_REMAINING_DOSES_COVERED, PredictionStatus.UNKNOWN_BEYOND_HORIZON -> {
                Text(czechPluralStringResource(R.plurals.prediction_covered, p.coveredDoses, p.coveredDoses), style = MaterialTheme.typography.headlineSmall)
            }
            else -> Unit
        }
        when (p.status) {
            PredictionStatus.DEPLETES_WITHIN_HORIZON -> {
                // Day precision only: no false precision for an estimate.
                Text(stringResource(R.string.prediction_refill_by, Format.fullDate(p.depletionAt!!.atZone(state.zone).toLocalDate())), style = MaterialTheme.typography.bodyLarge)
                p.firstUncovered?.let {
                    Text(
                        stringResource(R.string.prediction_first_uncovered, it.scheduleName.ifBlank { stringResource(R.string.schedule_unnamed) }, stringResource(R.string.unit_ml, Format.ml(it.amountMl))),
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                Text(stringResource(R.string.prediction_remaining_after, stringResource(R.string.unit_ml, Format.ml(ml(p.remainingAfterLastCoveredUl)))), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            PredictionStatus.INSUFFICIENT_FOR_NEXT_DOSE -> Text(stringResource(R.string.prediction_insufficient_next), color = MaterialTheme.colorScheme.error)
            PredictionStatus.BLOCKED_UNTIL_REFILL -> Text(stringResource(R.string.prediction_blocked), color = MaterialTheme.colorScheme.error)
            PredictionStatus.NO_FUTURE_DOSES -> Text(stringResource(R.string.prediction_no_doses), color = MaterialTheme.colorScheme.onSurfaceVariant)
            PredictionStatus.ALL_REMAINING_DOSES_COVERED -> Text(stringResource(R.string.prediction_all_covered))
            PredictionStatus.UNKNOWN_BEYOND_HORIZON -> Text(stringResource(R.string.prediction_unknown), color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
    body()
    Text(stringResource(R.string.prediction_note), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
}

@Composable
private fun RefillDialog(
    capacityText: String,
    capacityLabel: String,
    validate: (String) -> RefillError?,
    onDismiss: () -> Unit,
    onConfirm: (String) -> Unit,
) {
    var text by rememberSaveable { mutableStateOf(capacityText) }
    val error = validate(text)
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.refill_dialog_title)) },
        text = {
            Column(Modifier.imePadding(), verticalArrangement = Arrangement.spacedBy(Spacing.s)) {
                OutlinedTextField(
                    value = text,
                    onValueChange = { text = it },
                    label = { Text(stringResource(R.string.refill_dialog_label)) },
                    suffix = { Text(stringResource(R.string.editor_amount_suffix)) },
                    singleLine = true,
                    isError = error != null,
                    supportingText = {
                        Text(
                            when (error) {
                                null -> stringResource(R.string.refill_dialog_hint, capacityLabel)
                                RefillError.ABOVE_CAPACITY -> stringResource(R.string.refill_err_above_capacity, capacityLabel)
                                RefillError.NOT_POSITIVE -> stringResource(R.string.editor_err_amount_positive)
                                RefillError.TOO_PRECISE -> stringResource(R.string.editor_err_amount_precision)
                                RefillError.INVALID, RefillError.NOT_ACTIVE -> stringResource(R.string.editor_err_amount_invalid)
                            },
                        )
                    },
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
                    modifier = Modifier.fillMaxWidth(),
                )
                TextButton(onClick = { text = capacityText }) { Text(stringResource(R.string.refill_fill_to_capacity)) }
            }
        },
        confirmButton = { TextButton(onClick = { onConfirm(text) }, enabled = error == null) { Text(stringResource(R.string.refill_confirm)) } },
        dismissButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.dialog_cancel)) } },
    )
}

@Preview(name = "Reservoir 412", widthDp = 412, heightDp = 900, showBackground = true, backgroundColor = 0xFF111820)
@Composable
private fun ReservoirPreview() {
    val seed = MockSeed.default()
    val state = ReservoirUiState.Ready(
        reservoir = seed.reservoir,
        status = ReservoirStatus.NORMAL,
        lowThresholdPercent = 20,
        prediction = ReservoirPredictor().predict(seed.reservoir, seed.device.now, seed.schedules),
        zone = seed.device.zone,
    )
    DispenserTheme { ReservoirScreen(state, isDemo = true, onBack = {}, validateRefill = { null }, onRefill = {}) }
}
