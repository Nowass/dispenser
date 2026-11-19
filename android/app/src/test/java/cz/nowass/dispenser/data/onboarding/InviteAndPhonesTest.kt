package cz.nowass.dispenser.data.onboarding

import cz.nowass.dispenser.crypto.Invitation
import cz.nowass.dispenser.crypto.NativeNoiseProvider
import cz.nowass.dispenser.crypto.TestNative
import cz.nowass.dispenser.crypto.credentials.CredentialStore
import cz.nowass.dispenser.crypto.credentials.FakeKeyWrapper
import cz.nowass.dispenser.data.device.ConnectOutcome
import cz.nowass.dispenser.data.device.FakeDeviceRepository
import cz.nowass.dispenser.device.ops.DeviceError
import cz.nowass.dispenser.device.ops.DeviceErrorCode
import cz.nowass.dispenser.device.ops.InviteRecord
import cz.nowass.dispenser.device.ops.LocalRefusal
import cz.nowass.dispenser.device.ops.OpResult
import cz.nowass.dispenser.device.ops.PhoneRecord
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/** Owner-side invitation and phone list (M1.10e.4c) over a scripted device repository. */
class InviteAndPhonesTest {
    @get:Rule val tmp = TemporaryFolder()
    private lateinit var noise: NativeNoiseProvider
    private lateinit var store: CredentialStore
    private val fake = FakeDeviceRepository()
    private val devicePub = ByteArray(32) { (0x20 + it).toByte() }
    private val now = 1_795_000_000L

    @Before
    fun setUp() {
        noise = TestNative.load()
        store = CredentialStore(tmp.newFolder(), FakeKeyWrapper(), noise)
        store.commit(store.beginEnrollment(fake.deviceId), 0, devicePub, "A", 0).close()
        fake.connectOutcome = ConnectOutcome.Connected(1, 0)
    }

    @Test
    fun an_invitation_encodes_the_device_token_once_zeroizes_it_and_frees_the_connection() = runBlocking {
        val token = ByteArray(16) { (it + 1).toByte() }
        fake.inviteOutcome = OpResult.Ok(InviteRecord(token, 600))
        val r = InviteController(fake, store, nowUtc = { now }).create("Telefon B") as InviteOutcome.Created
        assertTrue("the device token is zeroized after encoding", token.all { it == 0.toByte() })
        assertEquals(1, fake.disconnects) // the new phone needs the device's only connection
        val inv = (Invitation.parse(r.qrText) as Invitation.Result.Ok).invitation
        assertArrayEquals(devicePub, inv.deviceStaticPub)
        assertEquals(fake.deviceId, inv.deviceId.joinToString("") { "%02x".format(it) })
        assertEquals(now, inv.createdUtc)
        assertEquals(now + 600, inv.expiresUtc)
        assertEquals(now + 600, r.expiresUtc)
        inv.inviteToken.read { assertArrayEquals(ByteArray(16) { (it + 1).toByte() }, it) }
        assertTrue("toString never shows the QR text", !r.toString().contains("DSP2I1"))
    }

    // ---- security review F6 / F7 ------------------------------------------------------------

    @Test
    fun a_caller_cancelled_during_the_request_still_clears_the_token_and_drops_the_link() = runBlocking {
        val token = ByteArray(16) { (it + 1).toByte() }
        fake.inviteOutcome = OpResult.Ok(InviteRecord(token, 600))
        val inRequest = kotlinx.coroutines.CompletableDeferred<Unit>()
        val answer = kotlinx.coroutines.CompletableDeferred<Unit>()
        fake.inviteHook = { inRequest.complete(Unit); answer.await() }
        val caller = launch(kotlinx.coroutines.Dispatchers.Default) { InviteController(fake, store, nowUtc = { now }).create("B") }
        kotlinx.coroutines.withTimeout(5_000) { inRequest.await() }
        caller.cancel() // e.g. the user closed the invitation screen while the device was answering
        answer.complete(Unit)
        kotlinx.coroutines.withTimeout(5_000) { caller.join() }
        assertTrue(caller.isCancelled)
        assertTrue("the token is cleared although nobody received it", token.all { it == 0.toByte() })
        assertEquals("one hard disconnect, never END_SESSION", listOf(false), fake.disconnectModes)
        assertEquals("exactly one INVITE_CREATE, no retry", listOf("invite B"), fake.calls)
    }

