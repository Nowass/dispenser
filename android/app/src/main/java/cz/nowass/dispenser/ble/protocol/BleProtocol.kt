package cz.nowass.dispenser.ble.protocol

/*
 * BLE protocol v1 codec (milestone M1.10a), normative specification docs/BLE_PROTOCOL.md,
 * shared vectors test-vectors/ble_protocol_v1.txt (also consumed by the firmware codec
 * dsp_bleproto). Pure Kotlin: no Android, no Bluetooth, no I/O; not yet used by the app
 * (BleDispenserRepository, M1.10e). Decoded bodies are schema-ordered maps; typed wrappers come
 * with the repository integration.
 */

/** Codec results; [wire] is the reason name used by the shared vectors. */
enum class BleResult(val wire: String) {
    OK("ok"),
    TRUNCATED("truncated"),
    LENGTH_MISMATCH("length_mismatch"),
    BAD_MAJOR("bad_major"),
    BAD_FLAGS("bad_flags"),
    UNKNOWN_TYPE("unknown_type"),
    NOT_A_REQUEST("not_a_request"),
    TRAILING("trailing"),
    BAD_BOOL("bad_bool"),
    BAD_ENUM("bad_enum"),
    BAD_RESERVED("bad_reserved"),
    BAD_DATE("bad_date"),
    BAD_STRING("bad_string"),
    TOO_LARGE("too_large"),
    BAD_ENCRYPTED("bad_encrypted"),
    BAD_MAGIC("bad_magic"),
    FRAG_NO_FIRST("frag_no_first"),
    FRAG_INDEX("frag_index"),
    FRAG_EMPTY("frag_empty"),
    FRAG_TIMEOUT("frag_timeout"),
    FRAG_OVERFLOW("frag_overflow"),
}

class BleCodecException(val result: BleResult, message: String = result.wire) : Exception(message)

data class BleDate(val year: Int, val month: Int, val day: Int) {
    override fun toString(): String = "%04d-%02d-%02d".format(year, month, day)
}

data class BleHeader(
    val major: Int,
    val minor: Int,
    val type: Int,
    val flags: Int,
    val txn: Int,
    val len: Int,
)

/** A frame: header plus either decoded plain [fields] or the opaque encrypted [body]. */
class BleFrame(val header: BleHeader, val fields: Map<String, Any>?, val body: ByteArray)

object BleProtocol {
    const val MAJOR = 1
    const val MINOR = 0
    const val HEADER_LEN = 8
    const val FRAME_MAX = 520
    const val PLAIN_BODY_MAX = 496
    const val TAG_LEN = 16
    const val FRAG_MAX = 64
    const val ATT_MTU_MIN = 23
    const val REASM_TIMEOUT_MS = 2000L
    const val INFO_LEN = 32
    const val ADVERT_LEN = 4
    const val FACTORY_RESET_CONFIRM = 0x54455352L

    const val F_RESPONSE = 1
    const val F_ERROR = 2
    const val F_EVENT = 4
    const val F_ENCRYPTED = 8

    enum class SecurityClass { PLAIN, ENCRYPTED, EITHER }

    // ---- Schema (transcribed from docs/BLE_PROTOCOL.md sections 5 and 7) ---------------------

    enum class StrKind { TEXT, ID, RAW, WPA }

    sealed class Kind {
        object U8 : Kind()
        object U16 : Kind()
        object U32 : Kind()
        object U64 : Kind()
        object I64 : Kind()
        object Bool : Kind()
        data class Enum8(val valid: Set<Int>) : Kind()
        data class Mask(val bytes: Int, val allowed: Long) : Kind()
        data class Const32(val value: Long) : Kind()
        data class Date(val zeroUnlessTrue: String? = null) : Kind()
        data class Reserved(val n: Int) : Kind()
        data class Bytes(val n: Int) : Kind()
        object Blob : Kind()
        data class Str(val min: Int, val max: Int, val kind: StrKind) : Kind()
        data class Record(val fields: List<Field>) : Kind()
        data class ListOf(val max: Int, val fields: List<Field>) : Kind()
    }

    data class Field(val name: String, val kind: Kind)

    private fun f(name: String, kind: Kind) = Field(name, kind)
    private fun range(a: Int, b: Int) = Kind.Enum8((a..b).toSet())

