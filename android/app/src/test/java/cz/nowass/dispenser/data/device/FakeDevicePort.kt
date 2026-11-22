package cz.nowass.dispenser.data.device

import cz.nowass.dispenser.ble.protocol.BleProtocol
import cz.nowass.dispenser.device.ops.ConfigRecord
import cz.nowass.dispenser.device.ops.LiveSession
import cz.nowass.dispenser.device.ops.MessageType as M
import cz.nowass.dispenser.device.ops.Records
import cz.nowass.dispenser.device.ops.SessionPort
import cz.nowass.dispenser.device.session.DeviceEvent
import cz.nowass.dispenser.device.session.OpenRequest
import cz.nowass.dispenser.device.session.RequestResult
import cz.nowass.dispenser.device.session.SessionFailure
import cz.nowass.dispenser.device.session.SessionState
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow

/**
 * A scripted in-memory dispenser behind a fake [SessionPort], for the repository's JVM tests. It
 * follows the protocol semantics the repository relies on (manual-dose seq floor and results,
 * revisions, REFILL, readiness) and injects faults per message type. The real firmware is covered
 * by the firmware-in-the-loop tests.
 */
class FakeDevicePort(val deviceId: String = "a0b1c2d3e4f5") : SessionPort {
    enum class Fault {
        /** Executed, response lost on an open link → Uncertain (the session closes, as the real client does). */
        EXECUTE_THEN_LOSE,
        /** Dropped before the device decrypted it → Uncertain, nothing executed (session closes). */
        DROP_BEFORE,
        /** Provably never sent. */
        NOT_SENT,
    }

    override val state = MutableStateFlow<SessionState>(SessionState.Idle)
    override val events = MutableSharedFlow<DeviceEvent>(extraBufferCapacity = 8)
    var generation = 0L
    private var current: Session? = null
    var openFailure: SessionFailure? = null

    // ---- device state ----
    var readiness = 1L
    var capacityUl = 200_000L
    var remainingUl = 150_000L
    var manualFloor = 1L
    val results = mutableMapOf<Long, Pair<Int, Long>>() // seq -> (GET_RESULT state, detail)
    var pumpRuns = 0
    var refills = 0
    val schedules = linkedMapOf<String, Map<String, Any>>() // id -> schedule record fields
    var setRevision = 1L
    var config = ConfigRecord(1, 200_000, 150_000, 20, 24, 0, 1000, 0, 5000, 20000, 10000, 1, 30000)
    var phones = mutableListOf(phone(0, "A", true))
    /** The Info record of the connection (firmware version); null = unknown (no GET_BATTERY). */
    var info: BleProtocol.Info? = info(0, 2, 0)
    /** GET_BATTERY answer: wire state, millivolts, age in seconds. */
    var battery = Triple(0L, 3970L, 5L)

    // ---- scripting ----
    val faults = mutableMapOf<Int, Fault>()
    val rejects = mutableMapOf<Int, Triple<Int, Long, Long>>()
    /** Runs when a request of the type arrives (before it is handled); may suspend or change state. */
    val hooks = mutableMapOf<Int, suspend () -> Unit>()
    /** Runs after a request of the type was answered (e.g. the link drops right after a response). */
    val after = mutableMapOf<Int, () -> Unit>()
    val received = mutableListOf<Pair<Int, Map<String, Any>>>()
    fun count(type: Int) = received.count { it.first == type }
    /** MANUAL_DOSE: the device suspends BLE for the pump and closes the link (normal path). */
    var doseClosesLink = true

    inner class Session(override val generation: Long) : LiveSession {
        @Volatile var open = true
        override val isOpen: Boolean get() = open
        override val info: BleProtocol.Info? get() = this@FakeDevicePort.info

        override suspend fun request(type: Int, fields: Map<String, Any>, retries: Int): RequestResult {
            if (!open) return RequestResult.NotSent("closed")
            // the body must be encodable as a real request (the codec validates every rule)
            BleProtocol.encodeBody(type, 0, fields)
            hooks.remove(type)?.invoke()
            if (!open) return RequestResult.Uncertain("closed while sending", 1)
            when (faults.remove(type)) {
                Fault.NOT_SENT -> return RequestResult.NotSent("injected")
                Fault.DROP_BEFORE -> {
                    received += type to fields
                    close()
                    return RequestResult.Uncertain("dropped before decryption", retries + 1)
                }
                Fault.EXECUTE_THEN_LOSE -> {
                    received += type to fields
                    handle(type, fields)
                    close()
                    return RequestResult.Uncertain("decryption failed", retries + 1)
                }
                null -> Unit
            }
            received += type to fields
            rejects.remove(type)?.let { (c, d, a) -> return RequestResult.DeviceError(c, d, a) }
            return handle(type, fields).also { after.remove(type)?.invoke() }
        }
    }

