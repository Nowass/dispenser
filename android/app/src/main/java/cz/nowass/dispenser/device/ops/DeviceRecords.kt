package cz.nowass.dispenser.device.ops

import cz.nowass.dispenser.ble.protocol.BleDate

/**
 * Typed views of the protocol records (docs/BLE_PROTOCOL.md 5, 7). Pure Kotlin; decoded from the
 * field maps of the existing `BleProtocol` codec, which has already validated every range, so the
 * mapping here only converts types. Integers arrive as Long (U8..U32, I64) or ULong (U64).
 */

/** Device error codes (BLE_PROTOCOL.md 5, 10.1); [UNKNOWN] for a value this app does not know. */
enum class DeviceErrorCode(val wire: Int) {
    MALFORMED(1), UNSUPPORTED_VERSION(2), UNKNOWN_TYPE(3), UNAUTHENTICATED(4), FORBIDDEN(5), BUSY(6),
    REFUSED(7), LIMIT(8), NOT_COVERED(9), INVALID(10), CONFLICT(11), NOT_FOUND(12), STORAGE(13),
    DEGRADED(14), PUMP_FAILED(15), SESSION_EXPIRED(16), DUPLICATE(17), HANDSHAKE_FAILED(18),
    SLOTS_FULL(19), UNAVAILABLE(20), UNKNOWN(-1);

    companion object {
        fun of(wire: Int): DeviceErrorCode = entries.firstOrNull { it.wire == wire } ?: UNKNOWN
    }
}

/** A refusal decided by the device: nothing beyond what [code] documents happened. */
data class DeviceError(val code: DeviceErrorCode, val detail: Long, val aux: Long)

enum class Readiness(val wire: Int) {
    READY(1), SERVICE_REQUIRED(2);

    companion object {
        fun of(wire: Int): Readiness? = entries.firstOrNull { it.wire == wire }
    }
}

enum class ClockTrust { UNKNOWN, SYNCED, HOLDOVER }

/** Status (7.2). [manualFloor] is the authenticated slot's next acceptable manual-dose `seq`. */
data class StatusRecord(
    val readiness: Readiness?,
    val dosingFlags: Int,
    val gates: Long,
    val pumpProblems: Long,
    val capacityUl: Long,
    val remainingUl: Long,
    val lowPercent: Int,
    val estimateUncertain: Boolean,
    val clockTrust: ClockTrust,
    val clockReason: Int,
    val deviceUtc: Long,
    val nextDueAt: Long?,
    val pendingNotes: Int,
    val highestSeverity: Int,
    val storageOk: Boolean,
    val sessionSecondsLeft: Int,
    val batteryPercent: Int?,
    val manualFloor: Long,
)

/** Diagnostics (7.3). */
data class DiagnosticsRecord(
    val notifyStorageOk: Boolean,
    val txDiag: Int,
    val txDiagStatus: Int,
    val rejectionsUnread: Int,
    val rejectionsTotal: Int,
    val clockTrust: ClockTrust,
    val clockReason: Int,
    val lastSyncUtc: Long,
    val lastWakeReason: Int,
)

/** Config (7.4). */
data class ConfigRecord(
    val revision: Long,
    val reservoirCapacityUl: Long,
    val reservoirRemainingUl: Long,
    val lowStockPercent: Int,
    val timeSyncIntervalH: Int,
    val notifyMask: Int,
    val pumpFlowUlPerS: Long,
    val calibratedAt: Long,
    val maxDoseUl: Long,
    val maxDailyUl: Long,
    val maxRuntimeMs: Long,
    val minIntervalS: Long,
    val hwMaxRuntimeMs: Long,
) {
    companion object {
        const val F_CAPACITY = 1 shl 0
        const val F_LOW_STOCK = 1 shl 1
        const val F_SYNC_INTERVAL = 1 shl 2
        const val F_NOTIFY = 1 shl 3
        const val F_FLOW = 1 shl 4
        const val F_CALIBRATED_AT = 1 shl 5
        const val F_MAX_DOSE = 1 shl 6
        const val F_MAX_DAILY = 1 shl 7
        const val F_MAX_RUNTIME = 1 shl 8
        const val F_MIN_INTERVAL = 1 shl 9
        const val F_ALL = 0x03FF
    }
}

/** Recurrence kinds of the Schedule record (7.5; firmware DSP_REC_*). */
enum class ScheduleKind(val wire: Int) { ONCE(0), DAILY(1), WEEKLY(2) }

