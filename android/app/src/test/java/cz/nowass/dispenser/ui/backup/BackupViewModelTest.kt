package cz.nowass.dispenser.ui.backup

import cz.nowass.dispenser.data.InMemoryAppSettingsRepository
import cz.nowass.dispenser.data.InMemoryScheduleStore
import cz.nowass.dispenser.data.InMemorySimulationStore
import cz.nowass.dispenser.data.StoredSchedule
import cz.nowass.dispenser.data.backup.BackupCodec
import cz.nowass.dispenser.data.backup.BackupContent
import cz.nowass.dispenser.data.backup.BackupError
import cz.nowass.dispenser.data.backup.BackupService
import cz.nowass.dispenser.data.backup.DocumentIo
import cz.nowass.dispenser.data.backup.MutexAccess
import cz.nowass.dispenser.data.backup.RawScheduleRows
import cz.nowass.dispenser.data.backup.RawSimulationRows
import cz.nowass.dispenser.data.backup.RestoreFiles
import cz.nowass.dispenser.data.backup.SimulationEffect
import cz.nowass.dispenser.domain.DEFAULT_ZONE
import cz.nowass.dispenser.domain.Recurrence
import cz.nowass.dispenser.domain.Schedule
import cz.nowass.dispenser.domain.SyncState
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.test.UnconfinedTestDispatcher
import kotlinx.coroutines.test.resetMain
import kotlinx.coroutines.test.runTest
import kotlinx.coroutines.test.setMain
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import java.io.IOException
import java.time.Clock
import java.time.Instant
import java.time.LocalDate
import java.time.LocalTime

@OptIn(ExperimentalCoroutinesApi::class)
class BackupViewModelTest {

    @Before fun setUp() = Dispatchers.setMain(UnconfinedTestDispatcher())
    @After fun tearDown() = Dispatchers.resetMain()

    private val clock = Clock.fixed(Instant.parse("2026-11-15T10:00:00Z"), DEFAULT_ZONE)
    private val existing = Schedule("old", 2, "Stará", true, 5.0, LocalDate.of(2026, 11, 1), LocalTime.of(7, 0), Recurrence.Daily())
    private val incoming = Schedule("new", 1, "Nová", true, 12.5, LocalDate.of(2026, 11, 1), LocalTime.of(9, 0), Recurrence.Daily(2))

    private class MemoryFiles : RestoreFiles {
        var snapshot: ByteArray? = null
        var journal: ByteArray? = null
        override fun journalExists() = journal != null
        override suspend fun writeSnapshot(bytes: ByteArray) { snapshot = bytes }
        override suspend fun readSnapshot() = snapshot
        override suspend fun writeJournal(bytes: ByteArray) { journal = bytes }
        override suspend fun readJournal() = journal
        override suspend fun deleteJournal() { journal = null }
    }

    private class FakeDocuments(val files: MutableMap<String, ByteArray> = mutableMapOf()) : DocumentIo {
        var failWrites = false
        override suspend fun write(uri: String, bytes: ByteArray) {
            if (failWrites) throw IOException("provider failed")
            files[uri] = bytes
        }
        override suspend fun read(uri: String, maxBytes: Int): ByteArray = files[uri] ?: throw IOException("no such document")
    }

    private val store = InMemoryScheduleStore(listOf(StoredSchedule(existing, SyncState.PENDING)))
    private val settings = InMemoryAppSettingsRepository("Původní")
    private val service = BackupService(store, InMemorySimulationStore(), settings, MemoryFiles(), MutexAccess(), clock, { "0.4.0 (4)" })
    private val docs = FakeDocuments()
    private val vm = BackupViewModel(service, docs, clock)

    private fun backupBytes() = BackupCodec.encode(BackupContent(clock.instant(), "0.4.0 (4)", "Nové", listOf(incoming), null)).toByteArray()

    @Test
    fun suggestedFileName_containsDate() {
        assertEquals("dispenser-zaloha-2026-11-15.json", vm.suggestedFileName())
    }

    @Test
    fun pickerCancellation_doesNothing() = runTest {
        vm.onExportDestination(null)
        vm.onImportSource(null)
        assertEquals(BackupUiState(), vm.uiState.value)
        assertTrue(docs.files.isEmpty())
    }

    @Test
    fun export_writesValidBackupToChosenDocument() = runTest {
        vm.setIncludeSimulation(true)
        vm.onExportDestination("content://doc/1")
        val msg = vm.uiState.value.message as BackupMessage.Exported
        assertEquals(1, msg.schedules)
        assertEquals(0, msg.historyEvents)
        val decoded = BackupCodec.decode(docs.files.getValue("content://doc/1")) as BackupCodec.Decoded.Valid
        assertEquals(listOf(existing), decoded.content.schedules)
        assertEquals("Původní", decoded.content.displayName)
        assertNotNull(decoded.content.simulation)
    }

    @Test
    fun exportFailure_isReported() = runTest {
        docs.failWrites = true
        vm.onExportDestination("content://doc/1")
        assertEquals(BackupMessage.ExportFailed, vm.uiState.value.message)
        assertTrue(!vm.uiState.value.busy)
    }

    @Test
    fun import_showsConfirmation_cancelKeepsData() = runTest {
        docs.files["content://in"] = backupBytes()
        vm.onImportSource("content://in")
        val plan = vm.uiState.value.pendingRestore
        assertNotNull(plan)
        assertEquals(1, plan!!.currentScheduleCount)
        assertEquals("Nové", plan.content.displayName)
        assertEquals(SimulationEffect.NONE, plan.simulationEffect)

        vm.cancelRestore()
        assertNull(vm.uiState.value.pendingRestore)
        assertEquals(listOf(existing), store.observe().first().schedules.map { it.schedule })
        assertEquals("Původní", settings.displayName.first())
    }

    @Test
    fun confirm_replacesConfiguration_andReportsSuccess() = runTest {
        docs.files["content://in"] = backupBytes()
        vm.onImportSource("content://in")
        vm.confirmRestore()
        assertEquals(BackupMessage.Restored(1, SimulationEffect.NONE), vm.uiState.value.message)
        assertNull(vm.uiState.value.pendingRestore)
        assertEquals(listOf(StoredSchedule(incoming, SyncState.PENDING)), store.observe().first().schedules)
        assertEquals("Nové", settings.displayName.first())
    }

    @Test
    fun invalidOrUnreadableFiles_areReported_withoutConfirmation() = runTest {
        docs.files["content://bad"] = "{ not json".toByteArray()
        vm.onImportSource("content://bad")
        assertEquals(BackupMessage.Invalid(BackupError(BackupError.Kind.NOT_JSON)), vm.uiState.value.message)
        assertNull(vm.uiState.value.pendingRestore)

        vm.onImportSource("content://missing")
        assertEquals(BackupMessage.ReadFailed, vm.uiState.value.message)
        assertEquals(listOf(existing), store.observe().first().schedules.map { it.schedule })
    }

    @Test
    fun inMemoryStores_supportRawSnapshotRoundTrip() = runTest {
        val raw: RawScheduleRows = store.captureRaw()
        val sim = InMemorySimulationStore()
        assertEquals(RawSimulationRows.EMPTY, sim.captureRaw())
        store.restoreRaw(raw)
        assertEquals(listOf(existing), store.observe().first().schedules.map { it.schedule })
    }
}
