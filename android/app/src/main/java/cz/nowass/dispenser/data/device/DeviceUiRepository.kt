package cz.nowass.dispenser.data.device

import cz.nowass.dispenser.data.CommandResult
import cz.nowass.dispenser.data.DispenserRepository
import cz.nowass.dispenser.data.ScheduleWriteResult
import cz.nowass.dispenser.device.ops.DeviceErrorCode
import cz.nowass.dispenser.device.ops.LocalRefusal
import cz.nowass.dispenser.device.ops.OpResult
import cz.nowass.dispenser.device.ops.StatusRecord
import cz.nowass.dispenser.device.session.SessionState
import cz.nowass.dispenser.domain.ConnectionState
import cz.nowass.dispenser.domain.DEFAULT_ZONE
import cz.nowass.dispenser.domain.DataSource
import cz.nowass.dispenser.domain.DeviceAccess
import cz.nowass.dispenser.domain.DeviceStatus
import cz.nowass.dispenser.crypto.credentials.CredentialState
import cz.nowass.dispenser.crypto.credentials.CredentialStore
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import cz.nowass.dispenser.domain.Freshness
import cz.nowass.dispenser.domain.Schedule
import cz.nowass.dispenser.domain.ScheduleDraft
import cz.nowass.dispenser.domain.SyncState
import cz.nowass.dispenser.domain.Volume
import cz.nowass.dispenser.domain.simulation.HistoryEvent
import cz.nowass.dispenser.domain.simulation.ReservoirState
import cz.nowass.dispenser.domain.toSchedule
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.flowOf
import kotlinx.coroutines.flow.map
import java.time.Clock
import java.time.Instant
import kotlin.time.Duration
import kotlin.time.Duration.Companion.minutes

/**
 * DEVICE-mode adapter from the device repository ([DeviceRepository], M1.10e.3) to the UI contract of
 * the shared screens ([DispenserRepository]); docs/ANDROID_BLE_DESIGN.md 2.1, 2.3, 7, 16 (M1.10e.4a).
 *
 * Nothing is simulated or guessed, and no DEMO data is ever read:
 * - device values carry their freshness ([Freshness.Live] only while the session that read them is the
 *   live one, otherwise [Freshness.Cached] with the device's read time, [Freshness.Unknown] if never read);
 * - the schedules are the device's set as last read back (device-confirmed, hence DEVICE_APPLIED);
 * - DEVICE-only outcomes stay explicit: [ScheduleWriteResult.Uncertain] / [CommandResult.Uncertain]
 *   are never mapped to a failure, [ScheduleWriteResult.NotSent] / [CommandResult.NotSent] mean that
 *   provably nothing reached the device;
 * - there is no device history in protocol v1 ([historyAvailable] = false).
 *
 * The device-only features (connection, manual dose, phones, configuration …) use [device] directly.
 */
