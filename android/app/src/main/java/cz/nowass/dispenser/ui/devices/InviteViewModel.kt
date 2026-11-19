package cz.nowass.dispenser.ui.devices

import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import cz.nowass.dispenser.data.onboarding.AdminFailure
import cz.nowass.dispenser.data.onboarding.InviteController
import cz.nowass.dispenser.data.onboarding.InviteOutcome
import cz.nowass.dispenser.data.onboarding.PhoneName
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

sealed interface InviteUiState {
    data class Form(val failure: AdminFailure? = null, val cancelled: Boolean = false, val cancelFailed: Boolean = false, val expired: Boolean = false) : InviteUiState
    data object Working : InviteUiState
    /** The QR text exists only in this state; it is dropped on expiry, cancel and when the screen closes. */
    class Showing(val qrText: String, val expiresUtc: Long) : InviteUiState {
        override fun toString() = "Showing(expires=$expiresUtc, qr=**)"
    }
}

/** The owner's invitation screen (M1.10e.4c). Holds the QR text only while it is shown. */
class InviteViewModel(
    private val controller: InviteController,
    private val nowUtc: () -> Long = { System.currentTimeMillis() / 1000 },
) : ViewModel() {
    private val _state = MutableStateFlow<InviteUiState>(InviteUiState.Form())
    val state: StateFlow<InviteUiState> = _state.asStateFlow()
    private var expiry: Job? = null

    fun create(nameHint: String) {
        if (_state.value == InviteUiState.Working || (nameHint.isNotBlank() && PhoneName.normalize(nameHint) == null)) return
        _state.value = InviteUiState.Working
        viewModelScope.launch {
            // the controller reports every failure as a result; anything unexpected must not crash the app
            val outcome = try {
                controller.create(nameHint)
            } catch (e: kotlinx.coroutines.CancellationException) {
                throw e
            } catch (e: Exception) {
                InviteOutcome.Failed(AdminFailure.INVALID)
            }
            when (val r = outcome) {
                is InviteOutcome.Created -> {
                    _state.value = InviteUiState.Showing(r.qrText, r.expiresUtc)
                    expiry?.cancel()
                    expiry = launch {
                        delay(((r.expiresUtc - nowUtc()).coerceAtLeast(0)) * 1000)
                        if (_state.value is InviteUiState.Showing) _state.value = InviteUiState.Form(expired = true)
                    }
                }
                is InviteOutcome.Failed -> _state.value = InviteUiState.Form(failure = r.reason)
            }
        }
    }

    fun cancel() {
        if (_state.value !is InviteUiState.Showing) return
        expiry?.cancel()
        _state.value = InviteUiState.Working // the QR text is dropped at once
        viewModelScope.launch {
            val ok = try {
                controller.cancel()
            } catch (e: kotlinx.coroutines.CancellationException) {
                throw e
            } catch (e: Exception) {
                false
            }
            _state.value = InviteUiState.Form(cancelled = ok, cancelFailed = !ok)
        }
    }

    override fun onCleared() {
        expiry?.cancel()
        _state.value = InviteUiState.Form()
    }

    companion object {
        fun factory(controller: () -> InviteController): ViewModelProvider.Factory = viewModelFactory {
            initializer { InviteViewModel(controller()) }
        }
    }
}
