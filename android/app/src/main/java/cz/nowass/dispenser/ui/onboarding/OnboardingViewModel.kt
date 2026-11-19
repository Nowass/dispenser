package cz.nowass.dispenser.ui.onboarding

import android.content.Intent
import android.content.IntentSender
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import cz.nowass.dispenser.data.onboarding.AssociationBegin
import cz.nowass.dispenser.data.onboarding.AssociationOutcome
import cz.nowass.dispenser.data.onboarding.EnrollOutcome
import cz.nowass.dispenser.data.onboarding.EnrollPhase
import cz.nowass.dispenser.data.onboarding.InvitationEntry
import cz.nowass.dispenser.data.onboarding.LabelEntry
import cz.nowass.dispenser.data.onboarding.OnboardingController
import cz.nowass.dispenser.data.onboarding.OnboardingPurpose
import cz.nowass.dispenser.data.onboarding.OnboardingReadiness
import cz.nowass.dispenser.data.onboarding.PhoneName
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

enum class OnboardingStep {
    /** What is needed: the label, the device's service window, Bluetooth. */
    INTRO,
    /** Bluetooth off, permission missing or unsupported. */
    READINESS,
    /** ADD: scan the owner's invitation QR code (no manual entry). */
    INVITATION,
    LABEL,
    ASSOCIATE,
    NAME,
    WORKING,
    DONE,
    FAILED,
    /** A staged enrollment whose result was never confirmed (11.7): confirm, or discard explicitly. */
    UNCONFIRMED,
}

enum class AssociationProblem { CANCELLED, UNSUPPORTED, FAILED }

data class OnboardingUiState(
    val purpose: OnboardingPurpose,
    val step: OnboardingStep = OnboardingStep.INTRO,
    val readiness: OnboardingReadiness? = null,
    val deviceId: String? = null,
    val labelProblem: LabelEntry.Problem? = null,
    val invitationProblem: InvitationEntry.Problem? = null,
    /** ADD: the phones' clocks differ (the invitation is still usable; the device decides). */
    val invitationClockWarning: Boolean = false,
    val associationProblem: AssociationProblem? = null,
    val phase: EnrollPhase? = null,
    val failure: EnrollOutcome.Reason? = null,
    /** The failed attempt left an unconfirmed staged key: the retry goes through confirm-or-discard. */
    val failedWithStaged: Boolean = false,
    /** True while the flow confirms a staged enrollment (no label involved). */
    val confirmingStaged: Boolean = false,
    val busy: Boolean = false,
)

/**
 * Onboarding flow state (M1.10e.4b). Holds no secret: the label text goes straight to the controller,
 * which parses it into a SecretBytes and zeroizes it when the flow ends ([onCleared] included).
 */
