package cz.nowass.dispenser.device.session

import cz.nowass.dispenser.crypto.Invitation
import cz.nowass.dispenser.crypto.NativeNoiseProvider
import cz.nowass.dispenser.crypto.SecretBytes
import cz.nowass.dispenser.crypto.SetupLabel
import cz.nowass.dispenser.crypto.TestNative
import cz.nowass.dispenser.crypto.credentials.CredentialState
import cz.nowass.dispenser.crypto.credentials.CredentialStore
import cz.nowass.dispenser.crypto.credentials.FakeKeyWrapper
import cz.nowass.dispenser.device.fwsim.FirmwareGattPortFactory
import cz.nowass.dispenser.device.fwsim.FirmwareSim
import cz.nowass.dispenser.device.transport.BluetoothReadiness
import cz.nowass.dispenser.device.transport.GattTransportFactory
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.After
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/**
 * Integration acceptance (decision E10): the Android client stack — DeviceSessionManager,
 * SecureSession, FrameLink, GattTransport and the JNI Noise provider — against the REAL firmware
 * application, security and BLE transport logic compiled for the host (libdsp_fwsim), over a
 * simulated GATT link. Host interoperability only: not radio behavior, not the target RNG, not
 * timing on the ESP32-C3 (M1.10e.5, M1.10c.2b, M1.10d.2).
 */
class FirmwareInTheLoopTest {
    @get:Rule val tmp = TemporaryFolder()

    private val address = "A0:B1:C2:D3:E4:F5"
    private val deviceIdHex = "a0b1c2d3e4f5"
    private val setup = ByteArray(16) { (0x30 + it).toByte() }
    private lateinit var noise: NativeNoiseProvider
    private lateinit var scope: CoroutineScope
    private val ports = FirmwareGattPortFactory()

    @Before
    fun setUp() {
        noise = TestNative.load()
        FirmwareSim.load()
        assertTrue(FirmwareSim.reset(hexBytes(deviceIdHex), setup))
        scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    }

    @After
    fun tearDown() {
        if (FirmwareSim.wakeRunning()) endWindow()
        val d = FirmwareSim.diagnostics()
        assertEquals("DSP_CHECK failures inside the firmware world", 0, d[0])
        assertEquals("dropped radio events", 0, d[1])
        assertEquals("security layer idle and wiped after the wake", 1, d[2])
        assertEquals("pump oracle: run without a durable decision, or a duplicate manual run", 0, d[5])
        scope.cancel()
    }

    private fun hexBytes(h: String) = ByteArray(h.length / 2) { h.substring(2 * it, 2 * it + 2).toInt(16).toByte() }

    private fun phone(name: String): Pair<CredentialStore, DeviceSessionManager> {
        val store = CredentialStore(tmp.newFolder(name), FakeKeyWrapper(), noise)
        return store to manager(store)
    }

    private fun manager(store: CredentialStore, timeouts: SessionTimeouts = SessionTimeouts()) = DeviceSessionManager(
        GattTransportFactory(ports, { BluetoothReadiness.Ready }, scope), noise, store, scope, timeouts)

    private fun label(code: ByteArray = setup): SetupLabel = (SetupLabel.parse(SetupLabel.format(deviceIdHex, code)) as SetupLabel.Result.Ok).label

    private fun wake(recoveryHold: Boolean = false) {
        assertTrue(FirmwareSim.buttonWake(recoveryHold))
        assertTrue(FirmwareSim.waitAdvertising(5_000))
    }

    /** Ends the device's service window by its maximum (virtual time jump) and waits for the wake to end. */
    private fun endWindow() {
        FirmwareSim.advance(24L * 3600 * 1000)
        assertTrue(FirmwareSim.joinWake(20_000))
    }

    private fun waitWakeEnd() = assertTrue(FirmwareSim.joinWake(20_000))

    private suspend fun DeviceSessionManager.claim(name: String): Int {
        val r = label().use { open(address, OpenRequest.Claim(it, name)) }
        assertTrue("claim: $r", r is OpenResult.Opened)
        return (r as OpenResult.Opened).enrolledSlot!!
    }

    private suspend fun DeviceSessionManager.sessionOk() {
        val r = open(address, OpenRequest.Session(deviceIdHex))
        assertTrue("session: $r", r is OpenResult.Opened)
    }

