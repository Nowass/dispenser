package cz.nowass.dispenser.data.onboarding

import cz.nowass.dispenser.crypto.Invitation
import cz.nowass.dispenser.crypto.SecretBytes
import cz.nowass.dispenser.crypto.credentials.CredentialState
import cz.nowass.dispenser.crypto.credentials.CredentialStore
import cz.nowass.dispenser.data.device.ConnectOutcome
import cz.nowass.dispenser.data.device.DeviceRepository
import cz.nowass.dispenser.device.ops.LocalRefusal
import cz.nowass.dispenser.device.ops.OpResult
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.withContext

/** Why a device-administration action could not run (UI terms; no session or transport types). */
enum class AdminFailure {
    /** No association address, Bluetooth not ready or the device not reachable (press its button). */
    NOT_CONNECTED,
    /** This phone's credential is unusable (recovery required). */
    CREDENTIAL,
    /** The device refused (e.g. SLOTS_FULL for an invitation, BUSY). */
    DEVICE_REFUSED,
    /** The outcome is unknown (link lost); verified at the next connection. */
    UNCERTAIN,
    INVALID,
}

sealed interface InviteOutcome {
    /** [qrText] is the canonical `DSP2I1:` text to show as a QR code until [expiresUtc]. */
    class Created(val qrText: String, val createdUtc: Long, val expiresUtc: Long) : InviteOutcome {
        override fun toString() = "Created(expires=$expiresUtc, qr=**)"
    }
    data class Failed(val reason: AdminFailure) : InviteOutcome
}

/**
 * The owner phone's invitation (docs/ANDROID_BLE_DESIGN.md 7.1 B, 11.3; M1.10e.4c). INVITE_CREATE in a
 * live session; the device generates the 128-bit token and enforces expiry and single use. The token is
 * wrapped into SecretBytes, encoded once into the QR text and zeroized; the link is then dropped
 * (without END_SESSION, which would end the device's service window) so the new phone can connect in
 * the same button-press window (the device serves one connection). Nothing is persisted or logged; the QR
 * text exists only on the invitation screen, which is FLAG_SECURE, and is dropped when it closes.
 */
class InviteController(
    private val repo: DeviceRepository,
    private val credentials: CredentialStore,
    private val nowUtc: () -> Long = { System.currentTimeMillis() / 1000 },
    private val io: CoroutineDispatcher = Dispatchers.IO,
) {
    suspend fun create(nameHint: String): InviteOutcome {
        if (PhoneName.normalize(nameHint) == null && nameHint.isNotBlank()) return InviteOutcome.Failed(AdminFailure.INVALID)
        val pub = try {
            withContext(io) {
                (credentials.load(repo.deviceId) as? CredentialState.Present)?.credential?.use { it.deviceStaticPublic.copyOf() }
            }
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            null
        } ?: return InviteOutcome.Failed(AdminFailure.CREDENTIAL)
        try {
            when (val c = repo.connect()) {
                is ConnectOutcome.Connected -> Unit
                ConnectOutcome.NoAddress -> return InviteOutcome.Failed(AdminFailure.NOT_CONNECTED)
                is ConnectOutcome.Failed -> return InviteOutcome.Failed(failureOf(c))
            }
            // Security review F6: once INVITE_CREATE is submitted it is manager-owned (D3) and completes
            // even if this caller is cancelled; its outcome — the token — is processed NonCancellable so
            // the token is always cleared after its last use. A cancelled caller then simply does not
            // receive the result (the invitation is never shown; single use, ≤ 10 min on the device).
            return withContext(NonCancellable) {
                val r = try {
                    repo.createInvite(nameHint.trim())
                } catch (e: Exception) {
                    OpResult.NotSent("invite: ${e.javaClass.simpleName}")
                }
                outcomeOf(r, pub)
            }
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            return InviteOutcome.Failed(AdminFailure.NOT_CONNECTED)
        } finally {
            // always a hard disconnect, never END_SESSION: the new phone needs the device's only
            // connection in the same service window (also when the caller was cancelled or something threw)
            withContext(NonCancellable) { runCatching { repo.disconnect(graceful = false) } }
        }
    }

    private fun outcomeOf(r: OpResult<cz.nowass.dispenser.device.ops.InviteRecord>, pub: ByteArray): InviteOutcome = when (r) {
        is OpResult.Ok -> {
            val raw = r.value.token
            try {
                val created = nowUtc()
                val text = SecretBytes.takeAndClear(raw).use { Invitation.encode(hexBytes(repo.deviceId), pub, it, created, r.value.expiresS) }
                InviteOutcome.Created(text, created, created + r.value.expiresS)
            } catch (e: Exception) {
                // e.g. an implausible phone clock or lifetime rejected by the encoder: nothing is shown
                InviteOutcome.Failed(AdminFailure.INVALID)
            } finally {
                raw.fill(0) // the record's copy, on every path (takeAndClear already cleared it on success)
            }
        }
        is OpResult.Rejected -> InviteOutcome.Failed(AdminFailure.DEVICE_REFUSED)
        // not journaled: an uncertain invitation is harmless (single use, ≤ 10 min) and its token is lost
        is OpResult.Uncertain -> InviteOutcome.Failed(AdminFailure.UNCERTAIN)
        is OpResult.NotSent -> InviteOutcome.Failed(AdminFailure.NOT_CONNECTED)
        is OpResult.Refused -> InviteOutcome.Failed(AdminFailure.INVALID)
    }

    /** INVITE_CANCEL (reconnects; the device may already have consumed or expired it). Never throws. */
    suspend fun cancel(): Boolean = try {
        repo.connect() is ConnectOutcome.Connected && withContext(NonCancellable) { repo.cancelInvite() is OpResult.Ok }
    } catch (e: CancellationException) {
        throw e
    } catch (e: Exception) {
        false
    } finally {
        withContext(NonCancellable) { runCatching { repo.disconnect(graceful = false) } }
    }

    private fun hexBytes(h: String) = ByteArray(h.length / 2) { h.substring(2 * it, 2 * it + 2).toInt(16).toByte() }
}

