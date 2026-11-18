package cz.nowass.dispenser.device.link

import cz.nowass.dispenser.ble.protocol.BleProtocol
import cz.nowass.dispenser.ble.protocol.BleResult
import cz.nowass.dispenser.device.transport.BluetoothReadiness
import cz.nowass.dispenser.device.transport.CloseReason
import cz.nowass.dispenser.device.transport.ConnectResult
import cz.nowass.dispenser.device.transport.FakeGatt
import cz.nowass.dispenser.device.transport.GattCallback
import cz.nowass.dispenser.device.transport.GattTransportFactory
import cz.nowass.dispenser.device.transport.TransportClosedException
import cz.nowass.dispenser.device.transport.TransportTimeouts
import kotlinx.coroutines.async
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.take
import kotlinx.coroutines.flow.toList
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

/** [FrameLink]: fragmentation, flow control, reassembly and fragment errors over the fake GATT. */
class FrameLinkTest {
    private val fake = FakeGatt()
    private var clock = 0L

    private suspend fun TestScope.link(): FrameLink {
        val f = GattTransportFactory(fake, { BluetoothReadiness.Ready }, backgroundScope, TransportTimeouts())
        val t = (f.connect("A0:B1:C2:D3:E4:F5") as ConnectResult.Connected).transport
        return FrameLink(t, backgroundScope) { clock }
    }

    private fun frame(n: Int, seed: Int = 1) = ByteArray(n) { (it * 31 + seed).toByte() }

    @Test
    fun echo_at_every_mtu_and_boundary_length() = runTest {
        for (mtu in listOf(23, 24, 64, 185, 247, 517)) {
            fake.deviceMtu = mtu
            val l = link()
            val data = mtu - 4
            for (n in listOf(1, data - 1, data, data + 1, 2 * data, 255, 519, 520).filter { it in 1..520 }) {
                val f = frame(n, mtu)
                val got = async { l.events.first { it is LinkEvent.Frame } as LinkEvent.Frame }
                l.send(f)
                assertArrayEquals("mtu $mtu len $n", f, got.await().bytes)
            }
            assertTrue(fake.last.written.all { it.size <= mtu - 3 })
            l.close()
        }
    }

    @Test
    fun next_fragment_only_after_the_write_response() = runTest {
        fake.deviceMtu = 23
        fake.autoAckWrites = false
        val l = link()
        val s = async { l.send(frame(60)) } // 4 fragments at MTU 23
        for (k in 1..4) {
            runCurrent()
            assertEquals(k, fake.last.written.size)
            fake.last.ackWrite()
        }
        s.await()
    }

    @Test
    fun concurrent_sends_never_interleave() = runTest {
        fake.deviceMtu = 23
        val frames = mutableListOf<ByteArray>()
        fake.device = { frames += it; emptyList() }
        val l = link()
        val a = async { l.send(frame(100, 1)) }
        val b = async { l.send(frame(100, 2)) }
        a.await(); b.await()
        assertEquals(2, frames.size) // the device reassembled both completely (interleaving would break it)
        assertArrayEquals(frame(100, 1), frames[0])
        assertArrayEquals(frame(100, 2), frames[1])
    }

    @Test
    fun malformed_fragments_from_the_device() = runTest {
        val l = link()
        val events = async { l.events.take(7).toList() }
        val p = fake.last
        p.notifyRaw(byteArrayOf(0x41, 1)) // no FIRST
        p.notifyRaw(byteArrayOf(0x80.toByte(), 1)); p.notifyRaw(byteArrayOf(0x42, 2)) // index gap
        p.notifyRaw(byteArrayOf(0x80.toByte(), 1)); p.notifyRaw(byteArrayOf(0x41)) // empty fragment
        p.notifyRaw(ByteArray(0)) // empty notification
        p.notifyRaw(byteArrayOf(0x80.toByte(), 1)); runCurrent(); clock += 2001; p.notifyRaw(byteArrayOf(0x41, 2)) // > 2 s gap
        runCurrent()
        // oversize: 3 x 243 data bytes > 520
        val big = ByteArray(244) { 1 }
        big[0] = 0x80.toByte(); p.notifyRaw(big.copyOf())
        big[0] = 0x01; p.notifyRaw(big.copyOf())
        big[0] = 0x42; p.notifyRaw(big.copyOf())
        p.notifyFrame(frame(30)) // a valid frame still arrives afterwards
        val got = events.await()
        assertEquals(listOf(BleResult.FRAG_NO_FIRST, BleResult.FRAG_INDEX, BleResult.FRAG_EMPTY, BleResult.FRAG_EMPTY,
            BleResult.FRAG_TIMEOUT, BleResult.FRAG_OVERFLOW), got.take(6).map { (it as LinkEvent.FragmentError).result })
        assertArrayEquals(frame(30), (got[6] as LinkEvent.Frame).bytes)
    }

    @Test
    fun mtu_change_applies_to_the_next_frame() = runTest {
        fake.deviceMtu = 23
        fake.autoAckWrites = false
        val l = link()
        val s = async { l.send(frame(60)) } // 4 fragments at MTU 23
        runCurrent()
        fake.last.mtu = 247 // the stack renegotiates while the frame is in progress (MTU only grows)
        fake.last.sink(GattCallback.MtuChanged(247, 0))
        repeat(4) { fake.last.ackWrite(); runCurrent() }
        s.await()
        assertEquals(listOf(20, 20, 20, 4), fake.last.written.map { it.size }) // the started frame kept its MTU
        val s2 = async { l.send(frame(300)) }
        repeat(2) { runCurrent(); fake.last.ackWrite() }
        s2.await()
        assertEquals(listOf(244, 58), fake.last.written.drop(4).map { it.size })
    }

    @Test
    fun closed_once_and_send_after_close_fails() = runTest {
        val l = link()
        val events = async { l.events.toList() }
        fake.last.drop(8)
        runCurrent()
        assertEquals(listOf<LinkEvent>(LinkEvent.Closed(CloseReason.Disconnected(8))), events.await())
        try {
            l.send(frame(10))
            fail("send after close")
        } catch (e: TransportClosedException) {
            assertEquals(CloseReason.Disconnected(8), e.reason)
        }
    }

    @Test
    fun frame_size_limits() = runTest {
        val l = link()
        for (n in listOf(0, BleProtocol.FRAME_MAX + 1)) {
            try {
                l.send(ByteArray(n))
                fail("frame of $n bytes accepted")
            } catch (_: IllegalArgumentException) {
            }
        }
    }

    /** Check 3: fragments always fit the negotiated MTU (517 requested, smaller agreed; Android 14+ auto negotiation). */
    @Test
    fun fragments_follow_the_negotiated_mtu() = runTest {
        for ((device, unsolicited) in listOf(185 to null, 23 to null, 100 to 100)) {
            fake.deviceMtu = device
            fake.unsolicitedMtuAfterConnect = unsolicited
            fake.acceptMtuRequest = unsolicited == null
            val l = link()
            val f = frame(520)
            val got = async { l.events.first { it is LinkEvent.Frame } as LinkEvent.Frame }
            l.send(f)
            assertArrayEquals(f, got.await().bytes)
            val sizes = fake.last.written.map { it.size }
            assertTrue("device $device: $sizes", sizes.all { it <= device - 3 })
            assertEquals((520 + device - 5) / (device - 4), sizes.size) // fragments at exactly that MTU
            l.close()
        }
    }
}
