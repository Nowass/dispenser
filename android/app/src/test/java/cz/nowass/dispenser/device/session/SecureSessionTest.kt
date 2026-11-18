package cz.nowass.dispenser.device.session

import cz.nowass.dispenser.ble.protocol.BleProtocol
import cz.nowass.dispenser.crypto.HandshakeInitiator
import cz.nowass.dispenser.crypto.NativeNoiseProvider
import cz.nowass.dispenser.crypto.NativeNoiseTestHooks
import cz.nowass.dispenser.crypto.SessionDerivation
import cz.nowass.dispenser.crypto.Purpose
import cz.nowass.dispenser.crypto.TestNative
import cz.nowass.dispenser.crypto.TransportCiphers
import cz.nowass.dispenser.crypto.credentials.CredentialStore
import cz.nowass.dispenser.crypto.credentials.FakeKeyWrapper
import cz.nowass.dispenser.device.transport.BluetoothReadiness
import cz.nowass.dispenser.device.transport.FakeGatt
import cz.nowass.dispenser.device.transport.GattTransportFactory
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/**
 * Client security rules of [SecureSession] against a scripted IK peer (test-only Noise responder over
 * the fake GATT), for faults the real firmware never produces: plaintext after authentication,
 * tampered ciphertext, fragment errors, device-originated requests, duplicate responses, HELLO
 * errors, saturation. The real-firmware acceptance is FirmwareInTheLoopTest.
 */
class SecureSessionTest {
    @get:Rule val tmp = TemporaryFolder()
    private val noise: NativeNoiseProvider = TestNative.load()
    private val fake = FakeGatt()
    private val id = "a0b1c2d3e4f5"
    private val device = noise.generateStaticKeyPair()
    private val devPriv = device.privateKey.copyBytes()
    private val sessionId = 0x5E55_0001L

    // ---- scripted peer -----------------------------------------------------------------------
    /** ADD impostor: answers HELLO with the invitation's identity hash but runs XX with another static key. */
    private var impostor: ByteArray? = null
    var hsMessages = 0
    private val setup = ByteArray(16) { (0x30 + it).toByte() }
    private val token = ByteArray(16) { 9 }

    private fun impostorXx(f: cz.nowass.dispenser.ble.protocol.BleFrame, t: Int): List<ByteArray> {
        hsMessages++
        if ((f.fields!!["msg_index"] as Number).toInt() != 0) return emptyList()
        val pro = SessionDerivation.prologue(1, 0, Purpose.ADD, SessionDerivation.claimedDeviceIdHash(device.publicKey), sessionId)
        val psk = cz.nowass.dispenser.crypto.SecretBytes.wrap(setup.copyOf()).use { sc ->
            cz.nowass.dispenser.crypto.SecretBytes.wrap(token.copyOf()).use { tk -> noise.derivePsk(Purpose.ADD, sc, tk).use { it.copyBytes() } }
        }
        val r = NativeNoiseTestHooks.handshake(true, false, pro, impostor!!, null, psk, null)
        r.readMessage(f.fields!!["noise"] as ByteArray)
        return listOf(BleProtocol.encode(0x02, BleProtocol.F_RESPONSE, t, mapOf("msg_index" to 1, "noise" to r.writeMessage())))
    }

    enum class Mode { NORMAL, PLAIN_AFTER_AUTH, TAMPER, DUPLICATE, SILENT, REQUEST_FROM_DEVICE }
    private var mode = Mode.NORMAL
    private var helloError: Int? = null
    private var helloHash: ByteArray? = null
    private var hs: HandshakeInitiator? = null
    private var ciphers: TransportCiphers? = null
    var received = 0
    /** Decrypted encrypted requests as the peer saw them: txn to plaintext body. */
    val requests = mutableListOf<Pair<Int, ByteArray>>()

    private fun hdr(type: Int, flags: Int, txn: Int, len: Int) =
        byteArrayOf(1, 0, type.toByte(), flags.toByte(), txn.toByte(), (txn shr 8).toByte(), len.toByte(), (len shr 8).toByte())

    private fun enc(type: Int, flags: Int, txn: Int, plain: ByteArray): ByteArray {
        val h = hdr(type, flags or BleProtocol.F_ENCRYPTED, txn, plain.size + 16)
        return h + ciphers!!.encrypt(h, plain)
    }