    fun live0(): Session? = current?.takeIf { it.open }
    override fun live(): LiveSession? = live0()

    override suspend fun open(address: String, request: OpenRequest): SessionFailure? {
        openFailure?.let { return it }
        generation++
        current = Session(generation)
        state.value = SessionState.Authenticated(deviceId, generation, 0)
        return null
    }

    override suspend fun end(graceful: Boolean) = close()

    fun close() {
        current?.open = false
        state.value = SessionState.Idle
    }

    private fun ok(fields: Map<String, Any>) = RequestResult.Ok(fields, 1)
    private fun err(code: Int, detail: Long = 0, aux: Long = 0) = RequestResult.DeviceError(code, detail, aux)

    fun statusFields(): Map<String, Any> = mapOf("status" to mapOf(
        "readiness" to readiness, "dosing_flags" to 0L, "gates" to 0L, "pump_problems" to 0L, "capacity_ul" to capacityUl,
        "remaining_ul" to remainingUl, "low_percent" to 20L, "estimate_uncertain" to false, "clock_trust" to 1L, "clock_reason" to 0L,
        "device_utc" to 1_795_000_000L, "next_due_known" to false, "next_due_at" to 0L, "pending_notes" to 0L, "highest_severity" to 0L,
        "storage_ok" to true, "session_seconds_left" to 600L, "battery_percent" to 255L, "manual_floor" to manualFloor.toULong(),
    ))

