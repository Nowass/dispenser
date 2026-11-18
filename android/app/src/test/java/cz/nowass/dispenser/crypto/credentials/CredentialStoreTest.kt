package cz.nowass.dispenser.crypto.credentials

import cz.nowass.dispenser.crypto.TestNative
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/** Credential lifecycle (docs/ANDROID_BLE_DESIGN.md 11.1): commit only after ENROLL_RESULT, loss, tamper, seq durability. */
class CredentialStoreTest {
    @get:Rule val tmp = TemporaryFolder()
    private val noise = TestNative.load()
    private val wrapper = FakeKeyWrapper()
    private val devicePub = ByteArray(32) { (0x40 + it).toByte() }
    private fun store() = CredentialStore(tmp.root, wrapper, noise)
    private val id = "a0b1c2d3e4f5"

    private fun enrolled(s: CredentialStore = store()): DeviceCredential {
        val p = s.beginEnrollment(id)
        return s.commit(p, 0, devicePub, "Telefon Ž", 1795000000L)
    }

    @Test
    fun nothing_is_stored_before_commit_and_abort_deletes_the_key() {
        val s = store()
        val p = s.beginEnrollment(id)
        assertEquals(1, wrapper.keys.size)
        assertEquals(CredentialState.Absent, s.load(id))
        s.abort(p)
        assertTrue(wrapper.keys.isEmpty())
        assertTrue(p.keyPair.privateKey.isClosed)
        assertEquals(CredentialState.Absent, s.load(id))
        assertTrue(tmp.root.listFiles()!!.isEmpty())
    }

    @Test
    fun commit_round_trip_and_process_restart() {
        enrolled().use { c -> assertEquals(0, c.slot) }
        // a new store instance (process restart) reads the same record
        val st = store().load(id) as CredentialState.Present
        st.credential.use { c ->
            assertEquals("Telefon Ž", c.name)
            assertArrayEquals(devicePub, c.deviceStaticPublic)
            assertArrayEquals(c.phoneStaticPublic, noise.publicKey(c.phoneStaticPrivate))
            assertEquals(0L, c.manualSeqHigh)
            assertEquals("DeviceCredential(device=a0b1c2d3e4f5, slot=0, key=**)", c.toString())
        }
        // the private key never appears in the file in clear
        val file = tmp.root.listFiles()!!.single { it.name.endsWith(".cred") }
        (store().load(id) as CredentialState.Present).credential.use { c ->
            c.phoneStaticPrivate.read { priv -> assertFalse(file.readBytes().toList().windowed(32).any { it == priv.toList() }) }
        }
    }

    @Test
    fun key_loss_is_lost_and_never_regenerated() {
        enrolled().close()
        wrapper.keys.clear() // reinstall / cleared storage / Keystore reset
        assertEquals(CredentialState.Lost, store().load(id))
        try {
            store().nextManualSeq(id, 1)
            fail("seq without a credential")
        } catch (_: IllegalStateException) {
        }
        assertTrue(wrapper.keys.isEmpty()) // no silent new key
    }

    @Test
    fun tampering_and_unknown_format_are_unreadable() {
        enrolled().close()
        val f = tmp.root.listFiles()!!.single { it.name.endsWith(".cred") }
        val b = f.readBytes()
        b[b.size - 5] = (b[b.size - 5].toInt() xor 1).toByte()
        f.writeBytes(b)
        assertEquals(CredentialState.Unreadable(CredentialState.Unreadable.Reason.TAMPERED), store().load(id))
        f.writeBytes("DSPC".toByteArray() + byteArrayOf(9, 1, 0x41))
        assertEquals(CredentialState.Unreadable(CredentialState.Unreadable.Reason.UNKNOWN_FORMAT), store().load(id))
    }

    @Test
    fun a_record_of_another_device_does_not_load() {
        enrolled().close()
        val f = tmp.root.listFiles()!!.single { it.name.endsWith(".cred") }
        f.copyTo(java.io.File(tmp.root, "000000000001.cred"))
        // AAD binds the device ID: the copied record fails authentication
        assertEquals(CredentialState.Unreadable(CredentialState.Unreadable.Reason.TAMPERED), store().load("000000000001"))
    }