    private fun peer(frame: ByteArray): List<ByteArray> {
        val f = BleProtocol.decode(frame)
        val t = f.header.txn
        return when (f.header.type) {
            0x01 -> listOf(helloError?.let { code ->
                BleProtocol.encode(0x01, BleProtocol.F_RESPONSE or BleProtocol.F_ERROR, t, mapOf("code" to code, "detail" to 0x0101, "aux" to 0))
            } ?: BleProtocol.encode(0x01, BleProtocol.F_RESPONSE, t, mapOf("proto" to 1, "claim_state" to 1, "flags" to 0, "caps" to 0,
                "session_id" to sessionId, "device_id_hash" to (helloHash ?: SessionDerivation.claimedDeviceIdHash(device.publicKey)))))
            0x02 -> if (impostor != null) impostorXx(f, t) else {
                val pro = SessionDerivation.prologue(1, 0, Purpose.SESSION, SessionDerivation.claimedDeviceIdHash(device.publicKey), sessionId)
                val r = NativeNoiseTestHooks.handshake(false, false, pro, devPriv, null, null, null)
                r.readMessage(f.fields!!["noise"] as ByteArray)
                val m2 = r.writeMessage()
                ciphers = r.split()
                listOf(BleProtocol.encode(0x02, BleProtocol.F_RESPONSE, t, mapOf("msg_index" to 1, "noise" to m2)))
            }
            else -> {
                received++
                val plain = ciphers!!.decrypt(frame.copyOf(8), frame.copyOfRange(8, frame.size))
                requests += t to plain.copyOf()
                val type = f.header.type
                val body = if (type == 0x70) byteArrayOf(0) else ByteArray(0) // PHONES_LIST: 0 items; others empty
                when (mode) {
                    Mode.NORMAL -> listOf(enc(type, BleProtocol.F_RESPONSE, t, body))
                    Mode.DUPLICATE -> listOf(enc(type, BleProtocol.F_RESPONSE, t, body), enc(type, BleProtocol.F_RESPONSE, t, body))
                    Mode.PLAIN_AFTER_AUTH -> listOf(BleProtocol.encode(0x7E, BleProtocol.F_RESPONSE, t, emptyMap()))
                    Mode.TAMPER -> listOf(enc(type, BleProtocol.F_RESPONSE, t, body).also { it[it.size - 1] = (it[it.size - 1].toInt() xor 1).toByte() })
                    Mode.SILENT -> emptyList()
                    Mode.REQUEST_FROM_DEVICE -> listOf(enc(0x10, 0, 9, ByteArray(0)))
                }.also { plain.fill(0) }
            }
        }
    }

    private suspend fun TestScope.open(): OpenResult {
        fake.device = ::peer
        val store = CredentialStore(tmp.newFolder(), FakeKeyWrapper(), noise)
        store.commit(store.beginEnrollment(id), 0, device.publicKey, "P", 0).close()
        val m = DeviceSessionManager(GattTransportFactory(fake, { BluetoothReadiness.Ready }, backgroundScope), noise, store, backgroundScope)
        manager = m
        return m.open("A0:B1:C2:D3:E4:F5", OpenRequest.Session(id))
    }

    private lateinit var manager: DeviceSessionManager
    private suspend fun TestScope.opened(): SecureSession = (open() as OpenResult.Opened).session

    @Test
    fun ik_session_requests_and_end() = runTest {
        val s = opened()
        assertEquals(sessionId, s.hello.sessionId)
        assertTrue(s.request(0x70) is RequestResult.Ok)
        assertTrue(manager.state.value is SessionState.Authenticated)
        manager.end()
        runCurrent()
        assertFalse(s.isOpen)
        assertEquals(SessionState.Idle, manager.state.value)
        assertTrue(s.request(0x70) is RequestResult.NotSent)
    }

    @Test
    fun plaintext_after_authentication_closes() = runTest {
        val s = opened()
        mode = Mode.PLAIN_AFTER_AUTH
        assertTrue(s.request(0x70) is RequestResult.Uncertain)
        assertEquals("plaintext after authentication", s.closed.value)
    }

    @Test
    fun tampered_ciphertext_closes_without_retry() = runTest {
        val s = opened()
        mode = Mode.TAMPER
        assertTrue(s.request(0x70) is RequestResult.Uncertain)
        assertEquals("decryption failed", s.closed.value)
        assertEquals(1, received) // never retransmitted after a security failure
    }

    @Test
    fun fragment_error_after_authentication_closes() = runTest {
        val s = opened()
        fake.last.notifyRaw(byteArrayOf(0x41, 1)) // continuation without FIRST
        runCurrent()
        assertFalse(s.isOpen)
        assertTrue(s.closed.value!!.startsWith("fragment error"))
    }