    val STATUS = listOf(
        f("readiness", Kind.U8), f("dosing_flags", Kind.U8), f("gates", Kind.U32),
        f("pump_problems", Kind.U32), f("capacity_ul", Kind.U32), f("remaining_ul", Kind.U32),
        f("low_percent", Kind.U8), f("estimate_uncertain", Kind.Bool), f("clock_trust", range(0, 2)),
        f("clock_reason", Kind.U8), f("device_utc", Kind.I64), f("next_due_known", Kind.Bool),
        f("next_due_at", Kind.I64), f("pending_notes", Kind.U8), f("highest_severity", range(0, 2)),
        f("storage_ok", Kind.Bool), f("session_seconds_left", Kind.U16),
        f("battery_percent", Kind.Enum8((0..100).toSet() + 255)), f("manual_floor", Kind.U64),
        f("", Kind.Reserved(5)),
    )
    val DIAGNOSTICS = listOf(
        f("notify_storage_ok", Kind.Bool), f("tx_diag", Kind.U8), f("tx_diag_status", Kind.U16),
        f("rejections_unread", Kind.U8), f("rejections_total", Kind.U16), f("clock_trust", range(0, 2)),
        f("clock_reason", Kind.U8), f("last_sync_utc", Kind.I64), f("last_wake_reason", Kind.U8),
        f("", Kind.Reserved(6)),
    )
    val CONFIG = listOf(
        f("revision", Kind.U32), f("reservoir_capacity_ul", Kind.U32), f("reservoir_remaining_ul", Kind.U32),
        f("low_stock_percent", Kind.U8), f("time_sync_interval_h", Kind.U16), f("notify_mask", Kind.U8),
        f("pump_flow_ul_per_s", Kind.U32), f("calibrated_at", Kind.I64), f("max_dose_ul", Kind.U32),
        f("max_daily_ul", Kind.U32), f("max_runtime_ms", Kind.U32), f("min_interval_s", Kind.U32),
        f("hw_max_runtime_ms", Kind.U32),
    )
    val SCHEDULE = listOf(
        f("id", Kind.Str(1, 64, StrKind.ID)), f("revision", Kind.U32), f("enabled", Kind.Bool),
        f("amount_ul", Kind.U32), f("start_date", Kind.Date()), f("hour", range(0, 23)),
        f("minute", range(0, 59)), f("kind", range(0, 2)), f("interval", Kind.U32),
        f("weekday_mask", Kind.Mask(1, 0x7F)), f("has_end", Kind.Bool), f("end_date", Kind.Date("has_end")),
        f("name", Kind.Str(0, 32, StrKind.TEXT)),
    )
    val ERROR = listOf(f("code", range(1, 20)), f("detail", Kind.U32), f("aux", Kind.U32))

    data class TypeSpec(
        val type: Int,
        val name: String,
        val cls: SecurityClass,
        val request: List<Field>?,
        val response: List<Field>?,
        val event: List<Field>?,
    )

    private val E = emptyList<Field>()
    private val SLOT = listOf(f("slot", range(0, 3)))
    private val STATUS_BODY = listOf(f("status", Kind.Record(STATUS)))
    private val ID = listOf(f("id", Kind.Str(1, 64, StrKind.ID)))

