package cz.nowass.dispenser.data.backup

import cz.nowass.dispenser.data.AppSettingsRepository
import cz.nowass.dispenser.data.ReplaceOutcome
import cz.nowass.dispenser.data.ScheduleStore
import cz.nowass.dispenser.data.SimulationStore
import cz.nowass.dispenser.data.StoredSchedule
import cz.nowass.dispenser.domain.Schedule
import cz.nowass.dispenser.domain.SyncState
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import java.time.Clock
import java.util.UUID

/** Serializes backup operations against schedule edits and simulation commands. */
interface ExclusiveStoreAccess {
    suspend fun <T> withExclusiveAccess(block: suspend () -> T): T
}

/** Whether the local stores may be used (startup recovery gate, R1). */
sealed interface StorageState {
    /** A pending restore journal is being rolled back; stores must not be used yet. */
    data object Recovering : StorageState

    data class Ready(val notice: RecoveryNotice? = null) : StorageState

    /** Recovery failed: the app must not continue with possibly partially restored data. */
    data class Blocked(val reason: BlockReason) : StorageState
}

enum class RecoveryNotice { INTERRUPTED_RESTORE_ROLLED_BACK }

enum class BlockReason {
    /** Journal present but the safety snapshot is missing, unreadable or does not match. */
    SNAPSHOT_UNAVAILABLE,
    /** Writing the snapshot back into the stores failed. */
    ROLLBACK_FAILED,
}

/** What happens to the simulation when a plan is restored (R2). */
enum class SimulationEffect {
    /** The backup contains simulation data: state, processed occurrences and history are replaced. */
    REPLACED,
    /** Schedules change and no simulation data is restored: the run stops, history is kept. */
    DEACTIVATED,
    /** Schedules are identical (ignoring revisions): the active run continues unchanged. */
    CONTINUES,
    /** No simulation is running; existing history stays unchanged. */
    NONE,
}

/** Validated backup plus everything the confirmation dialog must show. Nothing is written yet. */
data class RestorePlan(
    val content: BackupContent,
    val currentScheduleCount: Int,
    val currentUnreadableCount: Int,
    val currentDisplayName: String?,
    val currentHistoryCount: Int,
    val simulationEffect: SimulationEffect,
)

sealed interface PrepareResult {
    data class Ready(val plan: RestorePlan) : PrepareResult
    data class Invalid(val error: BackupError) : PrepareResult
}

sealed interface RestoreResult {
    data class Restored(val scheduleCount: Int, val simulationEffect: SimulationEffect) : RestoreResult
    /** Rejected before anything was changed (the stored configuration is untouched). */
    data class NotStarted(val error: BackupError?) : RestoreResult
    /** A write failed; the previous configuration was restored from the safety snapshot. */
    data object RolledBack : RestoreResult
    /** A write failed and so did the rollback; the app is blocked (see [StorageState.Blocked]). */
    data object Blocked : RestoreResult
}

data class ExportData(val bytes: ByteArray, val content: BackupContent, val unreadableSkipped: Int) {
    override fun equals(other: Any?) = other is ExportData && content == other.content && bytes.contentEquals(other.bytes)
    override fun hashCode() = content.hashCode()
}

/**
 * Configuration backup and restore (A1.4c.1).
 *
 * Restore protocol (R1), all under [ExclusiveStoreAccess] and non-cancellable:
 * 1. The file is fully decoded and validated first ([prepare]); invalid input never reaches storage.
 * 2. An exact safety snapshot of every store is written durably (`pre-restore.json`).
 * 3. Only then the journal (`restore.journal`, referencing the snapshot id) is written.
 * 4. Schedules (one transaction), simulation (one transaction) and settings (one atomic edit)
 *    are replaced.
 * 5. Deleting the journal is the commit point.
 *
 * Any exception in 4/5 rolls all stores back to the snapshot. If the process dies in between,
 * the journal survives and [recoverAtStartup] rolls back before any store is used. If rollback
 * is impossible, [state] becomes [StorageState.Blocked].
 */
