package cz.nowass.dispenser.data.onboarding

import android.content.Intent
import android.content.IntentSender
import cz.nowass.dispenser.crypto.SetupLabel
import cz.nowass.dispenser.crypto.credentials.CredentialState
import cz.nowass.dispenser.crypto.credentials.CredentialStore
import cz.nowass.dispenser.data.mode.AppModeController
import cz.nowass.dispenser.data.mode.DeviceAddressStore
import cz.nowass.dispenser.device.session.DeviceSessionManager
import cz.nowass.dispenser.device.session.OpenRequest
import cz.nowass.dispenser.device.session.OpenResult
import cz.nowass.dispenser.device.session.SessionFailure
import cz.nowass.dispenser.device.session.SessionState
import cz.nowass.dispenser.device.transport.AssociationStart
import cz.nowass.dispenser.device.transport.BluetoothPrerequisites
import cz.nowass.dispenser.device.transport.BluetoothReadiness
import cz.nowass.dispenser.device.transport.DeviceAssociator
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.withContext

/** What the onboarding establishes (docs/ANDROID_BLE_DESIGN.md 7, 7.1, 11.3). Invitations: M1.10e.4c. */
enum class OnboardingPurpose { CLAIM, RECOVERY }

/** Bluetooth readiness in UI terms (no transport types reach the UI). */
sealed interface OnboardingReadiness {
    data object Ready : OnboardingReadiness
    data object Unsupported : OnboardingReadiness
    data object BluetoothOff : OnboardingReadiness
    data class PermissionMissing(val permissions: List<String>) : OnboardingReadiness
}

sealed interface LabelEntry {
    data class Accepted(val deviceId: String) : LabelEntry
    data class Rejected(val problem: Problem) : LabelEntry
    /** PREFIX/FIELDS: not a "DSP2:" label; DEVICE_ID; CODE: length, characters or padding; CHECK: a typing error. */
    enum class Problem { FORMAT, DEVICE_ID, CODE, CHECK }
}

sealed interface AssociationBegin {
    class ShowDialog(val intentSender: IntentSender) : AssociationBegin
    data object Unsupported : AssociationBegin
    data object Failed : AssociationBegin
}

sealed interface AssociationOutcome {
    data class Associated(val address: String) : AssociationOutcome
    data object Cancelled : AssociationOutcome
    /** The user picked another dispenser than the one on the label (address = factory MAC = device ID). */
    data class OtherDevice(val address: String) : AssociationOutcome
}

/** Progress of a running enrollment, for the UI. */
sealed interface EnrollPhase {
    data object Connecting : EnrollPhase
    data object Handshaking : EnrollPhase
    /** RECOVERY: press the device button three times within [seconds]. */
    data class ConfirmOnDevice(val seconds: Int) : EnrollPhase
}

sealed interface EnrollOutcome {
    data class Enrolled(val deviceId: String, val slot: Int?) : EnrollOutcome
    /** The device may have stored this phone, but its answer was lost: never shown as enrolled (11.7). */
    data class Unconfirmed(val deviceId: String) : EnrollOutcome
    /**
     * [staged]: this phone now holds an unconfirmed staged key for the device (e.g. a plain, hence
     * unauthenticated, HANDSHAKE_FAILED after message 3 — 11.7). It must be confirmed or explicitly
     * discarded before another enrollment; it is never dropped silently.
     */
    data class Failed(val reason: Reason, val staged: Boolean = false) : EnrollOutcome

    enum class Reason {
        /** Bluetooth off, permission missing, no CDM. */
        NOT_READY,
        /** Could not connect (not advertising: press the button; out of range). */
        CONNECTION,
        /** The device on the label is not the one that answered. */
        WRONG_DEVICE,
        /** The device already belongs to a phone (use an invitation or recovery). */
        ALREADY_CLAIMED,
        /** RECOVERY / confirmation on a device that has no owner. */
        NOT_CLAIMED,
        /** RECOVERY without the button held at power-on. */
        RECOVERY_NOT_ARMED,
        /** The handshake failed: wrong setup code (or the device refused this key). */
        HANDSHAKE,
        /** The device refused the enrollment (slots full, busy, …). */
        DEVICE_REFUSED,
        /** No answer in time (e.g. the three presses did not come). */
        TIMEOUT,
        /** Another connection is active. */
        BUSY,
        /** The pinned device key changed: not the device this phone enrolled with. */
        DEVICE_KEY_CHANGED,
        /** Confirmation: the device did not store the staged enrollment (unclaimed, or another identity). */
        NOTHING_STAGED,
        /** No label entered / no device associated yet. */
        INCOMPLETE,
        /** A staged enrollment of this device awaits confirmation or an explicit discard first. */
        STAGED_PENDING,
        PROTOCOL,
    }
}

/**
 * Onboarding (M1.10e.4b): readiness, setup-label entry, CDM association, CLAIM / RECOVERY, confirmation
 * or explicit discard of a staged enrollment (11.7) and the switch to DEVICE mode afterwards.
 *
 * Secret handling (E4, 7.1): the setup code exists only inside the parsed [SetupLabel] (SecretBytes) for
 * this flow; it is never persisted, logged or passed to the UI, and is zeroized by [close] (end of the
 * flow, cancel, or the owning ViewModel being cleared). The CDM address is a locator only.
 */