class OnboardingViewModel(
    purpose: OnboardingPurpose,
    private val controller: OnboardingController,
) : ViewModel() {
    private val _state = MutableStateFlow(OnboardingUiState(purpose))
    val state: StateFlow<OnboardingUiState> = _state.asStateFlow()

    /** One-shot: the CDM dialog to launch. */
    private val _dialog = MutableStateFlow<IntentSender?>(null)
    val dialog: StateFlow<IntentSender?> = _dialog.asStateFlow()

    private var phaseJob: Job? = null

    init {
        viewModelScope.launch {
            controller.stagedDevices().firstOrNull()?.let { staged ->
                controller.selectStaged(staged)
                _state.update { it.copy(step = OnboardingStep.UNCONFIRMED, deviceId = staged, confirmingStaged = true) }
            }
        }
    }

    private val firstStep: OnboardingStep get() = if (_state.value.purpose == OnboardingPurpose.ADD) OnboardingStep.INVITATION else OnboardingStep.LABEL

    fun start() = checkReadiness(next = firstStep)

    /** After a permission or Bluetooth-enable result. */
    fun recheckReadiness() = checkReadiness(next = if (_state.value.confirmingStaged) OnboardingStep.UNCONFIRMED else firstStep)

    /**
     * A QR text from the scanner. Routed by the current step only (label or invitation), so repeated
     * detections across frames, or a late frame after the step changed, never trigger a second action.
     */
    fun onScanned(text: String) {
        when (_state.value.step) {
            OnboardingStep.INVITATION -> submitInvitation(text)
            OnboardingStep.LABEL -> submitLabel(text)
            else -> Unit
        }
    }

    fun submitInvitation(text: String): Boolean = when (val r = controller.enterInvitation(text)) {
        is InvitationEntry.Accepted -> {
            _state.update {
                it.copy(deviceId = r.deviceId, invitationProblem = null, invitationClockWarning = r.clockWarning, labelProblem = null, step = OnboardingStep.LABEL)
            }
            true
        }
        is InvitationEntry.Rejected -> {
            _state.update { it.copy(invitationProblem = r.problem) }
            false
        }
    }

    private fun checkReadiness(next: OnboardingStep) {
        val r = controller.readiness()
        _state.update { it.copy(readiness = r, step = if (r == OnboardingReadiness.Ready) next else OnboardingStep.READINESS) }
    }

    fun submitLabel(text: String): Boolean = when (val r = controller.enterLabel(text)) {
        is LabelEntry.Accepted -> {
            _state.update { it.copy(deviceId = r.deviceId, labelProblem = null, associationProblem = null, step = OnboardingStep.ASSOCIATE) }
            true
        }
        is LabelEntry.Rejected -> {
            _state.update { it.copy(labelProblem = r.problem) }
            false
        }
    }

    fun beginAssociation() {
        if (_state.value.busy) return
        _state.update { it.copy(busy = true, associationProblem = null) }
        viewModelScope.launch {
            when (val b = controller.beginAssociation()) {
                is AssociationBegin.ShowDialog -> _dialog.value = b.intentSender
                AssociationBegin.Unsupported -> _state.update { it.copy(associationProblem = AssociationProblem.UNSUPPORTED) }
                AssociationBegin.Failed -> _state.update { it.copy(associationProblem = AssociationProblem.FAILED) }
            }
            _state.update { it.copy(busy = false) }
        }
    }

    fun dialogLaunched() { _dialog.value = null }

    fun onAssociationResult(result: Intent?) {
        viewModelScope.launch {
            when (controller.finishAssociation(result)) {
                is AssociationOutcome.Associated -> afterAssociation()
                AssociationOutcome.Cancelled -> _state.update { it.copy(associationProblem = AssociationProblem.CANCELLED) }
            }
        }
    }

    /** Confirming a staged enrollment: reuse the stored association when there is one. */
    fun confirmStaged() {
        viewModelScope.launch {
            if (controller.readiness() != OnboardingReadiness.Ready) {
                checkReadiness(next = OnboardingStep.UNCONFIRMED)
                return@launch
            }
            val known = controller.knownAddress()
            if (known == null) {
                _state.update { it.copy(step = OnboardingStep.ASSOCIATE) }
            } else {
                controller.useAddress(known)
                runEnrollment { controller.confirmStaged() }
            }
        }
    }

    private fun afterAssociation() {
        if (_state.value.confirmingStaged) {
            runEnrollment { controller.confirmStaged() }
        } else {
            _state.update { it.copy(step = OnboardingStep.NAME) }
        }
    }

    fun enroll(phoneName: String) {
        if (PhoneName.normalize(phoneName) == null) return
        runEnrollment { controller.enroll(_state.value.purpose, phoneName) }
    }

    private fun runEnrollment(block: suspend () -> EnrollOutcome) {
        if (_state.value.step == OnboardingStep.WORKING) return
        _state.update { it.copy(step = OnboardingStep.WORKING, phase = null, failure = null) }
        phaseJob?.cancel()
        phaseJob = viewModelScope.launch { controller.phase.collect { p -> _state.update { it.copy(phase = p) } } }
        viewModelScope.launch {
            val r = block()
            phaseJob?.cancel()
            _state.update {
                when (r) {
                    is EnrollOutcome.Enrolled -> it.copy(step = OnboardingStep.DONE, deviceId = r.deviceId, phase = null)
                    is EnrollOutcome.Unconfirmed -> it.copy(step = OnboardingStep.UNCONFIRMED, deviceId = r.deviceId, confirmingStaged = true, phase = null)
                    is EnrollOutcome.Failed -> it.copy(step = OnboardingStep.FAILED, failure = r.reason, failedWithStaged = r.staged, phase = null)
                }
            }
        }
    }

    /** From FAILED: try the same step again (label and association are kept for this flow). */
    fun retry() {
        val s = _state.value
        _state.update {
            it.copy(
                failure = null,
                failedWithStaged = false,
                confirmingStaged = s.confirmingStaged || s.failedWithStaged,
                step = when {
                    s.confirmingStaged || s.failedWithStaged -> OnboardingStep.UNCONFIRMED
                    s.failure == EnrollOutcome.Reason.WRONG_DEVICE || s.failure == EnrollOutcome.Reason.HANDSHAKE -> OnboardingStep.LABEL
                    s.failure == EnrollOutcome.Reason.INCOMPLETE -> firstStep
                    else -> OnboardingStep.NAME
                },
            )
        }
    }

    /** Explicit user decision (after a confirmation dialog): discard the staged key, start over. */
    fun discardStaged() {
        viewModelScope.launch {
            controller.discardStaged()
            _state.update { OnboardingUiState(it.purpose) }
        }
    }

    /** DONE: switch the app to DEVICE mode (the whole UI is rebuilt for it). */
    fun finish() {
        val id = _state.value.deviceId ?: return
        viewModelScope.launch { controller.enterDeviceMode(id) }
    }

    override fun onCleared() {
        controller.close() // zeroizes the setup code if the flow is left early
    }

    companion object {
        const val ARG_PURPOSE = "purpose"

        fun factory(purpose: OnboardingPurpose, controller: () -> OnboardingController): ViewModelProvider.Factory = viewModelFactory {
            initializer { OnboardingViewModel(purpose, controller()) }
        }
    }
}