    @Test
    fun all_four_handshakes_over_one_device_lifetime() = runBlocking {
        val (storeA, a) = phone("A")
        // CLAIM
        wake()
        assertEquals(0, a.claim("Telefon A"))
        assertTrue(a.state.value is SessionState.Authenticated)
        val st = a.request(0x10) as RequestResult.Ok // GET_STATUS through the dispatcher
        assertTrue(st.fields.containsKey("status"))
        val inv = a.request(0x72, mapOf("name_hint" to "B".toByteArray())) as RequestResult.Ok // INVITE_CREATE
        val token = inv.fields["token"] as ByteArray
        val expires = (inv.fields["expires_s"] as Number).toInt()
        a.end() // owner's END_SESSION ends the window
        waitWakeEnd()
        assertEquals(SessionState.Idle, a.state.value)
        val credA = (storeA.load(deviceIdHex) as CredentialState.Present).credential
        val own1 = FirmwareSim.ownership()
        assertEquals(1, own1.claimState)
        assertArrayEquals(credA.deviceStaticPublic, own1.identity) // the phone pinned the device's identity
        assertArrayEquals(credA.phoneStaticPublic, own1.slots[0])
        val invitation = Invitation.encode(hexBytes(deviceIdHex), credA.deviceStaticPublic, SecretBytes.wrap(token.copyOf()),
            System.currentTimeMillis() / 1000, expires)
        credA.close()

        // SESSION (IK) by A
        wake()
        a.sessionOk()
        val phones = a.request(0x70) as RequestResult.Ok // PHONES_LIST
        assertEquals(1, (phones.fields["items"] as List<*>).size)
        a.end()
        waitWakeEnd()

        // ADD by B with the label and A's invitation
        val (storeB, b) = phone("B")
        wake()
        val add = label().use { l ->
            (Invitation.parse(invitation) as Invitation.Result.Ok).invitation.use { i -> b.open(address, OpenRequest.Add(l, i, "Telefon B")) }
        }
        assertTrue("add: $add", add is OpenResult.Opened)
        assertEquals(1, (add as OpenResult.Opened).enrolledSlot)
        b.end()
        waitWakeEnd()
        assertTrue(storeB.load(deviceIdHex) is CredentialState.Present)
        assertTrue(FirmwareSim.ownership().slots[1] != null)

        // RECOVERY by C: armed at boot, three presses during the confirmation
        val (storeC, c) = phone("C")
        wake(recoveryHold = true)
        val rec = async {
            label().use { c.open(address, OpenRequest.Recovery(it, "Telefon C")) }
        }
        withTimeout(15_000) { c.state.first { it is SessionState.ConfirmingOnDevice } }
        FirmwareSim.press3()
        val recR = rec.await()
        assertTrue("recovery: $recR", recR is OpenResult.Opened)
        assertEquals(0, (recR as OpenResult.Opened).enrolledSlot)
        c.end()
        waitWakeEnd()
        val own2 = FirmwareSim.ownership()
        val credC = (storeC.load(deviceIdHex) as CredentialState.Present).credential
        assertArrayEquals(credC.phoneStaticPublic, own2.slots[0])
        assertNull(own2.slots[1]) // recovery removed every other phone
        assertFalse(own2.identity.contentEquals(own1.identity)) // new device identity
        assertArrayEquals(credC.deviceStaticPublic, own2.identity)
        credC.close()

        // A's old credential no longer authenticates (identity replaced, slot removed)
        wake()
        val old = a.open(address, OpenRequest.Session(deviceIdHex))
        assertEquals(OpenResult.Failed(SessionFailure.WrongDevice), old) // HELLO already names another identity
        endWindow()
    }

