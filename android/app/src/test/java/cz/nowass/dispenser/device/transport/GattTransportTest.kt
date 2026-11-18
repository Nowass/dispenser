package cz.nowass.dispenser.device.transport

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.toList
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.advanceTimeBy
import kotlinx.coroutines.test.currentTime
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import kotlinx.coroutines.withTimeout
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import java.util.concurrent.CountDownLatch
import kotlin.concurrent.thread

/** [GattTransport] against the deterministic [FakeGatt] (virtual time). docs/ANDROID_BLE_DESIGN.md 4.2–4.4, 6.4. */
class GattTransportTest {
    private val addr = "A0:B1:C2:D3:E4:F5"
    private var readiness: BluetoothReadiness = BluetoothReadiness.Ready
    private val fake = FakeGatt()

    private fun TestScope.factory(capacity: Int = GattTransport.DEFAULT_NOTIFICATION_CAPACITY) =
        GattTransportFactory(fake, { readiness }, backgroundScope, TransportTimeouts(), capacity)

    private suspend fun GattTransportFactory.open(): GattTransport = (connect(addr) as ConnectResult.Connected).transport as GattTransport
    private suspend fun GattTransportFactory.failure(): CloseReason = (connect(addr) as ConnectResult.Failed).reason

    @Test
    fun setup_sequence_and_values() = runTest {
        val t = factory().open()
        assertEquals(listOf("connect", "priority", "discover", "mtu", "info", "subscribe"), fake.last.ops)
        assertEquals(247, t.mtu)
        assertEquals(1L, t.generation)
        assertEquals(0x1234, t.info.deviceTag)
        assertEquals(null, t.closeReason.value)
        t.close()
        assertEquals(CloseReason.Local, t.closeReason.value)
        assertTrue(fake.last.closed && fake.last.disconnected)
        t.close(CloseReason.BluetoothOff) // idempotent: the first reason stays
        assertEquals(CloseReason.Local, t.closeReason.value)
    }

    @Test
    fun mtu_negotiation_cases() = runTest {
        fake.deviceMtu = 23
        assertEquals(23, factory().open().mtu)
        fake.deviceMtu = 600 // the stack never reports more than requested
        assertEquals(517, factory().open().mtu)
        fake.reportedMtu = 600 // a misbehaving stack: clamped to the requested 517
        assertEquals(517, factory().open().mtu)
        fake.reportedMtu = 20 // below the ATT minimum: clamped to 23
        assertEquals(23, factory().open().mtu)
        fake.reportedMtu = null
        fake.deviceMtu = 247
        fake.acceptMtuRequest = false // request refused: keep 23, setup continues
        assertEquals(23, factory().open().mtu)
        fake.acceptMtuRequest = true
        fake.mtuStatus = 4 // failure status: keep 23
        assertEquals(23, factory().open().mtu)
        fake.mtuStatus = DispenserGatt.GATT_SUCCESS
        fake.respondToMtu = false // no callback: 5 s, then 23, setup continues
        val start = currentTime
        val t = factory().open()
        assertEquals(23, t.mtu)
        assertEquals(TransportTimeouts().mtuMs, currentTime - start)
        // an unsolicited MTU change (newer Android negotiates on its own) is used for the next frame
        fake.last.sink(GattCallback.MtuChanged(185, DispenserGatt.GATT_SUCCESS))
        runCurrent()
        assertEquals(185, t.mtu)
    }

