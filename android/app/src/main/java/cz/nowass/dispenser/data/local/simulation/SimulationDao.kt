package cz.nowass.dispenser.data.local.simulation

import androidx.room.Dao
import androidx.room.Insert
import androidx.room.OnConflictStrategy
import androidx.room.Query
import androidx.room.Transaction
import kotlinx.coroutines.flow.Flow

@Dao
abstract class SimulationDao {
    @Query("SELECT * FROM sim_state WHERE id = 1")
    abstract fun observeState(): Flow<SimStateEntity?>

    @Query("SELECT * FROM sim_state WHERE id = 1")
    abstract suspend fun getState(): SimStateEntity?

    @Query("SELECT * FROM sim_processed")
    abstract fun observeProcessed(): Flow<List<SimProcessedEntity>>

    @Query("SELECT * FROM sim_processed")
    abstract suspend fun getProcessed(): List<SimProcessedEntity>

    @Query("SELECT * FROM sim_events ORDER BY seq")
    abstract fun observeEvents(): Flow<List<SimEventEntity>>

    @Insert(onConflict = OnConflictStrategy.REPLACE)
    protected abstract suspend fun upsertState(state: SimStateEntity)

    @Insert(onConflict = OnConflictStrategy.REPLACE)
    protected abstract suspend fun insertProcessed(rows: List<SimProcessedEntity>)

    /** Events are immutable: an id collision is a bug and aborts the whole transaction. */
    @Insert(onConflict = OnConflictStrategy.ABORT)
    protected abstract suspend fun insertEvents(rows: List<SimEventEntity>)

    @Query("DELETE FROM sim_state") protected abstract suspend fun deleteState()
    @Query("DELETE FROM sim_processed") protected abstract suspend fun deleteProcessed()
    @Query("DELETE FROM sim_events") protected abstract suspend fun deleteEvents()

    /** Stores the new state, its processed set and the new events in one transaction. */
    @Transaction
    open suspend fun commit(state: SimStateEntity, processed: List<SimProcessedEntity>, newEvents: List<SimEventEntity>) {
        upsertState(state)
        deleteProcessed()
        insertProcessed(processed)
        insertEvents(newEvents)
    }

    @Transaction
    open suspend fun clearAll() {
        deleteState()
        deleteProcessed()
        deleteEvents()
    }
}
