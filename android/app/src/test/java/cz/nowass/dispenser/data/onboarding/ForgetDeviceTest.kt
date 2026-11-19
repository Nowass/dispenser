package cz.nowass.dispenser.data.onboarding

import android.content.Intent
import cz.nowass.dispenser.crypto.NativeNoiseProvider
import cz.nowass.dispenser.crypto.TestNative
import cz.nowass.dispenser.crypto.credentials.CredentialState
import cz.nowass.dispenser.crypto.credentials.CredentialStore
import cz.nowass.dispenser.crypto.credentials.FakeKeyWrapper
import cz.nowass.dispenser.data.device.CredentialHealth
import cz.nowass.dispenser.data.device.DeviceUiRepository
import cz.nowass.dispenser.data.mode.AppMode
import cz.nowass.dispenser.data.mode.AppModeController
import cz.nowass.dispenser.data.mode.AppModeStore
import cz.nowass.dispenser.data.mode.DeviceAddressStore
import cz.nowass.dispenser.data.mode.InMemoryAppModeStore
import cz.nowass.dispenser.data.mode.ResolvedMode
import cz.nowass.dispenser.data.mode.StoredMode
import cz.nowass.dispenser.device.transport.AssociationStart
import cz.nowass.dispenser.device.transport.DeviceAssociator
import cz.nowass.dispenser.domain.DeviceAccess
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.filterNotNull
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeFalse
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/**
 * Security review F1/F2/F3: "Forget device" under injected faults. DEMO is selected only when the
 * session is closed and both credential records and their keys are gone; every failure is reported
 * (no exception), keeps DEVICE mode, keeps the state consistent and is safe to retry.
 */
class ForgetDeviceTest {
    @get:Rule val tmp = TemporaryFolder()
    private val id = "a0b1c2d3e4f5"
    private val address = "A0:B1:C2:D3:E4:F5"
    private lateinit var noise: NativeNoiseProvider
    private lateinit var scope: CoroutineScope
    private lateinit var dir: java.io.File
    private val wrapper = FakeKeyWrapper()
    private lateinit var store: CredentialStore
    private val modeStore = InMemoryAppModeStore(StoredMode(AppMode.DEVICE, id))
    private lateinit var modes: AppModeController
    private lateinit var health: CredentialHealth

    /** Session side: scripted graceful end, a hard close that may not work, block bookkeeping. */
    private class Sessions : ForgetSessions {
        var live = true
        var closeWorks = true
        var graceful: suspend () -> Unit = {}
        var closes = 0
        val blocked = mutableSetOf<String>()
        var blockedDuringClose = false
        val devices = mutableListOf<String>()
        val hasLiveSession: Boolean get() = live
        override suspend fun <T> exclusive(deviceId: String, cleanup: suspend () -> T): T {
            blocked += deviceId
            try { return cleanup() } finally { blocked -= deviceId }
        }
        override suspend fun endGracefully(deviceId: String) { devices += deviceId; graceful() }
        override fun closeNow(deviceId: String) { devices += deviceId; closes++; blockedDuringClose = deviceId in blocked; if (closeWorks) live = false }
        override fun hasLiveSession(deviceId: String): Boolean { devices += deviceId; return live }
    }

    private class Associator(var result: () -> Boolean = { true }) : DeviceAssociator {
        var calls = 0
        override suspend fun start(): AssociationStart = AssociationStart.Unsupported
        override fun addressFrom(result: Intent?): String? = null
        override fun disassociate(address: String): Boolean { calls++; return result() }
    }

    /** Address store that can fail. */
    private class Addresses(val inner: InMemoryAppModeStore) : DeviceAddressStore {
        var failRemove = false
        override val addresses: Flow<Map<String, String>> get() = inner.addresses
        override suspend fun setAddress(deviceId: String, address: String) = inner.setAddress(deviceId, address)
        override suspend fun removeAddress(deviceId: String) {
            if (failRemove) throw java.io.IOException("injected address store failure")
            inner.removeAddress(deviceId)
        }
    }

    /** Mode store that can fail to clear the selection. */
    private class Modes(val inner: InMemoryAppModeStore) : AppModeStore {
        var failWrite = false
        override val stored: Flow<StoredMode> get() = inner.stored
        override suspend fun write(mode: StoredMode) {
            if (failWrite) throw java.io.IOException("injected mode store failure")
            inner.write(mode)
        }
    }

