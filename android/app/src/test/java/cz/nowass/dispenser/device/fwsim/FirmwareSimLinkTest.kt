package cz.nowass.dispenser.device.fwsim

import cz.nowass.dispenser.device.transport.GattCallback
import java.util.concurrent.ConcurrentLinkedQueue
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import kotlin.concurrent.thread
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

/**
 * The simulator's connection lifecycle with REUSED BLE connection handles (as a real controller
 * reuses them): every connection gets the same handle, while events, writes and disconnects are
 * bound to the harness link id, which is never reused. A previous connection's poller, late
 * events or late GATT calls must never reach a later connection; the later connection must lose
 * nothing and see nothing twice.
 */
class FirmwareSimLinkTest {
    private val deviceId = byteArrayOf(0xa0.toByte(), 0xb1.toByte(), 0xc2.toByte(), 0xd3.toByte(), 0xe4.toByte(), 0xf5.toByte())
    private val setup = ByteArray(16) { (0x30 + it).toByte() }

    @Before
    fun setUp() {
        FirmwareSim.load()
        assertTrue(FirmwareSim.reset(deviceId, setup))
        assertTrue(FirmwareSim.buttonWake(false))
    }

    @After
    fun tearDown() {
        if (FirmwareSim.wakeRunning()) {
            FirmwareSim.advance(24L * 3600 * 1000)
            assertTrue(FirmwareSim.joinWake(20_000))
        }
        val d = FirmwareSim.diagnostics()
        assertEquals("DSP_CHECK failures inside the firmware world", 0, d[0])
        assertEquals("dropped radio events", 0, d[1])
        assertEquals("security layer idle and wiped after the wake", 1, d[2])
    }

    /** Connects (waiting for the transport to advertise again); returns (link, handle). */
    private fun connect(): Pair<Long, Int> {
        assertTrue("advertising", FirmwareSim.waitAdvertising(5_000))
        val token = FirmwareSim.connect(247)
        assertTrue("connect refused", token != 0L)
        return FirmwareSim.linkOf(token) to FirmwareSim.handleOf(token)
    }

    /** A notification fragment (first|last) carrying [tag]; the transport layer never parses it here. */
    private fun frag(tag: Int) = byteArrayOf(0xC0.toByte(), 0, 0, 0x7F, (tag shr 8).toByte(), tag.toByte())
    private fun tagOf(payload: ByteArray) = ((payload[4].toInt() and 0xFF) shl 8) or (payload[5].toInt() and 0xFF)

    private fun pollTag(link: Long): Int {
        val e = assertNotNull(FirmwareSim.poll(link, 2_000))
        assertEquals(FirmwareSim.EVT_NOTIFY, e[0].toInt())
        assertEquals(link, FirmwareSim.eventLink(e))
        return tagOf(e.copyOfRange(FirmwareSim.EVT_HEADER, e.size))
    }

    private fun assertGone(link: Long) {
        val t0 = System.nanoTime()
        val e = assertNotNull(FirmwareSim.poll(link, 2_000))
        assertEquals("an ended link's poll reports GONE", FirmwareSim.EVT_GONE, e[0].toInt())
        assertEquals(link, FirmwareSim.eventLink(e))
        assertTrue("GONE is immediate, not a timeout", System.nanoTime() - t0 < 1_000_000_000L)
    }

    private fun <T> assertNotNull(v: T?): T = v.also { org.junit.Assert.assertNotNull(it) }!!

    @Test
    fun repeated_connect_disconnect_cycles_reuse_the_same_handle() {
        var lastLink = 0L
        repeat(25) { i ->
            val (link, handle) = connect()
            assertEquals("the controller reuses the handle", 0x0001, handle)
            assertTrue("link ids are never reused", link > lastLink)
            lastLink = link
            assertTrue(FirmwareSim.injectNotify(frag(i)))
            val e = assertNotNull(FirmwareSim.poll(link, 2_000))
            assertEquals(0x0001, FirmwareSim.eventHandle(e))
            assertEquals(i, tagOf(e.copyOfRange(FirmwareSim.EVT_HEADER, e.size)))
            FirmwareSim.disconnect(link)
            assertGone(link)
            assertEquals("a write on an ended link reaches nothing", -2, FirmwareSim.write(link, frag(i)))
        }
        assertEquals(0, FirmwareSim.staleEvents())
    }