    @Test
    fun setup_failures_close_and_release() = runTest {
        val f = factory()
        fake.connectStatus = 133
        assertEquals(CloseReason.Disconnected(133), f.failure())
        assertTrue(fake.last.closed)
        fake.connectStatus = DispenserGatt.GATT_SUCCESS

        fake.respondToConnect = false
        val start = currentTime
        assertEquals(CloseReason.Timeout(CloseReason.Step.CONNECT), f.failure())
        assertEquals(TransportTimeouts().connectMs, currentTime - start)
        fake.respondToConnect = true

        fake.servicesComplete = false
        assertEquals(CloseReason.NotADispenser, f.failure())
        fake.servicesComplete = true
        fake.discoveryStatus = 129
        assertEquals(CloseReason.GattFailure(CloseReason.Step.DISCOVERY, 129), f.failure())
        fake.discoveryStatus = DispenserGatt.GATT_SUCCESS
        fake.respondToDiscovery = false
        assertEquals(CloseReason.Timeout(CloseReason.Step.DISCOVERY), f.failure())
        fake.respondToDiscovery = true

        fake.info = ByteArray(32) // no magic
        assertEquals(CloseReason.NotADispenser, f.failure())
        fake.info = FakeGatt.validInfo().copyOf(31)
        assertEquals(CloseReason.NotADispenser, f.failure())
        fake.info = FakeGatt.validInfo()
        fake.infoStatus = 2
        assertEquals(CloseReason.GattFailure(CloseReason.Step.INFO, 2), f.failure())
        fake.infoStatus = DispenserGatt.GATT_SUCCESS

        fake.subscribeStatus = 3
        assertEquals(CloseReason.GattFailure(CloseReason.Step.SUBSCRIBE, 3), f.failure())
        fake.subscribeStatus = DispenserGatt.GATT_SUCCESS
        fake.respondToSubscribe = false
        assertEquals(CloseReason.Timeout(CloseReason.Step.SUBSCRIBE), f.failure())
        assertTrue(fake.ports.all { it.closed && it.disconnected })
    }

    @Test
    fun not_ready_attempts_nothing() = runTest {
        for (r in listOf(BluetoothReadiness.BluetoothOff, BluetoothReadiness.Unsupported,
            BluetoothReadiness.PermissionMissing(listOf("android.permission.BLUETOOTH_CONNECT")))) {
            readiness = r
            assertEquals(ConnectResult.NotReady(r), factory().connect(addr))
        }
        assertTrue(fake.ports.isEmpty())
        try {
            factory().connect("a0:b1:c2:d3:e4:f5")
            fail("lower-case address accepted")
        } catch (_: IllegalArgumentException) {
        }
    }

    @Test
    fun permission_revoked_at_every_step() = runTest {
        for (op in listOf("connect", "priority", "discover", "mtu", "info", "subscribe")) {
            fake.securityExceptionOn = op
            assertEquals(op, CloseReason.PermissionRevoked, factory().failure())
            assertTrue(fake.last.closed)
        }
        fake.securityExceptionOn = null
        val t = factory().open()
        fake.securityExceptionOn = "write"
        try {
            t.writeFragment(byteArrayOf(0xC0.toByte(), 1))
            fail("write accepted")
        } catch (e: TransportClosedException) {
            assertEquals(CloseReason.PermissionRevoked, e.reason)
        }
        assertEquals(CloseReason.PermissionRevoked, t.closeReason.value)
    }

    @Test
    fun bluetooth_off_and_link_drop() = runTest {
        val f = factory()
        val a = f.open()
        val events = async { a.events.toList() }
        fake.last.drop(8)
        runCurrent()
        assertEquals(CloseReason.Disconnected(8), a.closeReason.value)
        assertEquals(listOf<TransportEvent>(TransportEvent.Closed(CloseReason.Disconnected(8))), events.await())
        val b = f.open()
        readiness = BluetoothReadiness.BluetoothOff // the user switched Bluetooth off: the stack drops the link
        fake.last.drop(19)
        runCurrent()
        assertEquals(CloseReason.BluetoothOff, b.closeReason.value)
    }

    @Test
    fun generations_and_stale_callbacks_never_reach_a_newer_connection() = runTest {
        val f = factory()
        val a = f.open()
        val oldPort = fake.last
        a.close()
        val b = f.open()
        assertEquals(2L, b.generation)
        val events = async { b.events.toList() }
        // the old connection's callbacks arrive late (binder threads): all dropped
        oldPort.notifyRaw(byteArrayOf(0xC0.toByte(), 9, 9))
        oldPort.drop(8)
        oldPort.sink(GattCallback.RequestWritten(0))
        oldPort.sink(GattCallback.MtuChanged(23, 0))
        runCurrent()
        assertEquals(null, b.closeReason.value)
        assertEquals(247, b.mtu)
        // the new connection works and only its own notification arrives
        fake.last.notifyRaw(byteArrayOf(0xC0.toByte(), 1))
        runCurrent()
        b.close()
        val got = events.await()
        assertEquals(2, got.size)
        assertTrue((got[0] as TransportEvent.Notification).fragment.contentEquals(byteArrayOf(0xC0.toByte(), 1)))
        assertEquals(TransportEvent.Closed(CloseReason.Local), got[1])
    }

