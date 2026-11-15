package cz.nowass.dispenser.ui.backup

import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import cz.nowass.dispenser.data.backup.BackupCodec
import cz.nowass.dispenser.data.backup.BackupError
import cz.nowass.dispenser.data.backup.BackupService
import cz.nowass.dispenser.data.backup.DocumentIo
import cz.nowass.dispenser.data.backup.PrepareResult
import cz.nowass.dispenser.data.backup.RestorePlan
import cz.nowass.dispenser.data.backup.RestoreResult
import cz.nowass.dispenser.data.backup.SimulationEffect
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import java.time.Clock
import java.time.LocalDate

data class BackupUiState(
    val includeSimulation: Boolean = false,
    val busy: Boolean = false,
    /** Validated backup awaiting the user's explicit confirmation; nothing written yet. */
    val pendingRestore: RestorePlan? = null,
    val message: BackupMessage? = null,
)

sealed interface BackupMessage {
    data class Exported(val schedules: Int, val historyEvents: Int?, val unreadableSkipped: Int) : BackupMessage
    data object ExportFailed : BackupMessage
    data object ReadFailed : BackupMessage
    data class Invalid(val error: BackupError) : BackupMessage
    data class Restored(val schedules: Int, val effect: SimulationEffect) : BackupMessage
    data object RolledBack : BackupMessage
    data class NotStarted(val error: BackupError?) : BackupMessage
}

/**
 * Backup screen state. All JSON handling and validation live in [BackupService] /
 * [BackupCodec]; this class only sequences picker results, confirmation and feedback.
 * A null URI means the user cancelled the system picker: nothing happens.
 */
class BackupViewModel(
    private val service: BackupService,
    private val documents: DocumentIo,
    private val clock: Clock,
) : ViewModel() {

    private val _uiState = MutableStateFlow(BackupUiState())
    val uiState: StateFlow<BackupUiState> = _uiState.asStateFlow()

    fun suggestedFileName(): String = "dispenser-zaloha-${LocalDate.now(clock)}.json"

    fun setIncludeSimulation(include: Boolean) = _uiState.update { it.copy(includeSimulation = include) }

    fun dismissMessage() = _uiState.update { it.copy(message = null) }

    fun onExportDestination(uri: String?) {
        if (uri == null || _uiState.value.busy) return
        val include = _uiState.value.includeSimulation
        runBusy {
            val message = try {
                val data = service.export(include)
                documents.write(uri, data.bytes)
                BackupMessage.Exported(
                    schedules = data.content.schedules.size,
                    historyEvents = data.content.simulation?.history?.size,
                    unreadableSkipped = data.unreadableSkipped,
                )
            } catch (e: Exception) {
                if (e is kotlinx.coroutines.CancellationException) throw e
                BackupMessage.ExportFailed
            }
            _uiState.update { it.copy(message = message) }
        }
    }

    fun onImportSource(uri: String?) {
        if (uri == null || _uiState.value.busy) return
        runBusy {
            val bytes = try {
                documents.read(uri, BackupCodec.MAX_BYTES)
            } catch (e: Exception) {
                if (e is kotlinx.coroutines.CancellationException) throw e
                _uiState.update { it.copy(message = BackupMessage.ReadFailed) }
                return@runBusy
            }
            when (val r = service.prepare(bytes)) {
                is PrepareResult.Invalid -> _uiState.update { it.copy(message = BackupMessage.Invalid(r.error)) }
                is PrepareResult.Ready -> _uiState.update { it.copy(pendingRestore = r.plan, message = null) }
            }
        }
    }

    /** Dismisses the confirmation; the stored configuration stays untouched. */
    fun cancelRestore() = _uiState.update { it.copy(pendingRestore = null) }

    fun confirmRestore() {
        val plan = _uiState.value.pendingRestore ?: return
        if (_uiState.value.busy) return
        _uiState.update { it.copy(pendingRestore = null) }
        runBusy {
            val message = when (val r = service.restore(plan)) {
                is RestoreResult.Restored -> BackupMessage.Restored(r.scheduleCount, r.simulationEffect)
                RestoreResult.RolledBack -> BackupMessage.RolledBack
                is RestoreResult.NotStarted -> BackupMessage.NotStarted(r.error)
                // The storage gate replaces the whole UI with the blocked screen.
                RestoreResult.Blocked -> null
            }
            _uiState.update { it.copy(message = message) }
        }
    }

    private fun runBusy(block: suspend () -> Unit) {
        _uiState.update { it.copy(busy = true) }
        viewModelScope.launch {
            try {
                block()
            } finally {
                _uiState.update { it.copy(busy = false) }
            }
        }
    }

    companion object {
        fun factory(service: BackupService, documents: DocumentIo, clock: Clock): ViewModelProvider.Factory = viewModelFactory {
            initializer { BackupViewModel(service, documents, clock) }
        }
    }
}