class DeviceUiRepository(
    val device: DeviceRepository,
    private val clock: Clock,
    /** Re-emits the phone time on each boundary (as the DEMO repository); null = once. */
    private val clockTick: Duration? = 1.minutes,
    /**
     * Credential health of [device] ([credentialAccess]). While recovery is required, the connection is
     * reported as [ConnectionState.RECOVERY_REQUIRED] and every mutation is refused locally (NotSent).
     */
    val access: StateFlow<DeviceAccess> = MutableStateFlow(DeviceAccess.OK),
) : DispenserRepository {

    override val historyAvailable: Boolean get() = false
    override val writesRequireConnection: Boolean get() = true

    private fun phoneNow(): Flow<Instant> = flow {
        emit(clock.instant())
        val tickMs = (clockTick ?: return@flow).inWholeMilliseconds
        while (true) {
            delay(tickMs - Math.floorMod(clock.millis(), tickMs))
            emit(clock.instant())
        }
    }

    private fun connectionOf(state: SessionState): ConnectionState = when (state) {
        is SessionState.Authenticated -> if (state.deviceIdHex == device.deviceId) ConnectionState.CONNECTED else ConnectionState.DISCONNECTED
        is SessionState.Connecting -> if (state.deviceIdHex == device.deviceId) ConnectionState.CONNECTING else ConnectionState.DISCONNECTED
        is SessionState.Handshaking -> if (state.deviceIdHex == device.deviceId) ConnectionState.CONNECTING else ConnectionState.DISCONNECTED
        is SessionState.ConfirmingOnDevice -> if (state.deviceIdHex == device.deviceId) ConnectionState.CONNECTING else ConnectionState.DISCONNECTED
        else -> ConnectionState.DISCONNECTED
    }

    override fun observeDevice(): Flow<DeviceStatus> =
        combine(
            device.connection, device.status(), phoneNow(), access,
            device.pendingOperations().map { rows -> rows.count { it.unresolved } }.distinctUntilChanged(),
        ) { state, status, now, access, unresolved ->
            DeviceStatus(
                source = DataSource.DEVICE,
                connection = if (access.recoveryRequired) ConnectionState.RECOVERY_REQUIRED else connectionOf(state),
                access = access,
                unresolvedOperations = unresolved,
                // The phone clock: the device clock is reported (with its trust) by the device status screen.
                now = now,
                batteryPercent = status.valueOrNull?.batteryPercent,
                zone = DEFAULT_ZONE,
                simulationActive = false,
                freshness = status.freshness(),
            )
        }

    override fun observeSchedules(): Flow<List<Schedule>> =
        device.schedules().map { f -> f.valueOrNull?.schedules?.map { it.schedule }.orEmpty() }.distinctUntilChanged()

    override fun observeScheduleFreshness(): Flow<Freshness> = device.schedules().map { it.freshness() }.distinctUntilChanged()

    /** Every schedule shown in DEVICE mode was read back from the device. */
    override fun observeScheduleSync(): Flow<Map<String, SyncState>> =
        observeSchedules().map { list -> list.associate { it.id to SyncState.DEVICE_APPLIED } }

    override fun observeUnreadableScheduleCount(): Flow<Int> =
        device.schedules().map { it.valueOrNull?.unreadable ?: 0 }.distinctUntilChanged()

    override fun observeReservoir(): Flow<ReservoirState?> = device.status().map { f ->
        val s = f.valueOrNull ?: return@map null
        val readAt = when (f) {
            is Fresh.Live -> f.readAt
            is Fresh.Cached -> f.readAt
            Fresh.Unknown -> return@map null
        }
        reservoirOf(s, readAt)
    }.distinctUntilChanged()

    override fun observeHistory(): Flow<List<HistoryEvent>> = flowOf(emptyList())

    /** No mutation while the credential is unusable (fail closed; nothing is journaled or sent). */
    private val blocked: Boolean get() = access.value.recoveryRequired

    override suspend fun refillReservoir(remainingUl: Long): CommandResult =
        if (blocked) CommandResult.NotSent else commandOf(device.refill(remainingUl))

    override suspend fun configureReservoir(capacityUl: Long, remainingUl: Long): CommandResult =
        if (blocked) CommandResult.NotSent else commandOf(device.refill(remainingUl, capacityUl))

    private fun commandOf(r: OpResult<StatusRecord>): CommandResult = when (r) {
        is OpResult.Ok -> CommandResult.Done(newEvents = 0)
        is OpResult.Uncertain -> CommandResult.Uncertain
        is OpResult.NotSent -> CommandResult.NotSent
        is OpResult.Rejected -> CommandResult.DeviceRejected(r.error.code.wire)
        is OpResult.Refused -> when (r.reason) {
            is LocalRefusal.Unresolved, LocalRefusal.InProgress -> CommandResult.AwaitingVerification
            is LocalRefusal.Invalid -> CommandResult.NotAccepted
            // Nothing was sent in every remaining case (session changed, persistence failed, …).
            else -> CommandResult.NotSent
        }
    }

    override suspend fun createSchedule(draft: ScheduleDraft): ScheduleWriteResult =
        if (blocked) ScheduleWriteResult.NotSent else writeResult(device.createSchedule(draft), draft, fallbackRevision = 1)

    override suspend fun updateSchedule(id: String, expectedRevision: Int, draft: ScheduleDraft): ScheduleWriteResult =
        if (blocked) ScheduleWriteResult.NotSent
        else writeResult(device.updateSchedule(id, expectedRevision.toLong(), draft), draft, fallbackRevision = expectedRevision + 1)

    override suspend fun deleteSchedule(id: String, expectedRevision: Int): ScheduleWriteResult =
        if (blocked) ScheduleWriteResult.NotSent else when (val r = device.deleteSchedule(id, expectedRevision.toLong())) {
            is OpResult.Ok -> ScheduleWriteResult.Success(null)
            else -> failure(r)
        }

    private suspend fun writeResult(r: OpResult<ScheduleWrite>, draft: ScheduleDraft, fallbackRevision: Int): ScheduleWriteResult =
        when (r) {
            is OpResult.Ok -> {
                // The repository re-reads the set after a confirmed write: prefer the device's own record.
                val readBack = device.schedules().first().valueOrNull?.schedules?.firstOrNull { it.schedule.id == r.value.id }?.schedule
                val revision = r.value.revision?.takeIf { it in 1..Int.MAX_VALUE.toLong() }?.toInt() ?: fallbackRevision
                ScheduleWriteResult.Success(readBack ?: draft.toSchedule(r.value.id, revision))
            }
            else -> failure(r)
        }

    private fun failure(r: OpResult<*>): ScheduleWriteResult = when (r) {
        is OpResult.Ok -> error("not a failure")
        is OpResult.Uncertain -> ScheduleWriteResult.Uncertain
        is OpResult.NotSent -> ScheduleWriteResult.NotSent
        is OpResult.Rejected -> when (r.error.code) {
            DeviceErrorCode.CONFLICT -> ScheduleWriteResult.Conflict
            DeviceErrorCode.NOT_FOUND -> ScheduleWriteResult.NotFound
            else -> ScheduleWriteResult.DeviceRejected(r.error.code.wire)
        }
        is OpResult.Refused -> when (val reason = r.reason) {
            is LocalRefusal.Unresolved, LocalRefusal.InProgress -> ScheduleWriteResult.AwaitingVerification
            is LocalRefusal.Invalid -> ScheduleWriteResult.DeviceLimit(reason.problems.map { it.name }.toSet())
            else -> ScheduleWriteResult.NotSent
        }
    }

    companion object {
        /**
         * Credential health of [deviceId] in [store], for the recovery-required state. Loads (and at once
         * zeroizes) the credential: the Keystore key must exist and the record must decrypt. A Keystore
         * error that is not a recognised loss is treated as unreadable (fail closed, never as OK).
         */
        fun credentialAccess(store: CredentialStore, deviceId: String): DeviceAccess = try {
            when (val st = store.load(deviceId)) {
                is CredentialState.Present -> { st.credential.close(); DeviceAccess.OK }
                CredentialState.Lost -> DeviceAccess.KEY_MISSING
                is CredentialState.Unreadable -> DeviceAccess.RECORD_UNREADABLE
                CredentialState.Absent -> DeviceAccess.RECORD_MISSING
            }
        } catch (e: Exception) {
            DeviceAccess.RECORD_UNREADABLE
        }

        fun <T> Fresh<T>.freshness(): Freshness = when (this) {
            Fresh.Unknown -> Freshness.Unknown
            is Fresh.Live -> Freshness.Live(readAt)
            is Fresh.Cached -> Freshness.Cached(readAt)
        }

        /** The device's own estimate; null if it reports no usable capacity (never invented). */
        internal fun reservoirOf(s: StatusRecord, readAt: Instant): ReservoirState? {
            if (s.capacityUl !in 1..Volume.MAX_MICROLITRES || s.remainingUl !in 0..s.capacityUl) return null
            return ReservoirState(capacityUl = s.capacityUl, remainingUl = s.remainingUl, updatedAt = readAt)
        }
    }
}