/** A phone enrolled with the device, as the owner sees it. */
data class PhoneUi(val slot: Int, val name: String, val enrolledAtUtc: Long, val lastSeenUtc: Long, val isThisPhone: Boolean)

sealed interface PhonesOutcome {
    data class Listed(val phones: List<PhoneUi>) : PhonesOutcome
    data class Failed(val reason: AdminFailure) : PhonesOutcome
}

enum class RemoveOutcome {
    REMOVED,
    /** Link lost: may have been removed; verified by read-back at the next connection (journaled). Never a failure. */
    UNCERTAIN,
    NOT_SENT,
    /** An earlier removal of this slot is unresolved or still running. */
    AWAITING_VERIFICATION,
    REJECTED,
    /** This phone's own slot: use "Forget device" here and remove it from another phone. */
    NOT_ALLOWED,
}

/**
 * The device's phone list and PHONE_REMOVE (journaled, resolved by read-back; M1.10e.3). The owner
 * removes OTHER phones; this phone's own slot is not removed from here (it would revoke itself while
 * connected). The session belongs to the application (DeviceSessionManager), not to this screen:
 * leaving the screen neither disconnects nor sends END_SESSION, so the authenticated session stays
 * available to every screen and [load] reuses it (decision C10: an encrypted END_SESSION would end the
 * device's whole service window). The background grace (BackgroundGrace) ends it with a plain disconnect.
 */
class PhonesController(private val repo: DeviceRepository) {
    suspend fun load(): PhonesOutcome {
        when (val c = repo.connect()) {
            is ConnectOutcome.Connected -> Unit
            ConnectOutcome.NoAddress -> return PhonesOutcome.Failed(AdminFailure.NOT_CONNECTED)
            is ConnectOutcome.Failed -> return PhonesOutcome.Failed(failureOf(c))
        }
        return when (val r = repo.refreshPhones()) {
            is OpResult.Ok -> PhonesOutcome.Listed(r.value.map { PhoneUi(it.slot, it.name, it.enrolledAt, it.lastSeen, it.isSelf) }.sortedBy { it.slot })
            is OpResult.Rejected -> PhonesOutcome.Failed(AdminFailure.DEVICE_REFUSED)
            is OpResult.Uncertain -> PhonesOutcome.Failed(AdminFailure.UNCERTAIN)
            is OpResult.NotSent -> PhonesOutcome.Failed(AdminFailure.NOT_CONNECTED)
            is OpResult.Refused -> PhonesOutcome.Failed(AdminFailure.INVALID)
        }
    }

    suspend fun remove(phone: PhoneUi): RemoveOutcome {
        if (phone.isThisPhone) return RemoveOutcome.NOT_ALLOWED
        return when (val r = repo.removePhone(phone.slot)) {
            is OpResult.Ok -> RemoveOutcome.REMOVED
            is OpResult.Uncertain -> RemoveOutcome.UNCERTAIN
            is OpResult.NotSent -> RemoveOutcome.NOT_SENT
            is OpResult.Rejected -> RemoveOutcome.REJECTED
            is OpResult.Refused -> when (r.reason) {
                is LocalRefusal.Unresolved, LocalRefusal.InProgress -> RemoveOutcome.AWAITING_VERIFICATION
                else -> RemoveOutcome.NOT_SENT
            }
        }
    }
}

private fun failureOf(c: ConnectOutcome.Failed): AdminFailure {
    val r = OnboardingController.reasonOf(c.failure)
    return when (r) {
        EnrollOutcome.Reason.PROTOCOL, EnrollOutcome.Reason.HANDSHAKE, EnrollOutcome.Reason.DEVICE_KEY_CHANGED -> AdminFailure.CREDENTIAL
        EnrollOutcome.Reason.DEVICE_REFUSED, EnrollOutcome.Reason.BUSY -> AdminFailure.DEVICE_REFUSED
        else -> AdminFailure.NOT_CONNECTED
    }
}