class OnboardingController(
    private val prerequisites: BluetoothPrerequisites,
    private val associator: DeviceAssociator,
    private val manager: DeviceSessionManager,
    private val credentials: CredentialStore,
    private val addresses: DeviceAddressStore,
    private val modes: AppModeController,
    /** After a confirmed enrollment (e.g. journal resolution and a live status read in DEVICE mode). */
    private val afterEnrollment: suspend (deviceId: String) -> Unit = {},
    private val io: CoroutineDispatcher = Dispatchers.IO,
) : AutoCloseable {
    private var label: SetupLabel? = null
    private var associated: String? = null

    /** The device the flow is about: the label's, or a staged one being confirmed. */
    var deviceId: String? = null
        private set

    val phase: Flow<EnrollPhase?> = manager.state.map { st ->
        when (st) {
            is SessionState.Connecting -> EnrollPhase.Connecting
            is SessionState.Handshaking -> EnrollPhase.Handshaking
            is SessionState.ConfirmingOnDevice -> EnrollPhase.ConfirmOnDevice(st.seconds)
            else -> null
        }
    }

    fun readiness(): OnboardingReadiness = when (val r = prerequisites.check()) {
        BluetoothReadiness.Ready -> OnboardingReadiness.Ready
        BluetoothReadiness.Unsupported -> OnboardingReadiness.Unsupported
        BluetoothReadiness.BluetoothOff -> OnboardingReadiness.BluetoothOff
        is BluetoothReadiness.PermissionMissing -> OnboardingReadiness.PermissionMissing(r.permissions)
    }

    /** Staged enrollments awaiting confirmation (file names only). */
    suspend fun stagedDevices(): List<String> = withContext(io) { credentials.stagedDevices() }

    /** Parses a typed (or scanned) label; the previous one is zeroized. The text itself is not kept. */
    fun enterLabel(text: String): LabelEntry {
        clearLabel()
        return when (val r = SetupLabel.parse(text)) {
            is SetupLabel.Result.Ok -> {
                label = r.label
                if (deviceId != r.label.deviceIdHex) associated = null
                deviceId = r.label.deviceIdHex
                LabelEntry.Accepted(r.label.deviceIdHex)
            }
            is SetupLabel.Result.Invalid -> LabelEntry.Rejected(
                when (r.problem) {
                    SetupLabel.Problem.PREFIX, SetupLabel.Problem.FIELDS -> LabelEntry.Problem.FORMAT
                    SetupLabel.Problem.DEVICE_ID -> LabelEntry.Problem.DEVICE_ID
                    SetupLabel.Problem.LENGTH, SetupLabel.Problem.CHARSET, SetupLabel.Problem.PAD -> LabelEntry.Problem.CODE
                    SetupLabel.Problem.CHECK -> LabelEntry.Problem.CHECK
                },
            )
        }
    }

    /** Confirming a staged enrollment needs no label: the staged key is used (11.7). */
    fun selectStaged(id: String) {
        clearLabel()
        if (deviceId != id) associated = null
        deviceId = id
    }

    /** A stored association address for the current device, if any (no dialog needed then). */
    suspend fun knownAddress(): String? = deviceId?.let { addresses.addresses.first()[it] }

    suspend fun beginAssociation(): AssociationBegin = when (val s = associator.start()) {
        is AssociationStart.NeedsUser -> AssociationBegin.ShowDialog(s.intentSender)
        AssociationStart.Unsupported -> AssociationBegin.Unsupported
        is AssociationStart.Failed -> AssociationBegin.Failed
    }

    /**
     * The dialog's result. The address is the device's factory MAC, which is also its device ID, so a
     * different pick is reported before any connection (a match grants nothing: 4.1).
     */
    suspend fun finishAssociation(result: Intent?): AssociationOutcome {
        val address = associator.addressFrom(result) ?: return AssociationOutcome.Cancelled
        val id = deviceId
        if (id != null && address.replace(":", "").lowercase() != id) return AssociationOutcome.OtherDevice(address)
        useAddress(address)
        return AssociationOutcome.Associated(address)
    }

    /** Uses an already known address (stored association) for the current device. */
    suspend fun useAddress(address: String) {
        associated = address
        deviceId?.let { addresses.setAddress(it, address) }
    }

    /** CLAIM or RECOVERY with the entered label; this phone's [phoneName] is shown in the device's phone list. */
    suspend fun enroll(purpose: OnboardingPurpose, phoneName: String): EnrollOutcome {
        val l = label ?: return EnrollOutcome.Failed(EnrollOutcome.Reason.INCOMPLETE)
        val address = associated ?: return EnrollOutcome.Failed(EnrollOutcome.Reason.INCOMPLETE)
        val name = PhoneName.normalize(phoneName) ?: return EnrollOutcome.Failed(EnrollOutcome.Reason.INCOMPLETE)
        if (readiness() != OnboardingReadiness.Ready) return EnrollOutcome.Failed(EnrollOutcome.Reason.NOT_READY)
        // a staged key of this device is never overwritten by a new enrollment (11.7)
        if (hasStaged(l.deviceIdHex)) return EnrollOutcome.Failed(EnrollOutcome.Reason.STAGED_PENDING, staged = true)
        val request = when (purpose) {
            OnboardingPurpose.CLAIM -> OpenRequest.Claim(l, name)
            OnboardingPurpose.RECOVERY -> OpenRequest.Recovery(l, name)
        }
        return finish(l.deviceIdHex, manager.open(address, request))
    }

    /** Confirms the staged enrollment of [deviceId] with an IK session using the staged key (11.7). */
    suspend fun confirmStaged(): EnrollOutcome {
        val id = deviceId ?: return EnrollOutcome.Failed(EnrollOutcome.Reason.INCOMPLETE)
        val address = associated ?: return EnrollOutcome.Failed(EnrollOutcome.Reason.INCOMPLETE)
        if (readiness() != OnboardingReadiness.Ready) return EnrollOutcome.Failed(EnrollOutcome.Reason.NOT_READY)
        return finish(id, manager.open(address, OpenRequest.ConfirmStaged(id)))
    }

    /**
     * Explicit user decision only (11.7.5): discards the staged key of the current device. A confirmed
     * credential of the same device is untouched.
     */
    suspend fun discardStaged() {
        val id = deviceId ?: return
        withContext(io) { credentials.discardPending(id) }
    }

    private suspend fun finish(id: String, r: OpenResult): EnrollOutcome = when (r) {
        is OpenResult.Opened -> {
            // the enrollment is committed (or the staged one confirmed): the flow's secret is no longer needed
            clearLabel()
            val confirmed = withContext(io) { (credentials.load(id) as? CredentialState.Present)?.credential?.use { true } ?: false }
            if (!confirmed) {
                EnrollOutcome.Failed(EnrollOutcome.Reason.PROTOCOL)
            } else {
                runCatching { afterEnrollment(id) }
                EnrollOutcome.Enrolled(id, r.enrolledSlot)
            }
        }
        is OpenResult.Failed -> when (val f = r.failure) {
            SessionFailure.EnrollmentUnconfirmed -> EnrollOutcome.Unconfirmed(id)
            else -> EnrollOutcome.Failed(reasonOf(f), staged = hasStaged(id))
        }
    }

    private suspend fun hasStaged(id: String): Boolean = withContext(io) { credentials.loadPending(id) !is CredentialState.Absent }

    /** Switches the app to DEVICE mode for the enrolled device (refused without a confirmed record). */
    suspend fun enterDeviceMode(id: String): Boolean = modes.enterDevice(id) == null

    private fun clearLabel() {
        label?.close()
        label = null
    }

    override fun close() = clearLabel()

    companion object {
        fun reasonOf(f: SessionFailure): EnrollOutcome.Reason = when (f) {
            is SessionFailure.NotReady -> EnrollOutcome.Reason.NOT_READY
            is SessionFailure.Transport -> EnrollOutcome.Reason.CONNECTION
            is SessionFailure.HelloRejected -> EnrollOutcome.Reason.DEVICE_REFUSED
            SessionFailure.WrongDevice -> EnrollOutcome.Reason.WRONG_DEVICE
            SessionFailure.NotUnclaimed -> EnrollOutcome.Reason.ALREADY_CLAIMED
            SessionFailure.NotClaimed -> EnrollOutcome.Reason.NOT_CLAIMED
            SessionFailure.RecoveryNotArmed -> EnrollOutcome.Reason.RECOVERY_NOT_ARMED
            SessionFailure.HandshakeFailed -> EnrollOutcome.Reason.HANDSHAKE
            SessionFailure.DeviceKeyMismatch -> EnrollOutcome.Reason.DEVICE_KEY_CHANGED
            is SessionFailure.EnrollmentRefused -> EnrollOutcome.Reason.DEVICE_REFUSED
            SessionFailure.NothingStaged -> EnrollOutcome.Reason.NOTHING_STAGED
            is SessionFailure.Timeout -> EnrollOutcome.Reason.TIMEOUT
            SessionFailure.AlreadyActive -> EnrollOutcome.Reason.BUSY
            SessionFailure.EnrollmentUnconfirmed, SessionFailure.NoCredential, SessionFailure.CredentialLost,
            SessionFailure.CredentialUnreadable, is SessionFailure.ProtocolError -> EnrollOutcome.Reason.PROTOCOL
        }
    }
}

/** This phone's name as shown in the device's phone list: trimmed, no control characters, 1–24 UTF-8 bytes. */
object PhoneName {
    const val MAX_BYTES = 24

    fun normalize(input: String): String? {
        val t = input.trim()
        if (t.isEmpty() || t.any { it.isISOControl() } || t.toByteArray(Charsets.UTF_8).size > MAX_BYTES) return null
        return t
    }
}