    @Test
    fun authentication_failures_store_nothing() = runBlocking {
        val (storeA, a) = phone("A")
        wake()
        // SESSION without a credential
        assertEquals(OpenResult.Failed(SessionFailure.NoCredential), a.open(address, OpenRequest.Session(deviceIdHex)))
        // CLAIM with a wrong setup code: the device rejects message 3 (HANDSHAKE_FAILED)
        val wrong = setup.copyOf().also { it[0] = (it[0] + 1).toByte() }
        val r = label(wrong).use { a.open(address, OpenRequest.Claim(it, "X")) }
        assertEquals(OpenResult.Failed(SessionFailure.HandshakeFailed), r)
        assertEquals(SessionState.Idle, a.state.value)
        // the staged key is kept (unauthenticated failure) until the user discards it explicitly
        assertTrue(storeA.loadPending(deviceIdHex) is CredentialState.Present)
        storeA.discardPending(deviceIdHex)
        // the correct code then works within the same window (one failure so far)
        assertEquals(0, a.claim("A"))
        a.end()
        waitWakeEnd()
        val idPub = FirmwareSim.ownership().identity

        // ADD with an invitation naming another device key: aborted before message 3, nothing staged
        val (storeB, b) = phone("B")
        wake()
        val fakeInvite = Invitation.encode(hexBytes(deviceIdHex), ByteArray(32) { 7 }, SecretBytes.wrap(ByteArray(16) { 1 }), System.currentTimeMillis() / 1000, 600)
        // the HELLO hash already tells it is another identity
        val add = label().use { l -> (Invitation.parse(fakeInvite) as Invitation.Result.Ok).invitation.use { i -> b.open(address, OpenRequest.Add(l, i, "B")) } }
        assertEquals(OpenResult.Failed(SessionFailure.WrongDevice), add)
        assertEquals(CredentialState.Absent, storeB.loadPending(deviceIdHex))
        assertEquals(CredentialState.Absent, storeB.load(deviceIdHex))
        // SESSION with a key the device never enrolled (forged credential pinned to the real identity)
        val forge = storeB.beginEnrollment(deviceIdHex)
        storeB.commit(forge, 1, idPub, "forged", 0).close()
        assertEquals(OpenResult.Failed(SessionFailure.HandshakeFailed), b.open(address, OpenRequest.Session(deviceIdHex)))
        endWindow()
        val own = FirmwareSim.ownership()
        assertNull(own.slots[1]) // nothing was enrolled by any failure
        assertArrayEquals(idPub, own.identity)
    }

    @Test
    fun lost_enroll_result_then_process_death_confirms_with_ik() = runBlocking {
        val dir = tmp.newFolder("A")
        val wrapper = FakeKeyWrapper()
        val store1 = CredentialStore(dir, wrapper, noise)
        val m1 = manager(store1)
        wake()
        ports.dropFrameType = SecureSession.T_ENROLL_RESULT // the device commits, its answer is lost ...
        ports.disconnectAfterDrop = true // ... and the link drops
        val r = label().use { m1.open(address, OpenRequest.Claim(it, "A")) }
        assertEquals(OpenResult.Failed(SessionFailure.EnrollmentUnconfirmed), r)
        assertEquals(1, ports.dropped)
        assertEquals(CredentialState.Absent, store1.load(deviceIdHex)) // never shown as enrolled
        // process death: a new store and manager over the same files
        val store2 = CredentialStore(dir, wrapper, noise)
        val m2 = manager(store2)
        assertTrue(store2.loadPending(deviceIdHex) is CredentialState.Present)
        val c = m2.open(address, OpenRequest.ConfirmStaged(deviceIdHex)) // same window: the device advertises again
        assertTrue("confirm: $c", c is OpenResult.Opened)
        assertEquals(0, (c as OpenResult.Opened).enrolledSlot)
        assertEquals(CredentialState.Absent, store2.loadPending(deviceIdHex))
        (store2.load(deviceIdHex) as CredentialState.Present).credential.use { assertEquals(0, it.slot) }
        m2.end()
        waitWakeEnd()
    }

    @Test
    fun disconnect_during_a_request_and_a_new_generation_afterwards() = runBlocking {
        val (_, a) = phone("A")
        wake()
        a.claim("A")
        val first = a.session!!
        val sid1 = first.hello.sessionId
        ports.dropFrameType = 0x10 // GET_STATUS answer lost, then the link drops
        ports.disconnectAfterDrop = true
        val r = a.request(0x10)
        assertTrue("$r", r is RequestResult.Uncertain) // outcome unknown: read state in the next session
        withTimeout(5_000) { a.state.first { it == SessionState.Idle } }
        assertFalse(first.isOpen)
        assertTrue(first.request(0x10) is RequestResult.NotSent) // the old session can never be used again
        // reconnect: new transport generation, new HELLO session ID, new IK handshake
        a.sessionOk()
        val second = a.session!!
        assertNotEquals(first.generation, second.generation)
        assertNotEquals(sid1, second.hello.sessionId)
        assertTrue(a.request(0x10) is RequestResult.Ok)
        a.end()
        waitWakeEnd()
    }