    @Test
    fun a_request_from_the_device_closes() = runTest {
        val s = opened()
        mode = Mode.REQUEST_FROM_DEVICE
        assertTrue(s.request(0x70) is RequestResult.Uncertain)
        assertEquals("request from the device", s.closed.value)
    }

    @Test
    fun duplicate_response_first_wins_and_session_continues() = runTest {
        val s = opened()
        mode = Mode.DUPLICATE
        assertTrue(s.request(0x70) is RequestResult.Ok)
        runCurrent()
        assertEquals(1, s.discardedResponses.get())
        mode = Mode.NORMAL
        assertTrue(s.request(0x70) is RequestResult.Ok)
    }

    /**
     * Every attempt unanswered: the outcome is UNCERTAIN (the peer received and processed all three),
     * never a failure, and the session is closed because its nonce state is unknown (10.4a). The
     * retransmissions are the same txn with byte-identical plaintext: the manual-dose seq is kept.
     */
    @Test
    fun unanswered_request_is_retransmitted_identically_then_uncertain_and_closed() = runTest {
        val s = opened()
        mode = Mode.SILENT
        val dose = mapOf("seq" to 0x1122334455667788L, "volume_ul" to 2_000L)
        assertEquals(RequestResult.Uncertain("timeout", 3), s.request(0x40, dose, retries = 2))
        assertEquals(3, received) // original + 2 retransmissions
        assertEquals(1, requests.map { it.first }.distinct().size) // one txn
        val bodies = requests.map { it.second.toList() }.distinct()
        assertEquals(1, bodies.size) // identical plaintext
        assertEquals(0x1122334455667788L, (BleProtocol.decodeBody(cz.nowass.dispenser.ble.protocol.BleHeader(1, 0, 0x40, 0, requests[0].first, bodies[0].size),
            bodies[0].toByteArray())["seq"] as ULong).toLong()) // the original seq, never a new one
        assertEquals(SecureSession.REASON_TIMEOUT, s.closed.value)
        runCurrent()
        assertEquals(SessionState.Idle, manager.state.value)
        // nothing is retried in another session; the old one refuses provably without sending
        assertTrue(manager.request(0x40, dose) is RequestResult.NotSent)
        assertTrue(s.request(0x40, dose) is RequestResult.NotSent)
        assertEquals(3, received)
    }

    // ---- NotSent vs Uncertain boundary: NotSent only while nothing can have reached the device ----

    /** A write that started but failed (or was refused): the frame may be on the air — Uncertain, closed. */
    @Test
    fun failed_write_is_uncertain_never_not_sent() = runTest {
        val s = opened()
        fake.writeStatus = 133 // the write completion reports a failure: some bytes may have gone out
        val r = s.request(0x42, mapOf("capacity_ul" to 100_000L, "remaining_ul" to 90_000L))
        assertTrue("$r", r is RequestResult.Uncertain && r.attempts == 1)
        assertFalse(s.isOpen)
    }

    @Test
    fun refused_write_start_is_still_uncertain() = runTest {
        val s = opened()
        fake.writeStartFails = true // conservative: link.send was entered, so never NotSent
        val r = s.request(0x42, mapOf("capacity_ul" to 100_000L, "remaining_ul" to 90_000L))
        assertTrue("$r", r is RequestResult.Uncertain)
        assertFalse(s.isOpen)
    }

    /** A request queued behind another when the session closes never reaches the link: NotSent; the sent one: Uncertain. */
    @Test
    fun queued_request_of_a_closed_session_is_not_sent_the_sent_one_uncertain() = runTest {
        val s = opened()
        mode = Mode.SILENT
        val sent = async { s.request(0x42, mapOf("capacity_ul" to 100_000L, "remaining_ul" to 90_000L), retries = 0) }
        val queued = async { s.request(0x10) }
        runCurrent()
        assertEquals(1, received)
        s.close("disconnected by the app")
        assertEquals(RequestResult.Uncertain("disconnected by the app", 1), sent.await())
        assertEquals(RequestResult.NotSent("disconnected by the app"), queued.await())
        assertEquals(1, received) // the queued one was never encrypted or written
    }

    /** A caller cancelled after sending gets no result, so the session is closed (outcome unknown, nonce state untrusted). */
    @Test
    fun cancelled_request_after_send_closes_the_session() = runTest {
        val s = opened()
        mode = Mode.SILENT
        val job = async { s.request(0x42, mapOf("capacity_ul" to 100_000L, "remaining_ul" to 90_000L)) }
        runCurrent()
        assertEquals(1, received)
        job.cancel()
        runCurrent()
        assertEquals("request cancelled: outcome unknown", s.closed.value)
        assertTrue(s.request(0x10) is RequestResult.NotSent)
    }

