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

    /** Connects to [address] (a locator, never an identity) and opens [request]. */
    suspend fun open(address: String, request: OpenRequest): OpenResult = openLock.withLock {
        if (current.get()?.isOpen == true) return@withLock OpenResult.Failed(SessionFailure.AlreadyActive)
        stateFlow.value = SessionState.Connecting(request.deviceIdHex)
        try {
            val transport = when (val c = transports.connect(address)) {
                is ConnectResult.Connected -> c.transport
                is ConnectResult.Failed -> return@withLock fail(SessionFailure.Transport(c.reason))
                is ConnectResult.NotReady -> return@withLock fail(SessionFailure.NotReady(c.readiness))
            }
            stateFlow.value = SessionState.Handshaking(request.deviceIdHex, request::class.simpleName ?: "?")
            val link = FrameLink(transport, scope)
            val r = SecureSession.open(link, request, noise, store, scope, timeouts, nowUtc) { seconds ->
                stateFlow.value = SessionState.ConfirmingOnDevice(request.deviceIdHex, seconds)
            }
            when (r) {
                is OpenResult.Opened -> adopt(r.session, request.deviceIdHex, r.enrolledSlot)
                is OpenResult.Failed -> stateFlow.value = SessionState.Idle
            }
            r
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