    @Test
    fun manual_seq_is_durable_before_it_is_returned() {
        enrolled().close()
        val s = store()
        assertEquals(1L, s.nextManualSeq(id, 1))
        assertEquals(2L, s.nextManualSeq(id, 1))
        assertEquals(10L, s.nextManualSeq(id, 10)) // device floor after a reinstall of the counter
        // restart: the high water survived
        assertEquals(11L, store().nextManualSeq(id, 1))
        (store().load(id) as CredentialState.Present).credential.use { assertEquals(11L, it.manualSeqHigh) }
    }

    @Test
    fun replacement_keeps_the_old_credential_until_the_new_commit() {
        enrolled().close()
        val s = store()
        val oldAlias = wrapper.keys.keys.single()
        val p = s.beginEnrollment(id) // e.g. RECOVERY
        assertTrue(store().load(id) is CredentialState.Present) // old one still usable
        s.commit(p, 0, ByteArray(32) { 7 }, "Nový", 1795000100L).close()
        assertFalse(oldAlias in wrapper.keys) // deleted after the new record is durable
        (store().load(id) as CredentialState.Present).credential.use { assertEquals("Nový", it.name) }
    }

    @Test
    fun delete_removes_file_and_key() {
        enrolled().close()
        store().delete(id)
        assertEquals(CredentialState.Absent, store().load(id))
        assertTrue(wrapper.keys.isEmpty())
    }

    /** 11.7: ENROLL_RESULT lost after the device committed — the phone keeps the staged key. */
    @Test
    fun staged_enrollment_survives_a_lost_enroll_result_and_confirms() {
        val s = store()
        val p = s.beginEnrollment(id)
        s.stage(p, devicePub, "Telefon", 1795000000L) // before XX message 3
        try {
            s.abort(p) // a staged enrollment cannot be dropped silently
            fail("staged enrollment aborted")
        } catch (_: IllegalStateException) {
        }
        s.release(p) // connection lost, ENROLL_RESULT never arrived
        assertEquals(CredentialState.Absent, store().load(id)) // not yet usable as confirmed
        val pending = store().loadPending(id) as CredentialState.Present // process restart: still there
        pending.credential.use { c ->
            assertEquals(-1, c.slot)
            assertArrayEquals(devicePub, c.deviceStaticPublic)
        }
        // the IK SESSION with the staged key succeeded (session layer): confirm with the slot from PHONES_LIST
        store().confirmPending(id, 2)
        (store().load(id) as CredentialState.Present).credential.use { assertEquals(2, it.slot) }
        assertEquals(CredentialState.Absent, store().loadPending(id))
        assertEquals(1, wrapper.keys.size)
    }

    @Test
    fun staged_enrollment_normal_commit_and_explicit_discard() {
        val s = store()
        val p = s.beginEnrollment(id)
        s.stage(p, devicePub, "A", 1L)
        s.commit(p, 0, devicePub, "A", 1L).close() // ENROLL_RESULT received
        assertEquals(CredentialState.Absent, store().loadPending(id))
        assertTrue(store().load(id) is CredentialState.Present)
        assertEquals(1, wrapper.keys.size) // same key: the staging record only
        // a second staged enrollment (e.g. RECOVERY) whose confirmation failed: discard keeps the old one
        val p2 = s.beginEnrollment(id)
        s.stage(p2, ByteArray(32) { 5 }, "B", 2L)
        s.release(p2)
        store().discardPending(id)
        assertEquals(CredentialState.Absent, store().loadPending(id))
        (store().load(id) as CredentialState.Present).credential.use { assertEquals("A", it.name) }
        assertEquals(1, wrapper.keys.size)
    }

    @Test
    fun staged_key_loss_is_reported() {
        val s = store()
        val p = s.beginEnrollment(id)
        s.stage(p, devicePub, "A", 1L)
        s.release(p)
        wrapper.keys.clear()
        assertEquals(CredentialState.Lost, store().loadPending(id))
    }
}
