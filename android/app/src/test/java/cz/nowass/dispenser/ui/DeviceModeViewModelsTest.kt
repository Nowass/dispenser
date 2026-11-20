package cz.nowass.dispenser.ui

import androidx.lifecycle.SavedStateHandle
import cz.nowass.dispenser.data.InMemoryAppSettingsRepository
import cz.nowass.dispenser.data.AppVersion
import cz.nowass.dispenser.data.device.DeviceSchedules
import cz.nowass.dispenser.data.device.DeviceUiRepository
import cz.nowass.dispenser.data.device.FakeDeviceRepository
import cz.nowass.dispenser.data.device.Fresh
import cz.nowass.dispenser.data.device.ScheduleWrite
import cz.nowass.dispenser.device.ops.LocalRefusal
import cz.nowass.dispenser.device.ops.OpResult
import cz.nowass.dispenser.device.session.SessionState
import cz.nowass.dispenser.domain.ConnectionState
import cz.nowass.dispenser.domain.Freshness
import cz.nowass.dispenser.domain.RecurrenceKind
import cz.nowass.dispenser.domain.ScheduleDraft
import cz.nowass.dispenser.ui.calendar.CalendarUiState
import cz.nowass.dispenser.ui.calendar.CalendarViewModel
import cz.nowass.dispenser.ui.dashboard.DashboardUiState
import cz.nowass.dispenser.ui.dashboard.DashboardViewModel
import cz.nowass.dispenser.ui.history.HistoryUiState
import cz.nowass.dispenser.ui.history.HistoryViewModel
import cz.nowass.dispenser.ui.reservoir.RefillOutcome
import cz.nowass.dispenser.ui.reservoir.ReservoirUiState
import cz.nowass.dispenser.device.ops.DeviceValidation
import cz.nowass.dispenser.ui.reservoir.RefillError
import cz.nowass.dispenser.ui.reservoir.ReservoirConfigErrors
import cz.nowass.dispenser.ui.reservoir.ReservoirViewModel
import cz.nowass.dispenser.ui.schedule.EditorResult
import cz.nowass.dispenser.ui.schedule.ScheduleEditorUiState
import cz.nowass.dispenser.ui.schedule.ScheduleEditorViewModel
import cz.nowass.dispenser.ui.schedule.ScheduleWriteNotice
import cz.nowass.dispenser.ui.settings.AppSettingsUiState
import cz.nowass.dispenser.ui.settings.AppSettingsViewModel
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.filterIsInstance
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.UnconfinedTestDispatcher
import kotlinx.coroutines.test.resetMain
import kotlinx.coroutines.test.runTest
import kotlinx.coroutines.test.setMain
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import java.time.Clock
import java.time.Instant
import java.time.LocalDate
import java.time.LocalTime
import java.time.ZoneOffset

/**
 * The existing shared screens in DEVICE mode (M1.10e.4a), over the adapter with a scripted device:
 * no simulation labels, cached values never live, online-only edits, and DEVICE outcomes (Uncertain
 * above all) never presented as an ordinary failure that invites a retry.
 */
@OptIn(ExperimentalCoroutinesApi::class)
class DeviceModeViewModelsTest {
    @Before fun setUp() = Dispatchers.setMain(UnconfinedTestDispatcher())
    @After fun tearDown() = Dispatchers.resetMain()

    private val now = Instant.parse("2026-11-19T10:00:00Z")
    private val readAt = Instant.parse("2026-11-19T09:30:00Z")
    private val fake = FakeDeviceRepository()
    private val repo = DeviceUiRepository(fake, Clock.fixed(now, ZoneOffset.UTC), clockTick = null)
    private val draft = ScheduleDraft("Ranní", 2.0, LocalDate.of(2027, 1, 4), LocalTime.of(8, 0), RecurrenceKind.DAILY)
    private val stored = FakeDeviceRepository.schedule(draft, "id-1", 3)

    private fun connected() {
        fake.authenticated(7)
        fake.statusF.value = Fresh.Live(FakeDeviceRepository.status(), readAt, 7)
        fake.schedulesF.value = Fresh.Live(DeviceSchedules(9, listOf(stored)), readAt, 7)
    }

    private fun disconnectedWithCache() {
        fake.connection.value = SessionState.Idle
        fake.statusF.value = Fresh.Cached(FakeDeviceRepository.status(), readAt)
        fake.schedulesF.value = Fresh.Cached(DeviceSchedules(9, listOf(stored)), readAt)
    }

    private suspend inline fun <reified T> TestScope.ready(flow: kotlinx.coroutines.flow.StateFlow<*>): T {
        backgroundScope.launch { flow.collect {} }
        return flow.filterIsInstance<T>().first()
    }

