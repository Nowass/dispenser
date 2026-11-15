package cz.nowass.dispenser.data

import cz.nowass.dispenser.data.backup.RawScheduleRows
import cz.nowass.dispenser.data.local.ScheduleEntity
import cz.nowass.dispenser.data.local.ScheduleEntityMapper
import cz.nowass.dispenser.data.local.ScheduleRawRows
import cz.nowass.dispenser.domain.Schedule
import cz.nowass.dispenser.domain.SyncState
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.update

/** A stored schedule definition together with its device confirmation state. */
data class StoredSchedule(val schedule: Schedule, val sync: SyncState)

enum class StoreWriteOutcome { OK, CONFLICT, NOT_FOUND }

/** Result of replacing all readable schedules (backup restore). */
sealed interface ReplaceOutcome {
    data object Replaced : ReplaceOutcome
    /** Imported ids that equal ids of unreadable stored rows; nothing was written. */
    data class IdCollision(val ids: Set<String>) : ReplaceOutcome
}

/**
 * Current store content. [unreadableCount] counts stored records that could not be
 * loaded (corrupted); they are kept untouched and are not part of [schedules].
 */
data class StoreSnapshot(val schedules: List<StoredSchedule>, val unreadableCount: Int = 0)

/**
 * Persistence port for schedules. Implementations must apply each conditional write
 * atomically (revision check and write together). Domain validation happens in the
 * repository before calling the store; stores only persist valid [Schedule]s.
 *
 * Implemented by [cz.nowass.dispenser.data.local.RoomScheduleStore] (app) and
 * [InMemoryScheduleStore] (tests, previews). A future BLE repository can use the same
 * store as its local cache of pending / device-applied configuration.
 */
interface ScheduleStore {
    /** All valid stored schedules plus the number of unreadable records; emits after every change. */
    fun observe(): Flow<StoreSnapshot>

    /** Inserts a new schedule; returns false if the id already exists. */
    suspend fun insert(stored: StoredSchedule): Boolean

    /** Replaces the schedule with the same id if its stored revision equals [expectedRevision]. */
    suspend fun replace(stored: StoredSchedule, expectedRevision: Int): StoreWriteOutcome

    /** Deletes schedule [id] if its stored revision equals [expectedRevision]. */
    suspend fun delete(id: String, expectedRevision: Int): StoreWriteOutcome

    /** Ids of stored records that cannot be loaded (see [StoreSnapshot.unreadableCount]). */
    suspend fun unreadableIds(): Set<String>

    /**
     * Backup restore: atomically replaces every readable schedule with [schedules] while keeping
     * unreadable records untouched. Rejects (writes nothing) if an id equals an unreadable record.
     */
    suspend fun replaceReadable(schedules: List<StoredSchedule>): ReplaceOutcome

    /** Exact copy of every stored record (including unreadable ones) for the safety snapshot. */
    suspend fun captureRaw(): RawScheduleRows

    /** Atomically restores an exact copy taken by [captureRaw] (rollback). */
    suspend fun restoreRaw(rows: RawScheduleRows)
}

/** Non-persistent store for unit tests and Compose previews. */
class InMemoryScheduleStore(
    initial: List<StoredSchedule> = emptyList(),
    /** Raw rows that fail validation (tests of the unreadable-row rules). */
    unreadable: List<ScheduleEntity> = emptyList(),
) : ScheduleStore {

    private val state = MutableStateFlow(initial)
    private val unreadableRows = MutableStateFlow(unreadable)

    override fun observe(): Flow<StoreSnapshot> =
        combine(state, unreadableRows) { list, bad -> StoreSnapshot(list, bad.size) }

    override suspend fun unreadableIds(): Set<String> = unreadableRows.value.mapTo(mutableSetOf()) { it.id }

    override suspend fun replaceReadable(schedules: List<StoredSchedule>): ReplaceOutcome {
        val collisions = schedules.map { it.schedule.id }.filterTo(mutableSetOf()) { it in unreadableIds() }
        if (collisions.isNotEmpty()) return ReplaceOutcome.IdCollision(collisions)
        require(schedules.map { it.schedule.id }.toSet().size == schedules.size) { "duplicate ids" }
        state.value = schedules
        return ReplaceOutcome.Replaced
    }

    override suspend fun captureRaw(): RawScheduleRows = RawScheduleRows(
        (state.value.map { ScheduleEntityMapper.toEntity(it, 0L) } + unreadableRows.value).map(ScheduleRawRows::toJson),
    )

    override suspend fun restoreRaw(rows: RawScheduleRows) {
        val valid = ArrayList<StoredSchedule>()
        val bad = ArrayList<ScheduleEntity>()
        rows.rows.map(ScheduleRawRows::fromJson).forEach { e ->
            when (val d = ScheduleEntityMapper.fromEntity(e)) {
                is ScheduleEntityMapper.Decoded.Valid -> valid += d.stored
                is ScheduleEntityMapper.Decoded.Invalid -> bad += e
            }
        }
        state.value = valid
        unreadableRows.value = bad
    }

    override suspend fun insert(stored: StoredSchedule): Boolean {
        var inserted = false
        state.update { list ->
            inserted = list.none { it.schedule.id == stored.schedule.id }
            if (inserted) list + stored else list
        }
        return inserted
    }

    override suspend fun replace(stored: StoredSchedule, expectedRevision: Int): StoreWriteOutcome {
        var outcome = StoreWriteOutcome.NOT_FOUND
        state.update { list ->
            val current = list.firstOrNull { it.schedule.id == stored.schedule.id }
            outcome = when {
                current == null -> StoreWriteOutcome.NOT_FOUND
                current.schedule.revision != expectedRevision -> StoreWriteOutcome.CONFLICT
                else -> StoreWriteOutcome.OK
            }
            if (outcome == StoreWriteOutcome.OK) list.map { if (it.schedule.id == stored.schedule.id) stored else it } else list
        }
        return outcome
    }

    override suspend fun delete(id: String, expectedRevision: Int): StoreWriteOutcome {
        var outcome = StoreWriteOutcome.NOT_FOUND
        state.update { list ->
            val current = list.firstOrNull { it.schedule.id == id }
            outcome = when {
                current == null -> StoreWriteOutcome.NOT_FOUND
                current.schedule.revision != expectedRevision -> StoreWriteOutcome.CONFLICT
                else -> StoreWriteOutcome.OK
            }
            if (outcome == StoreWriteOutcome.OK) list.filterNot { it.schedule.id == id } else list
        }
        return outcome
    }
}
