package cz.nowass.dispenser.device.transport

import cz.nowass.dispenser.ble.protocol.BleCodecException
import cz.nowass.dispenser.ble.protocol.BleProtocol
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.onEach
import kotlinx.coroutines.flow.receiveAsFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withTimeoutOrNull
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong

/**
 * Pure-Kotlin GATT connection logic over a [GattPort] (docs/ANDROID_BLE_DESIGN.md 4.2–4.4):
 *
 * - setup: connect -> high connection priority -> service discovery (service, Info, Request,
 *   Response + CCCD) -> MTU 517 requested, the reported value used (23 if none or failed) -> Info
 *   read and validated -> notifications enabled;
 * - exactly one GATT operation in flight ([ops]); every operation has a timeout;
 * - callbacks (any thread) enter one channel and are handled by one coroutine; callbacks of other
 *   ports can never arrive (each port has its own sink), callbacks after [close] are dropped;
 * - notifications are buffered up to [notificationCapacity] unconsumed fragments, then the
 *   connection is closed ([CloseReason.NotificationOverflow]) — memory stays bounded;
 * - an unsolicited MTU change (newer Android versions negotiate on their own) updates [mtu] for
 *   the next frame;
 * - SecurityException from any port call closes with [CloseReason.PermissionRevoked]; a link drop
 *   while Bluetooth is off is reported as [CloseReason.BluetoothOff].
 */
