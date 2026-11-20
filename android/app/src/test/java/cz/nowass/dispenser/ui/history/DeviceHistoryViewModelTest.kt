package cz.nowass.dispenser.ui.history

import cz.nowass.dispenser.data.device.ConnectOutcome
import cz.nowass.dispenser.data.device.DeviceHistoryLoader
import cz.nowass.dispenser.data.device.DeviceHistoryPage
import cz.nowass.dispenser.data.device.DeviceSchedules
import cz.nowass.dispenser.data.device.DeviceUiRepository
import cz.nowass.dispenser.data.device.FakeDeviceRepository
import cz.nowass.dispenser.data.device.Fresh
import cz.nowass.dispenser.device.ops.DeviceError
import cz.nowass.dispenser.device.ops.DeviceErrorCode
import cz.nowass.dispenser.device.ops.HistoryEventKind
import cz.nowass.dispenser.device.ops.HistoryEventRecord
import cz.nowass.dispenser.device.ops.HistoryResult
import cz.nowass.dispenser.device.ops.OpResult
import cz.nowass.dispenser.domain.RecurrenceKind
import cz.nowass.dispenser.domain.ScheduleDraft
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

/** Phase C: the DEVICE-mode history screen over the device log (paging, duplicates, states). */
@OptIn(ExperimentalCoroutinesApi::class)
class DeviceHistoryViewModelTest {
    @Before fun setUp() = Dispatchers.setMain(UnconfinedTestDispatcher())
    @After fun tearDown() = Dispatchers.resetMain()

    private val now = Instant.parse("2026-11-20T10:00:00Z")
    private val fake = FakeDeviceRepository()
    private val repo = DeviceUiRepository(fake, Clock.fixed(now, ZoneOffset.UTC), clockTick = null)
    private val draft = ScheduleDraft("Ranní", 5.0, LocalDate.of(2026, 11, 20), LocalTime.of(8, 0), RecurrenceKind.ONCE)
    private val stored = FakeDeviceRepository.schedule(draft, "sched-1", 1)

    private fun ev(seq: Long, kind: HistoryEventKind = HistoryEventKind.DOSE_SCHEDULED, result: HistoryResult = HistoryResult.OK,
                   at: Long? = 1_795_158_000L + seq * 60, id: String? = if (kind == HistoryEventKind.DOSE_SCHEDULED) "sched-1" else null) =
        HistoryEventRecord(seq, kind, result, at, true, if (kind.isDose) 5_000 else 0, 100_000, 95_000, 100_000, id, false, true)

    private fun connect() {
        fake.connectOutcome = ConnectOutcome.Connected(generation = 7, unresolved = 0)
        fake.schedulesF.value = Fresh.Live(DeviceSchedules(1, listOf(stored)), now, 7)
        fake.authenticated(7)
    }

    private suspend fun TestScope.state(vm: HistoryViewModel): DeviceHistoryUi {
        backgroundScope.launch { vm.uiState.collect {} }
        return vm.uiState.filterIsInstance<HistoryUiState.Device>().first().ui
    }

    private fun seqs(ui: DeviceHistoryUi) = ui.days.flatMap { d -> d.rows.map { it.event.seq } }

    @Test
    fun empty_history_is_an_empty_list_not_an_error() = runTest {
        connect()
        val vm = HistoryViewModel(repo)
        val ui = state(vm)
        assertTrue(ui.loaded)
        assertTrue(ui.days.isEmpty())
        assertFalse(ui.canLoadMore)
        assertNull(ui.problem)
        assertEquals(listOf("history 0 4"), fake.calls.filter { it.startsWith("history") })
    }

    @Test
    fun newest_first_paging_without_duplicates() = runTest {
        fake.historyLog = (1L..30L).map { ev(it) }
        connect() // loads by itself once connected: 3 pages = 12 events
        val vm = HistoryViewModel(repo)
        var ui = state(vm)
        assertEquals((30L downTo 19L).toList(), seqs(ui))
        assertTrue(ui.canLoadMore)
        vm.loadMore()
        ui = state(vm)
        assertEquals((30L downTo 7L).toList(), seqs(ui))
        vm.loadMore()
        ui = state(vm)
        assertEquals((30L downTo 1L).toList(), seqs(ui))
        assertFalse(ui.canLoadMore)
        vm.loadMore() // nothing more is requested at the end
        val requests = fake.calls.count { it.startsWith("history") }
        vm.loadMore()
        assertEquals(requests, fake.calls.count { it.startsWith("history") })
        // A refresh never duplicates rows, and shows an event appended meanwhile.
        fake.historyLog = fake.historyLog + ev(31)
        vm.refresh()
        ui = state(vm)
        assertEquals((31L downTo 20L).toList(), seqs(ui))
        assertEquals(seqs(ui).toSet().size, seqs(ui).size)
    }