    val TYPES: Map<Int, TypeSpec> = listOf(
        TypeSpec(0x01, "HELLO", SecurityClass.PLAIN,
            listOf(f("proto_min", Kind.U8), f("proto_max", Kind.U8)),
            listOf(f("proto", Kind.U8), f("claim_state", range(0, 1)), f("flags", Kind.Mask(1, 0x03)),
                f("caps", Kind.Mask(4, 0x1F)), f("session_id", Kind.U32), f("device_id_hash", Kind.Bytes(8))), null),
        TypeSpec(0x02, "HS", SecurityClass.PLAIN,
            listOf(f("purpose", range(1, 4)), f("msg_index", range(0, 2)), f("noise", Kind.Blob)),
            listOf(f("msg_index", range(0, 2)), f("noise", Kind.Blob)), null),
        TypeSpec(0x08, "ENROLL_RESULT", SecurityClass.ENCRYPTED, null, SLOT, null),
        TypeSpec(0x10, "GET_STATUS", SecurityClass.ENCRYPTED, E, STATUS_BODY, null),
        TypeSpec(0x11, "GET_DIAGNOSTICS", SecurityClass.ENCRYPTED, E, listOf(f("diag", Kind.Record(DIAGNOSTICS))), null),
        TypeSpec(0x12, "ACK_REJECTIONS", SecurityClass.ENCRYPTED, E, E, null),
        TypeSpec(0x20, "GET_CONFIG", SecurityClass.ENCRYPTED, E, listOf(f("config", Kind.Record(CONFIG))), null),
        TypeSpec(0x21, "SET_CONFIG", SecurityClass.ENCRYPTED,
            listOf(f("expected_revision", Kind.U32), f("field_mask", Kind.Mask(2, 0x03FF)), f("config", Kind.Record(CONFIG))),
            listOf(f("revision", Kind.U32)), null),
        TypeSpec(0x30, "LIST_SCHEDULES", SecurityClass.ENCRYPTED, listOf(f("page", Kind.U8)),
            listOf(f("set_revision", Kind.U32), f("total", Kind.U8), f("page", Kind.U8),
                f("items", Kind.ListOf(4, listOf(f("id", Kind.Str(1, 64, StrKind.ID)), f("revision", Kind.U32))))), null),
        TypeSpec(0x31, "GET_SCHEDULE", SecurityClass.ENCRYPTED, ID, listOf(f("schedule", Kind.Record(SCHEDULE))), null),
        TypeSpec(0x32, "CREATE_SCHEDULE", SecurityClass.ENCRYPTED, listOf(f("schedule", Kind.Record(SCHEDULE))),
            listOf(f("set_revision", Kind.U32)), null),
        TypeSpec(0x33, "UPDATE_SCHEDULE", SecurityClass.ENCRYPTED,
            listOf(f("expected_revision", Kind.U32), f("schedule", Kind.Record(SCHEDULE))),
            listOf(f("revision", Kind.U32), f("set_revision", Kind.U32)), null),
        TypeSpec(0x34, "DELETE_SCHEDULE", SecurityClass.ENCRYPTED,
            listOf(f("id", Kind.Str(1, 64, StrKind.ID)), f("expected_revision", Kind.U32)),
            listOf(f("set_revision", Kind.U32)), null),
        TypeSpec(0x40, "MANUAL_DOSE", SecurityClass.ENCRYPTED, listOf(f("seq", Kind.U64), f("volume_ul", Kind.U32)),
            listOf(f("outcome", range(0, 1)), f("volume_ul", Kind.U32), f("runtime_ms", Kind.U32),
                f("status", Kind.Record(STATUS))), null),
        TypeSpec(0x41, "GET_RESULT", SecurityClass.ENCRYPTED, listOf(f("seq", Kind.U64)),
            listOf(f("state", range(0, 6)), f("detail", Kind.U32)), null),
        TypeSpec(0x42, "REFILL", SecurityClass.ENCRYPTED, listOf(f("capacity_ul", Kind.U32), f("remaining_ul", Kind.U32)),
            STATUS_BODY, null),
        TypeSpec(0x43, "SET_READINESS", SecurityClass.ENCRYPTED, listOf(f("readiness", range(1, 2))), STATUS_BODY, null),
        TypeSpec(0x50, "SET_TIME", SecurityClass.ENCRYPTED,
            listOf(f("phone_utc_ms", Kind.I64), f("uncertainty_ms", Kind.U32)),
            listOf(f("decision", range(0, 4)), f("offset_s", Kind.I64)), null),
        TypeSpec(0x60, "WIFI_SET", SecurityClass.ENCRYPTED,
            listOf(f("ssid", Kind.Str(1, 32, StrKind.RAW)), f("passphrase", Kind.Str(0, 63, StrKind.WPA))), E, null),
        TypeSpec(0x61, "WIFI_GET", SecurityClass.ENCRYPTED, E,
            listOf(f("ssid", Kind.Str(0, 32, StrKind.RAW)), f("passphrase_set", Kind.Bool), f("last_result", range(0, 5))), null),
        TypeSpec(0x62, "WIFI_CLEAR", SecurityClass.ENCRYPTED, E, E, null),
        TypeSpec(0x70, "PHONES_LIST", SecurityClass.ENCRYPTED, E,
            listOf(f("items", Kind.ListOf(4, listOf(f("slot", range(0, 3)), f("name", Kind.Str(0, 24, StrKind.TEXT)),
                f("enrolled_at", Kind.I64), f("last_seen", Kind.I64), f("is_self", Kind.Bool))))), null),
        TypeSpec(0x71, "PHONE_REMOVE", SecurityClass.ENCRYPTED, SLOT, E, null),
        TypeSpec(0x72, "INVITE_CREATE", SecurityClass.ENCRYPTED, listOf(f("name_hint", Kind.Str(0, 24, StrKind.TEXT))),
            listOf(f("token", Kind.Bytes(16)), f("expires_s", Kind.U16)), null),
        TypeSpec(0x73, "INVITE_CANCEL", SecurityClass.ENCRYPTED, E, E, null),
        TypeSpec(0x74, "RECOVERY_CANCEL", SecurityClass.ENCRYPTED, E, E, null),
        TypeSpec(0x7D, "FACTORY_RESET", SecurityClass.ENCRYPTED, listOf(f("confirm", Kind.Const32(FACTORY_RESET_CONFIRM))), E, null),
        TypeSpec(0x7E, "END_SESSION", SecurityClass.EITHER, E, E, null),
        TypeSpec(0x80, "EV_SESSION_CLOSING", SecurityClass.ENCRYPTED, null, null,
            listOf(f("reason", range(1, 4)), f("seconds_left", Kind.U16))),
        TypeSpec(0x81, "EV_SUSPENDING_FOR_DOSE", SecurityClass.ENCRYPTED, null, null, listOf(f("seconds_until", Kind.U16))),
        TypeSpec(0x82, "EV_BUSY", SecurityClass.PLAIN, null, null, E),
        TypeSpec(0x83, "EV_CONFIRM_ON_DEVICE", SecurityClass.ENCRYPTED, null, null,
            listOf(f("action", range(1, 1)), f("seconds", Kind.U8))),
    ).associateBy { it.type }