/** Schedule (7.5), exactly as on the wire (names as UTF-8 bytes are kept as a String here). */
data class ScheduleRecord(
    val id: String,
    val revision: Long,
    val enabled: Boolean,
    val amountUl: Long,
    val startDate: BleDate,
    val hour: Int,
    val minute: Int,
    val kind: ScheduleKind,
    val interval: Long,
    /** bit 0 Monday … bit 6 Sunday. */
    val weekdayMask: Int,
    val endDate: BleDate?,
    val name: String,
)

data class ScheduleListPage(val setRevision: Long, val total: Int, val page: Int, val items: List<Pair<String, Long>>)

data class PhoneRecord(val slot: Int, val name: String, val enrolledAt: Long, val lastSeen: Long, val isSelf: Boolean)

/** INVITE_CREATE response: the token is secret (docs/ANDROID_BLE_DESIGN.md 7.1); the caller wipes it. */
class InviteRecord(val token: ByteArray, val expiresS: Int)

/** MANUAL_DOSE response (rare: the device normally suspends BLE for the pump and never answers). */
data class ManualDoseRecord(val outcome: Int, val volumeUl: Long, val runtimeMs: Long, val status: StatusRecord) {
    val pumpFailed: Boolean get() = outcome == 1
}

/** GET_RESULT `state` (BLE_PROTOCOL.md 10.4). */
enum class ManualResultState(val wire: Int) {
    NOT_DECIDED(0), PENDING(1), DONE(2), FAILED(3), INTERRUPTED(4), REFUSED(5), ALREADY_DECIDED_RESULT_UNAVAILABLE(6);

    companion object {
        fun of(wire: Int): ManualResultState? = entries.firstOrNull { it.wire == wire }
    }
}

data class ManualResultRecord(val state: ManualResultState, val detail: Long)

/** SET_TIME `decision` (10.5). */
data class TimeDecisionRecord(val decision: Int, val offsetS: Long) {
    val accepted: Boolean get() = decision == 0
}

/** Field-map ⇄ record conversion (the maps of `BleProtocol.decodeBody` / `encodeBody`). */
object Records {
    private fun Map<String, Any>.long(k: String): Long = when (val v = get(k)) {
        is Long -> v
        is Int -> v.toLong()
        is ULong -> v.toLong()
        else -> error("field $k: $v")
    }
    private fun Map<String, Any>.int(k: String): Int = long(k).toInt()
    private fun Map<String, Any>.bool(k: String): Boolean = get(k) as Boolean
    private fun Map<String, Any>.str(k: String): String = (get(k) as ByteArray).toString(Charsets.UTF_8)
    @Suppress("UNCHECKED_CAST")
    private fun Map<String, Any>.rec(k: String): Map<String, Any> = get(k) as Map<String, Any>

    private fun trust(v: Int) = when (v) {
        1 -> ClockTrust.SYNCED
        2 -> ClockTrust.HOLDOVER
        else -> ClockTrust.UNKNOWN
    }

    fun status(body: Map<String, Any>): StatusRecord = statusRecord(body.rec("status"))

    fun statusRecord(m: Map<String, Any>) = StatusRecord(
        readiness = Readiness.of(m.int("readiness")), dosingFlags = m.int("dosing_flags"), gates = m.long("gates"),
        pumpProblems = m.long("pump_problems"), capacityUl = m.long("capacity_ul"), remainingUl = m.long("remaining_ul"),
        lowPercent = m.int("low_percent"), estimateUncertain = m.bool("estimate_uncertain"), clockTrust = trust(m.int("clock_trust")),
        clockReason = m.int("clock_reason"), deviceUtc = m.long("device_utc"),
        nextDueAt = if (m.bool("next_due_known")) m.long("next_due_at") else null, pendingNotes = m.int("pending_notes"),
        highestSeverity = m.int("highest_severity"), storageOk = m.bool("storage_ok"), sessionSecondsLeft = m.int("session_seconds_left"),
        batteryPercent = m.int("battery_percent").takeIf { it in 0..100 }, manualFloor = m.long("manual_floor"),
    )

    fun diagnostics(body: Map<String, Any>): DiagnosticsRecord = body.rec("diag").let { m ->
        DiagnosticsRecord(m.bool("notify_storage_ok"), m.int("tx_diag"), m.int("tx_diag_status"), m.int("rejections_unread"),
            m.int("rejections_total"), trust(m.int("clock_trust")), m.int("clock_reason"), m.long("last_sync_utc"), m.int("last_wake_reason"))
    }

    fun config(body: Map<String, Any>): ConfigRecord = configRecord(body.rec("config"))

