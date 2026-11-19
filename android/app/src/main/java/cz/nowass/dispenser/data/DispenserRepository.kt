package cz.nowass.dispenser.data

import cz.nowass.dispenser.domain.DeviceStatus
import cz.nowass.dispenser.domain.Freshness
import cz.nowass.dispenser.domain.simulation.HistoryEvent
import cz.nowass.dispenser.domain.simulation.ReservoirState
import cz.nowass.dispenser.domain.Schedule
import cz.nowass.dispenser.domain.ScheduleDraft
import cz.nowass.dispenser.domain.SyncState
import cz.nowass.dispenser.domain.ValidationError
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.flowOf

/**
 * Single boundary between UI/ViewModels and dispenser data.
 *
 * Implemented by [LocalDispenserRepository] (DEMO: schedules persisted on the phone, device values
 * simulated) and by [cz.nowass.dispenser.data.device.DeviceUiRepository] (DEVICE: an adapter over the
 * device repository; M1.10e.4). The two are never mixed: the app shows exactly one of them.
 *
 * DEVICE-only outcomes are explicit result variants ([ScheduleWriteResult.NotSent],
 * [ScheduleWriteResult.Uncertain], …, [CommandResult.Uncertain], …); the DEMO implementation never
 * returns them. An uncertain mutation is never reported as a failure.
 */
interface DispenserRepository {
    fun observeDevice(): Flow<DeviceStatus>
    fun observeSchedules(): Flow<List<Schedule>>
    /** Estimated reservoir state; null when unknown (no device connected, no simulation running). */
    fun observeReservoir(): Flow<ReservoirState?>

    /** Dispensing / reservoir history; every event is tagged with its [cz.nowass.dispenser.domain.simulation.EventOrigin]. */
    fun observeHistory(): Flow<List<HistoryEvent>>

    /** Refill (D7): sets the absolute remaining volume in µl, ≤ capacity. */
    suspend fun refillReservoir(remainingUl: Long): CommandResult

    /**
     * Device confirmation state per schedule id. Locally saved schedules are
     * [SyncState.PENDING] ("Uloženo v telefonu"): stored on the phone, not transferred
     * to or accepted by the dispenser. Only a future device read-back may report
     * [SyncState.DEVICE_APPLIED]; it is never simulated.
     */
    fun observeScheduleSync(): Flow<Map<String, SyncState>>

    /**
     * Number of stored schedules that could not be loaded (corrupted data). They are kept
     * untouched; the UI shows a non-blocking warning instead of hiding the problem.
     */
    fun observeUnreadableScheduleCount(): Flow<Int>

    /** Creates a schedule with a new stable id and revision 1. */
    suspend fun createSchedule(draft: ScheduleDraft): ScheduleWriteResult

    /**
     * Replaces the definition of schedule [id] if its current revision equals
     * [expectedRevision]; the stored revision is then incremented.
     */
    suspend fun updateSchedule(id: String, expectedRevision: Int, draft: ScheduleDraft): ScheduleWriteResult

    /** Deletes schedule [id] if its revision matches. Event history is kept. */
    suspend fun deleteSchedule(id: String, expectedRevision: Int): ScheduleWriteResult

    /**
     * Freshness of [observeSchedules] (DEVICE: the device's schedule set as last read back, live or
     * cached with its read time). DEMO: [Freshness.NotApplicable].
     */
    fun observeScheduleFreshness(): Flow<Freshness> = flowOf(Freshness.NotApplicable)

    /** False when the source keeps no event history (DEVICE mode: not in protocol v1). */
    val historyAvailable: Boolean get() = true

    /**
     * True when mutations need a live connection to the device (DEVICE mode: online-only, E12).
     * The UI disables editing while [cz.nowass.dispenser.domain.DeviceStatus.connection] is not
     * CONNECTED; a mutation attempted anyway returns a NotSent result.
     */
    val writesRequireConnection: Boolean get() = false
}

sealed interface ScheduleWriteResult {
    /** [schedule] is the stored result, or null after a delete. */
    data class Success(val schedule: Schedule?) : ScheduleWriteResult
    data class Invalid(val errors: Set<ValidationError>) : ScheduleWriteResult
    /** Stored revision differs from the expected one (concurrent edit). */
    data object Conflict : ScheduleWriteResult
    data object NotFound : ScheduleWriteResult

    // ---- DEVICE mode only (never returned in DEMO) ----

    /** Provably never reached the device (not connected, session ended before sending, nothing persisted): nothing changed. */
    data object NotSent : ScheduleWriteResult

    /**
     * May have been applied by the device: NOT a failure. Resolved by read-back at the next session;
     * the user must not be invited to repeat the change.
     */
    data object Uncertain : ScheduleWriteResult

    /** An earlier change of this schedule is still being processed or awaits verification by read-back; nothing was sent. */
    data object AwaitingVerification : ScheduleWriteResult

    /** The device refused the change ([code]: BLE_PROTOCOL.md 10.1); nothing changed. */
    data class DeviceRejected(val code: Int) : ScheduleWriteResult

    /** A device limit the phone checks before sending (name length, timezone, ranges); nothing was sent. */
    data class DeviceLimit(val problems: Set<String>) : ScheduleWriteResult
}