    /** A slow answer: retransmitted with the same txn, answered from the device's response cache; the late duplicate is discarded. */
    @Test
    fun slow_response_is_retransmitted_and_answered_from_the_cache() = runBlocking {
        val store = CredentialStore(tmp.newFolder("A"), FakeKeyWrapper(), noise)
        val a = manager(store, SessionTimeouts(requestMs = 400))
        wake()
        a.claim("A")
        ports.delayFrameType = 0x72 // the first INVITE_CREATE answer arrives after the request timeout
        ports.delayMs = 600 // between one and two request timeouts
        val r = a.request(0x72, mapOf("name_hint" to "B".toByteArray())) as RequestResult.Ok
        assertEquals(2, r.attempts) // retransmitted once with the same txn and plaintext
        val s = a.session!!
        withTimeout(5_000) { while (s.discardedResponses.get() < 1) kotlinx.coroutines.delay(20) }
        assertEquals(1, s.discardedResponses.get()) // the cached duplicate: the first response won
        assertTrue(a.request(0x10) is RequestResult.Ok) // nonces stayed in step: the session continues
        a.end()
        waitWakeEnd()
    }

    // ---- uncertain outcomes (BLE_PROTOCOL.md 10.4a; docs/ANDROID_BLE_DESIGN.md 14.3) ---------------

    private val dose = 0x40
    private val getResult = 0x41
    private val refill = 0x42
    private val duplicate = 17 // BLE error DUPLICATE

    @Suppress("UNCHECKED_CAST")
    private suspend fun DeviceSessionManager.status(): Map<String, Any> =
        ((request(0x10) as RequestResult.Ok).fields["status"] as Map<String, Any>)

    private suspend fun DeviceSessionManager.floor(): Long = (status()["manual_floor"] as ULong).toLong()

    private suspend fun DeviceSessionManager.result(seq: Long): Pair<Int, Long> {
        val r = request(getResult, mapOf("seq" to seq)) as RequestResult.Ok
        return (r.fields["state"] as Number).toInt() to (r.fields["detail"] as Number).toLong()
    }

    /** Waits until the manager has no session (the device or the phone closed it). */
    private suspend fun DeviceSessionManager.idle() = withTimeout(10_000) { state.first { it == SessionState.Idle } }

    /** A dispenser ready for a manual dose (time, reservoir, readiness), through the owner's session. */
    private suspend fun DeviceSessionManager.prepareForDosing() {
        assertTrue(request(0x50, mapOf("phone_utc_ms" to System.currentTimeMillis(), "uncertainty_ms" to 100L)) is RequestResult.Ok)
        assertTrue(request(refill, mapOf("capacity_ul" to 200_000L, "remaining_ul" to 200_000L)) is RequestResult.Ok)
        assertTrue(request(0x43, mapOf("readiness" to 1)) is RequestResult.Ok) // READY
    }

    /**
     * MANUAL_DOSE over BLE never answers (the device suspends BLE for the pump, T12/T14): the client
     * reports UNCERTAIN, never failure; a new session reconciles it with GET_RESULT for the SAME
     * seq; re-sending that same seq is answered DUPLICATE and never runs the pump again.
     */
    @Test
    fun manual_dose_is_uncertain_and_reconciled_with_the_same_seq_in_a_new_session() = runBlocking {
        val (_, a) = phone("A")
        wake()
        a.claim("A")
        a.prepareForDosing()
        val seq = a.floor()
        val pumps = FirmwareSim.pumpRuns()
        val r = a.request(dose, mapOf("seq" to seq, "volume_ul" to 1_000L))
        assertTrue("$r", r is RequestResult.Uncertain) // the link closed for the pump: outcome unknown
        a.idle()
        a.sessionOk() // the device resumes BLE after the run
        assertEquals(pumps + 1, FirmwareSim.pumpRuns())
        assertEquals(2 to 1_000L, a.result(seq)) // DONE, 1,000 µl
        // reconciliation by re-sending the SAME seq is safe: decided exactly once
        val again = a.request(dose, mapOf("seq" to seq, "volume_ul" to 1_000L))
        assertEquals(RequestResult.DeviceError(duplicate, 2, 1_000), again)
        assertEquals(seq + 1, a.floor())
        assertEquals(pumps + 1, FirmwareSim.pumpRuns())
        assertEquals(2, ports.sent(dose)) // the original and the deliberate same-seq check only
        a.end()
        waitWakeEnd()
    }