    @Test
    fun dashboard_shows_device_values_with_freshness_never_simulated() = runTest {
        disconnectedWithCache()
        val vm = DashboardViewModel(repo)
        val s = ready<DashboardUiState.Ready>(vm.uiState)
        assertFalse(s.isDemo)
        assertFalse(s.battery.isSimulated)
        assertEquals(80, s.battery.percent)
        assertEquals(Freshness.Cached(readAt), s.freshness)
        assertEquals(150.0, s.reservoir!!.estimatedRemainingMl, 0.0)

        connected()
        assertEquals(Freshness.Live(readAt), vm.uiState.filterIsInstance<DashboardUiState.Ready>().first { it.freshness is Freshness.Live }.freshness)
    }

    @Test
    fun history_is_unavailable_in_device_mode() = runTest {
        assertEquals(HistoryUiState.Unavailable, HistoryViewModel(repo).uiState.value)
    }

    @Test
    fun settings_show_device_mode_and_cached_freshness() = runTest {
        disconnectedWithCache()
        val s = ready<AppSettingsUiState.Ready>(AppSettingsViewModel(InMemoryAppSettingsRepository(), repo, AppVersion("0.5.0", 5)).uiState)
        assertFalse(s.isDemo)
        assertFalse(s.simulationActive)
        assertEquals(ConnectionState.DISCONNECTED, s.connection)
        assertEquals(Freshness.Cached(readAt), s.freshness)
    }

    @Test
    fun editor_is_online_only_and_an_uncertain_save_closes_without_retry() = runTest {
        disconnectedWithCache()
        val vm = ScheduleEditorViewModel(repo, SavedStateHandle(mapOf(ScheduleEditorViewModel.ARG_SCHEDULE_ID to "id-1")))
        var s = ready<ScheduleEditorUiState.Editing>(vm.uiState)
        vm.setName("Ranní 2")
        s = vm.uiState.value as ScheduleEditorUiState.Editing
        assertTrue(s.needsConnection)
        assertFalse("no save while disconnected", s.canSave)

        connected()
        s = vm.uiState.filterIsInstance<ScheduleEditorUiState.Editing>().first { !it.needsConnection }
        assertTrue(s.canSave)

        // Provably not sent (e.g. the link dropped before sending): the editor stays open with the reason.
        fake.writeOutcome = OpResult.NotSent("session ended")
        vm.save()
        s = vm.uiState.value as ScheduleEditorUiState.Editing
        assertEquals(ScheduleWriteNotice.NOT_SENT, s.notice)
        assertNull(vm.closeResult.value)
        assertTrue(s.canSave)

        // An earlier change awaits verification: nothing sent, editor stays open.
        fake.writeOutcome = OpResult.Refused(LocalRefusal.Unresolved(3))
        vm.save()
        assertEquals(ScheduleWriteNotice.AWAITING_VERIFICATION, (vm.uiState.value as ScheduleEditorUiState.Editing).notice)

        // Uncertain: not a failure, not a conflict; the editor closes with the "verified later" result.
        fake.writeOutcome = OpResult.Uncertain("lost reply", journalId = 4)
        vm.save()
        assertEquals(EditorResult.UNCERTAIN, vm.closeResult.value)
        s = vm.uiState.value as ScheduleEditorUiState.Editing
        assertFalse(s.conflict)
        assertFalse(s.deletedMeanwhile)
        assertEquals(listOf("update id-1 3 enabled=true"), fake.calls.distinct())
    }

    @Test
    fun editor_confirmed_save_reports_saved() = runTest {
        connected()
        val vm = ScheduleEditorViewModel(repo, SavedStateHandle(mapOf(ScheduleEditorViewModel.ARG_SCHEDULE_ID to "id-1")))
        ready<ScheduleEditorUiState.Editing>(vm.uiState)
        vm.setName("Ranní 2")
        fake.writeOutcome = OpResult.Ok(ScheduleWrite("id-1", 4, 10))
        vm.save()
        assertEquals(EditorResult.SAVED, vm.closeResult.value)
    }