    /** Layout of a body for type + flags, or null when that combination does not exist. */
    fun layoutFor(type: Int, flags: Int): List<Field>? {
        val t = TYPES[type] ?: return null
        return when (flags and (F_RESPONSE or F_ERROR or F_EVENT)) {
            0 -> t.request
            F_RESPONSE -> t.response
            F_RESPONSE or F_ERROR -> if (t.response != null) ERROR else null
            F_EVENT -> t.event
            else -> null
        }
    }

    // ---- Value rules ------------------------------------------------------------------------------

    private fun dateOk(d: BleDate) = d.year in 2000..2099 && d.month in 1..12 && d.day in 1..31
    private fun dateZero(d: BleDate) = d.year == 0 && d.month == 0 && d.day == 0

    /** Well-formed UTF-8 (RFC 3629), optionally without control characters. */
    fun utf8Ok(b: ByteArray, noControls: Boolean): Boolean {
        var i = 0
        while (i < b.size) {
            val c = b[i].toInt() and 0xFF
            if (c < 0x80) {
                if (noControls && (c < 0x20 || c == 0x7F)) return false
                i++
                continue
            }
            val (need, init, min) = when (c) {
                in 0xC2..0xDF -> Triple(1, c and 0x1F, 0x80)
                in 0xE0..0xEF -> Triple(2, c and 0x0F, 0x800)
                in 0xF0..0xF4 -> Triple(3, c and 0x07, 0x10000)
                else -> return false
            }
            if (b.size - i - 1 < need) return false
            var cp = init
            for (k in 1..need) {
                val cc = b[i + k].toInt() and 0xFF
                if (cc and 0xC0 != 0x80) return false
                cp = (cp shl 6) or (cc and 0x3F)
            }
            if (cp < min || cp > 0x10FFFF || cp in 0xD800..0xDFFF) return false
            i += need + 1
        }
        return true
    }

    private fun strOk(k: Kind.Str, s: ByteArray): Boolean {
        if (s.size < k.min || s.size > k.max) return false
        return when (k.kind) {
            StrKind.TEXT -> utf8Ok(s, true)
            StrKind.ID -> s.all { val c = it.toInt().toChar(); c in 'A'..'Z' || c in 'a'..'z' || c in '0'..'9' || c == '.' || c == '_' || c == '-' }
            StrKind.RAW -> true
            StrKind.WPA -> (s.isEmpty() || s.size >= 8) && s.all { it.toInt() in 0x20..0x7E }
        }
    }