class BackupService(
    private val schedules: ScheduleStore,
    private val simulation: SimulationStore,
    private val settings: AppSettingsRepository,
    private val files: RestoreFiles,
    private val access: ExclusiveStoreAccess,
    private val clock: Clock,
    private val appVersion: () -> String,
    private val snapshotIds: () -> String = { UUID.randomUUID().toString() },
    private val log: (String) -> Unit = {},
) {
    private val _state = MutableStateFlow<StorageState>(
        if (files.journalExists()) StorageState.Recovering else StorageState.Ready(),
    )

    /** Startup gate: UI and repositories may use the stores only in [StorageState.Ready]. */
    val state: StateFlow<StorageState> = _state.asStateFlow()

    // ------------------------------------------------------------------ export

    /** Consistent export (no edits or simulation steps can interleave). Self-checked before returning. */
    suspend fun export(includeSimulation: Boolean): ExportData = access.withExclusiveAccess {
        val snapshot = schedules.observe().first()
        val content = BackupContent(
            exportedAt = clock.instant(),
            applicationVersion = appVersion(),
            displayName = settings.displayName.first(),
            schedules = snapshot.schedules.map { it.schedule }.sortedBy { it.id },
            simulation = if (includeSimulation) SimulationBackup(simulation.currentState(), simulation.observeHistory().first()) else null,
        )
        val bytes = BackupCodec.encode(content).toByteArray(Charsets.UTF_8)
        // Never hand out a file this app could not restore.
        val check = BackupCodec.decode(bytes)
        check(check is BackupCodec.Decoded.Valid && check.content == content) { "export self-check failed: $check" }
        ExportData(bytes, content, snapshot.unreadableCount)
    }

    // ------------------------------------------------------------------ restore

    /** Decodes, validates and describes a backup. Read-only. */
    suspend fun prepare(bytes: ByteArray): PrepareResult {
        val content = when (val d = BackupCodec.decode(bytes)) {
            is BackupCodec.Decoded.Invalid -> return PrepareResult.Invalid(d.error)
            is BackupCodec.Decoded.Valid -> d.content
        }
        return access.withExclusiveAccess {
            collision(content)?.let { return@withExclusiveAccess PrepareResult.Invalid(it) }
            val snapshot = schedules.observe().first()
            PrepareResult.Ready(
                RestorePlan(
                    content = content,
                    currentScheduleCount = snapshot.schedules.size,
                    currentUnreadableCount = snapshot.unreadableCount,
                    currentDisplayName = settings.displayName.first(),
                    currentHistoryCount = simulation.observeHistory().first().size,
                    simulationEffect = effectOf(content, snapshot.schedules.map { it.schedule }, simulation.currentState() != null),
                ),
            )
        }
    }

    /** Replaces the configuration with [plan] (explicit "replace" semantics, never a merge). */
    suspend fun restore(plan: RestorePlan): RestoreResult = withContext(NonCancellable) {
        if (_state.value !is StorageState.Ready) return@withContext RestoreResult.NotStarted(null)
        access.withExclusiveAccess { restoreLocked(plan.content) }
    }

    private suspend fun restoreLocked(content: BackupContent): RestoreResult {
        collision(content)?.let { return RestoreResult.NotStarted(it) }
        val current = schedules.observe().first().schedules.map { it.schedule }
        val effect = effectOf(content, current, simulation.currentState() != null)

        // 1+2: durable safety snapshot before anything else.
        val snapshot = try {
            SafetySnapshot(snapshotIds(), clock.instant(), schedules.captureRaw(), simulation.captureRaw(), settings.captureRaw())
                .also { files.writeSnapshot(SafetySnapshotCodec.encode(it)) }
        } catch (e: Exception) {
            log("Safety snapshot failed (${e.javaClass.simpleName}); nothing changed")
            return RestoreResult.NotStarted(null)
        }
        // 3: journal; from here on an interrupted restore is rolled back at startup.
        try {
            files.writeJournal(SafetySnapshotCodec.encodeJournal(snapshot.id))
        } catch (e: Exception) {
            log("Journal write failed (${e.javaClass.simpleName}); nothing changed")
            runCatching { files.deleteJournal() }
            return RestoreResult.NotStarted(null)
        }
        // 4+5: apply, then commit by deleting the journal.
        return try {
            val outcome = schedules.replaceReadable(content.schedules.map { StoredSchedule(it, SyncState.PENDING) })
            check(outcome == ReplaceOutcome.Replaced) { "unexpected id collision" }
            when (effect) {
                SimulationEffect.REPLACED -> content.simulation!!.let { simulation.replaceAll(it.state, it.history) }
                SimulationEffect.DEACTIVATED -> simulation.deactivate()
                SimulationEffect.CONTINUES, SimulationEffect.NONE -> Unit
            }
            settings.setDisplayName(content.displayName)
            files.deleteJournal()
            RestoreResult.Restored(content.schedules.size, effect)
        } catch (e: Exception) {
            log("Restore failed (${e.javaClass.simpleName}); rolling back")
            rollback(snapshot)
        }
    }

    private suspend fun rollback(snapshot: SafetySnapshot): RestoreResult {
        try {
            applySnapshot(snapshot)
        } catch (e: Exception) {
            log("Rollback failed (${e.javaClass.simpleName}); blocking")
            _state.value = StorageState.Blocked(BlockReason.ROLLBACK_FAILED)
            return RestoreResult.Blocked
        }
        // Stores already hold the previous state; a leftover journal only repeats the same rollback.
        runCatching { files.deleteJournal() }.onFailure { log("Journal cleanup failed after rollback") }
        return RestoreResult.RolledBack
    }

    private suspend fun applySnapshot(s: SafetySnapshot) {
        schedules.restoreRaw(s.schedules)
        simulation.restoreRaw(s.simulation)
        settings.restoreRaw(s.displayNameRaw)
    }

    // ------------------------------------------------------------------ startup recovery

    /**
     * Must complete before repositories, simulation or UI use the stores. Without a journal
     * this is a no-op. With a journal, the stores are rolled back to the safety snapshot.
     */
    suspend fun recoverAtStartup(): StorageState = withContext(NonCancellable) { recoveryLock.withLock { recoverLocked() } }

    private val recoveryLock = Mutex()

    private suspend fun recoverLocked(): StorageState {
        if (!files.journalExists()) {
            // No pending restore: the stores are consistent (a failed rollback always keeps the journal).
            _state.value = StorageState.Ready()
            return _state.value
        }
        _state.value = StorageState.Recovering
        val snapshot = try {
            val journalId = SafetySnapshotCodec.decodeJournal(requireNotNull(files.readJournal()))
            SafetySnapshotCodec.decode(requireNotNull(files.readSnapshot())).also { require(it.id == journalId) { "snapshot mismatch" } }
        } catch (e: Exception) {
            log("Restore journal present but snapshot unavailable (${e.javaClass.simpleName})")
            _state.value = StorageState.Blocked(BlockReason.SNAPSHOT_UNAVAILABLE)
            return _state.value
        }
        try {
            applySnapshot(snapshot)
        } catch (e: Exception) {
            log("Startup rollback failed (${e.javaClass.simpleName})")
            _state.value = StorageState.Blocked(BlockReason.ROLLBACK_FAILED)
            return _state.value
        }
        runCatching { files.deleteJournal() }.onFailure { log("Journal cleanup failed after startup rollback") }
        _state.value = StorageState.Ready(RecoveryNotice.INTERRUPTED_RESTORE_ROLLED_BACK)
        return _state.value
    }

    fun acknowledgeNotice() {
        _state.update { if (it is StorageState.Ready) StorageState.Ready() else it }
    }

    // ------------------------------------------------------------------ helpers

    private suspend fun collision(content: BackupContent): BackupError? {
        val unreadable = schedules.unreadableIds()
        val index = content.schedules.indexOfFirst { it.id in unreadable }
        return if (index < 0) null else BackupError(BackupError.Kind.ID_COLLISION_UNREADABLE, "schedules[$index].id")
    }

    companion object {
        /** R2: identical schedule definitions (revisions ignored) keep an active run valid. */
        fun effectOf(content: BackupContent, current: List<Schedule>, simulationActive: Boolean): SimulationEffect = when {
            content.simulation != null -> SimulationEffect.REPLACED
            !simulationActive -> SimulationEffect.NONE
            sameDefinitions(current, content.schedules) -> SimulationEffect.CONTINUES
            else -> SimulationEffect.DEACTIVATED
        }

        private fun sameDefinitions(a: List<Schedule>, b: List<Schedule>): Boolean =
            a.associate { it.id to it.copy(revision = 0) } == b.associate { it.id to it.copy(revision = 0) }
    }
}
