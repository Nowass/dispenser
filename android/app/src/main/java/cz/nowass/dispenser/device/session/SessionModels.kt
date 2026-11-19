package cz.nowass.dispenser.device.session

import cz.nowass.dispenser.crypto.Invitation
import cz.nowass.dispenser.crypto.SetupLabel
import cz.nowass.dispenser.device.transport.BluetoothReadiness
import cz.nowass.dispenser.device.transport.CloseReason

/** What a new connection should establish (docs/ANDROID_BLE_DESIGN.md 3.2, 11.7). */
sealed interface OpenRequest {
    val deviceIdHex: String

    /** IK SESSION with the stored credential of this device. */
    data class Session(override val deviceIdHex: String) : OpenRequest

    /** XXpsk3 CLAIM of an UNCLAIMED device; [label] (setup code) is closed by the caller afterwards. */
    class Claim(val label: SetupLabel, val name: String) : OpenRequest {
        override val deviceIdHex: String get() = label.deviceIdHex
    }

    /** XXpsk3 ADD with the device label and an owner's invitation (both closed by the caller). */
    class Add(val label: SetupLabel, val invitation: Invitation, val name: String) : OpenRequest {
        override val deviceIdHex: String get() = label.deviceIdHex
    }

    /** XXpsk3 RECOVERY of an armed device; the device then waits for three button presses. */
    class Recovery(val label: SetupLabel, val name: String) : OpenRequest {
        override val deviceIdHex: String get() = label.deviceIdHex
    }

    /** Confirms a staged enrollment whose ENROLL_RESULT was lost: IK SESSION with the staged key. */
    data class ConfirmStaged(override val deviceIdHex: String) : OpenRequest
}

/** HELLO response of the connection (BLE_PROTOCOL.md 5). */
data class HelloInfo(val proto: Int, val claimState: Int, val flags: Int, val caps: Long, val sessionId: Long, val deviceIdHash: ByteArray) {
    val claimed: Boolean get() = claimState == 1
    val recoveryArmed: Boolean get() = flags and 0x01 != 0
    val credentialsDamaged: Boolean get() = flags and 0x02 != 0

    override fun equals(other: Any?) = other is HelloInfo && other.proto == proto && other.claimState == claimState &&
        other.flags == flags && other.caps == caps && other.sessionId == sessionId && other.deviceIdHash.contentEquals(deviceIdHash)
    override fun hashCode() = sessionId.hashCode()
}

/** Why opening a session failed. Never contains key material. */
sealed interface SessionFailure {
    data class NotReady(val readiness: BluetoothReadiness) : SessionFailure
    data class Transport(val reason: CloseReason) : SessionFailure
    /** HELLO answered with an error (e.g. UNSUPPORTED_VERSION) or malformed. */
    data class HelloRejected(val code: Int) : SessionFailure
    /** The device is not the one the request is for (HELLO device_id_hash / claim state). */
    data object WrongDevice : SessionFailure
    data object NotUnclaimed : SessionFailure
    data object NotClaimed : SessionFailure
    data object RecoveryNotArmed : SessionFailure
    data object NoCredential : SessionFailure
    /** The Keystore key of the credential is gone: re-enrollment needed (11.3). */
    data object CredentialLost : SessionFailure
    data object CredentialUnreadable : SessionFailure
    /** HANDSHAKE_FAILED: wrong setup code or token, unknown or revoked key, admission refused (no detail by design). */
    data object HandshakeFailed : SessionFailure
    /** ADD: the device static key differs from the invitation's (aborted before message 3). */
    data object DeviceKeyMismatch : SessionFailure
    /** Message 3 was sent but ENROLL_RESULT did not arrive: the enrollment stays staged (11.7). */
    data object EnrollmentUnconfirmed : SessionFailure
    /** The device answered the enrollment with an encrypted error (e.g. SLOTS_FULL, REFUSED after recovery confirmation). */
    data class EnrollmentRefused(val code: Int, val detail: Long) : SessionFailure
    /** No staged enrollment to confirm, or the device did not store it. */
    data object NothingStaged : SessionFailure
    data class Timeout(val step: String) : SessionFailure
    /** A frame the protocol does not allow at this point (the connection was closed). */
    data class ProtocolError(val what: String) : SessionFailure
    data object AlreadyActive : SessionFailure
    /** This device is being forgotten on this phone: no new session for it (M1.10e.4b, F1). */
    data object Blocked : SessionFailure
}

sealed interface OpenResult {
    class Opened(val session: SecureSession, val enrolledSlot: Int?) : OpenResult
    data class Failed(val failure: SessionFailure) : OpenResult
}

/** Encrypted events (BLE_PROTOCOL.md 5). */
sealed interface DeviceEvent {
    data class SessionClosing(val reason: Int, val secondsLeft: Int) : DeviceEvent
    data class SuspendingForDose(val secondsUntil: Int) : DeviceEvent
    data class ConfirmOnDevice(val action: Int, val seconds: Int) : DeviceEvent
}

/**
 * Outcome of an encrypted request (docs/ANDROID_BLE_DESIGN.md 5, 14.3; BLE_PROTOCOL.md 10.4a).
 * Only [Ok] and [DeviceError] prove what the device did. [NotSent] and [Busy] prove the request never
 * left the phone. [Uncertain] proves nothing either way — it is never an operation failure.
 */
sealed interface RequestResult {
    data class Ok(val fields: Map<String, Any>, val attempts: Int) : RequestResult
    data class DeviceError(val code: Int, val detail: Long, val aux: Long) : RequestResult

    /** Nothing was encrypted or written (no live authenticated session): certainly not executed. */
    data class NotSent(val reason: String) : RequestResult

    /**
     * The request (or a same-`txn` retransmission of it, [attempts] in all) may have reached the
     * device, but no response was received: timeout, disconnect, or the session closed on a nonce
     * or security failure. The operation MAY HAVE BEEN EXECUTED. The session that sent it is closed
     * (its nonce state cannot be trusted). Callers must:
     * - report the outcome as unknown, never as failed and never as done;
     * - reconcile it against durable device state in a NEW session (`GET_RESULT` with the same
     *   manual-dose `seq`; revisions for configuration and schedules; `GET_STATUS` after REFILL);
     * - never allocate a new manual-dose `seq` for it automatically, and never repeat REFILL (or any
     *   other operation that is not idempotent) automatically.
     */
    data class Uncertain(val reason: String, val attempts: Int) : RequestResult

    /** More than the bounded number of requests are waiting (11.6): never sent. */
    data object Busy : RequestResult
}

/** Session timeouts (docs/ANDROID_BLE_DESIGN.md 4.4; tuned in M1.10e.5). */
data class SessionTimeouts(
    val helloMs: Long = 5_000,
    val handshakeStepMs: Long = 10_000,
    val enrollResultMs: Long = 10_000,
    val requestMs: Long = 5_000,
    /** Added to the device's EV_CONFIRM_ON_DEVICE seconds while waiting for the recovery result. */
    val confirmMarginMs: Long = 5_000,
)

/** Process-wide session state for the UI and repositories (docs/ANDROID_BLE_DESIGN.md 4.4). */
sealed interface SessionState {
    data object Idle : SessionState
    data class Connecting(val deviceIdHex: String) : SessionState
    data class Handshaking(val deviceIdHex: String, val purpose: String) : SessionState
    data class ConfirmingOnDevice(val deviceIdHex: String, val seconds: Int) : SessionState
    data class Authenticated(val deviceIdHex: String, val generation: Long, val slot: Int?) : SessionState
}
