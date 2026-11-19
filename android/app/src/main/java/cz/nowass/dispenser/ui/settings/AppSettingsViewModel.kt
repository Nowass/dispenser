package cz.nowass.dispenser.ui.settings

import cz.nowass.dispenser.domain.ConnectionState
import cz.nowass.dispenser.domain.DataSource
import cz.nowass.dispenser.domain.DeviceAccess
import cz.nowass.dispenser.domain.Freshness
import java.time.Instant

import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import cz.nowass.dispenser.data.AppSettingsRepository
import cz.nowass.dispenser.data.AppVersion
import cz.nowass.dispenser.data.DispenserRepository
import cz.nowass.dispenser.data.DisplayNameRules
import cz.nowass.dispenser.domain.simulation.ReservoirState
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch

sealed interface AppSettingsUiState {
    data object Loading : AppSettingsUiState

    data class Ready(
        /** Custom name, or null when the default is used. */
        val customName: String?,
        val version: AppVersion,
        /** Read-only reservoir summary; null when unknown (no device, no simulation). */
        val reservoir: ReservoirState?,
        val simulationActive: Boolean,
        /** False in DEVICE mode: no "simulace" labels, device freshness instead. */
        val isDemo: Boolean = true,
        /** Freshness of [reservoir] in DEVICE mode (never shown as live when cached). */
        val freshness: Freshness = Freshness.NotApplicable,
        val connection: ConnectionState = ConnectionState.DEMO,
        val now: Instant = Instant.EPOCH,
        /** DEVICE mode: credential health (recovery required when the credential is unusable). */
        val access: DeviceAccess? = null,
        /** DEVICE mode: unresolved operations of the device (always shown, never dropped). */
        val unresolvedOperations: Int = 0,
    ) : AppSettingsUiState
}

/** Settings "Aplikace" / "Zásobník" / "Zařízení" sections. Simulation controls have their own VM. */
class AppSettingsViewModel(
    private val settings: AppSettingsRepository,
    repository: DispenserRepository,
    private val version: AppVersion,
) : ViewModel() {

    val uiState: StateFlow<AppSettingsUiState> = combine(
        settings.displayName,
        repository.observeReservoir(),
        repository.observeDevice(),
    ) { name, reservoir, device ->
        AppSettingsUiState.Ready(
            name, version, reservoir, device.simulationActive,
            isDemo = device.source == DataSource.DEMO,
            freshness = device.freshness,
            connection = device.connection,
            now = device.now,
            access = device.access,
            unresolvedOperations = device.unresolvedOperations,
        )
    }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), AppSettingsUiState.Loading)

    fun validateName(text: String): DisplayNameRules.Error? = DisplayNameRules.validate(text)

    /** Saves a valid name; blank restores the default. Invalid input is ignored (UI blocks it). */
    fun saveName(text: String) {
        if (validateName(text) != null) return
        viewModelScope.launch { settings.setDisplayName(text) }
    }

    fun resetName() {
        viewModelScope.launch { settings.setDisplayName(null) }
    }

    companion object {
        fun factory(settings: AppSettingsRepository, repository: DispenserRepository, version: AppVersion): ViewModelProvider.Factory =
            viewModelFactory { initializer { AppSettingsViewModel(settings, repository, version) } }
    }
}
