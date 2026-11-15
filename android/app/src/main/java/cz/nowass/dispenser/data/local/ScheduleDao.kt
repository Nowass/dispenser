package cz.nowass.dispenser.data.local

import androidx.room.Dao
import androidx.room.Insert
import androidx.room.OnConflictStrategy
import androidx.room.Query
import androidx.room.Transaction
import androidx.room.Update
import cz.nowass.dispenser.data.StoreWriteOutcome
import kotlinx.coroutines.flow.Flow

@Dao
abstract class ScheduleDao {

    @Query("SELECT * FROM schedules ORDER BY local_time, name, id")
    abstract fun observeAll(): Flow<List<ScheduleEntity>>

    @Query("SELECT * FROM schedules WHERE id = :id")
    abstract suspend fun get(id: String): ScheduleEntity?

    /** Returns the new row id, or -1 if a row with the same primary key exists. */
    @Insert(onConflict = OnConflictStrategy.IGNORE)
    abstract suspend fun insert(entity: ScheduleEntity): Long

    /** Every stored row, including rows that fail domain validation (raw, unvalidated). */
    @Query("SELECT * FROM schedules ORDER BY id")
    abstract suspend fun getAll(): List<ScheduleEntity>

    /** Bulk insert; any primary-key collision aborts (and rolls back) the enclosing transaction. */
    @Insert(onConflict = OnConflictStrategy.ABORT)
    protected abstract suspend fun insertAllOrAbort(rows: List<ScheduleEntity>)

    @Query("DELETE FROM schedules")
    protected abstract suspend fun deleteAll()

    @Query("DELETE FROM schedules WHERE id NOT IN (:keepIds)")
    protected abstract suspend fun deleteAllExcept(keepIds: List<String>)

    /** Restores an exact row set (rollback of a backup restore): all columns as given. */
    @Transaction
    open suspend fun replaceAllRows(rows: List<ScheduleEntity>) {
        deleteAll()
        insertAllOrAbort(rows)
    }

    /**
     * Backup restore: removes every row except [keepIds] (unreadable rows, left untouched) and
     * inserts [rows] in one transaction. An id collision with a kept row aborts everything.
     */
    @Transaction
    open suspend fun replaceKeeping(keepIds: List<String>, rows: List<ScheduleEntity>) {
        deleteAllExcept(keepIds)
        insertAllOrAbort(rows)
    }

    @Update
    protected abstract suspend fun update(entity: ScheduleEntity): Int

    @Query("DELETE FROM schedules WHERE id = :id")
    protected abstract suspend fun deleteById(id: String): Int

    /** Optimistic concurrency: check the stored revision and write in one transaction. */
    @Transaction
    open suspend fun replaceIfRevision(entity: ScheduleEntity, expectedRevision: Int): StoreWriteOutcome {
        val current = get(entity.id) ?: return StoreWriteOutcome.NOT_FOUND
        if (current.revision != expectedRevision) return StoreWriteOutcome.CONFLICT
        update(entity)
        return StoreWriteOutcome.OK
    }

    @Transaction
    open suspend fun deleteIfRevision(id: String, expectedRevision: Int): StoreWriteOutcome {
        val current = get(id) ?: return StoreWriteOutcome.NOT_FOUND
        if (current.revision != expectedRevision) return StoreWriteOutcome.CONFLICT
        deleteById(id)
        return StoreWriteOutcome.OK
    }
}
