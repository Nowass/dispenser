package cz.nowass.dispenser.crypto

import cz.nowass.dispenser.crypto.TestNative.hex
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Test

/**
 * test-vectors/ble_session_v1.txt (Python standard-library reference): PSK through the native
 * libsodium HKDF (approved C1), prologue and device_id_hash in Kotlin.
 */
class SessionVectorsTest {
    private val noise = TestNative.load()
    private val records = TestNative.records(TestNative.vectorFile("ble_session_v1.txt"))

    private fun purpose(n: Int) = Purpose.entries.first { it.wire == n }

    @Test
    fun psk_records() {
        var n = 0
        for ((rec, kv) in records.filter { it.first == "psk" || it.first == "psk_not" }) {
            val p = purpose(kv.getValue("purpose").toInt())
            if (!p.usesPsk) continue // SESSION (IK) has no PSK; the API refuses one
            val token = hex(kv.getValue("token"))
            val got = SecretBytes.wrap(hex(kv.getValue("setup"))).use { s ->
                (if (token.isEmpty()) null else SecretBytes.wrap(token)).use { t -> noise.derivePsk(p, s, t).use { it.copyBytes() } }
            }
            assertArrayEquals(kv["name"], hex(kv.getValue("psk")), got)
            if (rec == "psk_not") assertFalse(hex(kv.getValue("wrong")).contentEquals(got))
            n++
        }
        assertEquals(32, n) // 36 psk/psk_not records, minus the 4 SESSION ones
    }

    @Test
    fun prologue_and_idhash_records() {
        var n = 0
        for ((rec, kv) in records) {
            when (rec) {
                "prologue" -> {
                    val got = SessionDerivation.prologue(kv.getValue("major").toInt(), kv.getValue("minor").toInt(),
                        purpose(kv.getValue("purpose").toInt()), hex(kv.getValue("hash")), kv.getValue("session_id").toLong())
                    assertArrayEquals(kv["name"], hex(kv.getValue("prologue")), got)
                    n++
                }
                "idhash" -> {
                    val input = hex(kv.getValue("input"))
                    val got = if (kv["kind"] == "claimed") SessionDerivation.claimedDeviceIdHash(input) else SessionDerivation.unclaimedDeviceIdHash(input)
                    assertArrayEquals(kv["name"], hex(kv.getValue("hash")), got)
                    n++
                }
            }
        }
        assertEquals(30, n)
    }
}
