package cz.nowass.dispenser.device.session

import cz.nowass.dispenser.ble.protocol.BleCodecException
import cz.nowass.dispenser.ble.protocol.BleHeader
import cz.nowass.dispenser.ble.protocol.BleProtocol
import cz.nowass.dispenser.crypto.HandshakeInitiator
import cz.nowass.dispenser.crypto.NoiseException
import cz.nowass.dispenser.crypto.NoiseProvider
import cz.nowass.dispenser.crypto.Purpose
import cz.nowass.dispenser.crypto.SecretBytes
import cz.nowass.dispenser.crypto.SessionDerivation
import cz.nowass.dispenser.crypto.TransportCiphers
import cz.nowass.dispenser.crypto.credentials.CredentialState
import cz.nowass.dispenser.crypto.credentials.CredentialStore
import cz.nowass.dispenser.crypto.credentials.PendingEnrollment
import cz.nowass.dispenser.device.link.FrameLink
import cz.nowass.dispenser.device.link.LinkEvent
import cz.nowass.dispenser.device.transport.CloseReason
import cz.nowass.dispenser.device.transport.TransportClosedException
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withTimeoutOrNull
import java.security.MessageDigest
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicInteger

/**
 * One secure session on one connection (docs/BLE_PROTOCOL.md 10.6–11; docs/ANDROID_BLE_DESIGN.md
 * 3.2, 4.3–4.5, 5, 11.6–11.7). Created by [open]; bound to one [FrameLink] generation; never reused
 * after a disconnect (C7). The phone is the Noise initiator; every cryptographic operation is the
 * native provider's.
 *
 * - Frames are routed by `txn` to the waiting step or request; plain responses are accepted only
 *   before authentication (HELLO, HS, and the HANDSHAKE_FAILED answer to XX message 3); after it,
 *   a plaintext frame, an undecryptable frame or a fragment error closes the session.
 * - Encrypted requests: one in flight, at most [MAX_WAITING] waiting, retransmitted with the same
 *   `txn` and identical plaintext after a timeout (protocol 10.4a; the device answers from its
 *   response cache); a later duplicate response is discarded (the first wins). A request without
 *   a response is [RequestResult.Uncertain] and the session closes (nonce state not trusted).
 * - Closing (any path) frees the Noise state (native memory zeroized) and closes the link.
 */
