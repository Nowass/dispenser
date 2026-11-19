package cz.nowass.dispenser.device.session

import cz.nowass.dispenser.crypto.NoiseProvider
import cz.nowass.dispenser.crypto.credentials.CredentialStore
import cz.nowass.dispenser.device.link.FrameLink
import cz.nowass.dispenser.device.transport.ConnectResult
import cz.nowass.dispenser.device.transport.GattTransportFactory
import java.util.concurrent.atomic.AtomicReference
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

/**
 * The process-wide owner of the single live device session (docs/ANDROID_BLE_DESIGN.md 2.2, 4.4,
 * 11.5–11.6). Not tied to any screen: ViewModels observe [state] and call [request]; nothing here is
 * persisted except through the [CredentialStore] (a process death loses the live session by design;
 * staged enrollments are resolved with [OpenRequest.ConfirmStaged] afterwards).
 *
 * - One session at a time: [open] while one is live fails with [SessionFailure.AlreadyActive].
 * - Every connection is a new transport generation, a new HELLO and a new handshake; a session that
 *   closed (disconnect, error, [end]) is never reused, and its requests and events can never reach
 *   a later session.
 */
class DeviceSessionManager(
    private val transports: GattTransportFactory,
    private val noise: NoiseProvider,
    private val store: CredentialStore,
    private val scope: CoroutineScope,
    private val timeouts: SessionTimeouts = SessionTimeouts(),
    private val nowUtc: () -> Long = { System.currentTimeMillis() / 1000 },
) {
    private val openLock = Mutex()
    /** The adopted session; cleared only by compare-and-set, so a late close of an older session never clears a newer one. */
    private val current = AtomicReference<SecureSession?>(null)
    private val stateFlow = MutableStateFlow<SessionState>(SessionState.Idle)
    private val eventFlow = MutableSharedFlow<DeviceEvent>(extraBufferCapacity = 32)

    val state: StateFlow<SessionState> = stateFlow.asStateFlow()
    /** Events of the live session only. */
    val events: SharedFlow<DeviceEvent> = eventFlow.asSharedFlow()
    /** The live session, if any (for diagnostics and tests). */
    val session: SecureSession? get() = current.get()?.takeIf { it.isOpen }

    /**
     * Devices being forgotten (F1), with the number of Forget runs in progress: overlapping runs keep the
     * device blocked until the LAST one ends (review of 78f5b97). No session may be opened or adopted.
     */
    private val forgetsInProgress = java.util.concurrent.ConcurrentHashMap<String, Int>()
    private fun isBlocked(id: String): Boolean = forgetsInProgress.containsKey(id)

    /**
     * Per device, how many times "Forget device" has started (F4). An open that began before (or while)
     * Forget started for its device is refused and never adopted, even if it ran to completion.
     */
    private val forgetEpochs = java.util.concurrent.ConcurrentHashMap<String, Long>()
    private fun epochOf(id: String): Long = forgetEpochs[id] ?: 0L

    /**
     * Runs [cleanup] for "Forget device" of [deviceIdHex] exclusively with every open() (security review
     * F4): new and queued opens of the device are refused; an open already in progress is waited for
     * (it is bounded by its own timeouts) and then neither adopted nor able to run while [cleanup]
     * deletes credentials, so no credential can be created or restored for the device meanwhile.
     * [cleanup] must not call open() (it would wait for itself); ending or closing sessions is fine.
     */
    suspend fun <T> forgetting(deviceIdHex: String, cleanup: suspend () -> T): T {
        val id = deviceIdHex.lowercase()
        forgetsInProgress.merge(id, 1, Int::plus)
        forgetEpochs.merge(id, 1L, Long::plus)
        try {
            return openLock.withLock { cleanup() }
        } finally {
            // atomic decrement; the entry (the block) disappears only when the last run ends
            forgetsInProgress.computeIfPresent(id) { _, n -> if (n > 1) n - 1 else null }
        }
    }

    private fun liveFor(deviceIdHex: String): SecureSession? =
        current.get()?.takeIf { it.isOpen && it.deviceIdHex.equals(deviceIdHex, ignoreCase = true) }

    /** Encrypted END_SESSION, then close — only if the live session belongs to [deviceIdHex]. */
    suspend fun endFor(deviceIdHex: String) {
        liveFor(deviceIdHex)?.end()
    }

    /**
     * Hard close of the live session of [deviceIdHex] — never another device's — synchronous and
     * non-suspending (cannot be cancelled); the manager stops exposing it before this returns (D2).
     */
    fun closeNow(deviceIdHex: String, reason: String = "closed by the app") {
        liveFor(deviceIdHex)?.close(reason)
    }

    /** True while an authenticated session of [deviceIdHex] is exposed. */
    fun hasLiveSession(deviceIdHex: String): Boolean =
        liveFor(deviceIdHex) != null || (state.value as? SessionState.Authenticated)?.deviceIdHex.equals(deviceIdHex, ignoreCase = true)

    /** Connects to [address] (a locator, never an identity) and opens [request]. */
    suspend fun open(address: String, request: OpenRequest): OpenResult {
        val id = request.deviceIdHex.lowercase()
        val startEpoch = epochOf(id)
        // started while the device is being forgotten: refused at once (never queued behind Forget)
        if (isBlocked(id)) return OpenResult.Failed(SessionFailure.Blocked)
        return openLock.withLock { openLocked(address, request, id, startEpoch) }
    }

    private fun forgottenSince(id: String, startEpoch: Long) = isBlocked(id) || epochOf(id) != startEpoch

    private suspend fun openLocked(address: String, request: OpenRequest, id: String, startEpoch: Long): OpenResult {
        if (forgottenSince(id, startEpoch)) return OpenResult.Failed(SessionFailure.Blocked)
        if (current.get()?.isOpen == true) return OpenResult.Failed(SessionFailure.AlreadyActive)
        stateFlow.value = SessionState.Connecting(request.deviceIdHex)
        try {
            val transport = when (val c = transports.connect(address)) {
                is ConnectResult.Connected -> c.transport
                is ConnectResult.Failed -> return fail(SessionFailure.Transport(c.reason))
                is ConnectResult.NotReady -> return fail(SessionFailure.NotReady(c.readiness))
            }
            stateFlow.value = SessionState.Handshaking(request.deviceIdHex, request::class.simpleName ?: "?")
            val link = FrameLink(transport, scope)
            val r = SecureSession.open(link, request, noise, store, scope, timeouts, nowUtc) { seconds ->
                stateFlow.value = SessionState.ConfirmingOnDevice(request.deviceIdHex, seconds)
            }
            if (r is OpenResult.Opened && forgottenSince(id, startEpoch)) {
                // forgotten while this open was in progress: never adopted
                r.session.close("device being forgotten")
                stateFlow.value = SessionState.Idle
                return OpenResult.Failed(SessionFailure.Blocked)
            }
            when (r) {
                is OpenResult.Opened -> adopt(r.session, request.deviceIdHex, r.enrolledSlot)
                is OpenResult.Failed -> stateFlow.value = SessionState.Idle
            }
            return r
        } catch (e: CancellationException) {
            stateFlow.value = SessionState.Idle
            throw e
        }
    }

    private fun fail(f: SessionFailure): OpenResult {
        stateFlow.value = SessionState.Idle
        return OpenResult.Failed(f)
    }

    private fun adopt(s: SecureSession, deviceIdHex: String, slot: Int?) {
        val authenticated = SessionState.Authenticated(deviceIdHex, s.generation, slot)
        current.set(s)
        stateFlow.value = authenticated
        // subscribed before returning (undispatched): no event of the new session can be missed
        scope.launch(start = CoroutineStart.UNDISPATCHED) {
            s.events.collect { if (current.get() === s) eventFlow.emit(it) }
        }
        // synchronous invalidation (D2): when any close() of s returns, s is no longer exposed;
        // registered last, so a session that closed during adoption is invalidated at once
        s.setCloseListener { closed -> invalidate(closed, authenticated) }
    }

    /**
     * Drops [s] and its Authenticated state, each only if it is still the current one: lock-free
     * compare-and-set, idempotent, no callback and no blocking — a newer session (or a newer open's
     * Connecting/Handshaking state) is never cleared by an older session's late close.
     */
    private fun invalidate(s: SecureSession, authenticated: SessionState.Authenticated) {
        current.compareAndSet(s, null)
        stateFlow.compareAndSet(authenticated, SessionState.Idle)
    }

    /**
     * An encrypted request on the live session; [RequestResult.NotSent] when there is none. Never
     * retried across sessions: an [RequestResult.Uncertain] result goes back to the caller, which
     * reconciles it against durable device state in a new session (see [RequestResult.Uncertain]).
     */
    suspend fun request(type: Int, fields: Map<String, Any> = emptyMap(), retries: Int = 2): RequestResult =
        session?.request(type, fields, retries) ?: RequestResult.NotSent("no session")

    /** Ends the live session: graceful = encrypted END_SESSION (ends the device's window), else just disconnect. */
    suspend fun end(graceful: Boolean = true) {
        val s = current.get() ?: return
        if (graceful) s.end() else s.close("disconnected by the app")
    }
}
