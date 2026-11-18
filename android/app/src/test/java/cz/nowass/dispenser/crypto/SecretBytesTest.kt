package cz.nowass.dispenser.crypto

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

class SecretBytesTest {
    @Test
    fun close_zeroizes_and_blocks_access() {
        val raw = ByteArray(16) { (it + 1).toByte() }
        val s = SecretBytes.wrap(raw)
        assertEquals("SecretBytes(**)", s.toString())
        s.close()
        assertTrue(raw.all { it.toInt() == 0 })
        assertTrue(s.isClosed)
        try {
            s.read { }
            fail("read after close")
        } catch (_: IllegalStateException) {
        }
        s.close() // idempotent
    }

    @Test
    fun takeAndClear_clears_the_source() {
        val src = ByteArray(8) { 7 }
        SecretBytes.takeAndClear(src).use { s -> s.read { assertEquals(7, it[0].toInt()) } }
        assertTrue(src.all { it.toInt() == 0 })
    }

    @Test
    fun constant_time_compare() {
        SecretBytes.wrap(byteArrayOf(1, 2, 3)).use { a ->
            SecretBytes.wrap(byteArrayOf(1, 2, 3)).use { b -> assertTrue(a.contentEqualsConstantTime(b)) }
            SecretBytes.wrap(byteArrayOf(1, 2, 4)).use { c -> assertFalse(a.contentEqualsConstantTime(c)) }
            SecretBytes.wrap(byteArrayOf(1, 2)).use { d -> assertFalse(a.contentEqualsConstantTime(d)) }
        }
    }
}