class SecureSession private constructor(
    private val link: FrameLink,
    scope: CoroutineScope,
    private val timeouts: SessionTimeouts,
    /** The device this session was opened for. */
    val deviceIdHex: String,
) {
    private class Incoming(val header: BleHeader, val fields: Map<String, Any>, val encrypted: Boolean)

    private var txn = 0
    private val waiters = ConcurrentHashMap<Int, CompletableDeferred<Incoming>>()
    private val sendLock = Mutex()
    private val waiting = AtomicInteger(0)
    @Volatile private var ciphers: TransportCiphers? = null
    @Volatile private var authenticated = false
    // a short replay: events between authentication and the manager's subscription are not lost
    private val eventFlow = MutableSharedFlow<DeviceEvent>(replay = 4, extraBufferCapacity = 32)
    private val closedState = MutableStateFlow<String?>(null)
    @Volatile private var closeListener: ((SecureSession) -> Unit)? = null

    /** Encrypted events of this session (EV_SESSION_CLOSING, EV_SUSPENDING_FOR_DOSE, EV_CONFIRM_ON_DEVICE). */
    val events: SharedFlow<DeviceEvent> = eventFlow.asSharedFlow()
    /** Null while open; why it closed. */
    val closed: StateFlow<String?> = closedState.asStateFlow()
    val generation: Long get() = link.generation
    val isOpen: Boolean get() = closedState.value == null
    /** Responses that matched no waiter (duplicates after a retransmission; diagnostics). */
    val discardedResponses = AtomicInteger(0)

    /** The HELLO response of this connection (set by [open] before the handshake). */
    lateinit var hello: HelloInfo
        private set

    /** EV_CONFIRM_ON_DEVICE of a recovery (completed whenever it arrives, so it cannot be missed). */
    private val confirmEvent = CompletableDeferred<DeviceEvent.ConfirmOnDevice>()

    init {
        scope.launch {
            link.events.collect { e ->
                when (e) {
                    is LinkEvent.Frame -> receive(e.bytes)
                    is LinkEvent.FragmentError -> close("fragment error ${e.result.wire}")
                    is LinkEvent.Closed -> close(describe(e.reason))
                }
            }
        }
    }

    private fun receive(frame: ByteArray) {
        if (!isOpen) return
        val parsed = try {
            BleProtocol.decode(frame)
        } catch (e: BleCodecException) {
            close("malformed frame ${e.result.wire}")
            return
        }
        val h = parsed.header
        val encrypted = h.flags and BleProtocol.F_ENCRYPTED != 0
        val incoming = if (encrypted) {
            val c = ciphers ?: return close("encrypted frame before the handshake")
            val plain = try {
                c.decrypt(frame.copyOf(BleProtocol.HEADER_LEN), frame.copyOfRange(BleProtocol.HEADER_LEN, frame.size))
            } catch (e: NoiseException) {
                // tampering, or nonces out of step after a lost frame (10.4a): never answered, never
                // retried in this session; the caller reconciles in a new one
                return close("decryption failed")
            }
            val ph = h.copy(flags = h.flags and BleProtocol.F_ENCRYPTED.inv(), len = plain.size)
            val fields = try {
                BleProtocol.decodeBody(ph, plain)
            } catch (e: BleCodecException) {
                return close("malformed encrypted body ${e.result.wire}")
            } finally {
                plain.fill(0)
            }
            Incoming(ph, fields, true)
        } else {
            if (authenticated) return close("plaintext after authentication")
            Incoming(h, checkNotNull(parsed.fields), false)
        }
        val f = incoming.header.flags
        when {
            f and BleProtocol.F_EVENT != 0 -> if (incoming.encrypted) emitEvent(incoming) else close("plaintext event")
            f and BleProtocol.F_RESPONSE != 0 -> {
                val w = waiters.remove(incoming.header.txn)
                if (w != null) w.complete(incoming) else discardedResponses.incrementAndGet()
            }
            else -> close("request from the device")
        }
    }

    private fun emitEvent(i: Incoming) {
        val ev = when (i.header.type) {
            T_EV_SESSION_CLOSING -> DeviceEvent.SessionClosing(num(i.fields["reason"]), num(i.fields["seconds_left"]))
            T_EV_SUSPENDING -> DeviceEvent.SuspendingForDose(num(i.fields["seconds_until"]))
            T_EV_CONFIRM -> DeviceEvent.ConfirmOnDevice(num(i.fields["action"]), num(i.fields["seconds"]))
            else -> null
        }
        if (ev is DeviceEvent.ConfirmOnDevice) confirmEvent.complete(ev)
        if (ev != null) eventFlow.tryEmit(ev)
    }

    /**
     * An encrypted request (authenticated sessions only). Returns the response ([RequestResult.Ok]),
     * the device's error, [RequestResult.NotSent] / [RequestResult.Busy] (never left the phone) or
     * [RequestResult.Uncertain] (may have been executed; this session is then closed).
     *
     * Retries (BLE_PROTOCOL.md 10.4a, docs/ANDROID_BLE_DESIGN.md 14.3): after [timeoutMs] the SAME
     * plaintext (same body, so the same manual-dose `seq`) is retransmitted with the SAME `txn`, at
     * most [retries] times. Transport nonces are implicit, so this recovers a delayed response only;
     * after a request the device dropped before decrypting it, or a response lost on an open link,
     * the nonces are out of step: the device or this phone fails the next decryption and closes.
     * When every attempt timed out the nonce state is unknown too, so the session is closed here.
     * Nothing is ever retried in another session.
     */
    suspend fun request(type: Int, fields: Map<String, Any> = emptyMap(), retries: Int = 2, timeoutMs: Long = timeouts.requestMs): RequestResult {
        if (waiting.incrementAndGet() > MAX_WAITING) {
            waiting.decrementAndGet()
            return RequestResult.Busy
        }
        try {
            return sendLock.withLock {
                if (!isOpen || !authenticated) return@withLock RequestResult.NotSent(closedState.value ?: "not authenticated")
                val t = nextTxn()
                val plain = BleProtocol.encodeBody(type, 0, fields) // encoded once: every attempt is identical
                // NotSent only while no attempt has entered link.send (the first byte may go out there);
                // from then on every outcome without a response is Uncertain
                var mayHaveSent = 0
                try {
                    for (attempt in 1..retries + 1) {
                        val d = CompletableDeferred<Incoming>()
                        waiters[t] = d
                        val frame = try {
                            encryptFrame(type, t, plain) // not suspending: no cancellation here
                        } catch (e: Exception) {
                            // no transport keys (closed concurrently), ciphers freed, or a Noise failure:
                            // nothing of this attempt was written; the cipher state is not trusted any more
                            val why = closedState.value ?: (e as? SessionClosedException)?.reason ?: "encryption failed"
                            close(why)
                            return@withLock if (mayHaveSent == 0) RequestResult.NotSent(why) else RequestResult.Uncertain(why, mayHaveSent)
                        }
                        mayHaveSent = attempt
                        try {
                            link.send(frame)
                        } catch (e: CancellationException) {
                            close("request cancelled: outcome unknown") // possibly a partial frame on the link
                            throw e
                        } catch (e: Exception) {
                            // the session's own close reason first: a close (e.g. a decryption failure) aborts a pending write
                            val why = closedState.value ?: (e as? TransportClosedException)?.let { describe(it.reason) }
                                ?: "send failed: ${e.javaClass.simpleName}"
                            close(why)
                            return@withLock RequestResult.Uncertain(why, attempt) // may have been written
                        }
                        val r = try {
                            withTimeoutOrNull(timeoutMs) { d.await() }
                        } catch (e: SessionClosedException) {
                            return@withLock RequestResult.Uncertain(e.reason, attempt)
                        } catch (e: CancellationException) {
                            close("request cancelled: outcome unknown")
                            throw e
                        }
                        if (r != null) {
                            return@withLock if (r.header.flags and BleProtocol.F_ERROR != 0) {
                                RequestResult.DeviceError(num(r.fields["code"]), long(r.fields["detail"]), long(r.fields["aux"]))
                            } else {
                                RequestResult.Ok(r.fields, attempt)
                            }
                        }
                        waiters.remove(t, d)
                    }
                    close(REASON_TIMEOUT)
                    RequestResult.Uncertain("timeout", retries + 1)
                } finally {
                    plain.fill(0)
                }
            }
        } finally {
            waiting.decrementAndGet()
        }
    }

    /**
     * Ends the session: an authenticated owner's encrypted END_SESSION (ends the device's service
     * window), then the connection is closed. The response is best effort (protocol 11.3): a lost
     * one changes nothing.
     */
    suspend fun end() {
        if (isOpen && authenticated) request(T_END_SESSION, retries = 0, timeoutMs = END_RESPONSE_MS)
        close("ended")
    }

    /**
     * Idempotent: frees the Noise state, fails every waiter, closes the link. Every call — the first
     * and any concurrent or later one — runs the close listener before it returns, so when any
     * close() returns, the owner no longer exposes this session (D2).
     */
    fun close(reason: String) {
        val first = synchronized(this) {
            if (closedState.value != null) {
                false
            } else {
                closedState.value = reason
                true
            }
        }
        if (first) {
            authenticated = false
            ciphers?.close()
            ciphers = null
        }
        // outside this session's lock; the listener is idempotent and never blocks
        closeListener?.invoke(this)
        if (!first) return
        val ex = SessionClosedException(reason)
        confirmEvent.completeExceptionally(ex)
        waiters.values.forEach { it.completeExceptionally(ex) }
        waiters.clear()
        link.close(CloseReason.Local)
    }

    /**
     * The owner's synchronous invalidation hook (one owner): run by every [close] before it returns,
     * outside any lock of this session; run at once if the session is already closed. Must be
     * idempotent and non-blocking (it may run more than once, also concurrently).
     */
    internal fun setCloseListener(listener: (SecureSession) -> Unit) {
        closeListener = listener
        if (!isOpen) listener(this)
    }

    // ---- framing -----------------------------------------------------------------------------

    private fun nextTxn(): Int {
        txn = if (txn >= 0xFFFF) 1 else txn + 1
        return txn
    }

    private suspend fun sendPlain(type: Int, t: Int, fields: Map<String, Any>) = link.send(BleProtocol.encode(type, 0, t, fields))

    /** Encrypts one request frame; writes nothing. */
    private fun encryptFrame(type: Int, t: Int, plain: ByteArray): ByteArray {
        val c = ciphers ?: throw SessionClosedException("no transport keys")
        val len = plain.size + BleProtocol.TAG_LEN
        val header = byteArrayOf(1, 0, type.toByte(), BleProtocol.F_ENCRYPTED.toByte(), t.toByte(), (t shr 8).toByte(), len.toByte(), (len shr 8).toByte())
        return header + c.encrypt(header, plain)
    }

    private suspend fun await(d: CompletableDeferred<Incoming>, timeoutMs: Long, step: String): Incoming =
        try {
            withTimeoutOrNull(timeoutMs) { d.await() } ?: throw StepFailed(SessionFailure.Timeout(step))
        } catch (e: SessionClosedException) {
            throw StepFailed(SessionFailure.Transport(CloseReason.Disconnected(-1)))
        }

    /** HS message: plain request, answer HS response (or a plain error = HANDSHAKE_FAILED). */
    private suspend fun hsExchange(purpose: Purpose, index: Int, noise: ByteArray, expectResponse: Boolean): ByteArray? {
        val t = nextTxn()
        val d = CompletableDeferred<Incoming>()
        if (expectResponse) waiters[t] = d
        sendPlain(T_HS, t, mapOf("purpose" to purpose.wire, "msg_index" to index, "noise" to noise))
        if (!expectResponse) return null
        val r = await(d, timeouts.handshakeStepMs, "handshake message ${index + 1}")
        if (r.header.flags and BleProtocol.F_ERROR != 0) throw StepFailed(SessionFailure.HandshakeFailed)
        if (r.encrypted || r.header.type != T_HS || num(r.fields["msg_index"]) != index + 1) throw StepFailed(SessionFailure.ProtocolError("unexpected HS response"))
        return r.fields["noise"] as ByteArray
    }

    private class StepFailed(val failure: SessionFailure) : Exception()
    class SessionClosedException(val reason: String) : Exception("session closed: $reason")

    companion object {
        const val MAX_WAITING = 8
        /** Close reason after a request whose every attempt timed out (nonce state unknown). */
        const val REASON_TIMEOUT = "request timed out: nonce state unknown"
        private const val END_RESPONSE_MS = 2_000L
        const val T_HELLO = 0x01
        const val T_HS = 0x02
        const val T_ENROLL_RESULT = 0x08
        const val T_PHONES_LIST = 0x70
        const val T_END_SESSION = 0x7E
        private const val T_EV_SESSION_CLOSING = 0x80
        private const val T_EV_SUSPENDING = 0x81
        private const val T_EV_CONFIRM = 0x83

        private fun num(v: Any?): Int = (v as? Number)?.toInt() ?: -1
        private fun long(v: Any?): Long = (v as? Number)?.toLong() ?: -1

        private fun describe(r: CloseReason): String = "transport: $r"

        /**
         * Opens a session on [link]: HELLO, then the handshake of [request]. On success the session
         * is authenticated; on failure it is closed (and the link with it). [onConfirming] is called
         * when a recovering device asks for the three presses.
         */
        suspend fun open(
            link: FrameLink,
            request: OpenRequest,
            noise: NoiseProvider,
            store: CredentialStore,
            scope: CoroutineScope,
            timeouts: SessionTimeouts = SessionTimeouts(),
            nowUtc: () -> Long = { System.currentTimeMillis() / 1000 },
            onConfirming: (Int) -> Unit = {},
        ): OpenResult {
            val s = SecureSession(link, scope, timeouts, request.deviceIdHex)
            return try {
                val h = s.exchangeHello()
                s.hello = h
                val slot = when (request) {
                    is OpenRequest.Session -> s.ik(request.deviceIdHex, h, noise, store, staged = false)
                    is OpenRequest.ConfirmStaged -> s.ik(request.deviceIdHex, h, noise, store, staged = true)
                    is OpenRequest.Claim -> s.enroll(Purpose.CLAIM, request.label.setupCode, null, null, request.name, h, noise, store, nowUtc, onConfirming)
                    is OpenRequest.Add -> s.enroll(Purpose.ADD, request.label.setupCode, request.invitation.inviteToken, request.invitation,
                        request.name, h, noise, store, nowUtc, onConfirming)
                    is OpenRequest.Recovery -> s.enroll(Purpose.RECOVERY, request.label.setupCode, null, null, request.name, h, noise, store, nowUtc, onConfirming)
                }
                OpenResult.Opened(s, slot)
            } catch (e: StepFailed) {
                s.close("open failed: ${e.failure}")
                OpenResult.Failed(e.failure)
            } catch (e: TransportClosedException) {
                s.close(describe(e.reason))
                OpenResult.Failed(SessionFailure.Transport(e.reason))
            }
        }
    }

    private suspend fun exchangeHello(): HelloInfo {
        val t = nextTxn() // 1
        val d = CompletableDeferred<Incoming>()
        waiters[t] = d
        sendPlain(T_HELLO, t, mapOf("proto_min" to 1, "proto_max" to 1))
        val r = await(d, timeouts.helloMs, "hello")
        if (r.header.flags and BleProtocol.F_ERROR != 0) throw StepFailed(SessionFailure.HelloRejected(num(r.fields["code"])))
        if (r.header.type != T_HELLO || r.encrypted || num(r.fields["proto"]) != 1) throw StepFailed(SessionFailure.ProtocolError("unexpected HELLO response"))
        return HelloInfo(num(r.fields["proto"]), num(r.fields["claim_state"]), num(r.fields["flags"]), long(r.fields["caps"]),
            long(r.fields["session_id"]), r.fields["device_id_hash"] as ByteArray)
    }

    private fun prologue(purpose: Purpose, h: HelloInfo) = SessionDerivation.prologue(h.proto, BleProtocol.MINOR, purpose, h.deviceIdHash, h.sessionId)

    /** IK SESSION with the stored (or staged) credential; returns the slot when known. */
    private suspend fun ik(deviceIdHex: String, h: HelloInfo, noise: NoiseProvider, store: CredentialStore, staged: Boolean): Int? {
        val st = if (staged) store.loadPending(deviceIdHex) else store.load(deviceIdHex)
        val cred = when (st) {
            is CredentialState.Present -> st.credential
            CredentialState.Absent -> throw StepFailed(if (staged) SessionFailure.NothingStaged else SessionFailure.NoCredential)
            CredentialState.Lost -> throw StepFailed(SessionFailure.CredentialLost)
            is CredentialState.Unreadable -> throw StepFailed(SessionFailure.CredentialUnreadable)
        }
        cred.use { c ->
            if (!h.claimed || h.credentialsDamaged) throw StepFailed(if (staged) SessionFailure.NothingStaged else SessionFailure.NotClaimed)
            // the device must be the pinned identity (an identity check only; IK authenticates)
            if (!MessageDigest.isEqual(h.deviceIdHash, SessionDerivation.claimedDeviceIdHash(c.deviceStaticPublic))) {
                throw StepFailed(if (staged) SessionFailure.NothingStaged else SessionFailure.WrongDevice)
            }
            val hs = noise.newInitiator(Purpose.SESSION, prologue(Purpose.SESSION, h), c.phoneStaticPrivate, c.deviceStaticPublic, null)
            hs.use {
                val m2 = hsExchange(Purpose.SESSION, 0, hs.writeMessage(), true)!!
                try {
                    hs.readMessage(m2)
                } catch (e: NoiseException) {
                    throw StepFailed(SessionFailure.HandshakeFailed)
                }
                if (!hs.isFinished) throw StepFailed(SessionFailure.ProtocolError("IK not finished"))
                ciphers = hs.split()
            }
            authenticated = true
            if (!staged) return c.slot.takeIf { it >= 0 }
        }
        // staged: the IK success proves the device stored it; the slot comes from PHONES_LIST
        val r = request(T_PHONES_LIST)
        val slot = ((r as? RequestResult.Ok)?.fields?.get("items") as? List<*>)
            ?.mapNotNull { it as? Map<*, *> }?.firstOrNull { it["is_self"] == true }?.get("slot")?.let { num(it) }
            ?: throw StepFailed(SessionFailure.ProtocolError("own slot not reported"))
        store.confirmPending(deviceIdHex, slot)
        return slot
    }

    /** XXpsk3 CLAIM / ADD / RECOVERY; returns the enrolled slot. */
    private suspend fun enroll(
        purpose: Purpose, setupCode: SecretBytes, token: SecretBytes?, invitation: cz.nowass.dispenser.crypto.Invitation?,
        name: String, h: HelloInfo, noise: NoiseProvider, store: CredentialStore, nowUtc: () -> Long, onConfirming: (Int) -> Unit,
    ): Int {
        val idBytes = ByteArray(6) { deviceIdHex.substring(2 * it, 2 * it + 2).toInt(16).toByte() }
        when (purpose) {
            Purpose.CLAIM -> {
                if (h.claimed || h.credentialsDamaged) throw StepFailed(SessionFailure.NotUnclaimed)
                if (!MessageDigest.isEqual(h.deviceIdHash, SessionDerivation.unclaimedDeviceIdHash(idBytes))) throw StepFailed(SessionFailure.WrongDevice)
            }
            Purpose.ADD -> {
                val inv = checkNotNull(invitation)
                if (!h.claimed) throw StepFailed(SessionFailure.NotClaimed)
                if (!inv.deviceId.contentEquals(idBytes) || !MessageDigest.isEqual(h.deviceIdHash, inv.expectedDeviceIdHash)) {
                    throw StepFailed(SessionFailure.WrongDevice)
                }
            }
            Purpose.RECOVERY -> if (!h.recoveryArmed) throw StepFailed(SessionFailure.RecoveryNotArmed)
            Purpose.SESSION -> error("not an enrollment")
        }
        val nameBytes = name.toByteArray(Charsets.UTF_8)
        require(nameBytes.size <= 24) { "name longer than 24 bytes" }
        val pending: PendingEnrollment = store.beginEnrollment(deviceIdHex)
        var hs: HandshakeInitiator? = null
        try {
            hs = noise.derivePsk(purpose, setupCode, token).use { psk ->
                noise.newInitiator(purpose, prologue(purpose, h), pending.keyPair.privateKey, null, psk)
            }
            val m2 = hsExchange(purpose, 0, hs.writeMessage(), true)!!
            try {
                hs.readMessage(m2)
            } catch (e: NoiseException) {
                throw StepFailed(SessionFailure.HandshakeFailed)
            }
            val deviceKey = hs.remoteStatic()
            if (invitation != null && !invitation.matchesDeviceKey(deviceKey)) throw StepFailed(SessionFailure.DeviceKeyMismatch)
            // durable BEFORE message 3: the device stores the enrollment when it accepts message 3 (11.7)
            store.stage(pending, deviceKey, name, nowUtc())
            val m3 = hs.writeMessage(byteArrayOf(nameBytes.size.toByte()) + nameBytes)
            ciphers = hs.split()
            hs = null
            val t = nextTxn()
            val d = CompletableDeferred<Incoming>()
            waiters[t] = d
            try {
                sendPlain(T_HS, t, mapOf("purpose" to purpose.wire, "msg_index" to 2, "noise" to m3))
            } catch (e: TransportClosedException) {
                // message 3 may have reached the device (it stores the enrollment on accepting it):
                // the outcome is unknown, never a plain transport failure (11.7)
                store.release(pending)
                throw StepFailed(SessionFailure.EnrollmentUnconfirmed)
            }
            var wait = timeouts.enrollResultMs
            if (purpose == Purpose.RECOVERY) {
                val confirm = withTimeoutOrNull(timeouts.handshakeStepMs) {
                    try {
                        confirmEvent.await()
                    } catch (e: SessionClosedException) {
                        null
                    }
                }
                if (confirm != null) {
                    onConfirming(confirm.seconds)
                    wait = confirm.seconds * 1000L + timeouts.confirmMarginMs
                }
            }
            val r = try {
                withTimeoutOrNull(wait) { d.await() }
            } catch (e: SessionClosedException) {
                null
            }
            if (r == null) {
                store.release(pending) // ENROLL_RESULT lost: staged, resolved later (11.7)
                throw StepFailed(SessionFailure.EnrollmentUnconfirmed)
            }
            if (!r.encrypted) {
                // plain HANDSHAKE_FAILED to message 3 (e.g. a wrong setup code): unauthenticated, so the
                // staged record stays until the user discards it explicitly
                store.release(pending)
                throw StepFailed(SessionFailure.HandshakeFailed)
            }
            if (r.header.type != T_ENROLL_RESULT) throw StepFailed(SessionFailure.ProtocolError("expected ENROLL_RESULT"))
            if (r.header.flags and BleProtocol.F_ERROR != 0) {
                store.release(pending)
                throw StepFailed(SessionFailure.EnrollmentRefused(num(r.fields["code"]), long(r.fields["detail"])))
            }
            val slot = num(r.fields["slot"])
            store.commit(pending, slot, deviceKey, name, nowUtc()).close()
            authenticated = true
            return slot
        } catch (e: StepFailed) {
            if (!pending.staged) store.abort(pending)
            throw e
        } catch (e: Exception) {
            if (!pending.staged) store.abort(pending) else store.release(pending)
            throw e
        } finally {
            hs?.close()
        }
    }

}
