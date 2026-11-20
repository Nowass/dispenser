package cz.nowass.dispenser.data.onboarding

import cz.nowass.dispenser.data.device.ConnectOutcome
import cz.nowass.dispenser.data.device.FakeDeviceRepository
import cz.nowass.dispenser.device.ops.ConfigRecord
import cz.nowass.dispenser.device.ops.DeviceError
import cz.nowass.dispenser.device.ops.DeviceErrorCode
import cz.nowass.dispenser.device.ops.OpResult
import cz.nowass.dispenser.device.ops.Readiness
import cz.nowass.dispenser.ui.devices.BenchPumpViewModel
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.test.UnconfinedTestDispatcher
import kotlinx.coroutines.test.resetMain
import kotlinx.coroutines.test.runTest
import kotlinx.coroutines.test.setMain
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

/** BENCH ONLY pump configuration: validation, firmware rules mirrored locally, READY only on request. */
@OptIn(ExperimentalCoroutinesApi::class)
class BenchPumpControllerTest {
    @Before fun setUp() = Dispatchers.setMain(UnconfinedTestDispatcher())
    @After fun tearDown() = Dispatchers.resetMain()

    private val now = 1_795_200_000L
    private val cfg = ConfigRecord(7, 500_000, 300_000, 20, 24, 0, 0, 0, 0, 0, 0, 0, hwMaxRuntimeMs = 10_000)
    private val notReady = FakeDeviceRepository.status().copy(readiness = Readiness.SERVICE_REQUIRED, gates = (1L shl 2) or (1L shl 7), pumpProblems = 0x7F)
    private val valid = BenchPumpForm("1000", "15000", "60000", "10000", "60", calibrateNow = true)

    private fun repo() = FakeDeviceRepository().apply {
        connectOutcome = ConnectOutcome.Connected(generation = 1, unresolved = 0)
        configOutcome = OpResult.Ok(cfg)
        statusOutcome = OpResult.Ok(notReady)
    }

    @Test
    fun validation_ranges_and_hardware_limit() {
        val c = BenchPumpController(FakeDeviceRepository())
        assertTrue(c.validate(valid, 10_000).isEmpty())
        assertEquals(BenchFieldError.ABOVE_HW_MAX, c.validate(valid.copy(maxRuntimeMs = "10001"), 10_000)[BenchField.MAX_RUNTIME])
        assertEquals(BenchFieldError.NO_HW_MAX, c.validate(valid, 0)[BenchField.MAX_RUNTIME]) // simulated pump at 0: nothing can run
        assertEquals(BenchFieldError.DAILY_BELOW_DOSE, c.validate(valid.copy(maxDailyUl = "14999"), 10_000)[BenchField.MAX_DAILY])
        assertEquals(BenchFieldError.NOT_POSITIVE, c.validate(valid.copy(flowUlPerS = "0"), 10_000)[BenchField.FLOW])
        assertEquals(BenchFieldError.MISSING, c.validate(valid.copy(minIntervalS = " "), 10_000)[BenchField.MIN_INTERVAL])
        assertEquals(BenchFieldError.NOT_A_NUMBER, c.validate(valid.copy(maxDoseUl = "15,5"), 10_000)[BenchField.MAX_DOSE])
        assertEquals(BenchFieldError.NOT_A_NUMBER, c.validate(valid.copy(maxDoseUl = "-1"), 10_000)[BenchField.MAX_DOSE])
        assertEquals(BenchFieldError.TOO_LARGE, c.validate(valid.copy(minIntervalS = "4294967296"), 10_000)[BenchField.MIN_INTERVAL])
        assertTrue(c.validate(valid.copy(minIntervalS = "4294967295", maxDailyUl = "4294967295"), 10_000).isEmpty())
    }

