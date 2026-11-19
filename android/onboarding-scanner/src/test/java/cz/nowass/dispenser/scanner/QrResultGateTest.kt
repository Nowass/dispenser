package cz.nowass.dispenser.scanner

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class QrResultGateTest {
    @Test
    fun delivers_only_the_first_accepted_value() {
        val g = QrResultGate(listOf("DSP2:", "DSP2I1:"))
        assertNull(g.offer(null))
        assertNull(g.offer(" "))
        assertNull("unknown QR ignored", g.offer("https://example.com"))
        assertEquals("DSP2:a0b1c2d3e4f5:X", g.offer("DSP2:a0b1c2d3e4f5:X"))
        assertNull("one scan, one submission", g.offer("DSP2I1:ABC"))
    }

    @Test
    fun no_prefixes_accepts_any_non_blank_value() {
        assertEquals("x", QrResultGate().offer("x"))
    }
}