class GattTransport internal constructor(
    override val generation: Long,
    address: String,
    portFactory: GattPortFactory,
    scope: CoroutineScope,
    private val timeouts: TransportTimeouts,
    private val prerequisites: BluetoothPrerequisites,
    private val notificationCapacity: Int = DEFAULT_NOTIFICATION_CAPACITY,
) : ByteTransport {

    private val callbacks = Channel<GattCallback>(Channel.UNLIMITED)
    private val port: GattPort = portFactory.create(address) { cb -> callbacks.trySend(cb) }
    private val ops = Mutex()
    private val lock = Any()
    private var pending: Pending? = null
    private val out = Channel<TransportEvent>(Channel.UNLIMITED)
    private val queued = AtomicInteger(0)
    private val closeState = MutableStateFlow<CloseReason?>(null)

    /** Callbacks that matched no pending operation (diagnostics and tests). */
    val ignoredCallbacks = AtomicLong(0)

    @Volatile
    override var mtu: Int = DispenserGatt.MTU_MIN
        private set

    private var infoValue: BleProtocol.Info? = null
    override val info: BleProtocol.Info get() = checkNotNull(infoValue) { "transport not set up" }

    override val events: Flow<TransportEvent> =
        out.receiveAsFlow().onEach { if (it is TransportEvent.Notification) queued.decrementAndGet() }

    override val closeReason: StateFlow<CloseReason?> = closeState.asStateFlow()

    private class Pending(val accepts: (GattCallback) -> Boolean, val result: CompletableDeferred<GattCallback>)

    init {
        scope.launch {
            for (cb in callbacks) handle(cb)
        }
    }

    private fun handle(cb: GattCallback) {
        if (closeState.value != null) return
        when (cb) {
            is GattCallback.ConnectionState -> if (!cb.connected) {
                val off = runCatching { prerequisites.check() }.getOrNull() == BluetoothReadiness.BluetoothOff
                close(if (off) CloseReason.BluetoothOff else CloseReason.Disconnected(cb.status))
                return
            }
            is GattCallback.Notification -> {
                if (queued.incrementAndGet() > notificationCapacity) {
                    close(CloseReason.NotificationOverflow)
                } else {
                    out.trySend(TransportEvent.Notification(cb.value.copyOf()))
                }
                return
            }
            else -> Unit
        }
        val p = synchronized(lock) { pending?.takeIf { it.accepts(cb) }?.also { pending = null } }
        when {
            p != null -> p.result.complete(cb)
            cb is GattCallback.MtuChanged && cb.status == DispenserGatt.GATT_SUCCESS -> mtu = clampMtu(cb.mtu)
            else -> ignoredCallbacks.incrementAndGet()
        }
    }

    /** Runs the setup sequence; null when ready, else the reason (the transport is then closed). */
    internal suspend fun open(): CloseReason? = try {
        operation(CloseReason.Step.CONNECT, timeouts.connectMs, { port.connect(); true }) {
            it is GattCallback.ConnectionState && it.connected
        }
        guarded { port.requestHighPriority() }
        val sd = operation(CloseReason.Step.DISCOVERY, timeouts.discoveryMs, { port.discoverServices() }) {
            it is GattCallback.ServicesDiscovered
        } as GattCallback.ServicesDiscovered
        when {
            sd.status != DispenserGatt.GATT_SUCCESS -> fail(CloseReason.GattFailure(CloseReason.Step.DISCOVERY, sd.status))
            !sd.complete -> fail(CloseReason.NotADispenser)
        }
        negotiateMtu()
        val ir = operation(CloseReason.Step.INFO, timeouts.infoMs, { port.readInfo() }) { it is GattCallback.InfoRead } as GattCallback.InfoRead
        if (ir.status != DispenserGatt.GATT_SUCCESS) fail(CloseReason.GattFailure(CloseReason.Step.INFO, ir.status))
        infoValue = try {
            BleProtocol.decodeInfo(ir.value)
        } catch (e: BleCodecException) {
            fail(CloseReason.NotADispenser)
        }
        val ne = operation(CloseReason.Step.SUBSCRIBE, timeouts.subscribeMs, { port.enableNotifications() }) {
            it is GattCallback.NotificationsEnabled
        } as GattCallback.NotificationsEnabled
        if (ne.status != DispenserGatt.GATT_SUCCESS) fail(CloseReason.GattFailure(CloseReason.Step.SUBSCRIBE, ne.status))
        closeState.value
    } catch (e: TransportClosedException) {
        e.reason
    } catch (e: CancellationException) {
        close(CloseReason.Local)
        throw e
    }

    /** MTU: requested once; the reported value is used, 23 when the request fails or times out. */
    private suspend fun negotiateMtu() {
        val r = ops.withLock {
            checkOpen()
            val d = CompletableDeferred<GattCallback>()
            synchronized(lock) { pending = Pending({ it is GattCallback.MtuChanged }, d) }
            if (!guarded { port.requestMtu(DispenserGatt.MTU_REQUEST) }) {
                synchronized(lock) { pending = null }
                return
            }
            val res = withTimeoutOrNull(timeouts.mtuMs) { awaitOrClosed(d) }
            synchronized(lock) { if (pending?.result === d) pending = null }
            res as? GattCallback.MtuChanged
        }
        if (r != null && r.status == DispenserGatt.GATT_SUCCESS) mtu = clampMtu(r.mtu)
    }

    override suspend fun writeFragment(fragment: ByteArray) {
        require(fragment.isNotEmpty() && fragment.size <= mtu - 3) { "fragment longer than ATT_MTU - 3" }
        val copy = fragment.copyOf()
        val r = operation(CloseReason.Step.WRITE, timeouts.writeMs, { port.writeRequest(copy) }) {
            it is GattCallback.RequestWritten
        } as GattCallback.RequestWritten
        if (r.status != DispenserGatt.GATT_SUCCESS) fail(CloseReason.GattFailure(CloseReason.Step.WRITE, r.status))
    }

    private suspend fun operation(step: CloseReason.Step, timeoutMs: Long, start: () -> Boolean, accepts: (GattCallback) -> Boolean): GattCallback =
        ops.withLock {
            checkOpen()
            val d = CompletableDeferred<GattCallback>()
            synchronized(lock) { pending = Pending(accepts, d) }
            if (!guarded(start)) {
                synchronized(lock) { pending = null }
                fail(CloseReason.GattFailure(step, NOT_STARTED))
            }
            val res = withTimeoutOrNull(timeoutMs) { awaitOrClosed(d) }
            if (res == null) {
                synchronized(lock) { if (pending?.result === d) pending = null }
                fail(CloseReason.Timeout(step))
            }
            res
        }

    private suspend fun awaitOrClosed(d: CompletableDeferred<GattCallback>): GattCallback = d.await()

    /** Runs a port call; SecurityException closes the transport (permission revoked). */
    private inline fun <T> guarded(block: () -> T): T = try {
        block()
    } catch (e: SecurityException) {
        fail(CloseReason.PermissionRevoked)
    }

    private fun checkOpen() {
        closeState.value?.let { throw TransportClosedException(it) }
    }

    private fun fail(reason: CloseReason): Nothing {
        close(reason)
        throw TransportClosedException(closeState.value ?: reason)
    }

    override fun close(reason: CloseReason) {
        val p: Pending?
        synchronized(lock) {
            if (closeState.value != null) return
            closeState.value = reason
            p = pending
            pending = null
        }
        callbacks.close()
        p?.result?.completeExceptionally(TransportClosedException(reason))
        runCatching { port.disconnect() }
        runCatching { port.close() }
        out.trySend(TransportEvent.Closed(reason))
        out.close()
    }

    private fun clampMtu(m: Int) = m.coerceIn(DispenserGatt.MTU_MIN, DispenserGatt.MTU_REQUEST)

    companion object {
        const val DEFAULT_NOTIFICATION_CAPACITY = 128
        /** Status used when a port call returned false (the operation could not be started). */
        const val NOT_STARTED = -1
    }
}

/**
 * Creates transports: checks the prerequisites, assigns process-wide generations and runs the
 * setup (docs/ANDROID_BLE_DESIGN.md 4.2). The Bluetooth address is only a locator, never an identity.
 */
class GattTransportFactory(
    private val portFactory: GattPortFactory,
    private val prerequisites: BluetoothPrerequisites,
    private val scope: CoroutineScope,
    private val timeouts: TransportTimeouts = TransportTimeouts(),
    private val notificationCapacity: Int = GattTransport.DEFAULT_NOTIFICATION_CAPACITY,
) {
    private val generations = AtomicLong(0)

    suspend fun connect(address: String): ConnectResult {
        require(ADDRESS.matches(address)) { "Bluetooth address must be XX:XX:XX:XX:XX:XX (upper case)" }
        val readiness = prerequisites.check()
        if (readiness != BluetoothReadiness.Ready) return ConnectResult.NotReady(readiness)
        val t = GattTransport(generations.incrementAndGet(), address, portFactory, scope, timeouts, prerequisites, notificationCapacity)
        val failure = t.open()
        return if (failure == null) ConnectResult.Connected(t) else ConnectResult.Failed(failure)
    }

    private companion object {
        val ADDRESS = Regex("^([0-9A-F]{2}:){5}[0-9A-F]{2}$")
    }
}