    // ---- Decoding --------------------------------------------------------------------------------

    private class Reader(val b: ByteArray, val end: Int) {
        var pos = 0
        fun take(n: Int): Int {
            if (end - pos < n) throw BleCodecException(BleResult.TRUNCATED)
            val p = pos
            pos += n
            return p
        }
        fun le(n: Int): Long {
            val p = take(n)
            var v = 0L
            for (i in 0 until n) v = v or ((b[p + i].toLong() and 0xFF) shl (8 * i))
            return v
        }
    }

    private fun decFields(fields: List<Field>, r: Reader): Map<String, Any> {
        val out = LinkedHashMap<String, Any>()
        for (fd in fields) {
            when (val k = fd.kind) {
                Kind.U8 -> out[fd.name] = r.le(1)
                Kind.U16 -> out[fd.name] = r.le(2)
                Kind.U32 -> out[fd.name] = r.le(4)
                Kind.U64 -> out[fd.name] = r.le(8).toULong()
                Kind.I64 -> out[fd.name] = r.le(8)
                Kind.Bool -> {
                    val v = r.le(1)
                    if (v > 1) throw BleCodecException(BleResult.BAD_BOOL)
                    out[fd.name] = v == 1L
                }
                is Kind.Enum8 -> {
                    val v = r.le(1)
                    if (v.toInt() !in k.valid) throw BleCodecException(BleResult.BAD_ENUM)
                    out[fd.name] = v
                }
                is Kind.Mask -> {
                    val v = r.le(k.bytes)
                    if (v and k.allowed.inv() != 0L) throw BleCodecException(BleResult.BAD_ENUM)
                    out[fd.name] = v
                }
                is Kind.Const32 -> {
                    val v = r.le(4)
                    if (v != k.value) throw BleCodecException(BleResult.BAD_ENUM)
                    out[fd.name] = v
                }
                is Kind.Date -> {
                    val d = BleDate(r.le(2).toInt(), r.le(1).toInt(), r.le(1).toInt())
                    val mustBeZero = k.zeroUnlessTrue != null && out[k.zeroUnlessTrue] != true
                    if (if (mustBeZero) !dateZero(d) else !dateOk(d)) throw BleCodecException(BleResult.BAD_DATE)
                    out[fd.name] = d
                }
                is Kind.Reserved -> {
                    val p = r.take(k.n)
                    for (i in 0 until k.n) if (r.b[p + i].toInt() != 0) throw BleCodecException(BleResult.BAD_RESERVED)
                }
                is Kind.Bytes -> {
                    val p = r.take(k.n)
                    out[fd.name] = r.b.copyOfRange(p, p + k.n)
                }
                Kind.Blob -> {
                    val n = r.end - r.pos
                    val p = r.take(n)
                    out[fd.name] = r.b.copyOfRange(p, p + n)
                }
                is Kind.Str -> {
                    val n = r.le(1).toInt()
                    val p = r.take(n)
                    val s = r.b.copyOfRange(p, p + n)
                    if (!strOk(k, s)) throw BleCodecException(BleResult.BAD_STRING)
                    out[fd.name] = s
                }
                is Kind.Record -> out[fd.name] = decFields(k.fields, r)
                is Kind.ListOf -> {
                    val n = r.le(1).toInt()
                    if (n > k.max) throw BleCodecException(BleResult.BAD_ENUM)
                    out[fd.name] = List(n) { decFields(k.fields, r) }
                }
            }
        }
        return out
    }

    private fun checkFlags(type: Int, flags: Int): TypeSpec {
        if (flags and 0xF0 != 0) throw BleCodecException(BleResult.BAD_FLAGS)
        val f = flags and (F_RESPONSE or F_ERROR or F_EVENT)
        if (f != 0 && f != F_RESPONSE && f != (F_RESPONSE or F_ERROR) && f != F_EVENT) {
            throw BleCodecException(BleResult.BAD_FLAGS)
        }
        val t = TYPES[type] ?: throw BleCodecException(BleResult.UNKNOWN_TYPE)
        if (f == 0 && t.request == null) throw BleCodecException(BleResult.NOT_A_REQUEST)
        if ((f == F_RESPONSE || f == (F_RESPONSE or F_ERROR)) && t.response == null) throw BleCodecException(BleResult.BAD_FLAGS)
        if (f == F_EVENT && t.event == null) throw BleCodecException(BleResult.BAD_FLAGS)
        return t
    }