    @Test
    fun save_sends_the_fields_and_calibration_only_when_asked() = runBlocking {
        val r = repo()
        val c = BenchPumpController(r) { now }
        val snap = (c.load() as BenchOutcome.Loaded).snapshot
        r.setConfigOutcome = OpResult.Ok(8)
        assertTrue(c.save(snap, valid) is BenchOutcome.Loaded)
        val all = ConfigRecord.F_FLOW or ConfigRecord.F_MAX_DOSE or ConfigRecord.F_MAX_DAILY or ConfigRecord.F_MAX_RUNTIME or ConfigRecord.F_MIN_INTERVAL
        assertEquals("setConfig 7 0x${(all or ConfigRecord.F_CALIBRATED_AT).toString(16)}", r.calls.last { it.startsWith("setConfig") })
        val sent = r.lastConfig!!
        assertEquals(1000L, sent.pumpFlowUlPerS)
        assertEquals(15_000L, sent.maxDoseUl)
        assertEquals(60_000L, sent.maxDailyUl)
        assertEquals(10_000L, sent.maxRuntimeMs)
        assertEquals(60L, sent.minIntervalS)
        assertEquals(now, sent.calibratedAt)
        // without the explicit bench calibration the calibration field is not sent
        c.save(snap, valid.copy(calibrateNow = false))
        assertEquals("setConfig 7 0x${all.toString(16)}", r.calls.last { it.startsWith("setConfig") })
        assertTrue(r.calls.none { it.startsWith("readiness") }) // saving never changes readiness
    }

    @Test
    fun nothing_is_sent_for_invalid_input_or_while_ready() = runBlocking {
        val r = repo()
        val c = BenchPumpController(r) { now }
        val snap = (c.load() as BenchOutcome.Loaded).snapshot
        assertTrue(c.save(snap, valid.copy(maxRuntimeMs = "20000")) is BenchOutcome.Invalid)
        val ready = snap.copy(status = snap.status.copy(readiness = Readiness.READY))
        assertEquals(BenchOutcome.MustBeNotReady, c.save(ready, valid))
        assertTrue(r.calls.none { it.startsWith("setConfig") })
    }

    @Test
    fun ready_refusal_reports_the_closed_gates_and_problems() = runBlocking {
        val r = repo()
        val c = BenchPumpController(r) { now }
        r.readinessOutcome = OpResult.Rejected(DeviceError(DeviceErrorCode.REFUSED, (1L shl 7) or (1L shl 8), (1L shl 1) or (1L shl 6)))
        val res = c.setReady(true)
        assertEquals(BenchOutcome.ReadyRefused(listOf(BenchGate.PUMP_CONFIG, BenchGate.RESERVOIR_UNCONFIGURED),
            listOf(BenchPumpProblem.CALIBRATED_AT, BenchPumpProblem.HW_LIMIT)), res)
        assertEquals("readiness READY", r.calls.last())
        r.readinessOutcome = OpResult.Uncertain("lost", 4)
        assertEquals(BenchOutcome.Uncertain, c.setReady(false))
        assertEquals("readiness SERVICE_REQUIRED", r.calls.last())
    }

    @Test
    fun the_view_model_never_sends_ready_without_confirmation() = runTest {
        val r = repo()
        val vm = BenchPumpViewModel(BenchPumpController(r) { now })
        val s = vm.state.value
        assertFalse(s.loading)
        assertEquals("form starts from the device (unset fields empty)", "", s.form.flowUlPerS)
        assertTrue(r.calls.none { it.startsWith("readiness") }) // loading never touches readiness
        vm.requestReady()
        assertTrue(vm.state.value.confirmReady)
        assertTrue(r.calls.none { it.startsWith("readiness") })
        vm.cancelReady()
        assertTrue(r.calls.none { it.startsWith("readiness") })
        r.readinessOutcome = OpResult.Ok(FakeDeviceRepository.status())
        vm.requestReady()
        vm.confirmReady()
        assertEquals(listOf("readiness READY"), r.calls.filter { it.startsWith("readiness") })
        // the form validates live against the device's hardware maximum
        vm.edit(valid.copy(maxRuntimeMs = "10001"))
        assertEquals(BenchFieldError.ABOVE_HW_MAX, vm.state.value.errors[BenchField.MAX_RUNTIME])
    }
}
