package cz.nowass.dispenser.data.onboarding

import android.content.Intent
import cz.nowass.dispenser.crypto.Invitation
import cz.nowass.dispenser.crypto.NativeNoiseProvider
import cz.nowass.dispenser.crypto.SetupLabel
import cz.nowass.dispenser.crypto.TestNative
import cz.nowass.dispenser.crypto.credentials.CredentialState
import cz.nowass.dispenser.crypto.credentials.CredentialStore
import cz.nowass.dispenser.crypto.credentials.FakeKeyWrapper
import cz.nowass.dispenser.data.device.CredentialSeqAllocator
import cz.nowass.dispenser.data.device.DeviceDispenserRepository
import cz.nowass.dispenser.data.device.InMemoryDeviceCacheStore
import cz.nowass.dispenser.data.device.InMemoryPendingOpsJournal
import cz.nowass.dispenser.data.device.UnavailablePhoneTimeSource
import cz.nowass.dispenser.data.mode.AppModeController
import cz.nowass.dispenser.data.mode.InMemoryAppModeStore
import cz.nowass.dispenser.device.fwsim.FirmwareGattPortFactory
import cz.nowass.dispenser.device.fwsim.FirmwareSim
import cz.nowass.dispenser.device.ops.ManagerSessionPort
import cz.nowass.dispenser.device.session.DeviceSessionManager
import cz.nowass.dispenser.device.session.OpenRequest
import cz.nowass.dispenser.device.session.OpenResult
import cz.nowass.dispenser.device.session.SessionFailure
import cz.nowass.dispenser.device.transport.AssociationStart
import cz.nowass.dispenser.device.transport.BluetoothReadiness
import cz.nowass.dispenser.device.transport.DeviceAssociator
import cz.nowass.dispenser.device.transport.GattTransportFactory
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.runBlocking
import org.junit.After
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/**
 * M1.10e.4c against the REAL firmware logic: the owner creates an invitation (QR text), a new phone
 * joins with it and the device label (ADD), the owner lists the phones and removes the new one; a
 * cancelled invitation no longer works; the new phone's invitation checks (expiry, clocks, label of
 * another device).
 */
class InvitationFirmwareTest {
    @get:Rule val tmp = TemporaryFolder()
    private val address = "A0:B1:C2:D3:E4:F5"
    private val deviceId = "a0b1c2d3e4f5"
    private val setup = ByteArray(16) { (0x30 + it).toByte() }
    private lateinit var noise: NativeNoiseProvider
    private lateinit var scope: CoroutineScope
    private val ports = FirmwareGattPortFactory()

    private val associator = object : DeviceAssociator {
        override suspend fun start(): AssociationStart = AssociationStart.Unsupported
        override fun addressFrom(result: Intent?): String? = address
        override fun disassociate(address: String) = true
    }

    @Before
    fun setUp() {
        noise = TestNative.load()
        FirmwareSim.load()
        assertTrue(FirmwareSim.reset(ByteArray(6) { deviceId.substring(2 * it, 2 * it + 2).toInt(16).toByte() }, setup))
        scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    }

    @After
    fun tearDown() {
        if (FirmwareSim.wakeRunning()) {
            FirmwareSim.advance(24L * 3600 * 1000)
            assertTrue(FirmwareSim.joinWake(20_000))
        }
        val d = FirmwareSim.diagnostics()
        assertEquals("DSP_CHECK failures inside the firmware world", 0, d[0])
        assertEquals("security layer idle and wiped after the wake", 1, d[2])
        scope.cancel()
    }

    private fun wake() {
        assertTrue(FirmwareSim.buttonWake(false))
        assertTrue(FirmwareSim.waitAdvertising(5_000))
    }

    private fun labelText() = SetupLabel.format(deviceId, setup)

    private inner class Phone(name: String) {
        val store = CredentialStore(tmp.newFolder(name), FakeKeyWrapper(), noise)
        val manager = DeviceSessionManager(GattTransportFactory(ports, { BluetoothReadiness.Ready }, scope), noise, store, scope)
        val modeStore = InMemoryAppModeStore()
        val modes = AppModeController(modeStore, { CredentialStore.hasConfirmedRecord(tmp.root, it) }, scope)
        val repo = DeviceDispenserRepository(deviceId, { address }, ManagerSessionPort(manager), InMemoryPendingOpsJournal(),
            InMemoryDeviceCacheStore(), CredentialSeqAllocator(store), UnavailablePhoneTimeSource, scope)
        fun onboarding(now: () -> Long = { System.currentTimeMillis() / 1000 }) =
            OnboardingController({ BluetoothReadiness.Ready }, associator, manager, store, modeStore, modes, nowUtc = now)
    }

    private suspend fun ownerA(): Phone {
        val a = Phone("A")
        wake()
        val r = (SetupLabel.parse(labelText()) as SetupLabel.Result.Ok).label.use { a.manager.open(address, OpenRequest.Claim(it, "A")) }
        assertTrue("claim: $r", r is OpenResult.Opened)
        a.manager.closeNow(deviceId) // a dropped link keeps the service window (END_SESSION would end it)
        assertTrue(FirmwareSim.waitAdvertising(5_000))
        return a
    }

