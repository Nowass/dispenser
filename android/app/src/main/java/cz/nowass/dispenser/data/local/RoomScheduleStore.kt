package cz.nowass.dispenser.data.local

import android.util.Log
import cz.nowass.dispenser.data.ScheduleStore
import cz.nowass.dispenser.data.StoreSnapshot
import cz.nowass.dispenser.data.StoreWriteOutcome
import cz.nowass.dispenser.data.StoredSchedule
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import java.time.Clock

/**
 * Room-backed [ScheduleStore]. Rows failing [ScheduleEntityMapper] validation are left
 * untouched in the database (never deleted or repaired), excluded from the schedule list,
 * counted in [StoreSnapshot.unreadableCount] for a user-visible warning, and reported via
 * [onInvalidRow] (id + failing check only, no stored values) once per change of the set.
 */
class RoomScheduleStore(
    private val dao: ScheduleDao,
    private val clock: Clock,
    private val onInvalidRow: (id: String, reason: String) -> Unit = { id, reason ->
        Log.w(TAG, "Skipping invalid stored schedule '$id': $reason")
    },
) : ScheduleStore {

    @Volatile private var lastReportedInvalid: Set<String> = emptySet()

    override fun observe(): Flow<StoreSnapshot> =
        dao.observeAll()
            .map { rows ->
                val valid = ArrayList<StoredSchedule>(rows.size)
                val invalid = ArrayList<ScheduleEntityMapper.Decoded.Invalid>()
                for (row in rows) {
                    when (val d = ScheduleEntityMapper.fromEntity(row)) {
                        is ScheduleEntityMapper.Decoded.Valid -> valid += d.stored
                        is ScheduleEntityMapper.Decoded.Invalid -> invalid += d
                    }
                }
                val ids = invalid.mapTo(mutableSetOf()) { it.id }
                if (ids != lastReportedInvalid) {
                    invalid.filter { it.id !in lastReportedInvalid }.forEach { onInvalidRow(it.id, it.reason) }
                    lastReportedInvalid = ids
                }
                StoreSnapshot(valid, unreadableCount = invalid.size)
            }
            .distinctUntilChanged()

    override suspend fun insert(stored: StoredSchedule): Boolean =
        dao.insert(ScheduleEntityMapper.toEntity(stored, clock.millis())) != -1L

    override suspend fun replace(stored: StoredSchedule, expectedRevision: Int): StoreWriteOutcome =
        dao.replaceIfRevision(ScheduleEntityMapper.toEntity(stored, clock.millis()), expectedRevision)

    override suspend fun delete(id: String, expectedRevision: Int): StoreWriteOutcome =
        dao.deleteIfRevision(id, expectedRevision)

    private companion object {
        const val TAG = "RoomScheduleStore"
    }
}