    @Test
    fun a_chosen_handle_is_reused_too() {
        assertTrue(FirmwareSim.setConnectionHandle(0x0042))
        val (a, ha) = connect()
        assertFalse("not while connected", FirmwareSim.setConnectionHandle(0x0043))
        FirmwareSim.disconnect(a)
        val (b, hb) = connect()
        assertEquals(0x0042, ha)
        assertEquals(ha, hb)
        assertTrue(b > a)
        FirmwareSim.disconnect(b)
    }

    @Test
    fun undelivered_events_of_the_previous_link_are_never_delivered_to_the_next() {
        val (a, _) = connect()
        assertTrue(FirmwareSim.injectNotify(frag(1)))
        assertTrue(FirmwareSim.injectNotify(frag(2)))
        FirmwareSim.disconnect(a) // nobody read a's events: they arrive "late"
        val (b, _) = connect()
        assertTrue(FirmwareSim.injectNotify(frag(10)))
        assertTrue(FirmwareSim.injectNotify(frag(11)))
        assertEquals(10, pollTag(b)) // a's events are skipped, never handed to b
        assertEquals(11, pollTag(b))
        assertEquals("a's two events discarded as stale", 2, FirmwareSim.staleEvents())
        assertGone(a) // a's poller learns its link is over and stops
        assertTrue(FirmwareSim.injectNotify(frag(12)))
        assertEquals(12, pollTag(b)) // a's GONE took nothing from b
        FirmwareSim.disconnect(b)
    }

    @Test
    fun a_late_poller_reads_only_its_own_leftovers_then_stops() {
        val (a, _) = connect()
        assertTrue(FirmwareSim.injectNotify(frag(1)))
        assertTrue(FirmwareSim.injectNotify(frag(2)))
        FirmwareSim.disconnect(a)
        val (b, _) = connect()
        assertTrue(FirmwareSim.injectNotify(frag(10)))
        assertEquals(1, pollTag(a)) // its own, in order
        assertEquals(2, pollTag(a))
        assertGone(a) // b's event is at the head now: a stops without taking it
        assertGone(a)
        assertEquals(10, pollTag(b))
        assertEquals(0, FirmwareSim.staleEvents())
        FirmwareSim.disconnect(b)
    }

    @Test
    fun late_gatt_calls_of_the_previous_link_do_not_touch_the_next() {
        val (a, _) = connect()
        FirmwareSim.disconnect(a)
        val (b, _) = connect()
        FirmwareSim.disconnect(a) // a late disconnect from the old client: a no-op
        assertEquals(-2, FirmwareSim.write(a, frag(0))) // a late write from the old client: refused
        assertTrue("b is still connected", FirmwareSim.injectNotify(frag(7)))
        assertEquals(7, pollTag(b))
        FirmwareSim.disconnect(b)
    }