    @Test
    fun one_write_in_flight_and_write_failures() = runTest {
        fake.autoAckWrites = false
        val t = factory().open()
        val w1 = async { t.writeFragment(byteArrayOf(0x80.toByte(), 1)) }
        val w2 = async { t.writeFragment(byteArrayOf(0x41, 2)) }
        runCurrent()
        assertEquals(1, fake.last.written.size) // the second waits for the first write response
        fake.last.ackWrite()
        runCurrent()
        assertTrue(w1.isCompleted)
        assertEquals(2, fake.last.written.size)
        fake.last.ackWrite()
        w2.await()
        // a failing write status closes the connection
        val w3 = async { runCatching { t.writeFragment(byteArrayOf(0xC0.toByte(), 3)) } }
        runCurrent()
        fake.last.ackWrite(status = 133)
        assertEquals(CloseReason.GattFailure(CloseReason.Step.WRITE, 133), (w3.await().exceptionOrNull() as TransportClosedException).reason)
        // a write that is never acknowledged times out and closes
        val t2 = factory().open()
        val start = currentTime
        val w4 = async { runCatching { t2.writeFragment(byteArrayOf(0xC0.toByte(), 4)) } }
        advanceTimeBy(TransportTimeouts().writeMs + 1)
        assertEquals(CloseReason.Timeout(CloseReason.Step.WRITE), (w4.await().exceptionOrNull() as TransportClosedException).reason)
        assertTrue(currentTime - start >= TransportTimeouts().writeMs)
        // fragments above ATT_MTU - 3 are refused before reaching the stack
        val t3 = factory().open()
        try {
            t3.writeFragment(ByteArray(245))
            fail("long fragment accepted")
        } catch (_: IllegalArgumentException) {
        }
    }

    @Test
    fun disconnect_during_write_fails_the_write() = runTest {
        fake.autoAckWrites = false
        val t = factory().open()
        val w = async { runCatching { t.writeFragment(byteArrayOf(0xC0.toByte(), 1)) } }
        runCurrent()
        fake.last.drop(8)
        assertEquals(CloseReason.Disconnected(8), (w.await().exceptionOrNull() as TransportClosedException).reason)
        try {
            t.writeFragment(byteArrayOf(0xC0.toByte(), 1))
            fail("write after close")
        } catch (e: TransportClosedException) {
            assertEquals(CloseReason.Disconnected(8), e.reason)
        }
    }

    @Test
    fun notification_overflow_closes_with_bounded_memory() = runTest {
        val t = factory(capacity = 4).open()
        repeat(5) { fake.last.notifyRaw(byteArrayOf(0xC0.toByte(), it.toByte())) }
        runCurrent()
        assertEquals(CloseReason.NotificationOverflow, t.closeReason.value)
        val got = t.events.toList()
        assertEquals(5, got.size) // 4 notifications, then Closed
        assertEquals(TransportEvent.Closed(CloseReason.NotificationOverflow), got.last())
    }

    @Test
    fun duplicate_and_unexpected_callbacks_are_ignored() = runTest {
        val t = factory().open()
        val s = fake.last.sink
        s(GattCallback.ConnectionState(true, 0))
        s(GattCallback.RequestWritten(0))
        s(GattCallback.InfoRead(0, ByteArray(3)))
        s(GattCallback.ServicesDiscovered(0, false))
        s(GattCallback.NotificationsEnabled(5))
        runCurrent()
        assertEquals(null, t.closeReason.value)
        assertTrue(t.ignoredCallbacks.get() >= 5)
        t.writeFragment(byteArrayOf(0xC0.toByte(), 1)) // still fully functional
    }

    @Test
    fun cancelling_the_connect_closes_the_attempt() = runTest {
        fake.respondToDiscovery = false
        val f = factory()
        val job = async { f.connect(addr) }
        runCurrent()
        job.cancel()
        runCurrent()
        assertTrue(fake.last.closed && fake.last.disconnected)
    }