    private val sessions = Sessions()
    private val associator = Associator()
    private val addresses = Addresses(modeStore)
    private val modeWriter = Modes(modeStore)

    @Before
    fun setUp() = runBlocking {
        noise = TestNative.load()
        scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
        dir = tmp.newFolder()
        store = CredentialStore(dir, wrapper, noise)
        modes = AppModeController(modeWriter, { CredentialStore.hasConfirmedRecord(dir, it) }, scope).also { it.start() }
        health = CredentialHealth({ DeviceUiRepository.credentialAccess(store, id) })
        // a confirmed credential and a staged one (e.g. an unconfirmed re-enrollment) for the device
        store.commit(store.beginEnrollment(id), 0, ByteArray(32) { 1 }, "A", 0).close()
        store.beginEnrollment(id).also { store.stage(it, ByteArray(32) { 2 }, "A", 0); store.release(it) }
        addresses.setAddress(id, address)
        assertEquals(ResolvedMode.Device(id), modes.mode.filterNotNull().first())
        assertEquals(DeviceAccess.OK, health.refresh())
    }

    @After fun tearDown() = scope.cancel()

    private fun forget() = ForgetDevice(store, addresses, associator, modes, sessions, afterCredentialChange = { health.refresh() }, gracefulEndMs = 500)

    private suspend fun assertForgotten(r: ForgetResult, associationRemoved: Boolean = true) {
        assertEquals(ForgetResult.Forgotten(associationRemoved), r)
        assertFalse(sessions.hasLiveSession)
        assertEquals(CredentialState.Absent, store.load(id))
        assertEquals(CredentialState.Absent, store.loadPending(id))
        assertTrue("no Keystore key left", wrapper.keys.isEmpty())
        assertTrue(addresses.addresses.first().isEmpty())
        assertEquals(ResolvedMode.Demo, modes.mode.value)
        assertEquals(StoredMode.DEFAULT, modeStore.stored.first())
        assertEquals(DeviceAccess.RECORD_MISSING, health.state.value)
        assertTrue("unblocked afterwards", sessions.blocked.isEmpty())
        assertTrue("only the forgotten device's session is touched", sessions.devices.all { it == id })
    }

    private suspend fun assertStillDeviceMode() {
        assertEquals(ResolvedMode.Device(id), modes.mode.value)
        assertEquals(StoredMode(AppMode.DEVICE, id), modeStore.stored.first())
    }

    // ---- F1: session termination ------------------------------------------------------------

    @Test
    fun graceful_end_throwing_still_hard_closes() = runBlocking {
        sessions.graceful = { throw IllegalStateException("END_SESSION failed") }
        assertForgotten(forget()(id))
        assertEquals(1, sessions.closes)
        assertTrue("blocked while closing", sessions.blockedDuringClose)
    }

    @Test
    fun graceful_end_cancelled_still_hard_closes() = runBlocking {
        sessions.graceful = { throw CancellationException("request cancelled") }
        assertForgotten(forget()(id))
        assertEquals(1, sessions.closes)
    }

    @Test
    fun graceful_end_hanging_is_bounded_then_hard_closed() = runBlocking {
        sessions.graceful = { awaitCancellation() }
        assertForgotten(forget()(id))
        assertEquals(1, sessions.closes)
    }

    @Test
    fun a_cancelled_caller_does_not_interrupt_the_cleanup() = runBlocking {
        val reached = CompletableDeferred<Unit>()
        val gate = CompletableDeferred<Unit>()
        sessions.graceful = { reached.complete(Unit); gate.await() }
        // The cancelled caller never receives the result, so completion is observed at Forget's last
        // step: afterCredentialChange (the credential-health refresh) runs in the outermost finally,
        // after the exclusive section has removed the session block and after the switch to DEMO.
        val cleanupDone = CompletableDeferred<Unit>()
        val forget = ForgetDevice(store, addresses, associator, modes, sessions,
            afterCredentialChange = { health.refresh(); cleanupDone.complete(Unit) }, gracefulEndMs = 500)
        val caller = scope.async { forget(id) }
        withTimeout(5_000) { reached.await() }
        caller.cancel()
        gate.complete(Unit)
        // the NonCancellable cleanup runs to the end even though its caller is gone
        withTimeout(10_000) { cleanupDone.await() }
        assertTrue(caller.isCancelled)
        assertForgotten(ForgetResult.Forgotten(true))
    }

