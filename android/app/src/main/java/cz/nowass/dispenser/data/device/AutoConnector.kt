package cz.nowass.dispenser.data.device

import cz.nowass.dispenser.device.session.SessionFailure
import cz.nowass.dispenser.device.session.SessionState
import cz.nowass.dispenser.device.transport.BluetoothReadiness
import cz.nowass.dispenser.device.transport.DeviceScanner
import cz.nowass.dispenser.device.transport.ScanOutcome
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch

/** What the automatic connection shows (DEVICE mode only; DEMO never sees anything but [Idle]). */
sealed interface AutoConnectStatus {
    /** Not running: background, DEMO, no owned device, or a state that needs no banner. */
    data object Idle : AutoConnectStatus
    /**
     * Waiting for the device's advertising ("Připojování…", press the button). [scanPermission]: the
     * optional BLUETOOTH_SCAN permission that would make discovery faster (direct connects are used meanwhile).
     */
    data class Searching(val scanPermission: List<String> = emptyList()) : AutoConnectStatus
    data object Connecting : AutoConnectStatus
    data object Connected : AutoConnectStatus
    /** The search window expired: "Dávkovač není dostupný — stiskněte tlačítko" + retry. */
    data object Unavailable : AutoConnectStatus
    /** The device was found but the connection or authentication failed: "Připojení se nezdařilo" + retry. */
    data class Failed(val authentication: Boolean) : AutoConnectStatus
    data object BluetoothOff : AutoConnectStatus
    /** BLUETOOTH_CONNECT (and BLUETOOTH_SCAN) to request; resumes by itself once granted. */
    data class PermissionMissing(val permissions: List<String>) : AutoConnectStatus
}

/** The owned device to connect to automatically: its repository (IK session, journal resolution) and CDM address (locator only). */
class AutoConnectTarget(val repository: DeviceRepository, val address: String)

/**
 * Automatic foreground connection to the already-owned device (Android DEBUG increment after the GPIO3
 * wake button). The device sleeps and advertises only in a service window opened by its button, and BLE
 * can never wake it, so the app waits for the advertising for a bounded [windowMs] after it comes to the
 * foreground, then connects through the existing [DeviceRepository.connect] (IK handshake with the pinned
 * key, journal resolution, status read). Nothing else is ever done automatically: no claim, enrollment,
 * invitation, credential change, readiness or configuration write.
 *
 * - One job at a time; foreground transitions while it runs, or while a session of the device is live,
 *   start nothing new (no duplicate scans or sessions; concurrent connect() calls also coalesce in the
 *   repository).
 * - Background stops the search at once (the scan is cancelled); the live session itself is left to the
 *   existing background grace (30 s, plain disconnect).
 * - After an unexpected disconnect in the foreground one new window starts (e.g. the device suspended
 *   BLE for a dose and advertises again).
 * - Discovery: [scanner] (filtered by service UUID and address) when it can scan, else repeated direct
 *   connects to the associated address (each bounded by the transport's connect timeout).
 * - Authentication failures are never retried automatically; transient connect failures twice at most per window.
 */
