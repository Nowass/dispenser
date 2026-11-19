package cz.nowass.dispenser.data.device

import cz.nowass.dispenser.crypto.credentials.CredentialStore
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.update

/** Journaled DEVICE-mode mutations (docs/ANDROID_BLE_DESIGN.md 11.4, E12). */
enum class PendingKind { MANUAL_DOSE, REFILL, SET_CONFIG, CREATE_SCHEDULE, UPDATE_SCHEDULE, DELETE_SCHEDULE, SET_READINESS, PHONE_REMOVE }

enum class PendingState {
    /** Written before sending; the request may have reached the device ("result unknown"). */
    PREPARED,

    /** Manual dose the device reports NOT_DECIDED: the user may send it again with the SAME seq, or discard it. */
    AWAITING_USER,

    /** Read back and decided; kept until the user acknowledges the outcome. */
    RESOLVED,
}

/** What the read-back at the next session established. */
enum class Resolution {
    // manual dose (GET_RESULT, BLE_PROTOCOL.md 10.4)
    NOT_DECIDED, DONE, FAILED, INTERRUPTED, REFUSED, DECIDED_RESULT_UNAVAILABLE,
    // other writes (table 5.1)
    APPLIED, NOT_APPLIED, CHANGED_ELSEWHERE,
    /** REFILL: the device's current reservoir values were read back; never re-sent automatically. */
    READ_BACK,
}

/**
 * One journal row. [payload] is the request body as sent (protocol encoding), so the row alone
 * describes the operation after a process death; it contains no secret.
 */
data class PendingOp(
    val id: Long = 0,
    val deviceId: String,
    val kind: PendingKind,
    val state: PendingState = PendingState.PREPARED,
    val createdAtMs: Long,
    val seq: Long? = null,
    val volumeUl: Long? = null,
    val target: String? = null,
    val expectedRevision: Long? = null,
    val payload: ByteArray? = null,
    val resolution: Resolution? = null,
    val resolutionDetail: Long? = null,
    val resolvedAtMs: Long? = null,
) {
    val unresolved: Boolean get() = state != PendingState.RESOLVED

    override fun equals(other: Any?) = other is PendingOp && other.id == id && other.deviceId == deviceId && other.kind == kind &&
        other.state == state && other.createdAtMs == createdAtMs && other.seq == seq && other.volumeUl == volumeUl &&
        other.target == target && other.expectedRevision == expectedRevision && other.resolution == resolution &&
        other.resolutionDetail == resolutionDetail && other.resolvedAtMs == resolvedAtMs &&
        (other.payload ?: ByteArray(0)).contentEquals(payload ?: ByteArray(0))
    override fun hashCode() = id.hashCode() * 31 + kind.hashCode()
}

/** The durable journal (`device.db` `pending_ops`). Every write throws on failure; nothing is swallowed. */
interface PendingOpsJournal {
    fun observe(deviceId: String): Flow<List<PendingOp>>
    suspend fun list(deviceId: String): List<PendingOp>
    /** Durable when it returns; returns the row id. */
    suspend fun insert(op: PendingOp): Long
    suspend fun update(op: PendingOp)
    suspend fun delete(id: Long)
}

/** A cached device response body (protocol encoding) with the time it was read. */
class CacheEntry(val body: ByteArray, val readAtMs: Long)

/** The device cache (`device.db` `device_cache`): last device-confirmed snapshot per key. */
interface DeviceCacheStore {
    suspend fun get(deviceId: String, key: String): CacheEntry?
    suspend fun put(deviceId: String, key: String, entry: CacheEntry)
}

/** Durable manual-dose `seq` allocation (D4): persisted BEFORE it is returned; throws on failure. */
fun interface ManualSeqAllocator {
    fun next(deviceId: String, floor: Long): Long
}

/** Production allocator: the high-water mark stored with the credential (CredentialStore.nextManualSeq). */
class CredentialSeqAllocator(private val store: CredentialStore) : ManualSeqAllocator {
    override fun next(deviceId: String, floor: Long): Long = store.nextManualSeq(deviceId, floor)
}

/** Phone UTC time with a defensible uncertainty bound (D6). */
data class PhoneTime(val utcMs: Long, val uncertaintyMs: Long)

/**
 * Source of phone time for SET_TIME (E13, D6). Returns null when it cannot establish a defensible
 * uncertainty; SET_TIME is then refused locally (fail closed). The production source is a
 * separately tracked item (docs/ANDROID_BLE_DESIGN.md 15.4).
 */
fun interface PhoneTimeSource {
    fun now(): PhoneTime?
}

/** The current production source: no defensible uncertainty is available, so SET_TIME is never sent. */
object UnavailablePhoneTimeSource : PhoneTimeSource {
    override fun now(): PhoneTime? = null
}

/** Non-persistent journal for tests; [failInsert]/[failUpdate] inject persistence failures. */
class InMemoryPendingOpsJournal : PendingOpsJournal {
    private val rows = MutableStateFlow<List<PendingOp>>(emptyList())
    private var nextId = 1L
    @Volatile var failInsert = false
    @Volatile var failUpdate = false

    override fun observe(deviceId: String): Flow<List<PendingOp>> = rows.map { l -> l.filter { it.deviceId == deviceId } }
    override suspend fun list(deviceId: String): List<PendingOp> = rows.value.filter { it.deviceId == deviceId }

    override suspend fun insert(op: PendingOp): Long {
        if (failInsert) throw java.io.IOException("injected journal insert failure")
        val id = synchronized(this) { nextId++ }
        rows.update { it + op.copy(id = id) }
        return id
    }

    override suspend fun update(op: PendingOp) {
        if (failUpdate) throw java.io.IOException("injected journal update failure")
        rows.update { l -> l.map { if (it.id == op.id) op else it } }
    }

    override suspend fun delete(id: Long) = rows.update { l -> l.filterNot { it.id == id } }
}

class InMemoryDeviceCacheStore : DeviceCacheStore {
    private val map = java.util.concurrent.ConcurrentHashMap<String, CacheEntry>()
    override suspend fun get(deviceId: String, key: String): CacheEntry? = map["$deviceId/$key"]
    override suspend fun put(deviceId: String, key: String, entry: CacheEntry) {
        map["$deviceId/$key"] = entry
    }
}
