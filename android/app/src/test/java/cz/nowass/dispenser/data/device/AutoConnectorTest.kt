package cz.nowass.dispenser.data.device

import cz.nowass.dispenser.device.session.SessionFailure
import cz.nowass.dispenser.device.session.SessionState
import cz.nowass.dispenser.device.transport.BluetoothReadiness
import cz.nowass.dispenser.device.transport.CloseReason
import cz.nowass.dispenser.device.transport.DeviceScanner
import cz.nowass.dispenser.device.transport.ScanOutcome
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.delay
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.advanceTimeBy
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Automatic foreground connection in virtual time: a scripted device (advertising from a given
 * instant, scripted connect outcomes) behind a filtered scanner, the repository's connect() and the
 * Bluetooth readiness. Oracles: number of scans and connects, never two scans at once, final status.
 */
@OptIn(ExperimentalCoroutinesApi::class)
class AutoConnectorTest {
    private val id = "a0b1c2d3e4f5"
    private val address = "E0:72:A1:1E:1F:36"

    /** Scripted world shared by the fakes. */
    private inner class World(val scope: TestScope) {
        val fake = FakeDeviceRepository(id)
        var advertisingFrom: Long? = 0L        // virtual ms from which the device advertises; null = asleep
        var scanUnavailable = false
        var readiness: BluetoothReadiness = BluetoothReadiness.Ready
        var scanPermissionMissing = emptyList<String>()
        var targetOn = true
        val outcomes = ArrayDeque<ConnectOutcome>() // scripted connect results (default: success)
        var connects = 0
        var scans = 0
        var activeScans = 0
        var maxActiveScans = 0
        var cancelledScans = 0
        private var generation = 0L

        fun now() = scope.testScheduler.currentTime
        fun advertising() = advertisingFrom?.let { now() >= it } == true

        val repo: DeviceRepository = object : DeviceRepository by fake {
            override suspend fun connect(): ConnectOutcome {
                connects++
                delay(500)
                val o = outcomes.removeFirstOrNull()
                    ?: if (advertising()) ConnectOutcome.Connected(++generation, 0) else ConnectOutcome.Failed(SessionFailure.Transport(CloseReason.Disconnected(147)))
                if (o is ConnectOutcome.Connected) fake.connection.value = SessionState.Authenticated(id, o.generation, 0)
                return o
            }
        }

        val scanner = DeviceScanner { addr, timeoutMs ->
            assertEquals(address, addr)
            if (scanUnavailable) return@DeviceScanner ScanOutcome.Unavailable("permission")
            scans++; activeScans++; maxActiveScans = maxOf(maxActiveScans, activeScans)
            try {
                val end = now() + timeoutMs
                while (now() < end) {
                    if (advertising()) return@DeviceScanner ScanOutcome.Found
                    delay(100)
                }
                ScanOutcome.Timeout
            } catch (e: CancellationException) {
                cancelledScans++; throw e
            } finally {
                activeScans--
            }
        }

        val auto = AutoConnector(
            scope = scope.backgroundScope,
            target = { if (targetOn) AutoConnectTarget(repo, address) else null },
            readiness = { readiness },
            scanner = scanner,
            missingScanPermission = { scanPermissionMissing },
            nowMs = { now() },
        )

        fun step(ms: Long) { scope.advanceTimeBy(ms); scope.runCurrent() }
        val status get() = auto.status.value
    }

    private fun TestScope.world() = World(this)

    @Test
    fun foreground_with_an_owned_device_already_advertising_connects_once() = runTest {
        val w = world()
        w.auto.activityStarted()
        w.step(2_000)
        assertEquals(AutoConnectStatus.Connected, w.status)
        assertEquals(1, w.connects)
        assertEquals(1, w.scans)
        assertEquals(0, w.activeScans) // the scan stopped after the discovery
    }

    @Test
    fun device_that_starts_advertising_after_the_app_opened_is_connected() = runTest {
        val w = world()
        w.advertisingFrom = 30_000
        w.auto.activityStarted()
        w.step(10_000)
        assertEquals(AutoConnectStatus.Searching(), w.status)
        assertEquals(0, w.connects)
        w.step(21_000)
        assertEquals(AutoConnectStatus.Connected, w.status)
        assertEquals(1, w.connects)
    }

    @Test
    fun sleeping_device_ends_in_unavailable_and_retry_starts_a_new_window() = runTest {
        val w = world()
        w.advertisingFrom = null
        w.auto.activityStarted()
        w.step(AutoConnector.DEFAULT_WINDOW_MS + 1_000)
        assertEquals(AutoConnectStatus.Unavailable, w.status)
        assertEquals(0, w.connects)
        assertEquals(0, w.activeScans) // bounded: nothing scans after the window
        w.step(600_000)
        assertEquals(1, w.scans)
        w.auto.retry()
        w.step(1_000)
        assertTrue(w.status is AutoConnectStatus.Searching)
        w.advertisingFrom = w.now() + 5_000
        w.step(7_000)
        assertEquals(AutoConnectStatus.Connected, w.status)
        assertEquals(1, w.connects)
    }

    @Test
    fun repeated_foreground_transitions_never_duplicate_scans_or_sessions() = runTest {
        val w = world()
        w.advertisingFrom = 20_000
        repeat(5) { w.auto.activityStarted(); w.auto.activityStarted(); w.step(500); w.auto.activityStopped(); w.auto.activityStopped(); w.step(100) }
        w.auto.activityStarted()
        w.auto.activityStarted() // a second activity (e.g. a system dialog's host) adds nothing
        w.step(25_000)
        assertEquals(AutoConnectStatus.Connected, w.status)
        assertEquals(1, w.connects)
        assertEquals(1, w.maxActiveScans)
        // back to the background and foreground while the session is still live: reused, no new connect
        w.auto.activityStopped(); w.auto.activityStopped()
        w.step(5_000)
        w.auto.activityStarted()
        w.step(5_000)
        assertEquals(AutoConnectStatus.Connected, w.status)
        assertEquals(1, w.connects)
    }