    /** Validates the header of a complete frame (see dsp_ble_parse_header). */
    fun parseHeader(frame: ByteArray): BleHeader {
        if (frame.size < HEADER_LEN) throw BleCodecException(BleResult.TRUNCATED)
        fun u(i: Int) = frame[i].toInt() and 0xFF
        val h = BleHeader(u(0), u(1), u(2), u(3), u(4) or (u(5) shl 8), u(6) or (u(7) shl 8))
        if (h.major != MAJOR) throw BleCodecException(BleResult.BAD_MAJOR)
        if (h.len != frame.size - HEADER_LEN) throw BleCodecException(BleResult.LENGTH_MISMATCH)
        if (frame.size > FRAME_MAX) throw BleCodecException(BleResult.TOO_LARGE)
        checkFlags(h.type, h.flags)
        if (h.flags and F_ENCRYPTED != 0) {
            if (h.len < TAG_LEN) throw BleCodecException(BleResult.BAD_ENCRYPTED)
        } else if (h.len > PLAIN_BODY_MAX) {
            throw BleCodecException(BleResult.TOO_LARGE)
        }
        return h
    }

    /** Decodes a plain body; trailing bytes only accepted when header.minor > myMinor. */
    fun decodeBody(h: BleHeader, body: ByteArray, myMinor: Int = MINOR): Map<String, Any> {
        if (body.size > PLAIN_BODY_MAX) throw BleCodecException(BleResult.TOO_LARGE)
        val layout = layoutFor(h.type, h.flags) ?: throw BleCodecException(BleResult.BAD_FLAGS)
        val r = Reader(body, body.size)
        val fields = decFields(layout, r)
        if (r.pos < body.size && h.minor <= myMinor) throw BleCodecException(BleResult.TRAILING)
        return fields
    }

    /** Decodes a frame; encrypted frames are returned with fields = null. */
    fun decode(frame: ByteArray, myMinor: Int = MINOR): BleFrame {
        val h = parseHeader(frame)
        val body = frame.copyOfRange(HEADER_LEN, frame.size)
        if (h.flags and F_ENCRYPTED != 0) return BleFrame(h, null, body)
        return BleFrame(h, decodeBody(h, body, myMinor), body)
    }

    // ---- Encoding --------------------------------------------------------------------------------

    private class Writer {
        val out = java.io.ByteArrayOutputStream()
        fun le(v: Long, n: Int) {
            for (i in 0 until n) out.write(((v ushr (8 * i)) and 0xFF).toInt())
        }
        fun bytes(b: ByteArray) = out.write(b, 0, b.size)
    }

    private fun num(v: Any?): Long = when (v) {
        is Long -> v
        is Int -> v.toLong()
        is ULong -> v.toLong()
        else -> throw IllegalArgumentException("not a number: $v")
    }

    @Suppress("UNCHECKED_CAST")
    private fun encFields(fields: List<Field>, values: Map<String, Any>, w: Writer) {
        for (fd in fields) {
            val v = values[fd.name]
            when (val k = fd.kind) {
                Kind.U8 -> w.le(num(v), 1)
                Kind.U16 -> w.le(num(v), 2)
                Kind.U32 -> w.le(num(v), 4)
                Kind.U64, Kind.I64 -> w.le(num(v), 8)
                Kind.Bool -> w.le(if (v == true) 1 else 0, 1)
                is Kind.Enum8 -> {
                    if (num(v).toInt() !in k.valid) throw BleCodecException(BleResult.BAD_ENUM)
                    w.le(num(v), 1)
                }
                is Kind.Mask -> {
                    if (num(v) and k.allowed.inv() != 0L) throw BleCodecException(BleResult.BAD_ENUM)
                    w.le(num(v), k.bytes)
                }
                is Kind.Const32 -> {
                    if (num(v) != k.value) throw BleCodecException(BleResult.BAD_ENUM)
                    w.le(num(v), 4)
                }
                is Kind.Date -> {
                    val d = v as BleDate
                    val mustBeZero = k.zeroUnlessTrue != null && values[k.zeroUnlessTrue] != true
                    if (if (mustBeZero) !dateZero(d) else !dateOk(d)) throw BleCodecException(BleResult.BAD_DATE)
                    w.le(d.year.toLong(), 2)
                    w.le(d.month.toLong(), 1)
                    w.le(d.day.toLong(), 1)
                }
                is Kind.Reserved -> w.bytes(ByteArray(k.n))
                is Kind.Bytes -> {
                    val b = v as ByteArray
                    require(b.size == k.n) { "${fd.name}: ${k.n} bytes expected" }
                    w.bytes(b)
                }
                Kind.Blob -> w.bytes(v as ByteArray)
                is Kind.Str -> {
                    val b = v as ByteArray
                    if (!strOk(k, b)) throw BleCodecException(BleResult.BAD_STRING)
                    w.le(b.size.toLong(), 1)
                    w.bytes(b)
                }
                is Kind.Record -> encFields(k.fields, v as Map<String, Any>, w)
                is Kind.ListOf -> {
                    val items = v as List<Map<String, Any>>
                    if (items.size > k.max) throw BleCodecException(BleResult.BAD_ENUM)
                    w.le(items.size.toLong(), 1)
                    for (item in items) encFields(k.fields, item, w)
                }
            }
        }
    }

