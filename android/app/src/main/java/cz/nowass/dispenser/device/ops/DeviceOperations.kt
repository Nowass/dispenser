package cz.nowass.dispenser.device.ops

import cz.nowass.dispenser.ble.protocol.BleProtocol
import cz.nowass.dispenser.device.session.DeviceEvent
import cz.nowass.dispenser.device.session.DeviceSessionManager
import cz.nowass.dispenser.device.session.OpenRequest
import cz.nowass.dispenser.device.session.OpenResult
import cz.nowass.dispenser.device.session.RequestResult
import cz.nowass.dispenser.device.session.SecureSession
import cz.nowass.dispenser.device.session.SessionFailure
import cz.nowass.dispenser.device.session.SessionState
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.async
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow

/** Encrypted message types used by the operations (BLE_PROTOCOL.md 5). */
object MessageType {
    const val GET_STATUS = 0x10
    const val GET_DIAGNOSTICS = 0x11
    const val ACK_REJECTIONS = 0x12
    /** Firmware >= 0.2.0 only: older firmware closes the session on an unknown encrypted type. */
    const val GET_BATTERY = 0x13
    const val GET_CONFIG = 0x20
    const val SET_CONFIG = 0x21
    const val LIST_SCHEDULES = 0x30
    const val GET_SCHEDULE = 0x31
    const val CREATE_SCHEDULE = 0x32
    const val UPDATE_SCHEDULE = 0x33
    const val DELETE_SCHEDULE = 0x34
    const val MANUAL_DOSE = 0x40
    const val GET_RESULT = 0x41
    const val REFILL = 0x42
    const val SET_READINESS = 0x43
    const val SET_TIME = 0x50
    const val PHONES_LIST = 0x70
    const val PHONE_REMOVE = 0x71
    const val INVITE_CREATE = 0x72
    const val INVITE_CANCEL = 0x73
    const val RECOVERY_CANCEL = 0x74
    const val GET_HISTORY = 0x90
    /** Firmware >= 0.3.0 only (BLE_PROTOCOL.md 8.3): older firmware closes the session on an unknown encrypted type. */
    const val GET_UPDATE_STATUS = 0xB0
    /** Firmware >= 0.3.0 only; after ACCEPTED the device ends the session itself (10.8). */
    const val START_UPDATE = 0xB1
    /** Pump priming (10.9): firmware >= 0.3.3 only (8.4, `running_version` from GET_UPDATE_STATUS). */
    const val PRIME_START = 0xB2
    const val PRIME_STOP = 0xB3
    /** Status read that also renews the priming lease (sent once per second while running). */
    const val PRIME_STATUS = 0xB4
}

/** One authenticated session as the operations see it (bound to one connection generation). */
interface LiveSession {
    val generation: Long
    val isOpen: Boolean
    /** The device's public Info record read when this connection was set up (null = unknown). */
    val info: BleProtocol.Info? get() = null
    suspend fun request(type: Int, fields: Map<String, Any>, retries: Int): RequestResult
}

/** The process-wide session owner as the repository sees it; production: [ManagerSessionPort]. */
interface SessionPort {
    val state: StateFlow<SessionState>
    val events: SharedFlow<DeviceEvent>
    /** The live authenticated session, if any. */
    fun live(): LiveSession?
    /** Opens [request]; null when authenticated, else why not. */
    suspend fun open(address: String, request: OpenRequest): SessionFailure?
    suspend fun end(graceful: Boolean)
}

/** [SessionPort] over the M1.10e.2b [DeviceSessionManager]. */
class ManagerSessionPort(private val manager: DeviceSessionManager) : SessionPort {
    private class Live(val s: SecureSession) : LiveSession {
        override val generation: Long get() = s.generation
        override val isOpen: Boolean get() = s.isOpen
        override val info: BleProtocol.Info? get() = s.info
        override suspend fun request(type: Int, fields: Map<String, Any>, retries: Int) = s.request(type, fields, retries)
        override fun equals(other: Any?) = other is Live && other.s === s
        override fun hashCode() = System.identityHashCode(s)
    }

    override val state: StateFlow<SessionState> get() = manager.state
    override val events: SharedFlow<DeviceEvent> get() = manager.events
    override fun live(): LiveSession? = manager.session?.let { Live(it) }
    override suspend fun open(address: String, request: OpenRequest): SessionFailure? = when (val r = manager.open(address, request)) {
        is OpenResult.Opened -> null
        is OpenResult.Failed -> r.failure
    }
    override suspend fun end(graceful: Boolean) = manager.end(graceful)
}

/** Typed outcome of one device operation (docs/ANDROID_BLE_DESIGN.md 2.2, 5, 14.3). */
sealed interface OpResult<out T> {
    data class Ok<T>(val value: T) : OpResult<T>