    private suspend fun Phone.joinWith(qr: String): EnrollOutcome {
        val c = onboarding()
        assertTrue(c.enterInvitation(qr) is InvitationEntry.Accepted)
        assertEquals(LabelEntry.Accepted(deviceId), c.enterLabel(labelText()))
        assertEquals(AssociationOutcome.Associated(address), c.finishAssociation(null))
        return c.enroll(OnboardingPurpose.ADD, "B")
    }

    @Test
    fun invite_join_list_and_remove() = runBlocking {
        val a = ownerA()
        val created = InviteController(a.repo, a.store).create("Telefon B") as InviteOutcome.Created
        assertFalse("the owner frees the device's only connection", a.manager.hasLiveSession(deviceId))
        val inv = (Invitation.parse(created.qrText) as Invitation.Result.Ok).invitation
        val pinned = (a.store.load(deviceId) as CredentialState.Present).credential.use { it.deviceStaticPublic.copyOf() }
        assertArrayEquals("the invitation names the device identity the owner pinned", pinned, inv.deviceStaticPub)
        assertTrue(inv.expiresUtc - inv.createdUtc in 1..Invitation.MAX_LIFETIME_S)
        inv.close()

        assertTrue(FirmwareSim.waitAdvertising(5_000))
        val b = Phone("B")
        assertEquals(EnrollOutcome.Enrolled(deviceId, 1), b.joinWith(created.qrText))
        b.manager.closeNow(deviceId)
        assertTrue(FirmwareSim.waitAdvertising(5_000))

        val phones = PhonesController(a.repo)
        val listed = (phones.load() as PhonesOutcome.Listed).phones
        assertEquals(listOf(0, 1), listed.map { it.slot })
        assertTrue(listed.single { it.slot == 0 }.isThisPhone)
        assertEquals(RemoveOutcome.NOT_ALLOWED, phones.remove(listed.single { it.isThisPhone }))
        assertEquals(RemoveOutcome.REMOVED, phones.remove(listed.single { it.slot == 1 }))
        assertEquals(listOf(0), (phones.load() as PhonesOutcome.Listed).phones.map { it.slot })
        a.manager.closeNow(deviceId)

        // the removed phone no longer authenticates
        assertTrue(FirmwareSim.waitAdvertising(5_000))
        assertEquals(OpenResult.Failed(SessionFailure.HandshakeFailed), b.manager.open(address, OpenRequest.Session(deviceId)))
    }

    @Test
    fun a_cancelled_invitation_no_longer_works() = runBlocking {
        val a = ownerA()
        val invite = InviteController(a.repo, a.store)
        val created = invite.create("") as InviteOutcome.Created
        assertTrue(FirmwareSim.waitAdvertising(5_000))
        assertTrue(invite.cancel())
        assertTrue(FirmwareSim.waitAdvertising(5_000))
        val b = Phone("B")
        val r = b.joinWith(created.qrText)
        assertTrue("$r", r is EnrollOutcome.Failed)
        assertEquals(CredentialState.Absent, b.store.load(deviceId))
    }

    @Test
    fun the_new_phone_checks_expiry_clocks_and_the_label() = runBlocking {
        val a = ownerA()
        val created = InviteController(a.repo, a.store).create("") as InviteOutcome.Created
        val b = Phone("B")
        // expired on this phone's clock (beyond the 120 s grace): refused, nothing kept
        val late = b.onboarding { created.expiresUtc + Invitation.EXPIRY_GRACE_S + 1 }
        assertEquals(InvitationEntry.Rejected(InvitationEntry.Problem.EXPIRED), late.enterInvitation(created.qrText))
        assertEquals(LabelEntry.Accepted(deviceId), late.enterLabel(labelText())) // no invitation held: plain label
        // this phone's clock is behind: a warning, still usable (the device decides)
        val behind = b.onboarding { created.createdUtc - Invitation.CLOCK_SKEW_S - 1 }
        assertEquals(InvitationEntry.Accepted(deviceId, clockWarning = true), behind.enterInvitation(created.qrText))
        // a label of another device is refused while the invitation is held
        assertEquals(LabelEntry.Rejected(LabelEntry.Problem.OTHER_DEVICE), behind.enterLabel(SetupLabel.format("ffffffffffff", setup)))
        // other QR codes
        val c = b.onboarding()
        assertEquals(InvitationEntry.Rejected(InvitationEntry.Problem.NOT_INVITATION), c.enterInvitation(labelText()))
        // one character always changed (a misread QR): the check digest catches it
        val at = 30
        val damaged = created.qrText.substring(0, at) + (if (created.qrText[at] == 'A') 'B' else 'A') + created.qrText.substring(at + 1)
        assertEquals(InvitationEntry.Rejected(InvitationEntry.Problem.CORRUPT), c.enterInvitation(damaged))
    }
}
