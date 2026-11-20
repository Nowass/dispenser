package cz.nowass.dispenser.ui.devices

import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import cz.nowass.dispenser.data.onboarding.AdminFailure
import cz.nowass.dispenser.data.onboarding.PhoneUi
import cz.nowass.dispenser.data.onboarding.PhonesController
import cz.nowass.dispenser.data.onboarding.PhonesOutcome
import cz.nowass.dispenser.data.onboarding.RemoveOutcome
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

data class PhonesUiState(
    val loading: Boolean = true,
    val phones: List<PhoneUi> = emptyList(),
    val failure: AdminFailure? = null,
    val removal: RemoveOutcome? = null,
    val busy: Boolean = false,
)

/** The device's phone list (M1.10e.4c). [runAfterClose] drops the link (no END_SESSION) after the screen is gone. */
class PhonesViewModel(
    private val controller: PhonesController,
    private val runAfterClose: (suspend () -> Unit) -> Unit,
) : ViewModel() {
    private val _state = MutableStateFlow(PhonesUiState())
    val state: StateFlow<PhonesUiState> = _state.asStateFlow()

    init { load() }

    fun load() {
        _state.update { it.copy(loading = true, failure = null) }
        viewModelScope.launch {
            when (val r = controller.load()) {
                is PhonesOutcome.Listed -> _state.update { it.copy(loading = false, phones = r.phones) }
                is PhonesOutcome.Failed -> _state.update { it.copy(loading = false, failure = r.reason) }
            }
        }
    }

    fun remove(phone: PhoneUi) {
        if (_state.value.busy) return
        _state.update { it.copy(busy = true, removal = null) }
        viewModelScope.launch {
            val r = controller.remove(phone)
            _state.update { it.copy(busy = false, removal = r) }
            if (r == RemoveOutcome.REMOVED) load()
        }
    }

    override fun onCleared() {
        runAfterClose { controller.finish() }
    }

    companion object {
        fun factory(controller: () -> PhonesController, runAfterClose: (suspend () -> Unit) -> Unit): ViewModelProvider.Factory = viewModelFactory {
            initializer { PhonesViewModel(controller(), runAfterClose) }
        }
    }
}
