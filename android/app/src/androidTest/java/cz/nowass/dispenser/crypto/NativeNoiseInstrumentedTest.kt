package cz.nowass.dispenser.crypto

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

/**
 * The APK's libdspnoise on ART (emulator or test phone): ble_session_v1 PSK vectors through JNI,
 * key generation, the initiator's first messages, and the CSPRNG in use. Byte-exact transcripts
 * need fixed ephemerals, which the APK library deliberately does not contain; those run on the
 * Android-native (NDK, bionic, FORTIFY) static test binary and on the host JNI library.
 */
@RunWith(AndroidJUnit4::class)
class NativeNoiseInstrumentedTest {
    private val noise = NativeNoiseProvider()

    private fun hex(s: String): ByteArray {
        val t = s.removePrefix("x:")
        if (t == "-") return ByteArray(0)
        return ByteArray(t.length / 2) { t.substring(2 * it, 2 * it + 2).toInt(16).toByte() }
    }

    @Test
    fun psk_vectors_on_art() {
        val text = InstrumentationRegistry.getInstrumentation().context.assets.open("ble_session_v1.txt").bufferedReader().readText()
        var n = 0
        for (line in text.lines().filter { it.startsWith("psk ") || it.startsWith("psk_not ") }) {
            val kv = line.split(' ').drop(1).associate { it.substringBefore('=') to it.substringAfter('=') }
            val purpose = Purpose.entries.first { it.wire == kv.getValue("purpose").toInt() }
            if (!purpose.usesPsk) continue
            val token = hex(kv.getValue("token"))
            val got = SecretBytes.wrap(hex(kv.getValue("setup"))).use { s ->
                (if (token.isEmpty()) null else SecretBytes.wrap(token)).use { t -> noise.derivePsk(purpose, s, t).use { it.copyBytes() } }
            }
            assertArrayEquals(kv["name"], hex(kv.getValue("psk")), got)
            n++
        }
        assertEquals(32, n)
    }

    @Test
    fun keys_initiator_and_rng() {
        assertTrue(noise.rngName, noise.rngName in setOf("sysrandom", "internal"))
        val kp = noise.generateStaticKeyPair()
        assertArrayEquals(kp.publicKey, noise.publicKey(kp.privateKey))
        val dev = noise.generateStaticKeyPair()
        val prologue = ByteArray(26) { it.toByte() }
        val m1 = noise.newInitiator(Purpose.SESSION, prologue, kp.privateKey, dev.publicKey, null).use { it.writeMessage() }
        val m2 = noise.newInitiator(Purpose.SESSION, prologue, kp.privateKey, dev.publicKey, null).use { it.writeMessage() }
        assertEquals(96, m1.size)
        assertNotEquals(m1.toList(), m2.toList())
        val x = SecretBytes.wrap(ByteArray(32) { 1 }).use { psk ->
            noise.newInitiator(Purpose.CLAIM, prologue, kp.privateKey, null, psk).use { it.writeMessage() }
        }
        assertEquals(48, x.size)
        kp.close()
        dev.close()
    }
}