    /** Callbacks from several real threads at once (binder threads): no lost or duplicated events, one Closed. */
    @Test
    fun concurrent_callbacks_from_many_threads() {
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
        fake.device = { emptyList() } // writes are acknowledged, the device answers nothing
        try {
            runBlocking {
                val f = GattTransportFactory(fake, { BluetoothReadiness.Ready }, scope, TransportTimeouts(), notificationCapacity = 100_000)
                val t = (f.connect(addr) as ConnectResult.Connected).transport
                val collected = async(Dispatchers.Default) { t.events.toList() }
                val go = CountDownLatch(1)
                val threads = (0 until 4).map { k ->
                    thread {
                        go.await()
                        repeat(2_000) { i -> fake.last.notifyRaw(byteArrayOf(0xC0.toByte(), k.toByte(), (i shr 8).toByte(), i.toByte())) }
                    }
                }
                val writer = launch(Dispatchers.Default) {
                    go.await()
                    repeat(200) { runCatching { t.writeFragment(byteArrayOf(0xC0.toByte(), 7)) } }
                }
                go.countDown()
                threads.forEach { it.join() }
                writer.join()
                fake.last.drop(8)
                val got = withTimeout(10_000) { collected.await() }
                val notes = got.filterIsInstance<TransportEvent.Notification>()
                assertEquals(8_000, notes.size)
                assertEquals(8_000, notes.map { it.fragment.toList() }.toSet().size)
                assertEquals(1, got.count { it is TransportEvent.Closed })
                assertTrue(got.last() is TransportEvent.Closed)
                assertFalse(t.closeReason.value == null)
            }
        } finally {
            scope.cancel()
        }
    }

    /** Check 1: an immediate start failure and a failing completion are distinct, both close the connection. */
    @Test
    fun write_start_failure_and_completion_failure_are_both_handled() = runTest {
        val f = factory()
        val a = f.open()
        fake.writeStartFails = true // API 33+: writeCharacteristic() did not return BluetoothStatusCodes.SUCCESS
        val e1 = runCatching { a.writeFragment(byteArrayOf(0xC0.toByte(), 1)) }.exceptionOrNull() as TransportClosedException
        assertEquals(CloseReason.GattFailure(CloseReason.Step.WRITE, GattTransport.NOT_STARTED), e1.reason)
        fake.writeStartFails = false
        fake.autoAckWrites = false
        val b = f.open()
        val w = async { runCatching { b.writeFragment(byteArrayOf(0xC0.toByte(), 2)) } }
        runCurrent()
        assertTrue(w.isActive) // started successfully: the write is NOT complete until onCharacteristicWrite
        fake.last.ackWrite(status = 3) // onCharacteristicWrite reports a failure
        assertEquals(CloseReason.GattFailure(CloseReason.Step.WRITE, 3), (w.await().exceptionOrNull() as TransportClosedException).reason)
    }

    /** Check 2: subscription completes only with a successful CCCD write callback. */
    @Test
    fun subscription_requires_the_cccd_write_callback() = runTest {
        val f = factory()
        fake.subscribeStartFails = true // setCharacteristicNotification or writeDescriptor refused
        assertEquals(CloseReason.GattFailure(CloseReason.Step.SUBSCRIBE, GattTransport.NOT_STARTED), f.failure())
        fake.subscribeStartFails = false
        fake.respondToSubscribe = false // started, but onDescriptorWrite never arrives
        assertEquals(CloseReason.Timeout(CloseReason.Step.SUBSCRIBE), f.failure())
        fake.respondToSubscribe = true
        fake.subscribeStatus = 13 // onDescriptorWrite with a failure status
        assertEquals(CloseReason.GattFailure(CloseReason.Step.SUBSCRIBE, 13), f.failure())
    }

    /** Check 3: the negotiated MTU is used, including Android 14+ negotiating on its own. */
    @Test
    fun actually_negotiated_mtu_is_used() = runTest {
        fake.deviceMtu = 185 // the app asks for 517, the peripheral agrees to less
        assertEquals(185, factory().open().mtu)
        // Android 14+: the stack negotiates by itself right after connect (unsolicited 247) and the
        // app's own request is refused: the reported value is kept, not 23 and not 517
        fake.unsolicitedMtuAfterConnect = 247
        fake.acceptMtuRequest = false
        assertEquals(247, factory().open().mtu)
        // ... or the app's request is accepted but never answered: the unsolicited value stays
        fake.acceptMtuRequest = true
        fake.respondToMtu = false
        assertEquals(247, factory().open().mtu)
    }
}
