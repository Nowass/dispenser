package cz.nowass.dispenser.data

import cz.nowass.dispenser.domain.DeviceStatus
import cz.nowass.dispenser.domain.simulation.HistoryEvent
import cz.nowass.dispenser.domain.simulation.ReservoirState
import cz.nowass.dispenser.domain.Schedule
import cz.nowass.dispenser.domain.ScheduleDraft
import cz.nowass.dispenser.domain.SyncState
import cz.nowass.dispenser.domain.ValidationError
import kotlinx.coroutines.flow.Flow

/**
 * Single boundary between UI/ViewModels and dispenser data.
 *
 * Implemented by [LocalDispenserRepository] (schedules persisted on the phone, device
 * values simulated); a future `BleDispenserRepository` will implement the same contract.
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
}

sealed interface ScheduleWriteResult {
    /** [schedule] is the stored result, or null after a delete. */
    data class Success(val schedule: Schedule?) : ScheduleWriteResult
    data class Invalid(val errors: Set<ValidationError>) : ScheduleWriteResult
    /** Stored revision differs from the expected one (concurrent edit). */
    data object Conflict : ScheduleWriteResult
    data object NotFound : ScheduleWriteResult
}