    @Test
    fun unknown_reservoir_offers_configuration_with_validation_and_device_outcomes() = runTest {
        fake.authenticated(7)
        fake.statusF.value = Fresh.Live(FakeDeviceRepository.status(capacityUl = 0, remainingUl = 0), readAt, 7)
        val vm = ReservoirViewModel(repo)
        val s = vm.uiState.let { f -> backgroundScope.launch { f.collect {} }; f.filterIsInstance<ReservoirUiState.Ready>().first { !it.refillNeedsConnection } }
        assertNull("capacity unknown", s.reservoir)
        assertTrue(s.configurable)
        assertEquals(ReservoirConfigErrors(null, null), vm.validateConfigure("500", "300"))
        assertEquals(ReservoirConfigErrors(null, null), vm.validateConfigure("500", "500")) // full
        assertEquals(RefillError.NOT_POSITIVE, vm.validateConfigure("500", "0").remaining) // empty: refused, not substituted
        assertEquals(RefillError.ABOVE_CAPACITY, vm.validateConfigure("500", "501").remaining)
        assertEquals(RefillError.NOT_POSITIVE, vm.validateConfigure("0", "1").capacity)
        assertEquals(RefillError.OUT_OF_RANGE, vm.validateConfigure("4294968", "1").capacity) // > u32 µl
        assertEquals(ReservoirConfigErrors(null, null), vm.validateConfigure("4294967,295", "1"))
        assertEquals(RefillError.INVALID, vm.validateConfigure("abc", "1").capacity)
        assertEquals(RefillError.TOO_PRECISE, vm.validateConfigure("500", "1,0005").remaining)
        // invalid input is never sent
        vm.configure("500", "0")
        assertTrue(fake.calls.none { it.startsWith("refill") })
        // outcomes: uncertain is neither success nor failure; confirmed sets the done state
        fake.refillOutcome = OpResult.Uncertain("lost reply", journalId = 3)
        vm.configure("500", "300")
        assertEquals("refill 300000 capacity 500000", fake.calls.last())
        assertEquals(RefillOutcome.UNCERTAIN, vm.refillOutcome.value)
        assertFalse(vm.configured.value)
        vm.consumeRefillDone()
        fake.refillOutcome = OpResult.Refused(LocalRefusal.Invalid(setOf(DeviceValidation.CAPACITY_INVALID)))
        vm.configure("500", "300")
        assertEquals(RefillOutcome.NOT_ACCEPTED, vm.refillOutcome.value)
        vm.consumeRefillDone()
        fake.refillOutcome = OpResult.Ok(FakeDeviceRepository.status(capacityUl = 500_000, remainingUl = 300_000))
        vm.configure("500", "300")
        assertTrue(vm.configured.value)
        assertNull(vm.refillOutcome.value)
    }

    @Test
    fun reservoir_refill_is_online_only_and_uncertain_is_not_success_or_failure() = runTest {
        disconnectedWithCache()
        val vm = ReservoirViewModel(repo)
        var s = ready<ReservoirUiState.Ready>(vm.uiState)
        assertFalse(s.isDemo)
        assertTrue(s.refillNeedsConnection)
        assertEquals(Freshness.Cached(readAt), s.freshness)

        connected()
        s = vm.uiState.filterIsInstance<ReservoirUiState.Ready>().first { !it.refillNeedsConnection }
        assertEquals(Freshness.Live(readAt), s.freshness)

        fake.refillOutcome = OpResult.Uncertain("lost reply", journalId = 2)
        vm.refill("200")
        assertEquals(RefillOutcome.UNCERTAIN, vm.refillOutcome.value)
        assertFalse(vm.refillDone.value)

        fake.refillOutcome = OpResult.NotSent("not connected")
        vm.consumeRefillDone()
        vm.refill("200")
        assertEquals(RefillOutcome.NOT_SENT, vm.refillOutcome.value)

        fake.refillOutcome = OpResult.Ok(FakeDeviceRepository.status(remainingUl = 200_000))
        vm.consumeRefillDone()
        vm.refill("200")
        assertTrue(vm.refillDone.value)
        assertNull(vm.refillOutcome.value)
    }

    @Test
    fun calendar_shows_device_schedules_with_freshness_and_toggle_outcomes() = runTest {
        disconnectedWithCache()
        val vm = CalendarViewModel(repo, SavedStateHandle())
        var s = ready<CalendarUiState.Ready>(vm.uiState)
        assertEquals(listOf("id-1"), s.schedules.map { it.schedule.id })
        assertEquals(Freshness.Cached(readAt), s.scheduleFreshness)
        assertTrue(s.editingNeedsConnection)

        connected()
        s = vm.uiState.filterIsInstance<CalendarUiState.Ready>().first { !it.editingNeedsConnection }
        fake.writeOutcome = OpResult.Uncertain("lost reply", journalId = 1)
        vm.setEnabled("id-1", false)
        assertEquals(ScheduleWriteNotice.UNCERTAIN, vm.writeNotice.value)
        assertEquals(listOf("update id-1 3 enabled=false"), fake.calls)
    }
}
