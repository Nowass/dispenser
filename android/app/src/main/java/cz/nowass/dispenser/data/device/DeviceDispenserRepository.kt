package cz.nowass.dispenser.data.device

import cz.nowass.dispenser.ble.protocol.BleHeader
import cz.nowass.dispenser.ble.protocol.BleProtocol
import cz.nowass.dispenser.device.ops.ConfigRecord
import cz.nowass.dispenser.device.ops.DeviceErrorCode
import cz.nowass.dispenser.device.ops.DeviceOperations
import cz.nowass.dispenser.device.ops.DeviceValidation
import cz.nowass.dispenser.device.ops.DiagnosticsRecord
import cz.nowass.dispenser.device.ops.InviteRecord
import cz.nowass.dispenser.device.ops.LiveSession
import cz.nowass.dispenser.device.ops.LocalRefusal
import cz.nowass.dispenser.device.ops.ManualDoseRecord
import cz.nowass.dispenser.device.ops.ManualResultState
import cz.nowass.dispenser.device.ops.MessageType as M
import cz.nowass.dispenser.device.ops.OpResult
import cz.nowass.dispenser.device.ops.PhoneRecord
import cz.nowass.dispenser.device.ops.Readiness
import cz.nowass.dispenser.device.ops.Records
import cz.nowass.dispenser.device.ops.ScheduleRecord
import cz.nowass.dispenser.device.ops.SessionPort
import cz.nowass.dispenser.device.ops.StatusRecord
import cz.nowass.dispenser.device.ops.TimeDecisionRecord
import cz.nowass.dispenser.device.session.DeviceEvent
import cz.nowass.dispenser.device.session.OpenRequest
import cz.nowass.dispenser.device.session.SessionFailure
import cz.nowass.dispenser.device.session.SessionState
import cz.nowass.dispenser.domain.ScheduleDraft
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.async
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.time.Instant
import java.util.UUID

/**
 * [DeviceRepository] over the process-wide session owner (docs/ANDROID_BLE_DESIGN.md 2.1, 5.1, 11.4–11.6;
 * decisions D1–D8). Device values carry their freshness ([Fresh]); journaled mutations follow 11.4.
 *
 * Journal rules: a row is written durably BEFORE its request is submitted (a failed write means
 * nothing is sent); a definitive answer (response, refusal, provably not sent) removes it; an
 * Uncertain answer — or STORAGE, after which the device's state decides — keeps it for read-back
 * at the next session. Manual doses additionally allocate their `seq` durably before the row (D4).
 */
