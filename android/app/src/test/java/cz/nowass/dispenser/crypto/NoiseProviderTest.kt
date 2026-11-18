package cz.nowass.dispenser.crypto

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

/** Contract of [NativeNoiseProvider] and its native wrappers (host build of the JNI library). */
class NoiseProviderTest {
    private val noise = TestNative.load()
    private val prologue = ByteArray(26) { it.toByte() }

    @Test
    fun csprng_is_libsodiums_not_a_test_source() {
        assertTrue(noise.rngName, noise.rngName in setOf("sysrandom", "internal"))
    }

    @Test
    fun keypairs_are_fresh_and_consistent() {
        val a = noise.generateStaticKeyPair()
        val b = noise.generateStaticKeyPair()
        assertFalse(a.publicKey.contentEquals(b.publicKey))
        assertArrayEquals(a.publicKey, noise.publicKey(a.privateKey))
        assertEquals("StaticKeyPair(**)", a.toString())
        a.close(); b.close()
        assertTrue(a.privateKey.isClosed)
    }

    @Test
    fun argument_rules() {
        val k = SecretBytes.wrap(ByteArray(32) { 9 })
        val psk = SecretBytes.wrap(ByteArray(32) { 3 })
        expectIllegal { noise.newInitiator(Purpose.SESSION, prologue, k, null, null) }          // IK needs the pinned key
        expectIllegal { noise.newInitiator(Purpose.SESSION, prologue, k, ByteArray(32), psk) }  // IK has no PSK
        expectIllegal { noise.newInitiator(Purpose.CLAIM, prologue, k, null, null) }            // XXpsk3 needs a PSK
        expectIllegal { noise.newInitiator(Purpose.CLAIM, prologue, k, ByteArray(32), psk) }    // and no remote key
        SecretBytes.wrap(ByteArray(16)).use { setup ->
            expectIllegal { noise.derivePsk(Purpose.SESSION, setup, null) }
            expectIllegal { noise.derivePsk(Purpose.CLAIM, setup, SecretBytes.wrap(ByteArray(16))) }
            expectIllegal { noise.derivePsk(Purpose.ADD, setup, null) }
        }
        // wrong lengths reach the native checks
        try {
            SecretBytes.wrap(ByteArray(15)).use { noise.derivePsk(Purpose.CLAIM, it, null) }
            fail("short setup code accepted")
        } catch (e: NoiseException) {
            assertEquals(NoiseException.E_ARG, e.code)
        }
        k.close(); psk.close()
    }

    @Test
    fun state_errors_and_use_after_close() {
        val hs = SecretBytes.wrap(ByteArray(32) { 5 }).use { k ->
            SecretBytes.wrap(ByteArray(32) { 6 }).use { p -> noise.newInitiator(Purpose.CLAIM, prologue, k, null, p) }
        }
        try {
            hs.readMessage(ByteArray(32)) // the initiator writes first
            fail("read in write state")
        } catch (e: NoiseException) {
            assertEquals(NoiseException.E_STATE, e.code)
        }
        assertEquals(48, hs.writeMessage().size) // XXpsk3 message 1: e + tag of the empty payload (psk mixes e into the key)
        try {
            hs.split()
            fail("split before the end")
        } catch (e: NoiseException) {
            assertEquals(NoiseException.E_STATE, e.code)
        }
        hs.close()
        hs.close()
        try {
            hs.writeMessage()
            fail("use after close")
        } catch (_: IllegalStateException) {
        }
    }

    @Test
    fun ik_first_message_differs_per_run() {
        val dev = noise.generateStaticKeyPair()
        val m = (0 until 2).map {
            SecretBytes.wrap(ByteArray(32) { 1 }).use { k -> noise.newInitiator(Purpose.SESSION, prologue, k, dev.publicKey, null) }
                .use { it.writeMessage() }
        }
        assertEquals(96, m[0].size) // e, es, s, ss: 32 + 48 + 16
        assertNotEquals(m[0].toList(), m[1].toList())
        dev.close()
    }

    private fun expectIllegal(block: () -> Unit) {
        try {
            block()
            fail("accepted")
        } catch (_: IllegalArgumentException) {
        }
    }
}