    @Test
    fun overlapping_pages_merge_by_seq() {
        val a = listOf(ev(5), ev(4), ev(3))
        val b = listOf(ev(4), ev(3), ev(2))
        assertEquals(listOf(5L, 4L, 3L, 2L), HistoryViewModel.merge(a, b).map { it.seq })
    }

    @Test
    fun results_schedule_names_and_unknown_time() = runTest {
        fake.historyLog = listOf(
            ev(1, HistoryEventKind.RESERVOIR_INIT),
            ev(2, result = HistoryResult.INTERRUPTED),
            ev(3, result = HistoryResult.PUMP_FAILED, id = "deleted-schedule"),
            ev(4, HistoryEventKind.DOSE_MANUAL),
            ev(5, HistoryEventKind.RESERVOIR_REFILL, at = null),
        )
        connect()
        val ui = state(HistoryViewModel(repo))
        val rows = ui.days.flatMap { it.rows }
        assertEquals(listOf(5L, 4L, 3L, 2L, 1L), rows.map { it.event.seq })
        assertNull(ui.days.first().date) // unknown time stays in device order
        assertEquals("Ranní", rows.first { it.event.seq == 2L }.scheduleName)
        assertNull(rows.first { it.event.seq == 3L }.scheduleName) // deleted schedule: no invented name
        assertEquals(HistoryResult.INTERRUPTED, rows.first { it.event.seq == 2L }.event.result)
        assertEquals(LocalDate.of(2026, 11, 20), ui.days.last().date)
    }

    @Test
    fun unsupported_firmware_is_permanent_and_reads_nothing_more() = runTest {
        connect()
        for (code in listOf(DeviceErrorCode.UNKNOWN_TYPE, DeviceErrorCode.UNAVAILABLE)) {
            fake.historyOutcome = OpResult.Rejected(DeviceError(code, 0, 0))
            val vm = HistoryViewModel(repo)
            val ui = state(vm)
            assertEquals(DeviceHistoryProblem.UNSUPPORTED, ui.problem)
            assertFalse(ui.canLoadMore)
            assertTrue(ui.days.isEmpty())
        }
    }

    @Test
    fun disconnected_device_is_not_contacted_until_asked() = runTest {
        fake.historyLog = listOf(ev(1))
        val vm = HistoryViewModel(repo)
        var ui = state(vm)
        assertFalse(ui.loaded)
        assertFalse(ui.connected)
        assertTrue(fake.calls.none { it.startsWith("history") })
        vm.refresh() // explicit load: the connection cannot be made
        ui = state(vm)
        assertEquals(DeviceHistoryProblem.NOT_CONNECTED, ui.problem)
        assertTrue(ui.days.isEmpty())
    }

    @Test
    fun a_failed_page_keeps_what_was_shown() = runTest {
        fake.historyLog = (1L..20L).map { ev(it) }
        connect()
        val vm = HistoryViewModel(repo)
        assertEquals(12, seqs(state(vm)).size)
        fake.historyOutcome = OpResult.Uncertain("lost")
        vm.loadMore()
        var ui = state(vm)
        assertEquals(DeviceHistoryProblem.FAILED, ui.problem)
        assertEquals((20L downTo 9L).toList(), seqs(ui))
        assertTrue(ui.canLoadMore) // can be retried
        vm.refresh() // a failed refresh never clears the list
        ui = state(vm)
        assertEquals(12, seqs(ui).size)
        fake.historyOutcome = null
        vm.loadMore()
        ui = state(vm)
        assertNull(ui.problem)
        assertEquals((20L downTo 1L).toList(), seqs(ui))
    }

    @Test
    fun faults_reported_by_the_device_are_shown() = runTest {
        fake.historyLog = listOf(ev(1))
        fake.historyFaults = true
        connect()
        assertTrue(state(HistoryViewModel(repo)).faults)
    }

    @Test
    fun loader_maps_outcomes() = runTest {
        val loader = DeviceHistoryLoader(fake)
        assertEquals(DeviceHistoryPage.NotConnected, loader.page(0))
        fake.connectOutcome = ConnectOutcome.Connected(generation = 1, unresolved = 0)
        fake.historyOutcome = OpResult.NotSent("closed")
        assertEquals(DeviceHistoryPage.NotConnected, loader.page(0))
        fake.historyOutcome = OpResult.Rejected(DeviceError(DeviceErrorCode.DEGRADED, 0, 0))
        assertEquals(DeviceHistoryPage.Failed(14), loader.page(0))
        fake.historyOutcome = OpResult.Rejected(DeviceError(DeviceErrorCode.FORBIDDEN, 0, 0))
        assertEquals(DeviceHistoryPage.Failed(5), loader.page(0))
        fake.historyOutcome = null
        fake.historyLog = listOf(ev(3), ev(9))
        val p = loader.page(0) as DeviceHistoryPage.Loaded
        assertEquals(listOf(9L, 3L), p.events.map { it.seq })
        assertEquals(9L, p.newestSeq)
        assertEquals(3L, p.oldestSeq)
    }
}
