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
     * DEVICE mode: sets the reservoir capacity and the current amount together (initializes an
     * unconfigured reservoir, or changes the capacity of a configured one), through the same REFILL
     * as [refillReservoir] (journaled, verified by read-back). 0 < [remainingUl] <= [capacityUl]; an
     * empty reservoir (0) is not accepted by the current firmware and is never replaced by another
     * value. DEMO: [CommandResult.NotActive] (the simulation's capacity is fixed by its start).
     */
    suspend fun configureReservoir(capacityUl: Long, remainingUl: Long): CommandResult = CommandResult.NotActive

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

    /** DEVICE mode: the device's own LOW threshold (config `low_stock_percent`) as last read; null = not read / DEMO. */
    fun observeLowStockPercent(): Flow<Int?> = flowOf(null)

    /** DEVICE mode: reads the device configuration (the LOW threshold) when a session is live; DEMO: no-op. */
    suspend fun refreshReservoirSettings() {}

    /**
     * DEVICE mode: sets the device's LOW threshold (1–99 % of the capacity) through SET_CONFIG with only
     * the low-stock field selected, after a live configuration read (its revision), journaled and
     * verified by read-back. The firmware re-evaluates the reservoir LOW notification at once. Nothing
     * else changes (no refill, no schedule, no readiness). DEMO: [CommandResult.NotActive].
     */
    suspend fun setLowStockPercent(percent: Int): CommandResult = CommandResult.NotActive

    /** DEVICE mode: the battery card can request a new measurement ([refreshBattery]). DEMO: false. */
    val batteryRefreshAvailable: Boolean get() = false

    /**
     * DEVICE mode: asks the connected device for a new battery measurement (GET_BATTERY; the device
     * takes a new ADC sample unless one was taken within 2 s or a pump run just ended, and always
     * reports the sample's real age). Never connects by itself. DEMO: [BatteryRefreshResult.Unsupported].
     */
    suspend fun refreshBattery(): BatteryRefreshResult = BatteryRefreshResult.Unsupported

    /** False when the source keeps no local event history ([observeHistory]); DEVICE mode reads [deviceHistory]. */
    val historyAvailable: Boolean get() = true

    /** DEVICE mode (Phase C): the device's own event log, paged over BLE; null for DEMO. */
    val deviceHistory: cz.nowass.dispenser.data.device.DeviceHistorySource? get() = null

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

    /**
     * The device refused the amount: above its configured maximum dose ([maxDoseUl]; LIMIT with
     * DSP_DOSING_L_DOSE). Nothing changed. A genuine safety rule (such a dose could never run), shown
     * with its reason instead of the generic refusal.
     */
    data class AboveMaxDose(val maxDoseUl: Long) : ScheduleWriteResult

    /** A device limit the phone checks before sending (name length, timezone, ranges); nothing was sent. */
    data class DeviceLimit(val problems: Set<String>) : ScheduleWriteResult
}

/** Outcome of a user-requested battery measurement (the value itself arrives through observeDevice). */
enum class BatteryRefreshResult {
    /** The device answered; the reading (and its measurement time) is updated. */
    Refreshed,
    /** No live session: nothing was sent (the device must be awake and connected). */
    NotConnected,
    /** Sent but not answered, refused, or the session ended: the previous value stays, marked stale. */
    Failed,
    /** No battery telemetry here (DEMO, or firmware older than 0.2.0). */
    Unsupported,
}
