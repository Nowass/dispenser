package cz.nowass.dispenser.data.backup

import cz.nowass.dispenser.data.AppSettingsRepository
import cz.nowass.dispenser.data.ReplaceOutcome
import cz.nowass.dispenser.data.ScheduleStore
import cz.nowass.dispenser.data.SimulationStore
import cz.nowass.dispenser.data.StoredSchedule
import cz.nowass.dispenser.domain.simulation.HistoryEvent
import cz.nowass.dispenser.domain.simulation.SimulationState
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import java.io.IOException

/** Simulated process death: an [Error], so the service's `catch (Exception)` rollback never runs. */
class ProcessDeath(point: String) : Error("process died at $point")

/**
 * Failure injection at persistence boundaries. Each wrapped operation calls `"<op>.before"`
 * and `"<op>.after"`; the configured point throws an [IOException] (in-process failure) or
 * a [ProcessDeath] (interruption with no cleanup at all).
 */
class Faults {
    /** One-shot failure points. */
    val failAt = mutableSetOf<String>()
    var die: Boolean = false
    val visited = mutableListOf<String>()

    fun hit(point: String) {
        visited += point
        if (failAt.remove(point)) {
            if (die) throw ProcessDeath(point) else throw IOException("injected failure at $point")
        }
    }

    suspend fun <T> around(op: String, block: suspend () -> T): T {
        hit("$op.before")
        val r = block()
        hit("$op.after")
        return r
    }
}

class FaultyScheduleStore(private val inner: ScheduleStore, private val f: Faults) : ScheduleStore by inner {
    override suspend fun replaceReadable(schedules: List<StoredSchedule>): ReplaceOutcome = f.around("schedules.replace") { inner.replaceReadable(schedules) }
    override suspend fun restoreRaw(rows: RawScheduleRows) = f.around("schedules.restoreRaw") { inner.restoreRaw(rows) }
    override suspend fun captureRaw(): RawScheduleRows = f.around("schedules.captureRaw") { inner.captureRaw() }
}

class FaultySimulationStore(private val inner: SimulationStore, private val f: Faults) : SimulationStore by inner {
    override suspend fun replaceAll(state: SimulationState?, history: List<HistoryEvent>) = f.around("simulation.replaceAll") { inner.replaceAll(state, history) }
    override suspend fun deactivate() = f.around("simulation.deactivate") { inner.deactivate() }
    override suspend fun restoreRaw(rows: RawSimulationRows) = f.around("simulation.restoreRaw") { inner.restoreRaw(rows) }
}

class FaultySettings(private val inner: AppSettingsRepository, private val f: Faults) : AppSettingsRepository by inner {
    override suspend fun setDisplayName(name: String?) = f.around("settings.set") { inner.setDisplayName(name) }
    override suspend fun restoreRaw(value: String?) = f.around("settings.restoreRaw") { inner.restoreRaw(value) }
}

class FaultyFiles(private val inner: RestoreFiles, private val f: Faults) : RestoreFiles by inner {
    override suspend fun writeSnapshot(bytes: ByteArray) = f.around("files.writeSnapshot") { inner.writeSnapshot(bytes) }
    override suspend fun writeJournal(bytes: ByteArray) = f.around("files.writeJournal") { inner.writeJournal(bytes) }
    override suspend fun deleteJournal() = f.around("files.deleteJournal") { inner.deleteJournal() }
}

class MutexAccess : ExclusiveStoreAccess {
    private val mutex = Mutex()
    override suspend fun <T> withExclusiveAccess(block: suspend () -> T): T = mutex.withLock { block() }
}