class DeviceDispenserRepository(
    override val deviceId: String,
    /** The associated address (CDM, M1.10e.4); a locator only, never an identity. */
    private val address: () -> String?,
    private val port: SessionPort,
    private val journal: PendingOpsJournal,
    private val cache: DeviceCacheStore,
    private val seqAllocator: ManualSeqAllocator,
    private val timeSource: PhoneTimeSource,
    /** Manager-side scope: owns submitted operations (D3). */
    private val scope: CoroutineScope,
    private val nowMs: () -> Long = System::currentTimeMillis,
    private val newScheduleId: () -> String = { UUID.randomUUID().toString() },
) : DeviceRepository {
    private val ops = DeviceOperations(scope)

    /** A value with the generation it was read in (null = loaded from the cache). */
    private class Stamped<T>(val value: T, val readAtMs: Long, val generation: Long?)

    private val statusS = MutableStateFlow<Stamped<StatusRecord>?>(null)
    private val configS = MutableStateFlow<Stamped<ConfigRecord>?>(null)
    private val diagS = MutableStateFlow<Stamped<DiagnosticsRecord>?>(null)
    private val schedulesS = MutableStateFlow<Stamped<DeviceSchedules>?>(null)
    private val phonesS = MutableStateFlow<Stamped<List<PhoneRecord>>?>(null)

    override val connection: StateFlow<SessionState> get() = port.state
    override val events: SharedFlow<DeviceEvent> get() = port.events

    init {
        scope.launch { loadCache() }
    }

    // ---- freshness ---------------------------------------------------------------------------

    private fun <T> fresh(f: StateFlow<Stamped<T>?>): Flow<Fresh<T>> = combine(f, port.state) { s, st ->
        when {
            s == null -> Fresh.Unknown
            s.generation != null && st is SessionState.Authenticated && st.deviceIdHex == deviceId &&
                st.generation == s.generation && port.live()?.generation == s.generation ->
                Fresh.Live(s.value, Instant.ofEpochMilli(s.readAtMs), s.generation)
            else -> Fresh.Cached(s.value, Instant.ofEpochMilli(s.readAtMs))
        }
    }

    override fun status() = fresh(statusS)
    override fun config() = fresh(configS)
    override fun diagnostics() = fresh(diagS)
    override fun schedules() = fresh(schedulesS)
    override fun phones() = fresh(phonesS)
    override fun pendingOperations(): Flow<List<PendingOp>> = journal.observe(deviceId)

    /** The live authenticated session of THIS device, if any. */
    private fun session(): LiveSession? {
        val s = port.live() ?: return null
        val st = port.state.value
        return s.takeIf { it.isOpen && st is SessionState.Authenticated && st.deviceIdHex == deviceId && st.generation == it.generation }
    }

    /** Still the same open session (D5: abort when it changed or disconnected). */
    private fun same(s: LiveSession): Boolean = s.isOpen && port.live()?.generation == s.generation

    private fun notConnected(): OpResult.NotSent = OpResult.NotSent("not connected")

    private inline fun <V> live(block: (LiveSession) -> OpResult<V>): OpResult<V> {
        val s = session() ?: return notConnected()
        return block(s)
    }

    // ---- connection --------------------------------------------------------------------------

    /** The session generation whose journal resolution already ran. */
    @Volatile private var resolvedGeneration = -1L

    override suspend fun connect(): ConnectOutcome {
        val s = session() ?: run {
            val addr = address() ?: return ConnectOutcome.NoAddress
            // the open belongs to the manager scope (D3), not to the calling screen: a screen left
            // while connecting does not abort it, and the resulting session serves every screen
            val failure = scope.async { port.open(addr, OpenRequest.Session(deviceId)) }.await()
            // concurrent connect() calls coalesce: the manager opens one session (its open lock), a
            // later caller gets AlreadyActive and uses the session the first one opened for this device
            if (failure != null && !(failure == SessionFailure.AlreadyActive && session() != null)) {
                return ConnectOutcome.Failed(failure)
            }
            session() ?: return ConnectOutcome.Failed(SessionFailure.ProtocolError("session closed right after opening"))
        }
        // once per session (also one opened elsewhere, e.g. right after an enrollment): resolution
        // before any new mutation (11.4.3), then the live status. An unfinished resolution is reported
        // and retried by the next connect(); its rows stay unresolved and keep blocking their own
        // operations (every mutation checks the journal under its reservation), reads stay available.
        var complete = resolvedGeneration == s.generation
        if (!complete) {
            complete = resolvePending(s)
            readStatus(s)
            if (complete) resolvedGeneration = s.generation
        }
        return ConnectOutcome.Connected(s.generation, unresolvedCount(), complete)
    }

    override suspend fun disconnect(graceful: Boolean) = port.end(graceful)

    private suspend fun unresolvedCount(): Int = try {
        journal.list(deviceId).count { it.unresolved }
    } catch (e: Exception) {
        -1
    }

    // ---- reads -------------------------------------------------------------------------------

    private suspend fun <T> read(
        s: LiveSession, type: Int, fields: Map<String, Any> = emptyMap(), decode: (Map<String, Any>) -> T,
        store: suspend (T, ByteArray, LiveSession) -> Unit = { _, _, _ -> },
    ): OpResult<T> {
        var body: ByteArray? = null
        val r = ops.call(s, type, fields, retries = 2, decode = { m -> body = enc(type, m); decode(m) }) { r ->
            if (r is OpResult.Ok) store(r.value, checkNotNull(body), s)
        }
        return r
    }

    private suspend fun readStatus(s: LiveSession) = read(s, M.GET_STATUS, decode = Records::status) { v, b, ss -> keep(statusS, KEY_STATUS, v, b, ss) }

    override suspend fun refreshStatus(): OpResult<StatusRecord> = live { readStatus(it) }

    override suspend fun refreshDiagnostics(): OpResult<DiagnosticsRecord> = live { read(it, M.GET_DIAGNOSTICS, decode = Records::diagnostics) { v, b, s -> keep(diagS, KEY_DIAG, v, b, s) } }

    override suspend fun ackRejections(): OpResult<Unit> = live { ops.call(it, M.ACK_REJECTIONS, decode = { }) }

    override suspend fun refreshConfig(): OpResult<ConfigRecord> = live { readConfig(it) }

    private suspend fun readConfig(s: LiveSession) = read(s, M.GET_CONFIG, decode = Records::config) { v, b, ss -> keep(configS, KEY_CONFIG, v, b, ss) }

    override suspend fun refreshPhones(): OpResult<List<PhoneRecord>> = live { readPhones(it) }

    private suspend fun readPhones(s: LiveSession) = read(s, M.PHONES_LIST, decode = Records::phones) { v, b, ss -> keep(phonesS, KEY_PHONES, v, b, ss) }

    override suspend fun refreshSchedules(): OpResult<DeviceSchedules> = live { readSchedules(it) }

    /** LIST (4 per page) + GET each; restarted when the set changes meanwhile (another phone). */
    private suspend fun readSchedules(s: LiveSession): OpResult<DeviceSchedules> {
        repeat(3) {
            val ids = mutableListOf<String>()
            var setRev: Long? = null
            var page = 0
            var changed = false
            while (true) {
                val p = when (val r = ops.call(s, M.LIST_SCHEDULES, mapOf("page" to page.toLong()), decode = Records::schedulePage)) {
                    is OpResult.Ok -> r.value
                    else -> return r.cast()
                }
                if (setRev != null && p.setRevision != setRev) {
                    changed = true
                    break
                }
                setRev = p.setRevision
                ids += p.items.map { it.first }
                if (ids.size >= p.total || p.items.isEmpty()) break
                page++
            }
            if (changed) return@repeat
            val bodies = mutableListOf<ByteArray>()
            val list = mutableListOf<DeviceSchedule>()
            var unreadable = 0
            for (id in ids) {
                var body: ByteArray? = null
                when (val r = ops.call(s, M.GET_SCHEDULE, mapOf("id" to id.toByteArray()), decode = { m -> body = enc(M.GET_SCHEDULE, m); Records.schedule(m) })) {
                    is OpResult.Ok -> {
                        bodies += checkNotNull(body)
                        DeviceScheduleMapping.toSchedule(r.value)?.let { list += DeviceSchedule(it, r.value) } ?: unreadable++
                    }
                    is OpResult.Rejected -> if (r.error.code == DeviceErrorCode.NOT_FOUND) changed = true else return r
                    else -> return r.cast()
                }
                if (changed) break
            }
            if (changed) return@repeat
            val v = DeviceSchedules(checkNotNull(setRev), list, unreadable)
            keep(schedulesS, KEY_SCHEDULES, v, packSchedules(v.setRevision, bodies), s)
            return OpResult.Ok(v)
        }
        return OpResult.Refused(LocalRefusal.Unavailable("schedule set kept changing while being read"))
    }

    private suspend fun <V> keep(flow: MutableStateFlow<Stamped<V>?>, key: String, value: V, body: ByteArray, s: LiveSession) {
        val now = nowMs()
        flow.value = Stamped(value, now, s.generation)
        try {
            cache.put(deviceId, key, CacheEntry(body, now))
        } catch (e: Exception) {
            // the cache is informational: a failed write only loses the offline view
        }
    }

    // ---- journal helpers ---------------------------------------------------------------------

    /**
     * Operation-level reservation (review of M1.10e.3, finding 2). The guard (no unresolved journal row
     * of this kind/target) and the reservation of [key] happen atomically under [reservationLock]; the
     * reservation is held until the operation has settled (its own row inserted and settled, or nothing
     * sent). A concurrent caller of the same operation is refused at once ([LocalRefusal.InProgress]),
     * never queued behind it (11.6: duplicate intents are rejected). Other operations and reads are not
     * blocked. The session layer serializes single requests only; it does not make check-then-insert
     * atomic, so this is needed in addition.
     */
    private suspend inline fun <V> reserved(key: String, guard: () -> OpResult.Refused?, block: () -> OpResult<V>): OpResult<V> {
        reservationLock.withLock {
            if (key in inFlight) return OpResult.Refused(LocalRefusal.InProgress)
            guard()?.let { return it }
            inFlight += key
        }
        try {
            return block()
        } finally {
            inFlight -= key
        }
    }

    private val reservationLock = Mutex()
    private val inFlight: MutableSet<String> = java.util.Collections.newSetFromMap(java.util.concurrent.ConcurrentHashMap())

    private suspend fun unresolved(kind: PendingKind, target: String? = null): OpResult.Refused? = try {
        journal.list(deviceId).firstOrNull { it.unresolved && it.kind == kind && (target == null || it.target == target) }
            ?.let { OpResult.Refused(LocalRefusal.Unresolved(it.id)) }
    } catch (e: Exception) {
        OpResult.Refused(LocalRefusal.PersistenceFailed("journal read: ${e.javaClass.simpleName}"))
    }

    /** Journal row first (durable), then the request; the row settles by the outcome (see class doc). */
    private suspend fun <V> journaled(
        s: LiveSession, op: PendingOp, type: Int, fields: Map<String, Any>, decode: (Map<String, Any>) -> V,
        onOk: suspend (V) -> Unit = {},
    ): OpResult<V> {
        if (!same(s)) return OpResult.Refused(LocalRefusal.SessionChanged)
        val id = try {
            journal.insert(op.copy(payload = BleProtocol.encodeBody(type, 0, fields)))
        } catch (e: Exception) {
            return OpResult.Refused(LocalRefusal.PersistenceFailed("journal: ${e.javaClass.simpleName}"))
        }
        val r = ops.call(s, type, fields, retries = 2, decode = decode) { r ->
            val keepRow = r is OpResult.Uncertain || (r is OpResult.Rejected && r.error.code == DeviceErrorCode.STORAGE)
            if (!keepRow) runCatching { journal.delete(id) }
            if (r is OpResult.Ok) onOk(r.value)
        }
        return if (r is OpResult.Uncertain) r.copy(journalId = id) else r
    }

    private fun op(kind: PendingKind, target: String? = null, expectedRevision: Long? = null) =
        PendingOp(deviceId = deviceId, kind = kind, createdAtMs = nowMs(), target = target, expectedRevision = expectedRevision)

    // ---- configuration, readiness, phones ----------------------------------------------------

    override suspend fun setConfig(expectedRevision: Long, mask: Int, values: ConfigRecord): OpResult<Long> {
        if (mask == 0 || mask and ConfigRecord.F_ALL.inv() != 0) return OpResult.Refused(LocalRefusal.Invalid(setOf(DeviceValidation.CONFIG_MASK_INVALID)))
        val s = session() ?: return notConnected()
        return reserved("SET_CONFIG", { unresolved(PendingKind.SET_CONFIG) }) {
            val fields = mapOf("expected_revision" to expectedRevision, "field_mask" to mask.toLong(),
                "config" to Records.configFields(values.copy(revision = expectedRevision)))
            journaled(s, op(PendingKind.SET_CONFIG, "config", expectedRevision), M.SET_CONFIG, fields, Records::revision) { readConfig(s) }
        }
    }

    override suspend fun setReadiness(readiness: Readiness): OpResult<StatusRecord> {
        val s = session() ?: return notConnected()
        return reserved("SET_READINESS", { unresolved(PendingKind.SET_READINESS) }) {
            var body: ByteArray? = null
            journaled(s, op(PendingKind.SET_READINESS, readiness.name), M.SET_READINESS, mapOf("readiness" to readiness.wire.toLong()),
                decode = { m -> body = enc(M.SET_READINESS, m); Records.status(m) }) { v -> keep(statusS, KEY_STATUS, v, checkNotNull(body), s) }
        }
    }

    override suspend fun removePhone(slot: Int): OpResult<Unit> {
        if (slot !in 0..3) return OpResult.Refused(LocalRefusal.Invalid(setOf(DeviceValidation.SLOT_INVALID)))
        val s = session() ?: return notConnected()
        return reserved("PHONE_REMOVE:$slot", { unresolved(PendingKind.PHONE_REMOVE, slot.toString()) }) {
            journaled(s, op(PendingKind.PHONE_REMOVE, slot.toString()), M.PHONE_REMOVE, mapOf("slot" to slot.toLong()), decode = { }) { readPhones(s) }
        }
    }

    override suspend fun createInvite(nameHint: String): OpResult<InviteRecord> {
        val b = nameHint.toByteArray(Charsets.UTF_8)
        if (b.size > 24 || !BleProtocol.utf8Ok(b, noControls = true)) return OpResult.Refused(LocalRefusal.Invalid(setOf(DeviceValidation.NAME_HINT_TOO_LONG)))
        // not journaled: an uncertain invite is harmless (single use, ≤ 10 min, cancellable) and its token cannot be recovered
        return ops.call(session() ?: return notConnected(), M.INVITE_CREATE, mapOf("name_hint" to b), decode = Records::invite)
    }

    override suspend fun cancelInvite(): OpResult<Unit> = live { ops.call(it, M.INVITE_CANCEL, decode = { }) }
    override suspend fun cancelRecovery(): OpResult<Unit> = live { ops.call(it, M.RECOVERY_CANCEL, decode = { }) }

    // ---- time --------------------------------------------------------------------------------

    override suspend fun setTime(): OpResult<TimeDecisionRecord> {
        val s = session() ?: return notConnected()
        val t = timeSource.now()
        if (t == null || t.uncertaintyMs !in 0..MAX_TIME_UNCERTAINTY_MS) return OpResult.Refused(LocalRefusal.TimeUntrusted)
        // never retransmitted: a resend would carry a stale time (re-evaluated by the caller instead)
        return ops.call(s, M.SET_TIME, mapOf("phone_utc_ms" to t.utcMs, "uncertainty_ms" to t.uncertaintyMs), retries = 0, decode = Records::timeDecision)
    }

    // ---- REFILL (D5) -------------------------------------------------------------------------

    override suspend fun refill(remainingUl: Long): OpResult<StatusRecord> {
        val s = session() ?: return notConnected()
        return reserved("REFILL", { unresolved(PendingKind.REFILL) }) {
            // the capacity comes from THIS session, immediately before; never from the cache
            val st = when (val r = readStatus(s)) {
                is OpResult.Ok -> r.value
                is OpResult.Uncertain -> return OpResult.NotSent("status read failed: ${r.reason}") // REFILL itself was never sent
                else -> return r.cast()
            }
            if (!same(s)) return OpResult.Refused(LocalRefusal.SessionChanged)
            if (st.capacityUl <= 0) return OpResult.Refused(LocalRefusal.Invalid(setOf(DeviceValidation.CAPACITY_UNKNOWN)))
            if (remainingUl !in 0..st.capacityUl) return OpResult.Refused(LocalRefusal.Invalid(setOf(DeviceValidation.REMAINING_ABOVE_CAPACITY)))
            var body: ByteArray? = null
            journaled(s, op(PendingKind.REFILL).copy(volumeUl = remainingUl), M.REFILL,
                mapOf("capacity_ul" to st.capacityUl, "remaining_ul" to remainingUl),
                decode = { m -> body = enc(M.REFILL, m); Records.status(m) }) { v -> keep(statusS, KEY_STATUS, v, checkNotNull(body), s) }
        }
    }

    // ---- schedules ---------------------------------------------------------------------------

    override suspend fun createSchedule(draft: ScheduleDraft): OpResult<ScheduleWrite> {
        val id = newScheduleId()
        val rec = when (val m = DeviceScheduleMapping.toRecord(draft, id, 1)) {
            is DeviceScheduleMapping.ToRecord.Invalid -> return OpResult.Refused(LocalRefusal.Invalid(m.problems))
            is DeviceScheduleMapping.ToRecord.Ok -> m.record
        }
        val s = session() ?: return notConnected()
        return reserved("SCHEDULE:$id", { null }) { // a fresh ID: no earlier operation can target it
            journaled(s, op(PendingKind.CREATE_SCHEDULE, id), M.CREATE_SCHEDULE, mapOf("schedule" to Records.scheduleFields(rec)),
                decode = { m -> ScheduleWrite(id, 1, Records.setRevision(m)) }) { readSchedules(s) }
        }
    }

    override suspend fun updateSchedule(id: String, expectedRevision: Long, draft: ScheduleDraft): OpResult<ScheduleWrite> {
        val rec = when (val m = DeviceScheduleMapping.toRecord(draft, id, expectedRevision + 1)) {
            is DeviceScheduleMapping.ToRecord.Invalid -> return OpResult.Refused(LocalRefusal.Invalid(m.problems))
            is DeviceScheduleMapping.ToRecord.Ok -> m.record
        }
        val s = session() ?: return notConnected()
        return reserved("SCHEDULE:$id", { scheduleUnresolved(id) }) {
            journaled(s, op(PendingKind.UPDATE_SCHEDULE, id, expectedRevision), M.UPDATE_SCHEDULE,
                mapOf("expected_revision" to expectedRevision, "schedule" to Records.scheduleFields(rec)),
                decode = { m -> ScheduleWrite(id, Records.revision(m), Records.setRevision(m)) }) { readSchedules(s) }
        }
    }

    override suspend fun deleteSchedule(id: String, expectedRevision: Long): OpResult<ScheduleWrite> {
        val s = session() ?: return notConnected()
        return reserved("SCHEDULE:$id", { scheduleUnresolved(id) }) {
            journaled(s, op(PendingKind.DELETE_SCHEDULE, id, expectedRevision), M.DELETE_SCHEDULE,
                mapOf("id" to id.toByteArray(), "expected_revision" to expectedRevision),
                decode = { m -> ScheduleWrite(id, null, Records.setRevision(m)) }) { readSchedules(s) }
        }
    }

    private suspend fun scheduleUnresolved(id: String): OpResult.Refused? =
        unresolved(PendingKind.CREATE_SCHEDULE, id) ?: unresolved(PendingKind.UPDATE_SCHEDULE, id) ?: unresolved(PendingKind.DELETE_SCHEDULE, id)

    // ---- manual dose (D4, 11.4) ---------------------------------------------------------------

    override suspend fun manualDose(volumeUl: Long): OpResult<ManualDoseRecord> {
        if (volumeUl !in 1..DeviceScheduleMapping.U32_MAX) return OpResult.Refused(LocalRefusal.Invalid(setOf(DeviceValidation.VOLUME_INVALID)))
        val s = session() ?: return notConnected()
        // no stacking (11.4.4), and no concurrent second dose: one MANUAL_DOSE reservation at a time
        return reserved(KEY_DOSE, { unresolved(PendingKind.MANUAL_DOSE) }) { prepareAndSendDose(s, volumeUl) }
    }

    private suspend fun prepareAndSendDose(s: LiveSession, volumeUl: Long): OpResult<ManualDoseRecord> {
        // the floor comes from THIS session, immediately before; a cached status never authorizes a dose
        val st = when (val r = readStatus(s)) {
            is OpResult.Ok -> r.value
            is OpResult.Uncertain -> return OpResult.NotSent("status read failed: ${r.reason}")
            else -> return r.cast()
        }
        if (!same(s)) return OpResult.Refused(LocalRefusal.SessionChanged)
        // 1. the seq high-water mark, durably (a crash after this only skips the number)
        val seq = try {
            seqAllocator.next(deviceId, st.manualFloor)
        } catch (e: Exception) {
            return OpResult.Refused(LocalRefusal.PersistenceFailed("seq: ${e.javaClass.simpleName}"))
        }
        val fields = mapOf("seq" to seq, "volume_ul" to volumeUl)
        // 2. the PREPARED row, durably (a crash after this is resolved by GET_RESULT{seq})
        val id = try {
            journal.insert(op(PendingKind.MANUAL_DOSE).copy(seq = seq, volumeUl = volumeUl, payload = BleProtocol.encodeBody(M.MANUAL_DOSE, 0, fields)))
        } catch (e: Exception) {
            return OpResult.Refused(LocalRefusal.PersistenceFailed("journal: ${e.javaClass.simpleName}")) // seq burned, never reused
        }
        if (!same(s)) {
            runCatching { journal.delete(id) }
            return OpResult.Refused(LocalRefusal.SessionChanged)
        }
        // 3. only now the request
        return sendDose(s, id, fields, resend = false)
    }

    override suspend fun resendManualDose(journalId: Long): OpResult<ManualDoseRecord> {
        val s = session() ?: return notConnected()
        return reserved(KEY_DOSE, { null }) { resendDose(s, journalId) }
    }

    private suspend fun resendDose(s: LiveSession, journalId: Long): OpResult<ManualDoseRecord> {
        val row = try {
            journal.list(deviceId).firstOrNull { it.id == journalId }
        } catch (e: Exception) {
            return OpResult.Refused(LocalRefusal.PersistenceFailed("journal read: ${e.javaClass.simpleName}"))
        }
        if (row == null || row.kind != PendingKind.MANUAL_DOSE || row.state != PendingState.AWAITING_USER) {
            return OpResult.Refused(LocalRefusal.Unavailable("no NOT_DECIDED dose $journalId"))
        }
        try {
            journal.update(row.copy(state = PendingState.PREPARED)) // durable before sending again
        } catch (e: Exception) {
            return OpResult.Refused(LocalRefusal.PersistenceFailed("journal: ${e.javaClass.simpleName}"))
        }
        // the ORIGINAL seq: decided at most once by the device
        return sendDose(s, journalId, mapOf("seq" to checkNotNull(row.seq), "volume_ul" to checkNotNull(row.volumeUl)), resend = true)
    }

    private suspend fun sendDose(s: LiveSession, id: Long, fields: Map<String, Any>, resend: Boolean): OpResult<ManualDoseRecord> {
        // the device suspends BLE for the pump and closes the link: the answer normally never arrives
        val r = ops.call(s, M.MANUAL_DOSE, fields, retries = 1, decode = Records::manualDose) { r -> settleDose(id, r, resend) }
        return if (r is OpResult.Uncertain) r.copy(journalId = id) else r
    }

    private suspend fun settleDose(id: Long, r: OpResult<ManualDoseRecord>, resend: Boolean) {
        val row = runCatching { journal.list(deviceId).firstOrNull { it.id == id } }.getOrNull() ?: return
        runCatching {
            when (r) {
                is OpResult.Ok -> journal.delete(id) // decided, and the caller has the outcome
                is OpResult.Uncertain -> Unit // stays PREPARED: GET_RESULT{seq} at the next session
                is OpResult.Rejected -> when (r.error.code) {
                    DeviceErrorCode.DUPLICATE -> resolveDose(row, ManualResultState.of(r.error.detail.toInt()), r.error.aux)
                    DeviceErrorCode.STORAGE -> Unit // undecided unless a later GET_RESULT says otherwise
                    DeviceErrorCode.REFUSED, DeviceErrorCode.LIMIT, DeviceErrorCode.NOT_COVERED ->
                        if (resend) resolveDose(row, ManualResultState.REFUSED, r.error.detail) else journal.delete(id)
                    // not consumed (BUSY, DEGRADED, INVALID, …): a new dose is gone; a re-send stays NOT_DECIDED
                    else -> if (resend) journal.update(row.copy(state = PendingState.AWAITING_USER)) else journal.delete(id)
                }
                is OpResult.NotSent -> if (resend) journal.update(row.copy(state = PendingState.AWAITING_USER)) else journal.delete(id)
                is OpResult.Refused -> Unit
            }
        }
    }

    private suspend fun resolveDose(row: PendingOp, state: ManualResultState?, detail: Long) {
        when (state) {
            ManualResultState.NOT_DECIDED -> journal.update(row.copy(state = PendingState.AWAITING_USER, resolution = Resolution.NOT_DECIDED))
            ManualResultState.PENDING, null -> Unit // in progress / interrupted until the next boot reports it
            else -> journal.update(row.copy(state = PendingState.RESOLVED, resolvedAtMs = nowMs(), resolutionDetail = detail,
                resolution = when (state) {
                    ManualResultState.DONE -> Resolution.DONE
                    ManualResultState.FAILED -> Resolution.FAILED
                    ManualResultState.INTERRUPTED -> Resolution.INTERRUPTED
                    ManualResultState.REFUSED -> Resolution.REFUSED
                    else -> Resolution.DECIDED_RESULT_UNAVAILABLE
                }))
        }
    }

    // under the dose reservation: a discard can never race a re-send of the same row
    override suspend fun discardManualDose(journalId: Long): OpResult<Unit> = reserved(KEY_DOSE, { null }) {
        removeRow(journalId) { it.kind == PendingKind.MANUAL_DOSE && it.state == PendingState.AWAITING_USER }
    }

    override suspend fun acknowledge(journalId: Long): OpResult<Unit> = removeRow(journalId) { it.state == PendingState.RESOLVED }

    private suspend fun removeRow(id: Long, allowed: (PendingOp) -> Boolean): OpResult<Unit> = try {
        val row = journal.list(deviceId).firstOrNull { it.id == id }
        if (row == null || !allowed(row)) OpResult.Refused(LocalRefusal.Unavailable("journal row $id")) else OpResult.Ok(journal.delete(id))
    } catch (e: Exception) {
        OpResult.Refused(LocalRefusal.PersistenceFailed("journal: ${e.javaClass.simpleName}"))
    }

    // ---- resolution at session start (11.4.3, table 5.1) -------------------------------------

    /**
     * Resolves PREPARED rows by read-back; stops at the first request the session could not answer.
     * True only when no PREPARED row is left (every read-back answered and recorded).
     */
    private suspend fun resolvePending(s: LiveSession): Boolean {
        val rows = try {
            journal.list(deviceId).filter { it.state == PendingState.PREPARED }
        } catch (e: Exception) {
            return false
        }
        for (row in rows) {
            if (!same(s)) return false
            val continueOn = try {
                resolveOne(s, row)
            } catch (e: Exception) {
                false // a journal write failed: the row stays unresolved, retried by the next connect()
            }
            if (!continueOn) return false
        }
        return try {
            journal.list(deviceId).none { it.state == PendingState.PREPARED }
        } catch (e: Exception) {
            false
        }
    }

    private suspend fun resolveOne(s: LiveSession, row: PendingOp): Boolean {
        fun done(res: Resolution, detail: Long? = null) = row.copy(state = PendingState.RESOLVED, resolution = res, resolutionDetail = detail, resolvedAtMs = nowMs())
        return when (row.kind) {
            PendingKind.MANUAL_DOSE -> when (val r = ops.call(s, M.GET_RESULT, mapOf("seq" to checkNotNull(row.seq)), decode = Records::manualResult)) {
                is OpResult.Ok -> true.also { resolveDose(row, r.value.state, r.value.detail) }
                is OpResult.Rejected -> true
                else -> false
            }
            PendingKind.REFILL -> when (val r = readStatus(s)) {
                // read back, never re-sent: the user decides with the device's current values
                is OpResult.Ok -> true.also { journal.update(done(Resolution.READ_BACK, r.value.remainingUl)) }
                is OpResult.Rejected -> true
                else -> false
            }
            PendingKind.SET_READINESS -> when (val r = readStatus(s)) {
                is OpResult.Ok -> true.also { journal.update(done(if (r.value.readiness?.name == row.target) Resolution.APPLIED else Resolution.NOT_APPLIED)) }
                is OpResult.Rejected -> true
                else -> false
            }
            PendingKind.SET_CONFIG -> when (val r = readConfig(s)) {
                is OpResult.Ok -> true.also {
                    val req = decodeRequest(M.SET_CONFIG, checkNotNull(row.payload))
                    @Suppress("UNCHECKED_CAST")
                    val want = Records.configRecord(req["config"] as Map<String, Any>)
                    val mask = (req["field_mask"] as Long).toInt()
                    journal.update(done(if (configMatches(r.value, want, mask)) Resolution.APPLIED else Resolution.NOT_APPLIED))
                }
                is OpResult.Rejected -> true
                else -> false
            }
            PendingKind.PHONE_REMOVE -> when (val r = readPhones(s)) {
                is OpResult.Ok -> true.also {
                    journal.update(done(if (r.value.none { it.slot.toString() == row.target }) Resolution.APPLIED else Resolution.NOT_APPLIED))
                }
                is OpResult.Rejected -> true
                else -> false
            }
            PendingKind.CREATE_SCHEDULE, PendingKind.UPDATE_SCHEDULE, PendingKind.DELETE_SCHEDULE -> {
                val id = checkNotNull(row.target)
                val r = ops.call(s, M.GET_SCHEDULE, mapOf("id" to id.toByteArray()), decode = Records::schedule)
                val found = (r as? OpResult.Ok)?.value
                val notFound = r is OpResult.Rejected && r.error.code == DeviceErrorCode.NOT_FOUND
                if (found == null && !notFound) return r !is OpResult.NotSent && r !is OpResult.Uncertain
                val sent: ScheduleRecord? = row.payload?.takeIf { row.kind != PendingKind.DELETE_SCHEDULE }?.let { p ->
                    val req = decodeRequest(if (row.kind == PendingKind.CREATE_SCHEDULE) M.CREATE_SCHEDULE else M.UPDATE_SCHEDULE, p)
                    @Suppress("UNCHECKED_CAST")
                    Records.scheduleRecord(req["schedule"] as Map<String, Any>)
                }
                val res = when (row.kind) {
                    PendingKind.CREATE_SCHEDULE -> when {
                        found == null -> Resolution.NOT_APPLIED
                        DeviceScheduleMapping.sameDefinition(found, checkNotNull(sent)) -> Resolution.APPLIED
                        else -> Resolution.CHANGED_ELSEWHERE
                    }
                    PendingKind.UPDATE_SCHEDULE -> when {
                        found == null -> Resolution.CHANGED_ELSEWHERE
                        found.revision == row.expectedRevision!! + 1 && DeviceScheduleMapping.sameDefinition(found, checkNotNull(sent)) -> Resolution.APPLIED
                        found.revision == row.expectedRevision -> Resolution.NOT_APPLIED
                        else -> Resolution.CHANGED_ELSEWHERE
                    }
                    else -> when {
                        found == null -> Resolution.APPLIED
                        found.revision == row.expectedRevision -> Resolution.NOT_APPLIED
                        else -> Resolution.CHANGED_ELSEWHERE
                    }
                }
                journal.update(done(res, found?.revision))
                true
            }
        }
    }

    private fun configMatches(have: ConfigRecord, want: ConfigRecord, mask: Int): Boolean {
        fun bit(b: Int, eq: Boolean) = mask and b == 0 || eq
        return bit(ConfigRecord.F_CAPACITY, have.reservoirCapacityUl == want.reservoirCapacityUl) &&
            bit(ConfigRecord.F_LOW_STOCK, have.lowStockPercent == want.lowStockPercent) &&
            bit(ConfigRecord.F_SYNC_INTERVAL, have.timeSyncIntervalH == want.timeSyncIntervalH) &&
            bit(ConfigRecord.F_NOTIFY, have.notifyMask == want.notifyMask) &&
            bit(ConfigRecord.F_FLOW, have.pumpFlowUlPerS == want.pumpFlowUlPerS) &&
            bit(ConfigRecord.F_CALIBRATED_AT, have.calibratedAt == want.calibratedAt) &&
            bit(ConfigRecord.F_MAX_DOSE, have.maxDoseUl == want.maxDoseUl) &&
            bit(ConfigRecord.F_MAX_DAILY, have.maxDailyUl == want.maxDailyUl) &&
            bit(ConfigRecord.F_MAX_RUNTIME, have.maxRuntimeMs == want.maxRuntimeMs) &&
            bit(ConfigRecord.F_MIN_INTERVAL, have.minIntervalS == want.minIntervalS)
    }

    // ---- cache encoding (the device's own response bodies) -------------------------------------

    private suspend fun loadCache() {
        fun <V> load(flow: MutableStateFlow<Stamped<V>?>, e: CacheEntry?, decode: (ByteArray) -> V) {
            if (e == null) return
            val v = try {
                decode(e.body)
            } catch (ex: RuntimeException) {
                return // an unreadable cache entry is simply absent
            }
            flow.compareAndSet(null, Stamped(v, e.readAtMs, null)) // never overrides a value read meanwhile
        }
        try {
            load(statusS, cache.get(deviceId, KEY_STATUS)) { Records.status(dec(M.GET_STATUS, it)) }
            load(configS, cache.get(deviceId, KEY_CONFIG)) { Records.config(dec(M.GET_CONFIG, it)) }
            load(diagS, cache.get(deviceId, KEY_DIAG)) { Records.diagnostics(dec(M.GET_DIAGNOSTICS, it)) }
            load(phonesS, cache.get(deviceId, KEY_PHONES)) { Records.phones(dec(M.PHONES_LIST, it)) }
            load(schedulesS, cache.get(deviceId, KEY_SCHEDULES), ::unpackSchedules)
        } catch (e: Exception) {
            // no offline view; nothing else depends on it
        }
    }

    private fun enc(type: Int, fields: Map<String, Any>): ByteArray = BleProtocol.encodeBody(type, BleProtocol.F_RESPONSE, fields)
    private fun dec(type: Int, body: ByteArray): Map<String, Any> =
        BleProtocol.decodeBody(BleHeader(BleProtocol.MAJOR, BleProtocol.MINOR, type, BleProtocol.F_RESPONSE, 0, body.size), body)
    private fun decodeRequest(type: Int, body: ByteArray): Map<String, Any> =
        BleProtocol.decodeBody(BleHeader(BleProtocol.MAJOR, BleProtocol.MINOR, type, 0, 0, body.size), body)

    private fun packSchedules(setRevision: Long, bodies: List<ByteArray>): ByteArray {
        val b = ByteBuffer.allocate(12 + bodies.sumOf { 2 + it.size }).order(ByteOrder.LITTLE_ENDIAN)
        b.putLong(setRevision).putInt(bodies.size)
        bodies.forEach { b.putShort(it.size.toShort()).put(it) }
        return b.array()
    }

    private fun unpackSchedules(raw: ByteArray): DeviceSchedules {
        val b = ByteBuffer.wrap(raw).order(ByteOrder.LITTLE_ENDIAN)
        val setRev = b.getLong()
        var unreadable = 0
        val list = List(b.getInt()) {
            val body = ByteArray(b.getShort().toInt() and 0xFFFF).also { a -> b.get(a) }
            Records.schedule(dec(M.GET_SCHEDULE, body))
        }.mapNotNull { r -> DeviceScheduleMapping.toSchedule(r)?.let { DeviceSchedule(it, r) } ?: run { unreadable++; null } }
        return DeviceSchedules(setRev, list, unreadable)
    }

    private fun <A> OpResult<*>.cast(): OpResult<A> = when (this) {
        is OpResult.Ok -> error("not a failure")
        is OpResult.Rejected -> this
        is OpResult.Refused -> this
        is OpResult.NotSent -> this
        is OpResult.Uncertain -> this
    }

    companion object {
        const val MAX_TIME_UNCERTAINTY_MS = 2_000L
        private const val KEY_STATUS = "status"
        /** The one manual-dose reservation (new dose, re-send, discard). */
        private const val KEY_DOSE = "MANUAL_DOSE"
        private const val KEY_CONFIG = "config"
        private const val KEY_DIAG = "diagnostics"
        private const val KEY_PHONES = "phones"
        private const val KEY_SCHEDULES = "schedules"
    }
}