    /** The device refused; [error] says what (if anything) happened (BLE_PROTOCOL.md 10.1). */
    data class Rejected(val error: DeviceError) : OpResult<Nothing>

    /** The app refused before sending anything (validation, unresolved operation, untrusted time, persistence failure …). */
    data class Refused(val reason: LocalRefusal) : OpResult<Nothing>

    /** Provably never reached the device (no live session, queue full, session ended before sending). */
    data class NotSent(val reason: String) : OpResult<Nothing>

    /**
     * May have been executed: never a failure. Journaled operations are resolved by read-back at the
     * next session ([journalId] identifies the row); others are re-read by the caller.
     */
    data class Uncertain(val reason: String, val journalId: Long? = null) : OpResult<Nothing>
}

/** Why the app refused an operation without sending it. */
sealed interface LocalRefusal {
    /** Domain and device validation (the device's limits checked before sending). */
    data class Invalid(val problems: Set<DeviceValidation>) : LocalRefusal

    /** An earlier operation of this kind/target is unresolved (11.4, 11.6: no stacking, no duplicate intents). */
    data class Unresolved(val journalId: Long) : LocalRefusal

    /** The same operation (kind and target) is being processed right now (11.6: duplicate intents); nothing sent. */
    data object InProgress : LocalRefusal

    /** No phone time with a defensible uncertainty ≤ 2,000 ms (E13 / D6): SET_TIME is never sent. */
    data object TimeUntrusted : LocalRefusal

    /** A durable write that must precede the request failed; nothing was sent (D4). */
    data class PersistenceFailed(val what: String) : LocalRefusal

    /** The session changed or closed between the read that authorizes the mutation and the mutation (D5). */
    data object SessionChanged : LocalRefusal

    /** The operation needs a value the device did not provide in this session. */
    data class Unavailable(val what: String) : LocalRefusal
}

/** Validation problems checked before sending (domain rules plus the device's wire limits, D2). */
enum class DeviceValidation {
    // the existing domain rules (ScheduleValidator)
    AMOUNT_NOT_FINITE, AMOUNT_NOT_POSITIVE, AMOUNT_TOO_PRECISE, AMOUNT_OUT_OF_RANGE, TIME_NONEXISTENT,
    INTERVAL_TOO_SMALL, NO_WEEKDAYS, END_BEFORE_START,
    // device limits (BLE_PROTOCOL.md 7.5, 10.3)
    AMOUNT_ABOVE_U32, NAME_TOO_LONG, NAME_CONTROL_CHARACTER, DATE_OUT_OF_RANGE, ZONE_NOT_SUPPORTED, INTERVAL_TOO_LARGE,
    // other operations
    REMAINING_ABOVE_CAPACITY, CAPACITY_UNKNOWN, VOLUME_INVALID,
    // REFILL (firmware: 0 < remaining <= capacity, capacity a u32 in µl; an empty reservoir is not accepted yet)
    REMAINING_NOT_POSITIVE, CAPACITY_INVALID, SLOT_INVALID, NAME_HINT_TOO_LONG, CONFIG_MASK_INVALID,
}

/**
 * Sends typed requests on a given live session. The manager-side [scope] owns every request once it
 * is submitted (D3): a caller that is cancelled stops waiting, but the request still completes and
 * [onOutcome] (journal resolution, cache update) still runs. The session's own fail-closed rules
 * (M1.10e.2b) are unchanged: an unusable session closes and the result is Uncertain.
 */
class DeviceOperations(private val scope: CoroutineScope) {
    /** Retries per the retry table (docs/ANDROID_BLE_DESIGN.md 5.1): same txn, identical body. */
    suspend fun <T> call(
        session: LiveSession,
        type: Int,
        fields: Map<String, Any> = emptyMap(),
        retries: Int = 2,
        decode: (Map<String, Any>) -> T,
        onOutcome: suspend (OpResult<T>) -> Unit = {},
    ): OpResult<T> = scope.async {
        val r = map(session.request(type, fields, retries), decode)
        onOutcome(r)
        r
    }.await()

    companion object {
        fun <T> map(r: RequestResult, decode: (Map<String, Any>) -> T): OpResult<T> = when (r) {
            is RequestResult.Ok -> try {
                OpResult.Ok(decode(r.fields))
            } catch (e: RuntimeException) {
                // the codec accepted it, so this is an app/firmware mismatch: never a confirmed result
                OpResult.Uncertain("unexpected response: ${e.message}")
            }
            is RequestResult.DeviceError -> OpResult.Rejected(DeviceError(DeviceErrorCode.of(r.code), r.detail, r.aux))
            is RequestResult.NotSent -> OpResult.NotSent(r.reason)
            RequestResult.Busy -> OpResult.NotSent("busy")
            is RequestResult.Uncertain -> OpResult.Uncertain(r.reason)
        }
    }
}