class AutoConnector(
    private val scope: CoroutineScope,
    /** The owned device, or null when nothing may connect (DEMO, no address, recovery required, release build). */
    private val target: () -> AutoConnectTarget?,
    private val readiness: () -> BluetoothReadiness,
    private val scanner: DeviceScanner,
    /** The optional scan permission still missing (empty when granted or not applicable). */
    private val missingScanPermission: () -> List<String> = { emptyList() },
    private val nowMs: () -> Long = System::currentTimeMillis,
    private val windowMs: Long = DEFAULT_WINDOW_MS,
    private val pollMs: Long = POLL_MS,
    private val retryGapMs: Long = RETRY_GAP_MS,
) {
    private val statusFlow = MutableStateFlow<AutoConnectStatus>(AutoConnectStatus.Idle)
    val status: StateFlow<AutoConnectStatus> = statusFlow.asStateFlow()

    private var started = 0
    private var job: Job? = null

    /** Counts started activities like [cz.nowass.dispenser.device.session.BackgroundGrace]: the first start is the foreground transition. */
    @Synchronized
    fun activityStarted() {
        if (started++ == 0) startIfNeeded()
    }

    @Synchronized
    fun activityStopped() {
        if (started == 0) return
        if (--started == 0) {
            job?.cancel() // background: stop scanning; the session stays to the background grace
            job = null
            statusFlow.value = AutoConnectStatus.Idle
        }
    }

    val inForeground: Boolean @Synchronized get() = started > 0

    /** The target may have changed (mode resolved or switched, address loaded, credential health): start or stop accordingly. */
    @Synchronized
    fun refresh() {
        if (started == 0) return
        if (target() == null) {
            job?.cancel()
            job = null
            statusFlow.value = AutoConnectStatus.Idle
        } else {
            startIfNeeded()
        }
    }

    /** User's "Zkusit znovu": a new window (also after permissions were granted or Bluetooth was enabled). */
    @Synchronized
    fun retry() {
        if (started == 0) return
        job?.cancel()
        job = null
        startIfNeeded()
    }

    private fun startIfNeeded() {
        if (job?.isActive == true) return
        val t = target() ?: run { statusFlow.value = AutoConnectStatus.Idle; return }
        job = scope.launch { run(t) }
    }

    private fun live(t: AutoConnectTarget): Boolean =
        (t.repository.connection.value as? SessionState.Authenticated)?.deviceIdHex.equals(t.repository.deviceId, ignoreCase = true)

    private suspend fun run(t: AutoConnectTarget) {
        while (true) {
            if (!live(t) && !window(t)) return // the final status (Unavailable / Failed / Idle) stays shown
            statusFlow.value = AutoConnectStatus.Connected
            // reuse the live session; wait until it ends, then (still in the foreground) one new window
            t.repository.connection.first { s ->
                !(s is SessionState.Authenticated && s.deviceIdHex.equals(t.repository.deviceId, ignoreCase = true))
            }
        }
    }

    /** One bounded search; true when a session of the device is live at its end. */
    private suspend fun window(t: AutoConnectTarget): Boolean {
        var deadline = nowMs() + windowMs
        var transientFailures = 0
        while (nowMs() < deadline) {
            // Bluetooth off / permission missing: polled (no radio use) without consuming the window, so the
            // search starts in full as soon as Bluetooth comes on or the permission is granted
            when (val r = readiness()) {
                BluetoothReadiness.Ready -> Unit
                BluetoothReadiness.BluetoothOff -> {
                    statusFlow.value = AutoConnectStatus.BluetoothOff
                    delay(pollMs); deadline = nowMs() + windowMs; continue
                }
                is BluetoothReadiness.PermissionMissing -> {
                    statusFlow.value = AutoConnectStatus.PermissionMissing((r.permissions + missingScanPermission()).distinct())
                    delay(pollMs); deadline = nowMs() + windowMs; continue
                }
                BluetoothReadiness.Unsupported -> { statusFlow.value = AutoConnectStatus.Idle; return false }
            }
            statusFlow.value = AutoConnectStatus.Searching(missingScanPermission())
            val remaining = deadline - nowMs()
            val direct = when (scanner.awaitAdvertising(t.address, remaining)) {
                ScanOutcome.Found -> false
                ScanOutcome.Timeout -> break
                is ScanOutcome.Unavailable -> true // fall back to a direct connect attempt
            }
            if (!direct) statusFlow.value = AutoConnectStatus.Connecting
            when (val c = t.repository.connect()) {
                is ConnectOutcome.Connected -> return true
                ConnectOutcome.NoAddress -> { statusFlow.value = AutoConnectStatus.Idle; return false }
                is ConnectOutcome.Failed -> when {
                    c.failure.isAuthentication() -> { statusFlow.value = AutoConnectStatus.Failed(authentication = true); return false }
                    c.failure is SessionFailure.NotReady -> Unit // readiness is re-checked at the top
                    direct -> Unit // a direct attempt to a sleeping device: keep trying until the deadline
                    ++transientFailures >= MAX_TRANSIENT_FAILURES -> {
                        statusFlow.value = AutoConnectStatus.Failed(authentication = false)
                        return false
                    }
                }
            }
            if (live(t)) return true
            delay(retryGapMs)
        }
        if (live(t)) return true
        statusFlow.value = AutoConnectStatus.Unavailable
        return false
    }

    companion object {
        /** Long enough to open the app first and then walk to the device and press its button. */
        const val DEFAULT_WINDOW_MS = 90_000L
        const val POLL_MS = 1_000L
        const val RETRY_GAP_MS = 2_000L
        const val MAX_TRANSIENT_FAILURES = 2

        private fun SessionFailure.isAuthentication(): Boolean = when (this) {
            SessionFailure.HandshakeFailed, SessionFailure.CredentialLost, SessionFailure.CredentialUnreadable,
            SessionFailure.NoCredential, SessionFailure.WrongDevice, SessionFailure.NotClaimed, SessionFailure.Blocked,
            SessionFailure.DeviceKeyMismatch, is SessionFailure.HelloRejected -> true
            else -> false
        }
    }
}