    /**
     * A request the device drops before decrypting it (T3b; modeled by discarding the frame before
     * the firmware), then the same-txn retransmission: re-encrypted under the phone's next nonce, it
     * fails the device's decryption (nonce desynchronization) and the device closes. Uncertain; in a
     * new session GET_RESULT shows nothing was executed, and the SAME seq then runs exactly once.
     */
    @Test
    fun request_dropped_before_decryption_then_retry_meets_nonce_desync_and_recovers_in_a_new_session() = runBlocking {
        val store = CredentialStore(tmp.newFolder("A"), FakeKeyWrapper(), noise)
        val a = manager(store, SessionTimeouts(requestMs = 400))
        wake()
        a.claim("A")
        a.prepareForDosing()
        val seq = a.floor()
        val pumps = FirmwareSim.pumpRuns()
        ports.dropWriteType = dose
        val r = a.request(dose, mapOf("seq" to seq, "volume_ul" to 1_000L), retries = 2)
        assertTrue("$r", r is RequestResult.Uncertain && r.attempts == 2) // the retransmission hit the desync
        assertEquals(1, ports.droppedWrites)
        assertEquals(2, ports.sent(dose)) // original + one same-txn retransmission, nothing more
        a.idle() // the device closed on the failed decryption
        a.sessionOk()
        assertEquals(0 to 0L, a.result(seq)) // NOT_DECIDED: never executed
        assertEquals(seq, a.floor()) // not consumed
        assertEquals(pumps, FirmwareSim.pumpRuns())
        // the caller's reconciliation: the SAME seq (never a new one) now runs once
        assertTrue(a.request(dose, mapOf("seq" to seq, "volume_ul" to 1_000L)) is RequestResult.Uncertain)
        a.idle()
        a.sessionOk()
        assertEquals(2 to 1_000L, a.result(seq))
        assertEquals(seq + 1, a.floor())
        assertEquals(pumps + 1, FirmwareSim.pumpRuns())
        a.end()
        waitWakeEnd()
    }

    /**
     * An encrypted response lost while the link stays open (not a physical BLE case — notifications
     * are only lost with the link): the same-txn retransmission is answered from the device's
     * response cache (not executed again) under the device's next nonce, which the phone cannot
     * decrypt. The session closes; the REFILL outcome is uncertain, never repeated automatically,
     * and read back in a new session.
     */
    @Test
    fun lost_refill_response_on_an_open_link_is_uncertain_and_never_repeated() = runBlocking {
        val store = CredentialStore(tmp.newFolder("A"), FakeKeyWrapper(), noise)
        val a = manager(store, SessionTimeouts(requestMs = 400))
        wake()
        a.claim("A")
        val first = a.session!!
        ports.dropFrameType = refill
        val r = a.request(refill, mapOf("capacity_ul" to 150_000L, "remaining_ul" to 120_000L), retries = 2)
        assertEquals(RequestResult.Uncertain("decryption failed", 2), r) // the cached answer, next device nonce
        assertEquals(1, ports.dropped)
        assertEquals("decryption failed", first.closed.value)
        assertEquals(2, ports.sent(refill))
        a.idle()
        a.sessionOk() // a new connection works (new keys, new nonces)
        val st = a.status()
        assertEquals(150_000L, (st["capacity_ul"] as Number).toLong()) // applied (once; the retransmission came from the cache)
        assertEquals(120_000L, (st["remaining_ul"] as Number).toLong())
        assertEquals(2, ports.sent(refill)) // never repeated automatically
        a.end()
        waitWakeEnd()
    }

    @Test
    fun concurrent_requests_are_serialized_and_all_answered() = runBlocking {
        val (_, a) = phone("A")
        wake()
        a.claim("A")
        val results = (0 until 6).map { async { a.request(if (it % 2 == 0) 0x10 else 0x70) } }.awaitAll()
        assertTrue(results.toString(), results.all { it is RequestResult.Ok && it.attempts == 1 })
        a.end()
        waitWakeEnd()
    }

    @Test
    fun session_closing_event_and_cleanup_at_the_window_maximum() = runBlocking {
        val (_, a) = phone("A")
        wake()
        a.claim("A")
        val closing = async(start = CoroutineStart.UNDISPATCHED) { withTimeout(15_000) { a.events.first { it is DeviceEvent.SessionClosing } } }
        FirmwareSim.advance((15 * 60 - 25) * 1000L) // 25 s before the 15-minute maximum
        val ev = closing.await() as DeviceEvent.SessionClosing
        assertEquals(2, ev.reason) // MAX
        assertTrue(ev.secondsLeft in 1..30)
        FirmwareSim.advance(60_000) // past the maximum: the device closes the link
        withTimeout(10_000) { a.state.first { it == SessionState.Idle } }
        assertTrue(a.request(0x10) is RequestResult.NotSent)
        waitWakeEnd()
    }
}