    @Test
    fun background_stops_the_search_at_once() = runTest {
        val w = world()
        w.advertisingFrom = null
        w.auto.activityStarted()
        w.step(5_000)
        w.auto.activityStopped()
        w.step(1)
        assertEquals(1, w.cancelledScans)
        assertEquals(0, w.activeScans)
        assertEquals(AutoConnectStatus.Idle, w.status)
        w.advertisingFrom = 0
        w.step(60_000)
        assertEquals(0, w.connects) // nothing connects in the background
    }

    @Test
    fun unexpected_disconnect_in_the_foreground_starts_one_new_window() = runTest {
        val w = world()
        w.auto.activityStarted()
        w.step(2_000)
        assertEquals(1, w.connects)
        w.advertisingFrom = null
        w.fake.connection.value = SessionState.Idle // link lost (e.g. the device suspended BLE for a dose)
        w.step(1_000)
        assertTrue(w.status is AutoConnectStatus.Searching)
        w.advertisingFrom = w.now() + 10_000 // advertising again after the pump
        w.step(12_000)
        assertEquals(AutoConnectStatus.Connected, w.status)
        assertEquals(2, w.connects)
    }

    @Test
    fun missing_permission_is_reported_and_resumes_once_granted() = runTest {
        val w = world()
        w.readiness = BluetoothReadiness.PermissionMissing(listOf("android.permission.BLUETOOTH_CONNECT"))
        w.scanPermissionMissing = listOf("android.permission.BLUETOOTH_SCAN")
        w.auto.activityStarted()
        w.step(300_000) // longer than the window: waiting for a permission does not consume it
        assertEquals(
            AutoConnectStatus.PermissionMissing(listOf("android.permission.BLUETOOTH_CONNECT", "android.permission.BLUETOOTH_SCAN")),
            w.status,
        )
        assertEquals(0, w.scans)
        assertEquals(0, w.connects)
        w.readiness = BluetoothReadiness.Ready
        w.scanPermissionMissing = emptyList()
        w.step(3_000)
        assertEquals(AutoConnectStatus.Connected, w.status)
        assertEquals(1, w.connects)
    }

    @Test
    fun bluetooth_switched_on_during_the_search_resumes_it() = runTest {
        val w = world()
        w.readiness = BluetoothReadiness.BluetoothOff
        w.auto.activityStarted()
        w.step(10_000)
        assertEquals(AutoConnectStatus.BluetoothOff, w.status)
        w.readiness = BluetoothReadiness.Ready
        w.step(3_000)
        assertEquals(AutoConnectStatus.Connected, w.status)
    }

    @Test
    fun authentication_failure_is_shown_and_never_retried_automatically() = runTest {
        val w = world()
        w.outcomes += ConnectOutcome.Failed(SessionFailure.HandshakeFailed)
        w.auto.activityStarted()
        w.step(AutoConnector.DEFAULT_WINDOW_MS * 2)
        assertEquals(AutoConnectStatus.Failed(authentication = true), w.status)
        assertEquals(1, w.connects)
    }

    @Test
    fun repeated_connect_failures_of_an_advertising_device_end_in_failed() = runTest {
        val w = world()
        w.outcomes += ConnectOutcome.Failed(SessionFailure.Timeout("HELLO"))
        w.outcomes += ConnectOutcome.Failed(SessionFailure.Timeout("HELLO"))
        w.auto.activityStarted()
        w.step(20_000)
        assertEquals(AutoConnectStatus.Failed(authentication = false), w.status)
        assertEquals(AutoConnector.MAX_TRANSIENT_FAILURES, w.connects)
        w.auto.retry()
        w.step(3_000)
        assertEquals(AutoConnectStatus.Connected, w.status)
    }

    @Test
    fun without_scanning_it_falls_back_to_direct_connects_until_the_device_wakes() = runTest {
        val w = world()
        w.scanUnavailable = true
        w.scanPermissionMissing = listOf("android.permission.BLUETOOTH_SCAN")
        w.advertisingFrom = 20_000
        w.auto.activityStarted()
        w.step(5_000)
        assertEquals(AutoConnectStatus.Searching(listOf("android.permission.BLUETOOTH_SCAN")), w.status)
        w.step(20_000)
        assertEquals(AutoConnectStatus.Connected, w.status)
        assertTrue(w.connects > AutoConnector.MAX_TRANSIENT_FAILURES) // failed direct attempts to a sleeping device are not failures
        assertEquals(0, w.scans)
    }

    @Test
    fun demo_mode_never_scans_or_connects_and_a_mode_switch_stops_the_search() = runTest {
        val w = world()
        w.targetOn = false // DEMO (or no owned device)
        w.auto.activityStarted()
        w.step(120_000)
        assertEquals(AutoConnectStatus.Idle, w.status)
        assertEquals(0, w.scans)
        assertEquals(0, w.connects)
        // DEVICE mode resolved while in the foreground: starts
        w.advertisingFrom = null
        w.targetOn = true
        w.auto.refresh()
        w.step(1_000)
        assertTrue(w.status is AutoConnectStatus.Searching)
        // switched to DEMO: the search stops at once
        w.targetOn = false
        w.auto.refresh()
        w.step(1)
        assertEquals(AutoConnectStatus.Idle, w.status)
        assertEquals(0, w.activeScans)
        w.advertisingFrom = 0
        w.step(60_000)
        assertEquals(0, w.connects)
    }
}