    /** Without retransmissions too: one unanswered attempt closes the session (the device may have dropped it undecrypted). */
    @Test
    fun single_unanswered_attempt_is_uncertain_and_closes() = runTest {
        val s = opened()
        mode = Mode.SILENT
        assertEquals(RequestResult.Uncertain("timeout", 1), s.request(0x42, mapOf("capacity_ul" to 100_000L, "remaining_ul" to 90_000L), retries = 0))
        assertFalse(s.isOpen)
        assertEquals(1, received)
    }

    /** A response that cannot be decrypted while a request waits (nonce desynchronization looks the same): uncertain, closed. */
    @Test
    fun undecryptable_response_while_waiting_is_uncertain_not_failed() = runTest {
        val s = opened()
        mode = Mode.TAMPER
        val r = s.request(0x42, mapOf("capacity_ul" to 100_000L, "remaining_ul" to 90_000L))
        assertEquals(RequestResult.Uncertain("decryption failed", 1), r)
        assertEquals(1, received) // the device executed it once; never retransmitted after the failure
    }

    @Test
    fun bounded_waiting_requests() = runTest {
        val s = opened()
        mode = Mode.SILENT
        val all = (0 until 12).map { async { s.request(0x70, retries = 0) } }
        val results = all.awaitAll()
        assertTrue(results.count { it == RequestResult.Busy } >= 3) // more than 8 waiting
        assertTrue(results.all { it == RequestResult.Busy || it is RequestResult.Uncertain || it is RequestResult.NotSent })
    }

    @Test
    fun encrypted_events_are_delivered() = runTest {
        val s = opened()
        val ev = async { s.events.first() }
        runCurrent()
        val body = byteArrayOf(2, 25, 0) // reason MAX, 25 s
        fake.last.notifyFrame(enc(0x80, BleProtocol.F_EVENT, 0, body))
        assertEquals(DeviceEvent.SessionClosing(2, 25), ev.await())
        assertTrue(s.isOpen)
    }

    @Test
    fun hello_failures() = runTest {
        helloError = 2 // UNSUPPORTED_VERSION
        assertEquals(OpenResult.Failed(SessionFailure.HelloRejected(2)), open())
        helloError = null
        helloHash = ByteArray(8) { 1 } // another device answers
        assertEquals(OpenResult.Failed(SessionFailure.WrongDevice), open())
        assertTrue(fake.ports.all { it.closed })
    }

    /** ADD: the device key in XX message 2 differs from the invitation's -> abort before message 3, nothing staged. */
    @Test
    fun add_aborts_before_message_3_on_a_foreign_device_key() = runTest {
        impostor = noise.generateStaticKeyPair().privateKey.copyBytes()
        fake.device = ::peer
        val store = CredentialStore(tmp.newFolder(), FakeKeyWrapper(), noise)
        val m = DeviceSessionManager(GattTransportFactory(fake, { BluetoothReadiness.Ready }, backgroundScope), noise, store, backgroundScope)
        val labelText = cz.nowass.dispenser.crypto.SetupLabel.format(id, setup)
        val inviteText = cz.nowass.dispenser.crypto.Invitation.encode(TestNative.hex(id), device.publicKey,
            cz.nowass.dispenser.crypto.SecretBytes.wrap(token.copyOf()), 1_795_000_000, 600)
        val r = (cz.nowass.dispenser.crypto.SetupLabel.parse(labelText) as cz.nowass.dispenser.crypto.SetupLabel.Result.Ok).label.use { l ->
            (cz.nowass.dispenser.crypto.Invitation.parse(inviteText) as cz.nowass.dispenser.crypto.Invitation.Result.Ok).invitation.use { i ->
                m.open("A0:B1:C2:D3:E4:F5", OpenRequest.Add(l, i, "B"))
            }
        }
        assertEquals(OpenResult.Failed(SessionFailure.DeviceKeyMismatch), r)
        assertEquals(1, hsMessages) // message 3 (which would enroll) was never sent
        assertEquals(cz.nowass.dispenser.crypto.credentials.CredentialState.Absent, store.loadPending(id))
        assertEquals(cz.nowass.dispenser.crypto.credentials.CredentialState.Absent, store.load(id))
    }
}