    @Test
    fun an_encoding_failure_clears_the_token_drops_the_link_and_reports_failed() = runBlocking {
        val token = ByteArray(16) { 7 }
        fake.inviteOutcome = OpResult.Ok(InviteRecord(token, 601)) // beyond the 600 s lifetime: the encoder refuses
        assertEquals(InviteOutcome.Failed(AdminFailure.INVALID), InviteController(fake, store, nowUtc = { now }).create(""))
        assertTrue(token.all { it == 0.toByte() })
        assertEquals(listOf(false), fake.disconnectModes)
        // an implausible phone clock as well
        val t2 = ByteArray(16) { 7 }
        fake.inviteOutcome = OpResult.Ok(InviteRecord(t2, 600))
        assertEquals(InviteOutcome.Failed(AdminFailure.INVALID), InviteController(fake, store, nowUtc = { -1 }).create(""))
        assertTrue(t2.all { it == 0.toByte() })
        assertEquals(listOf(false, false), fake.disconnectModes)
    }

    @Test
    fun transport_and_repository_exceptions_are_failures_with_a_hard_disconnect() = runBlocking {
        fake.connectThrows = IllegalStateException("no Bluetooth adapter")
        assertEquals(InviteOutcome.Failed(AdminFailure.NOT_CONNECTED), InviteController(fake, store).create(""))
        assertEquals(listOf(false), fake.disconnectModes)
        assertEquals(false, InviteController(fake, store).cancel())
        fake.connectThrows = null
        fake.inviteHook = { throw SecurityException("permission revoked") }
        assertEquals(InviteOutcome.Failed(AdminFailure.NOT_CONNECTED), InviteController(fake, store).create(""))
        assertEquals(listOf(false, false, false), fake.disconnectModes)
    }

    @Test
    fun the_decoded_token_array_is_not_aliased_and_is_zeroized() {
        val decoded = ByteArray(16) { 5 }
        val record = cz.nowass.dispenser.device.ops.Records.invite(mapOf("token" to decoded, "expires_s" to 600L))
        assertTrue("the record holds its own copy", record.token !== decoded)
        assertTrue(record.token.all { it == 5.toByte() })
        assertTrue("the decoded field array is cleared at once", decoded.all { it == 0.toByte() })
    }

    @Test
    fun invitation_failures_are_typed_and_never_show_a_code() = runBlocking {
        val c = InviteController(fake, store, nowUtc = { now })
        fake.inviteOutcome = OpResult.Rejected(DeviceError(DeviceErrorCode.LIMIT, 0, 0))
        assertEquals(InviteOutcome.Failed(AdminFailure.DEVICE_REFUSED), c.create(""))
        fake.inviteOutcome = OpResult.Uncertain("lost")
        assertEquals(InviteOutcome.Failed(AdminFailure.UNCERTAIN), c.create(""))
        fake.connectOutcome = ConnectOutcome.NoAddress
        assertEquals(InviteOutcome.Failed(AdminFailure.NOT_CONNECTED), c.create(""))
        assertEquals("a name hint over 24 bytes is refused before connecting",
            InviteOutcome.Failed(AdminFailure.INVALID), c.create("x".repeat(25)))
        store.delete(fake.deviceId)
        assertEquals(InviteOutcome.Failed(AdminFailure.CREDENTIAL), c.create(""))
    }

    @Test
    fun phones_are_listed_and_only_other_phones_can_be_removed() = runBlocking {
        fake.phonesOutcome = OpResult.Ok(listOf(PhoneRecord(1, "B", 2, 3, false), PhoneRecord(0, "A", 1, 4, true)))
        val c = PhonesController(fake)
        val listed = c.load() as PhonesOutcome.Listed
        assertEquals(listOf(0, 1), listed.phones.map { it.slot })
        val self = listed.phones.first { it.isThisPhone }
        val other = listed.phones.first { !it.isThisPhone }
        assertEquals(RemoveOutcome.NOT_ALLOWED, c.remove(self))
        assertTrue("nothing sent for this phone's own slot", fake.calls.none { it == "remove 0" })
        fake.removeOutcome = OpResult.Ok(Unit)
        assertEquals(RemoveOutcome.REMOVED, c.remove(other))
        fake.removeOutcome = OpResult.Uncertain("lost", 9)
        assertEquals("never a failure", RemoveOutcome.UNCERTAIN, c.remove(other))
        fake.removeOutcome = OpResult.Refused(LocalRefusal.Unresolved(9))
        assertEquals(RemoveOutcome.AWAITING_VERIFICATION, c.remove(other))
        fake.removeOutcome = OpResult.Refused(LocalRefusal.InProgress)
        assertEquals(RemoveOutcome.AWAITING_VERIFICATION, c.remove(other))
        c.finish()
        assertEquals(1, fake.disconnects)
    }
}
