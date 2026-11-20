package cz.nowass.dispenser.data.device

import cz.nowass.dispenser.device.ops.ConfigRecord
import cz.nowass.dispenser.device.ops.DiagnosticsRecord
import cz.nowass.dispenser.device.ops.InviteRecord
import cz.nowass.dispenser.device.ops.ManualDoseRecord
import cz.nowass.dispenser.device.ops.OpResult
import cz.nowass.dispenser.device.ops.PhoneRecord
import cz.nowass.dispenser.device.ops.Readiness
import cz.nowass.dispenser.device.ops.ScheduleRecord
import cz.nowass.dispenser.device.ops.StatusRecord
import cz.nowass.dispenser.device.ops.TimeDecisionRecord
import cz.nowass.dispenser.device.session.DeviceEvent
import cz.nowass.dispenser.device.session.SessionFailure
import cz.nowass.dispenser.device.session.SessionState
import cz.nowass.dispenser.domain.Schedule
import cz.nowass.dispenser.domain.ScheduleDraft
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import java.time.Instant

/**
 * Freshness of a device value (decision D2). Never conflated:
 * - [Live]: read in the CURRENT authenticated session and connection [generation]; stops being
 *   Live as soon as that session ends (disconnect, close, another generation).
 * - [Cached]: the last device-confirmed value with its read time — informational only; it never
 *   authorizes a safety-relevant mutation (manual dose and REFILL re-read the status live).
 * - [Unknown]: never read.
 */
sealed interface Fresh<out T> {
    data object Unknown : Fresh<Nothing>
    data class Live<T>(val value: T, val readAt: Instant, val generation: Long) : Fresh<T>
    data class Cached<T>(val value: T, val readAt: Instant) : Fresh<T>

    val valueOrNull: T? get() = when (this) {
        Unknown -> null
        is Live -> value
        is Cached -> value
    }
}

/** The device's schedule set as read back (the device is the source of truth in DEVICE mode). */
data class DeviceSchedules(
    val setRevision: Long,
    val schedules: List<DeviceSchedule>,
    /** Records the domain model cannot represent (kept on the device, never guessed). */
    val unreadable: Int = 0,
)

data class DeviceSchedule(val schedule: Schedule, val record: ScheduleRecord)

/** A confirmed schedule write: the device's new revisions (the set is re-read afterwards). */
data class ScheduleWrite(val id: String, val revision: Long?, val setRevision: Long)

sealed interface ConnectOutcome {
    /** Authenticated; pending operations were resolved first (11.4); [unresolved] remain (PENDING, awaiting the user, or interrupted resolution). */
    data class Connected(
        val generation: Long,
        val unresolved: Int,
        /**
         * False when the journal resolution of this session could not finish (a journal read/write
         * failed, or the device did not answer a read-back). Unfinished rows stay unresolved and keep
         * blocking their own operations; reads stay available; calling connect() again retries.
         */
        val resolutionComplete: Boolean = true,
    ) : ConnectOutcome
    data class Failed(val failure: SessionFailure) : ConnectOutcome
    /** No associated address for this device (CDM association, M1.10e.4). */
    data object NoAddress : ConnectOutcome
}

/**
 * DEVICE-mode repository (docs/ANDROID_BLE_DESIGN.md 2.1, 5, 7, 11.4–11.6; decisions D1–D8). Every
 * mutation is online-only (E12) and returns a typed [OpResult]; nothing is retried across sessions,
 * an [OpResult.Uncertain] outcome is never a failure, REFILL and manual doses are never re-sent
 * automatically, and no replacement manual-dose `seq` is ever allocated automatically.
 *
 * Not wired into the UI in M1.10e.3 (DEMO and the existing screens are unchanged); the adaptation to
 * the UI contract [cz.nowass.dispenser.data.DispenserRepository] is M1.10e.4.
 */
interface DeviceRepository {
    val deviceId: String

    /** Connection/session state (separate from every device value). */
    val connection: StateFlow<SessionState>
    val events: SharedFlow<DeviceEvent>

    fun status(): Flow<Fresh<StatusRecord>>
    fun config(): Flow<Fresh<ConfigRecord>>
    fun diagnostics(): Flow<Fresh<DiagnosticsRecord>>
    fun schedules(): Flow<Fresh<DeviceSchedules>>
    fun phones(): Flow<Fresh<List<PhoneRecord>>>

    /** Journal rows: unresolved ("result unknown — connect to check"), awaiting the user, or resolved until acknowledged. */
    fun pendingOperations(): Flow<List<PendingOp>>

    /** IK session; then resolves journaled operations (before any new mutation) and reads the status. */
    suspend fun connect(): ConnectOutcome
    /** graceful = encrypted END_SESSION (ends the device's service window); else just disconnect. */
    suspend fun disconnect(graceful: Boolean)

    suspend fun refreshStatus(): OpResult<StatusRecord>
    suspend fun refreshDiagnostics(): OpResult<DiagnosticsRecord>
    suspend fun ackRejections(): OpResult<Unit>
    suspend fun refreshConfig(): OpResult<ConfigRecord>
    /** SET_CONFIG of the fields in [mask] (values from [values]); `CONFLICT` → read again, never auto-merged. */
    suspend fun setConfig(expectedRevision: Long, mask: Int, values: ConfigRecord): OpResult<Long>

    suspend fun refreshSchedules(): OpResult<DeviceSchedules>
    suspend fun createSchedule(draft: ScheduleDraft): OpResult<ScheduleWrite>
    suspend fun updateSchedule(id: String, expectedRevision: Long, draft: ScheduleDraft): OpResult<ScheduleWrite>
    suspend fun deleteSchedule(id: String, expectedRevision: Long): OpResult<ScheduleWrite>

    /** A new manual dose: live status read, `seq` durably allocated, journal row durable, only then sent (D4). */
    suspend fun manualDose(volumeUl: Long): OpResult<ManualDoseRecord>
    /** Re-sends a NOT_DECIDED dose with its ORIGINAL `seq` (user action; never automatic). */
    suspend fun resendManualDose(journalId: Long): OpResult<ManualDoseRecord>
    /** Discards a NOT_DECIDED dose on the user's request. */
    suspend fun discardManualDose(journalId: Long): OpResult<Unit>
    /** Removes a RESOLVED row once the user has seen its outcome. */
    suspend fun acknowledge(journalId: Long): OpResult<Unit>

    /**
     * REFILL (D5): sets the absolute [remainingUl]. [capacityUl] null keeps the capacity read live in the
     * same session immediately before (refused locally when the device has none); non-null sets a new
     * capacity, which also initializes an unconfigured reservoir. Validated locally before anything is
     * sent: 0 < remaining <= capacity <= u32 (the firmware does not accept an empty reservoir yet).
     */
    suspend fun refill(remainingUl: Long, capacityUl: Long? = null): OpResult<StatusRecord>
    suspend fun setReadiness(readiness: Readiness): OpResult<StatusRecord>
    /** SET_TIME only with a [PhoneTimeSource] uncertainty ≤ 2,000 ms, else refused locally (D6). */
    suspend fun setTime(): OpResult<TimeDecisionRecord>

    suspend fun refreshPhones(): OpResult<List<PhoneRecord>>
    suspend fun removePhone(slot: Int): OpResult<Unit>
    /** The token is secret: the caller shows it as a QR only and wipes it (7.1). */
    suspend fun createInvite(nameHint: String): OpResult<InviteRecord>
    suspend fun cancelInvite(): OpResult<Unit>
    suspend fun cancelRecovery(): OpResult<Unit>
}