    fun configRecord(m: Map<String, Any>) = ConfigRecord(
        m.long("revision"), m.long("reservoir_capacity_ul"), m.long("reservoir_remaining_ul"), m.int("low_stock_percent"),
        m.int("time_sync_interval_h"), m.int("notify_mask"), m.long("pump_flow_ul_per_s"), m.long("calibrated_at"),
        m.long("max_dose_ul"), m.long("max_daily_ul"), m.long("max_runtime_ms"), m.long("min_interval_s"), m.long("hw_max_runtime_ms"),
    )

    fun configFields(c: ConfigRecord): Map<String, Any> = mapOf(
        "revision" to c.revision, "reservoir_capacity_ul" to c.reservoirCapacityUl, "reservoir_remaining_ul" to c.reservoirRemainingUl,
        "low_stock_percent" to c.lowStockPercent.toLong(), "time_sync_interval_h" to c.timeSyncIntervalH.toLong(),
        "notify_mask" to c.notifyMask.toLong(), "pump_flow_ul_per_s" to c.pumpFlowUlPerS, "calibrated_at" to c.calibratedAt,
        "max_dose_ul" to c.maxDoseUl, "max_daily_ul" to c.maxDailyUl, "max_runtime_ms" to c.maxRuntimeMs,
        "min_interval_s" to c.minIntervalS, "hw_max_runtime_ms" to c.hwMaxRuntimeMs,
    )

    fun schedule(body: Map<String, Any>): ScheduleRecord = scheduleRecord(body.rec("schedule"))

    fun scheduleRecord(m: Map<String, Any>) = ScheduleRecord(
        id = m.str("id"), revision = m.long("revision"), enabled = m.bool("enabled"), amountUl = m.long("amount_ul"),
        startDate = m["start_date"] as BleDate, hour = m.int("hour"), minute = m.int("minute"),
        kind = ScheduleKind.entries.first { it.wire == m.int("kind") }, interval = m.long("interval"),
        weekdayMask = m.int("weekday_mask"), endDate = if (m.bool("has_end")) m["end_date"] as BleDate else null, name = m.str("name"),
    )

    fun scheduleFields(s: ScheduleRecord): Map<String, Any> = mapOf(
        "id" to s.id.toByteArray(Charsets.UTF_8), "revision" to s.revision, "enabled" to s.enabled, "amount_ul" to s.amountUl,
        "start_date" to s.startDate, "hour" to s.hour.toLong(), "minute" to s.minute.toLong(), "kind" to s.kind.wire.toLong(),
        "interval" to s.interval, "weekday_mask" to s.weekdayMask.toLong(), "has_end" to (s.endDate != null),
        "end_date" to (s.endDate ?: BleDate(0, 0, 0)), "name" to s.name.toByteArray(Charsets.UTF_8),
    )

    @Suppress("UNCHECKED_CAST")
    fun schedulePage(body: Map<String, Any>) = ScheduleListPage(body.long("set_revision"), body.int("total"), body.int("page"),
        (body["items"] as List<Map<String, Any>>).map { it.str("id") to it.long("revision") })

    @Suppress("UNCHECKED_CAST")
    fun phones(body: Map<String, Any>): List<PhoneRecord> = (body["items"] as List<Map<String, Any>>).map {
        PhoneRecord(it.int("slot"), it.str("name"), it.long("enrolled_at"), it.long("last_seen"), it.bool("is_self"))
    }

    /**
     * INVITE_CREATE response. The token is copied into the record and the decoded field array is zeroized
     * at once (security review F7, invite only): the record is the only codec-level copy left, and the
     * invitation controller clears it after its last use. The decrypted frame buffer the codec decoded
     * from is not zeroized here (general hardening item, docs/ANDROID_BLE_DESIGN.md 18.5).
     */
    fun invite(body: Map<String, Any>): InviteRecord {
        val decoded = body["token"] as ByteArray
        return try {
            InviteRecord(decoded.copyOf(), body.int("expires_s"))
        } finally {
            decoded.fill(0)
        }
    }

    fun manualDose(body: Map<String, Any>) =
        ManualDoseRecord(body.int("outcome"), body.long("volume_ul"), body.long("runtime_ms"), statusRecord(body.rec("status")))

    fun manualResult(body: Map<String, Any>) = ManualResultRecord(checkNotNull(ManualResultState.of(body.int("state"))), body.long("detail"))

    fun timeDecision(body: Map<String, Any>) = TimeDecisionRecord(body.int("decision"), body.long("offset_s"))

    fun setRevision(body: Map<String, Any>): Long = body.long("set_revision")
    fun revision(body: Map<String, Any>): Long = body.long("revision")
}