    /**
     * A raw poller of the previous link keeps polling — even after GONE, as a poller that missed
     * it would — for the whole time the next link connects and receives a burst: it must never get
     * anything but GONE, and the next link receives every event exactly once, in order.
     * Repeated for interleavings.
     */
    @Test
    fun reconnection_while_the_previous_poller_is_still_polling() {
        repeat(30) { round ->
            val (a, _) = connect()
            val polling = CountDownLatch(1)
            val staleGot = ConcurrentLinkedQueue<ByteArray>()
            val gones = java.util.concurrent.atomic.AtomicInteger()
            val stop = java.util.concurrent.atomic.AtomicBoolean(false)
            val stale = thread(name = "stale-poller-$round") {
                polling.countDown()
                // until stopped, and at least once after its link ended (a starved thread still checks)
                while (!stop.get() || gones.get() == 0) {
                    val e = FirmwareSim.poll(a, 5) ?: continue
                    if (e[0].toInt() == FirmwareSim.EVT_GONE) gones.incrementAndGet() else staleGot += e
                }
            }
            assertTrue(polling.await(2, TimeUnit.SECONDS))
            FirmwareSim.disconnect(a)
            val (b, _) = connect()
            val n = 40
            val got = ArrayList<Int>()
            val producer = thread { repeat(n) { assertTrue(FirmwareSim.injectNotify(frag(round * 100 + it))) } }
            while (got.size < n) got += pollTag(b)
            producer.join()
            stop.set(true)
            stale.join(2_000)
            assertFalse("the stale poller stopped", stale.isAlive)
            assertTrue("the stale poller was told its link is over", gones.get() > 0)
            assertTrue("nothing went to a's poller: ${staleGot.map { FirmwareSim.eventLink(it) }}", staleGot.isEmpty())
            assertEquals((0 until n).map { round * 100 + it }, got) // none lost, none duplicated, in order
            FirmwareSim.disconnect(b)
            assertGone(b)
        }
    }

    /**
     * The real [FirmwareGattPortFactory] ports: the previous port is being stopped (its poller still
     * inside a poll) while the next port connects; the previous port never delivers a later
     * notification, its poller terminates, and the next port receives everything once, in order.
     */
    @Test
    fun gatt_ports_reconnecting_while_the_previous_poller_stops() {
        val factory = FirmwareGattPortFactory()
        repeat(20) { round ->
            val oldEvents = ConcurrentLinkedQueue<GattCallback>()
            val p1 = factory.create("A0:B1:C2:D3:E4:F5") { oldEvents += it } as FirmwareGattPortFactory.Port
            p1.connect()
            assertTrue(p1.link != 0L)
            assertTrue(FirmwareSim.injectNotify(frag(1))) // delivered to p1, its own link
            waitUntil { oldEvents.any { it is GattCallback.Notification } }
            p1.disconnect() // stopping: its poller may still be inside FirmwareSim.poll
            val newEvents = ConcurrentLinkedQueue<GattCallback>()
            val p2 = factory.create("A0:B1:C2:D3:E4:F5") { newEvents += it } as FirmwareGattPortFactory.Port
            p2.connect()
            assertEquals("same handle", p1.handle, p2.handle)
            assertTrue(p2.link > p1.link)
            val before = oldEvents.size
            val n = 30
            repeat(n) { assertTrue(FirmwareSim.injectNotify(frag(round * 100 + it))) }
            waitUntil { newEvents.count { it is GattCallback.Notification } == n }
            waitUntil { p1.pollerDone }
            p1.disconnect() // a late call from the old client: must not drop p2's link
            assertTrue("p2 is still connected", FirmwareSim.injectNotify(frag(9999)))
            waitUntil { newEvents.count { it is GattCallback.Notification } == n + 1 }
            assertEquals("the old port delivered nothing after stopping", before, oldEvents.size)
            val tags = newEvents.filterIsInstance<GattCallback.Notification>().map { tagOf(it.value) }
            assertEquals((0 until n).map { round * 100 + it } + 9999, tags)
            assertTrue(newEvents.none { it is GattCallback.ConnectionState && !it.connected })
            p2.disconnect()
            waitUntil { p2.pollerDone }
        }
    }

    private fun waitUntil(timeoutMs: Long = 3_000, cond: () -> Boolean) {
        val end = System.currentTimeMillis() + timeoutMs
        while (!cond()) {
            assertTrue("condition not reached in $timeoutMs ms", System.currentTimeMillis() < end)
            Thread.sleep(2)
        }
    }
}