    @Test
    fun a_session_that_cannot_be_closed_stops_before_anything_is_deleted() = runBlocking {
        sessions.closeWorks = false
        assertEquals(ForgetResult.Failed(ForgetResult.Step.SESSION), forget()(id))
        assertTrue(store.load(id) is CredentialState.Present)
        assertStillDeviceMode()
        assertTrue(sessions.blocked.isEmpty())
        sessions.closeWorks = true
        assertForgotten(forget()(id)) // retry
    }

    // ---- F2: partial failures ---------------------------------------------------------------

    @Test
    fun keystore_deletion_failure_keeps_a_consistent_retryable_state() = runBlocking {
        wrapper.failDelete = true
        assertEquals(ForgetResult.Failed(ForgetResult.Step.CREDENTIALS), forget()(id))
        // key first, then file: nothing half-removed, the credential still loads, DEVICE mode kept
        assertTrue(store.load(id) is CredentialState.Present)
        assertStillDeviceMode()
        assertEquals(DeviceAccess.OK, health.state.value)
        wrapper.failDelete = false
        assertForgotten(forget()(id))
    }

    @Test
    fun a_keystore_that_silently_keeps_the_key_is_detected() = runBlocking {
        wrapper.ignoreDelete = true
        assertEquals(ForgetResult.Failed(ForgetResult.Step.CREDENTIALS), forget()(id))
        assertStillDeviceMode()
        wrapper.ignoreDelete = false
        assertForgotten(forget()(id))
    }

    @Test
    fun file_deletion_failure_leaves_an_unusable_record_and_no_demo_switch() = runBlocking {
        assumeFalse("directory permissions do not apply to root", System.getProperty("user.name") == "root")
        assertTrue(dir.setWritable(false))
        try {
            assertEquals(ForgetResult.Failed(ForgetResult.Step.CREDENTIALS), forget()(id))
            // the key went first: the remaining record cannot be used; DEVICE mode shows recovery required
            assertEquals(CredentialState.Lost, store.load(id))
            assertEquals(DeviceAccess.KEY_MISSING, health.state.value)
            assertStillDeviceMode()
        } finally {
            dir.setWritable(true)
        }
        assertForgotten(forget()(id)) // retry removes the files
    }

    @Test
    fun address_store_failure_after_credential_removal_is_reported_and_retryable() = runBlocking {
        addresses.failRemove = true
        assertEquals(ForgetResult.Failed(ForgetResult.Step.ADDRESS), forget()(id))
        assertEquals(CredentialState.Absent, store.load(id))
        assertStillDeviceMode() // shows the recovery-required state, not "removed"
        assertEquals(DeviceAccess.RECORD_MISSING, health.state.value)
        addresses.failRemove = false
        assertForgotten(forget()(id))
    }

    @Test
    fun mode_store_failure_is_reported_and_retryable() = runBlocking {
        modeWriter.failWrite = true
        assertEquals(ForgetResult.Failed(ForgetResult.Step.MODE), forget()(id))
        assertStillDeviceMode()
        modeWriter.failWrite = false
        assertForgotten(forget()(id))
    }

    @Test
    fun cdm_disassociation_failure_is_not_fatal_but_reported() = runBlocking {
        associator.result = { false }
        assertForgotten(forget()(id), associationRemoved = false)
    }

    @Test
    fun cdm_disassociation_throwing_is_not_fatal_but_reported() = runBlocking {
        associator.result = { throw SecurityException("CDM refused") }
        assertForgotten(forget()(id), associationRemoved = false)
    }

    @Test
    fun forgetting_again_after_success_is_harmless() = runBlocking {
        assertForgotten(forget()(id))
        assertForgotten(forget()(id), associationRemoved = true)
    }

    @Test
    fun verified_removal_reports_a_file_that_could_not_be_deleted() = runBlocking {
        assumeFalse("directory permissions do not apply to root", System.getProperty("user.name") == "root")
        assertTrue(dir.setWritable(false))
        try {
            assertEquals(CredentialStore.RemovalProblem.FILE_STILL_PRESENT, store.removeVerified(id))
        } finally {
            dir.setWritable(true)
        }
        assertEquals(null, store.removeVerified(id))
        assertEquals(null, store.removeVerified(id)) // idempotent
    }
}