    @Suppress("UNCHECKED_CAST")
    private fun handle(type: Int, f: Map<String, Any>): RequestResult = when (type) {
        M.GET_STATUS -> ok(statusFields())
        M.GET_BATTERY -> if (!DeviceDispenserRepository.firmwareHasBattery(info)) {
            // firmware < 0.2.0: an unknown encrypted type closes the session (never answered)
            close()
            RequestResult.Uncertain("session closed by old firmware", 1)
        } else {
            ok(mapOf("battery" to mapOf("state" to battery.first, "battery_mv" to battery.second, "age_s" to battery.third)))
        }
        M.GET_DIAGNOSTICS -> ok(mapOf("diag" to mapOf("notify_storage_ok" to true, "tx_diag" to 0L, "tx_diag_status" to 0L,
            "rejections_unread" to 0L, "rejections_total" to 0L, "clock_trust" to 1L, "clock_reason" to 0L, "last_sync_utc" to 0L, "last_wake_reason" to 0L)))
        M.GET_CONFIG -> ok(mapOf("config" to Records.configFields(config)))
        M.SET_CONFIG -> {
            val exp = f["expected_revision"] as Long
            if (exp != config.revision) err(11, config.revision) else {
                val want = Records.configRecord(f["config"] as Map<String, Any>)
                val m = (f["field_mask"] as Long).toInt()
                config = config.copy(revision = config.revision + 1,
                    maxDoseUl = if (m and ConfigRecord.F_MAX_DOSE != 0) want.maxDoseUl else config.maxDoseUl,
                    lowStockPercent = if (m and ConfigRecord.F_LOW_STOCK != 0) want.lowStockPercent else config.lowStockPercent)
                ok(mapOf("revision" to config.revision))
            }
        }
        M.LIST_SCHEDULES -> {
            val page = (f["page"] as Long).toInt()
            val all = schedules.entries.toList()
            if (page > 0 && page * 4 >= all.size) err(10) else ok(mapOf("set_revision" to setRevision, "total" to all.size.toLong(), "page" to page.toLong(),
                "items" to all.drop(page * 4).take(4).map { (id, r) -> mapOf("id" to id.toByteArray(), "revision" to r["revision"]!!) }))
        }
        M.GET_SCHEDULE -> schedules[String(f["id"] as ByteArray)]?.let { ok(mapOf("schedule" to it)) } ?: err(12)
        M.CREATE_SCHEDULE -> {
            val r = f["schedule"] as Map<String, Any>
            val id = String(r["id"] as ByteArray)
            if (id in schedules) err(17) else {
                schedules[id] = r
                ok(mapOf("set_revision" to ++setRevision))
            }
        }
        M.UPDATE_SCHEDULE -> {
            val r = f["schedule"] as Map<String, Any>
            val id = String(r["id"] as ByteArray)
            val cur = schedules[id]
            when {
                cur == null -> err(12)
                cur["revision"] != f["expected_revision"] -> err(11, cur["revision"] as Long)
                else -> {
                    schedules[id] = r
                    ok(mapOf("revision" to r["revision"]!!, "set_revision" to ++setRevision))
                }
            }
        }
        M.DELETE_SCHEDULE -> {
            val id = String(f["id"] as ByteArray)
            val cur = schedules[id]
            when {
                cur == null -> err(12)
                cur["revision"] != f["expected_revision"] -> err(11, cur["revision"] as Long)
                else -> {
                    schedules.remove(id)
                    ok(mapOf("set_revision" to ++setRevision))
                }
            }
        }
        M.MANUAL_DOSE -> {
            val seq = (f["seq"] as Long)
            val vol = f["volume_ul"] as Long
            if (seq < manualFloor) {
                val (st, d) = results[seq] ?: (6 to 0L)
                err(17, st.toLong(), d)
            } else {
                manualFloor = seq + 1
                results[seq] = 2 to vol
                pumpRuns++
                remainingUl -= vol
                if (doseClosesLink) {
                    close()
                    RequestResult.Uncertain("transport: Disconnected(19)", 1)
                } else {
                    ok(mapOf("outcome" to 0L, "volume_ul" to vol, "runtime_ms" to 1000L) + statusFields())
                }
            }
        }
        M.GET_RESULT -> {
            val seq = f["seq"] as Long
            val (st, d) = if (seq >= manualFloor) 0 to 0L else results[seq] ?: (6 to 0L)
            ok(mapOf("state" to st.toLong(), "detail" to d))
        }
        M.REFILL -> {
            capacityUl = f["capacity_ul"] as Long
            remainingUl = f["remaining_ul"] as Long
            refills++
            ok(statusFields())
        }
        M.SET_READINESS -> {
            readiness = f["readiness"] as Long
            ok(statusFields())
        }
        M.SET_TIME -> ok(mapOf("decision" to 0L, "offset_s" to 0L))
        M.PHONES_LIST -> ok(mapOf("items" to phones))
        M.PHONE_REMOVE -> {
            val slot = f["slot"] as Long
            phones.removeAll { it["slot"] == slot }
            ok(emptyMap())
        }
        M.INVITE_CREATE -> ok(mapOf("token" to ByteArray(16) { 1 }, "expires_s" to 600L))
        M.INVITE_CANCEL, M.RECOVERY_CANCEL, M.ACK_REJECTIONS -> ok(emptyMap())
        else -> err(3)
    }

    companion object {
        fun phone(slot: Int, name: String, self: Boolean): Map<String, Any> =
            mapOf("slot" to slot.toLong(), "name" to name.toByteArray(), "enrolled_at" to 0L, "last_seen" to 0L, "is_self" to self)
    }
}

/** A durable-looking allocator for tests: max(high + 1, floor), [fail] injects a persistence failure. */
class TestSeqAllocator : ManualSeqAllocator {
    var high = 0L
    var calls = 0
    var fail = false
    override fun next(deviceId: String, floor: Long): Long {
        calls++
        if (fail) throw java.io.IOException("injected seq persistence failure")
        high = maxOf(high + 1, floor)
        return high
    }
}

/** An Info record with the given firmware version (other fields as a claimed v1 device). */
fun info(major: Int, minor: Int, patch: Int) = BleProtocol.Info(1, 1, 1, 0, 0x06, major, minor, patch, 0x1234)
