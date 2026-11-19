package cz.nowass.dispenser.data.device.local

import android.content.Context
import androidx.room.ColumnInfo
import androidx.room.Dao
import androidx.room.Database
import androidx.room.Entity
import androidx.room.Insert
import androidx.room.OnConflictStrategy
import androidx.room.PrimaryKey
import androidx.room.Query
import androidx.room.Room
import androidx.room.RoomDatabase
import androidx.room.Update
import cz.nowass.dispenser.data.device.CacheEntry
import cz.nowass.dispenser.data.device.DeviceCacheStore
import cz.nowass.dispenser.data.device.PendingKind
import cz.nowass.dispenser.data.device.PendingOp
import cz.nowass.dispenser.data.device.PendingOpsJournal
import cz.nowass.dispenser.data.device.PendingState
import cz.nowass.dispenser.data.device.Resolution
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.map

/**
 * DEVICE-mode database `device.db` (docs/ANDROID_BLE_DESIGN.md 2.1, 11.4–11.5): the pending-operations
 * journal and the device cache. Separate from `dispenser.db` (DEMO schedules) and `simulation.db`;
 * excluded from backup and device transfer by data_extraction_rules.xml / backup_rules.xml (every
 * database is). No destructive migration fallback: losing the journal could hide an unresolved dose.
 */
@Database(entities = [PendingOpEntity::class, CacheEntity::class], version = 1, exportSchema = true)
abstract class DeviceDatabase : RoomDatabase() {
    abstract fun dao(): DeviceDao

    companion object {
        const val FILE_NAME = "device.db"

        fun build(context: Context, name: String = FILE_NAME): DeviceDatabase =
            Room.databaseBuilder(context.applicationContext, DeviceDatabase::class.java, name).build()
    }
}

@Entity(tableName = "pending_ops")
data class PendingOpEntity(
    @PrimaryKey(autoGenerate = true) val id: Long = 0,
    @ColumnInfo(name = "device_id") val deviceId: String,
    val kind: String,
    val state: String,
    @ColumnInfo(name = "created_at_ms") val createdAtMs: Long,
    val seq: Long?,
    @ColumnInfo(name = "volume_ul") val volumeUl: Long?,
    val target: String?,
    @ColumnInfo(name = "expected_revision") val expectedRevision: Long?,
    val payload: ByteArray?,
    val resolution: String?,
    @ColumnInfo(name = "resolution_detail") val resolutionDetail: Long?,
    @ColumnInfo(name = "resolved_at_ms") val resolvedAtMs: Long?,
)

@Entity(tableName = "device_cache", primaryKeys = ["device_id", "key"])
data class CacheEntity(
    @ColumnInfo(name = "device_id") val deviceId: String,
    val key: String,
    val body: ByteArray,
    @ColumnInfo(name = "read_at_ms") val readAtMs: Long,
)

@Dao
interface DeviceDao {
    @Query("SELECT * FROM pending_ops WHERE device_id = :deviceId ORDER BY id")
    fun observeOps(deviceId: String): Flow<List<PendingOpEntity>>

    @Query("SELECT * FROM pending_ops WHERE device_id = :deviceId ORDER BY id")
    suspend fun ops(deviceId: String): List<PendingOpEntity>

    @Insert
    suspend fun insertOp(op: PendingOpEntity): Long

    @Update
    suspend fun updateOp(op: PendingOpEntity): Int

    @Query("DELETE FROM pending_ops WHERE id = :id")
    suspend fun deleteOp(id: Long)

    @Query("SELECT * FROM device_cache WHERE device_id = :deviceId AND `key` = :key")
    suspend fun cache(deviceId: String, key: String): CacheEntity?

    @Insert(onConflict = OnConflictStrategy.REPLACE)
    suspend fun putCache(entry: CacheEntity)
}

class RoomPendingOpsJournal(private val dao: DeviceDao) : PendingOpsJournal {
    override fun observe(deviceId: String): Flow<List<PendingOp>> = dao.observeOps(deviceId).map { l -> l.map(::toOp) }
    override suspend fun list(deviceId: String): List<PendingOp> = dao.ops(deviceId).map(::toOp)
    override suspend fun insert(op: PendingOp): Long = dao.insertOp(toEntity(op.copy(id = 0)))
    override suspend fun update(op: PendingOp) {
        check(dao.updateOp(toEntity(op)) == 1) { "journal row ${op.id} missing" }
    }
    override suspend fun delete(id: Long) = dao.deleteOp(id)

    private fun toEntity(o: PendingOp) = PendingOpEntity(o.id, o.deviceId, o.kind.name, o.state.name, o.createdAtMs, o.seq, o.volumeUl,
        o.target, o.expectedRevision, o.payload, o.resolution?.name, o.resolutionDetail, o.resolvedAtMs)

    private fun toOp(e: PendingOpEntity) = PendingOp(e.id, e.deviceId, PendingKind.valueOf(e.kind), PendingState.valueOf(e.state),
        e.createdAtMs, e.seq, e.volumeUl, e.target, e.expectedRevision, e.payload, e.resolution?.let(Resolution::valueOf),
        e.resolutionDetail, e.resolvedAtMs)
}

class RoomDeviceCacheStore(private val dao: DeviceDao) : DeviceCacheStore {
    override suspend fun get(deviceId: String, key: String): CacheEntry? = dao.cache(deviceId, key)?.let { CacheEntry(it.body, it.readAtMs) }
    override suspend fun put(deviceId: String, key: String, entry: CacheEntry) = dao.putCache(CacheEntity(deviceId, key, entry.body, entry.readAtMs))
}
