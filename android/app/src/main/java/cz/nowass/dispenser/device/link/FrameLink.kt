package cz.nowass.dispenser.device.link

import cz.nowass.dispenser.ble.protocol.BleCodecException
import cz.nowass.dispenser.ble.protocol.BleProtocol
import cz.nowass.dispenser.ble.protocol.BleResult
import cz.nowass.dispenser.device.transport.ByteTransport
import cz.nowass.dispenser.device.transport.CloseReason
import cz.nowass.dispenser.device.transport.TransportEvent
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.receiveAsFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

/**
 * Protocol frames over a [ByteTransport] (docs/BLE_PROTOCOL.md section 3; docs/ANDROID_BLE_DESIGN.md 2.2):
 *
 * - [send] fragments a frame at the MTU in effect when the frame starts and writes one fragment at a
 *   time, each after the previous write response (protocol flow control); frames are serialized, so
 *   two callers never interleave fragments;
 * - notifications are reassembled with the shared codec (one partial frame, 2 s between
 *   fragments); a fragment error discards the partial frame and is reported as
 *   [LinkEvent.FragmentError] (the session layer decides; the link never answers on its own);
 * - after the transport closes, exactly one [LinkEvent.Closed] is emitted.
 */
class FrameLink(
    private val transport: ByteTransport,
    scope: CoroutineScope,
    private val nowMs: () -> Long = { System.nanoTime() / 1_000_000 },
) {
    private val sendLock = Mutex()
    private val reassembler = BleProtocol.Reassembler()
    private val out = Channel<LinkEvent>(Channel.UNLIMITED)

    val events: Flow<LinkEvent> = out.receiveAsFlow()
    val generation: Long get() = transport.generation
    /** The Info record the transport read during setup; null when not available. */
    val info: BleProtocol.Info? get() = runCatching { transport.info }.getOrNull()

    init {
        scope.launch {
            transport.events.collect { e ->
                when (e) {
                    is TransportEvent.Notification -> receive(e.fragment)
                    is TransportEvent.Closed -> {
                        out.trySend(LinkEvent.Closed(e.reason))
                        out.close()
                    }
                }
            }
        }
    }

    private fun receive(fragment: ByteArray) {
        val frame = try {
            reassembler.push(fragment, nowMs())
        } catch (e: BleCodecException) {
            out.trySend(LinkEvent.FragmentError(e.result))
            return
        }
        if (frame != null) out.trySend(LinkEvent.Frame(frame))
    }

    /**
     * Sends one protocol frame (1..520 bytes). Suspends until the last fragment's write response;
     * throws [cz.nowass.dispenser.device.transport.TransportClosedException] if the connection ends.
     */
    suspend fun send(frame: ByteArray) {
        require(frame.isNotEmpty() && frame.size <= BleProtocol.FRAME_MAX) { "frame size ${frame.size}" }
        sendLock.withLock {
            val fragments = BleProtocol.fragment(frame, transport.mtu)
            check(fragments.isNotEmpty()) { "frame not representable at MTU ${transport.mtu}" }
            for (f in fragments) transport.writeFragment(f)
        }
    }

    fun close(reason: CloseReason = CloseReason.Local) = transport.close(reason)
}

sealed interface LinkEvent {
    class Frame(val bytes: ByteArray) : LinkEvent
    data class FragmentError(val result: BleResult) : LinkEvent
    data class Closed(val reason: CloseReason) : LinkEvent
}
