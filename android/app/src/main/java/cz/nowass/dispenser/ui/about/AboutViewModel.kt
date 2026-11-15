package cz.nowass.dispenser.ui.about

import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import cz.nowass.dispenser.data.AppVersion
import cz.nowass.dispenser.data.update.UpdateChecker
import cz.nowass.dispenser.domain.update.ReleaseChannel
import cz.nowass.dispenser.domain.update.SemanticVersion
import cz.nowass.dispenser.domain.update.UpdateCheckResult
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

sealed interface UpdateUiState {
    data object Idle : UpdateUiState
    data object Checking : UpdateUiState
    /** Installed versionName is not semantic; checking would be meaningless. */
    data object UnsupportedVersion : UpdateUiState
    data class Done(val result: UpdateCheckResult) : UpdateUiState
}

data class AboutUiState(
    val version: AppVersion,
    /** Derived from the installed versionName; null if it is not a semantic version. */
    val channel: ReleaseChannel?,
    /** Repository page, or null while the source link is disabled (D5). */
    val sourceCodeUrl: String?,
    val update: UpdateUiState = UpdateUiState.Idle,
)

/** About screen: version from package metadata, user-initiated update check only. */
class AboutViewModel(
    private val checker: UpdateChecker,
    version: AppVersion,
    sourceCodeUrl: String?,
) : ViewModel() {

    private val installed: SemanticVersion? = SemanticVersion.parse(version.name)

    private val _state = MutableStateFlow(
        AboutUiState(
            version = version,
            channel = installed?.let(ReleaseChannel::forInstalledVersion),
            sourceCodeUrl = sourceCodeUrl,
        ),
    )
    val state: StateFlow<AboutUiState> = _state.asStateFlow()

    fun checkForUpdates() {
        val current = installed
        if (current == null) {
            _state.update { it.copy(update = UpdateUiState.UnsupportedVersion) }
            return
        }
        if (_state.value.update == UpdateUiState.Checking) return
        _state.update { it.copy(update = UpdateUiState.Checking) }
        viewModelScope.launch {
            val result = checker.check(current, ReleaseChannel.forInstalledVersion(current))
            _state.update { it.copy(update = UpdateUiState.Done(result)) }
        }
    }

    companion object {
        fun factory(checker: UpdateChecker, version: AppVersion, sourceCodeUrl: String?): ViewModelProvider.Factory =
            viewModelFactory { initializer { AboutViewModel(checker, version, sourceCodeUrl) } }
    }
}