    /** Encodes a plain body for [type]/[flags] (validating every rule). */
    fun encodeBody(type: Int, flags: Int, fields: Map<String, Any>): ByteArray {
        checkFlags(type, flags and F_ENCRYPTED.inv())
        val layout = layoutFor(type, flags) ?: throw BleCodecException(BleResult.BAD_FLAGS)
        val w = Writer()
        encFields(layout, fields, w)
        val body = w.out.toByteArray()
        if (body.size > PLAIN_BODY_MAX) throw BleCodecException(BleResult.TOO_LARGE)
        return body
    }

    /** Encodes a plain frame. */
    fun encode(type: Int, flags: Int, txn: Int, fields: Map<String, Any>, minor: Int = MINOR): ByteArray {
        require(flags and F_ENCRYPTED == 0) { "encrypted frames are built by the session layer" }
        require(txn in 0..0xFFFF)
        val body = encodeBody(type, flags, fields)
        val w = Writer()
        w.le(MAJOR.toLong(), 1)
        w.le(minor.toLong(), 1)
        w.le(type.toLong(), 1)
        w.le(flags.toLong(), 1)
        w.le(txn.toLong(), 2)
        w.le(body.size.toLong(), 2)
        w.bytes(body)
        return w.out.toByteArray()
    }

    // ---- Negotiation, Info, advertising ---------------------------------------------------------

    /** Highest common major, or null. */
    fun negotiate(devMin: Int, devMax: Int, cliMin: Int, cliMax: Int): Int? {
        if (devMin < 1 || cliMin < 1 || devMin > devMax || cliMin > cliMax) return null
        val lo = maxOf(devMin, cliMin)
        val hi = minOf(devMax, cliMax)
        return if (hi >= lo) hi else null
    }

    data class Info(
        val protoMin: Int, val protoMax: Int, val claimState: Int, val flags: Int, val caps: Long,
        val fwMajor: Int, val fwMinor: Int, val fwPatch: Int, val deviceTag: Int,
    )

    private val MAGIC = byteArrayOf('D'.code.toByte(), 'S'.code.toByte(), 'P'.code.toByte(), '2'.code.toByte())

    fun decodeInfo(b: ByteArray): Info {
        if (b.size < INFO_LEN) throw BleCodecException(BleResult.TRUNCATED)
        if (b.size > INFO_LEN) throw BleCodecException(BleResult.LENGTH_MISMATCH)
        if (!b.copyOfRange(0, 4).contentEquals(MAGIC)) throw BleCodecException(BleResult.BAD_MAGIC)
        for (i in 18 until INFO_LEN) if (b[i].toInt() != 0) throw BleCodecException(BleResult.BAD_RESERVED)
        val r = Reader(b, b.size).also { it.pos = 4 }
        val info = Info(r.le(1).toInt(), r.le(1).toInt(), r.le(1).toInt(), r.le(1).toInt(), r.le(4),
            r.le(1).toInt(), r.le(1).toInt(), r.le(2).toInt(), r.le(2).toInt())
        if (info.claimState > 1 || info.flags and 0x03.inv() != 0 || info.caps and 0x1FL.inv() != 0L) {
            throw BleCodecException(BleResult.BAD_ENUM)
        }
        return info
    }

