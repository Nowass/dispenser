package cz.nowass.dispenser.data.device

import cz.nowass.dispenser.crypto.NativeNoiseProvider
import cz.nowass.dispenser.crypto.SetupLabel
import cz.nowass.dispenser.crypto.TestNative
import cz.nowass.dispenser.crypto.credentials.CredentialStore
import cz.nowass.dispenser.crypto.credentials.FakeKeyWrapper
import cz.nowass.dispenser.device.fwsim.FirmwareGattPortFactory
import cz.nowass.dispenser.device.fwsim.FirmwareSim
import cz.nowass.dispenser.device.ops.ConfigRecord
import cz.nowass.dispenser.device.ops.LocalRefusal
import cz.nowass.dispenser.device.ops.ManagerSessionPort
import cz.nowass.dispenser.device.ops.MessageType as M
import cz.nowass.dispenser.device.ops.OpResult
import cz.nowass.dispenser.device.ops.Readiness
import cz.nowass.dispenser.device.session.DeviceSessionManager
import cz.nowass.dispenser.device.session.OpenRequest
import cz.nowass.dispenser.device.session.OpenResult
import cz.nowass.dispenser.device.transport.BluetoothReadiness
import cz.nowass.dispenser.device.transport.GattTransportFactory
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.cancel
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/**
 * Review of M1.10e.3, finding 2, against the REAL firmware logic (libdsp_fwsim): a second caller of
 * the same operation, arriving while the first one is between its journal guard and its own journal
 * row, is refused at once — exactly one pump run, exactly one REFILL frame.
 */
class DeviceRepositoryConcurrencyFirmwareTest {
    @get:Rule val tmp = TemporaryFolder()

    private val address = "A0:B1:C2:D3:E4:F5"
    private val deviceId = "a0b1c2d3e4f5"
    private val setup = ByteArray(16) { (0x30 + it).toByte() }
    private lateinit var noise: NativeNoiseProvider
    private lateinit var scope: CoroutineScope
    private val ports = FirmwareGattPortFactory()
    private val journal = DeviceRepositoryConcurrencyTest.GatedJournal(InMemoryPendingOpsJournal())
    private val trustedTime = PhoneTimeSource { PhoneTime(System.currentTimeMillis(), 100) } // test source only (D6)

    @Before
    fun setUp() {
        noise = TestNative.load()
        FirmwareSim.load()
        assertTrue(FirmwareSim.reset(hexBytes(deviceId), setup))
        scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    }

    @After
    fun tearDown() {
        if (FirmwareSim.wakeRunning()) {
            FirmwareSim.advance(24L * 3600 * 1000)
            assertTrue(FirmwareSim.joinWake(20_000))
        }
        val d = FirmwareSim.diagnostics()
        assertEquals("DSP_CHECK failures inside the firmware world", 0, d[0])
        assertEquals("pump oracle: run without a durable decision, or a duplicate manual run", 0, d[5])
        scope.cancel()
    }

    private fun hexBytes(h: String) = ByteArray(h.length / 2) { h.substring(2 * it, 2 * it + 2).toInt(16).toByte() }

    @Test
    fun concurrent_doses_and_refills_reach_the_firmware_once() = runBlocking {
        val store = CredentialStore(tmp.newFolder("cred"), FakeKeyWrapper(), noise)
        val manager = DeviceSessionManager(GattTransportFactory(ports, { BluetoothReadiness.Ready }, scope), noise, store, scope)
        val repo = DeviceDispenserRepository(deviceId, { address }, ManagerSessionPort(manager), journal, InMemoryDeviceCacheStore(),
            CredentialSeqAllocator(store), trustedTime, scope)
        assertTrue(FirmwareSim.buttonWake(false))
        assertTrue(FirmwareSim.waitAdvertising(5_000))
        val claimed = (SetupLabel.parse(SetupLabel.format(deviceId, setup)) as SetupLabel.Result.Ok).label.use { manager.open(address, OpenRequest.Claim(it, "A")) }
        assertTrue("$claimed", claimed is OpenResult.Opened)
        assertTrue(repo.connect() is ConnectOutcome.Connected)
        // ready to dose: trusted time, a capacity, a full reservoir, READY
        assertTrue((repo.setTime() as OpResult.Ok).value.decision in setOf(0, 3))
        assertTrue(repo.setReadiness(Readiness.SERVICE_REQUIRED) is OpResult.Ok)
        val cfg = (repo.refreshConfig() as OpResult.Ok).value
        assertTrue(repo.setConfig(cfg.revision, ConfigRecord.F_CAPACITY, cfg.copy(reservoirCapacityUl = 200_000)) is OpResult.Ok)
        assertTrue(repo.refill(200_000) is OpResult.Ok)
        assertTrue(repo.setReadiness(Readiness.READY) is OpResult.Ok)

        // REFILL: the second caller arrives while the first is parked after its guard
        var gate = journal.parkNextList()
        val refillsBefore = ports.sent(M.REFILL)
        val a = async(Dispatchers.Default) { repo.refill(150_000) }
        withTimeout(5_000) { journal.parked.await() }
        // the second caller waits only for the first one's short guard read, then is refused
        val b = async(Dispatchers.Default) { repo.refill(160_000) }
        kotlinx.coroutines.delay(100)
        gate.complete(Unit)
        assertEquals(OpResult.Refused(LocalRefusal.InProgress), withTimeout(10_000) { b.await() })
        val ra = withTimeout(10_000) { a.await() }
        assertTrue("$ra", ra is OpResult.Ok)
        assertEquals(refillsBefore + 1, ports.sent(M.REFILL))

        // MANUAL_DOSE: same, the device suspends BLE for the pump after the first
        gate = journal.parkNextList()
        val dose = async(Dispatchers.Default) { repo.manualDose(1_000) }
        withTimeout(5_000) { journal.parked.await() }
        val second = async(Dispatchers.Default) { repo.manualDose(1_000) }
        kotlinx.coroutines.delay(100)
        gate.complete(Unit)
        assertEquals(OpResult.Refused(LocalRefusal.InProgress), withTimeout(10_000) { second.await() })
        val first = withTimeout(20_000) { dose.await() }
        assertTrue("$first", first is OpResult.Uncertain || first is OpResult.Ok)
        assertEquals(1, ports.sent(M.MANUAL_DOSE))
        FirmwareSim.advance(60_000)
        withTimeout(10_000) { while (FirmwareSim.pumpRuns() < 1) kotlinx.coroutines.delay(5) }
        assertEquals("exactly one pump run", 1, FirmwareSim.pumpRuns())
    }
}