    fun encodeInfo(i: Info): ByteArray {
        if (i.claimState > 1 || i.flags and 0x03.inv() != 0 || i.caps and 0x1FL.inv() != 0L) {
            throw BleCodecException(BleResult.BAD_ENUM)
        }
        val w = Writer()
        w.bytes(MAGIC)
        w.le(i.protoMin.toLong(), 1); w.le(i.protoMax.toLong(), 1); w.le(i.claimState.toLong(), 1)
        w.le(i.flags.toLong(), 1); w.le(i.caps, 4); w.le(i.fwMajor.toLong(), 1); w.le(i.fwMinor.toLong(), 1)
        w.le(i.fwPatch.toLong(), 2); w.le(i.deviceTag.toLong(), 2)
        w.bytes(ByteArray(14))
        return w.out.toByteArray()
    }

    data class Advert(val protoMajor: Int, val flags: Int, val deviceTag: Int)

    fun decodeAdvert(b: ByteArray): Advert {
        if (b.size != ADVERT_LEN) throw BleCodecException(if (b.size < ADVERT_LEN) BleResult.TRUNCATED else BleResult.LENGTH_MISMATCH)
        val flags = b[1].toInt() and 0xFF
        if (flags and 0x07.inv() != 0) throw BleCodecException(BleResult.BAD_ENUM)
        return Advert(b[0].toInt() and 0xFF, flags, (b[2].toInt() and 0xFF) or ((b[3].toInt() and 0xFF) shl 8))
    }

    fun encodeAdvert(a: Advert): ByteArray {
        if (a.flags and 0x07.inv() != 0) throw BleCodecException(BleResult.BAD_ENUM)
        return byteArrayOf(a.protoMajor.toByte(), a.flags.toByte(), a.deviceTag.toByte(), (a.deviceTag shr 8).toByte())
    }

    // ---- Fragmentation ---------------------------------------------------------------------------

    /** Fragments of [frame] for ATT MTU [mtu]; empty if not representable. */
    fun fragment(frame: ByteArray, mtu: Int): List<ByteArray> {
        if (mtu < ATT_MTU_MIN || frame.isEmpty()) return emptyList()
        val data = mtu - 4
        val n = (frame.size + data - 1) / data
        if (n > FRAG_MAX) return emptyList()
        return List(n) { i ->
            val start = i * data
            val end = minOf(frame.size, start + data)
            val hdr = i or (if (i == 0) 0x80 else 0) or (if (i == n - 1) 0x40 else 0)
            byteArrayOf(hdr.toByte()) + frame.copyOfRange(start, end)
        }
    }

    /** Bounded reassembler: one partial frame, [REASM_TIMEOUT_MS] between fragments. */
    class Reassembler {
        private val buf = ByteArray(FRAME_MAX)
        private var len = 0
        private var next = 0
        private var active = false
        private var lastMs = 0L

        private fun fail(r: BleResult): Nothing {
            active = false
            len = 0
            next = 0
            throw BleCodecException(r)
        }

        /** Returns the complete frame, or null when more fragments are needed. */
        fun push(frag: ByteArray, nowMs: Long): ByteArray? {
            if (frag.isEmpty()) fail(BleResult.FRAG_EMPTY)
            var timedOut = false
            if (active && nowMs - lastMs > REASM_TIMEOUT_MS) {
                active = false; len = 0; next = 0; timedOut = true
            }
            val hdr = frag[0].toInt() and 0xFF
            val first = hdr and 0x80 != 0
            val last = hdr and 0x40 != 0
            val index = hdr and 0x3F
            if (first) {
                active = true; len = 0; next = 0
            } else if (!active) {
                fail(if (timedOut) BleResult.FRAG_TIMEOUT else BleResult.FRAG_NO_FIRST)
            }
            if (index != next) fail(BleResult.FRAG_INDEX)
            val data = frag.size - 1
            if (data == 0) fail(BleResult.FRAG_EMPTY)
            if (len + data > FRAME_MAX) fail(BleResult.FRAG_OVERFLOW)
            System.arraycopy(frag, 1, buf, len, data)
            len += data
            next++
            lastMs = nowMs
            if (last) {
                active = false
                next = 0
                return buf.copyOf(len)
            }
            if (next >= FRAG_MAX) fail(BleResult.FRAG_INDEX)
            return null
        }
    }
}
