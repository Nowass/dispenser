package cz.nowass.dispenser.ui.schedule

import androidx.lifecycle.SavedStateHandle
import cz.nowass.dispenser.data.InMemoryScheduleStore
import cz.nowass.dispenser.data.LocalDispenserRepository
import cz.nowass.dispenser.data.MockDispenserRepository
import cz.nowass.dispenser.data.MockSeed
import cz.nowass.dispenser.data.ScheduleStore
import cz.nowass.dispenser.data.StoreWriteOutcome
import cz.nowass.dispenser.data.StoredSchedule
import cz.nowass.dispenser.domain.DEFAULT_ZONE
import cz.nowass.dispenser.domain.RecurrenceKind
import cz.nowass.dispenser.domain.SyncState
import kotlinx.coroutines.CompletableDeferred
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
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import java.time.Clock
import java.time.LocalDate
import java.time.LocalTime

/** I3 (DST one-off), C2 (semantic "modified"), C4 (no navigation/edits while saving). */
@OptIn(ExperimentalCoroutinesApi::class)
class ScheduleEditorCorrectionsTest {

    @Before fun setUp() = Dispatchers.setMain(UnconfinedTestDispatcher())
    @After fun tearDown() = Dispatchers.resetMain()

    private suspend fun TestScope.editing(vm: ScheduleEditorViewModel): ScheduleEditorUiState.Editing {
        backgroundScope.launch { vm.uiState.collect {} }
        return vm.uiState.filterIsInstance<ScheduleEditorUiState.Editing>().first()
    }

    private fun newVm(repo: LocalDispenserRepository = MockDispenserRepository(), handle: SavedStateHandle = SavedStateHandle(mapOf(ScheduleEditorViewModel.ARG_DATE to "2027-03-28"))) =
        ScheduleEditorViewModel(repo, handle)

    private fun editVm(repo: LocalDispenserRepository = MockDispenserRepository(), id: String = "demo-morning", handle: SavedStateHandle? = null) =
        ScheduleEditorViewModel(repo, handle ?: SavedStateHandle(mapOf(ScheduleEditorViewModel.ARG_SCHEDULE_ID to id)))

    // ---- I3 ----

    @Test
    fun onceInSpringGap_blocksSaveWithTimeError_recurringDoesNot() = runTest {
        val vm = newVm()
        editing(vm)
        vm.setAmountText("5")
        vm.setKind(RecurrenceKind.ONCE)
        vm.setTime(LocalTime.of(2, 15))
        val gap = editing(vm)
        assertEquals(setOf(EditorFieldError.TIME_NONEXISTENT), gap.visibleErrors)
        assertFalse(gap.canSave)

        vm.setKind(RecurrenceKind.DAILY) // recurring: gap day skipped, no error
        assertTrue(editing(vm).visibleErrors.isEmpty())
        assertTrue(editing(vm).canSave)

        vm.setKind(RecurrenceKind.ONCE)
        vm.setTime(LocalTime.of(3, 15))
        assertTrue(editing(vm).canSave)
    }

    // ---- C2 ----

    @Test
    fun restoringOriginalValues_clearsModifiedState() = runTest {
        val vm = editVm()
        assertFalse(editing(vm).isDirty)
        vm.setName("Jiný název")
        assertTrue(editing(vm).isDirty)
        assertTrue(editing(vm).canSave)
        vm.setName("Ranní dávka")
        assertFalse(editing(vm).isDirty)
        assertFalse(editing(vm).canSave)

        vm.setAmountText("10,0") // same value, different text
        assertFalse(editing(vm).isDirty)
        vm.setTime(LocalTime.of(9, 0)); vm.setTime(LocalTime.of(8, 0))
        vm.setHasEndDate(true); vm.setHasEndDate(false) // end date not in effect -> irrelevant
        vm.changeInterval(+1); vm.changeInterval(-1)
        assertFalse(editing(vm).isDirty)
    }

    @Test
    fun newSchedule_backToDefaults_isNotModified() = runTest {
        val vm = newVm()
        editing(vm)
        vm.setAmountText("5")
        assertTrue(editing(vm).isDirty)
        vm.setAmountText("")
        assertFalse(editing(vm).isDirty)
        vm.setKind(RecurrenceKind.WEEKLY); vm.setKind(RecurrenceKind.DAILY) // weekdays irrelevant for DAILY
        assertFalse(editing(vm).isDirty)
    }

    @Test
    fun modifiedState_survivesRecreation_againstTheOriginal() = runTest {
        val repo = MockDispenserRepository()
        val handle = SavedStateHandle(mapOf(ScheduleEditorViewModel.ARG_SCHEDULE_ID to "demo-morning"))
        val first = editVm(repo, handle = handle)
        editing(first)
        first.setName("Změna")
        assertTrue(editing(editVm(repo, handle = handle)).isDirty)

        val second = editVm(repo, handle = handle)
        editing(second)
        second.setName("Ranní dávka")
        assertFalse(editing(editVm(repo, handle = handle)).isDirty)
    }

    // ---- C4 ----

    /** Store whose writes wait for [gate], to observe the in-flight save state. */
    private class GatedStore(private val inner: ScheduleStore) : ScheduleStore by inner {
        val gate = CompletableDeferred<Unit>()
        var writes = 0
        override suspend fun insert(stored: StoredSchedule): Boolean { writes++; gate.await(); return inner.insert(stored) }
        override suspend fun replace(stored: StoredSchedule, expectedRevision: Int): StoreWriteOutcome { writes++; gate.await(); return inner.replace(stored, expectedRevision) }
        override suspend fun delete(id: String, expectedRevision: Int): StoreWriteOutcome { writes++; gate.await(); return inner.delete(id, expectedRevision) }
    }

    @Test
    fun whileSaving_editsDeleteAndRepeatSaveAreIgnored_thenSavesExactlyOnce() = runTest {
        val seed = MockSeed.default()
        val store = GatedStore(InMemoryScheduleStore(seed.schedules.map { StoredSchedule(it, SyncState.PENDING) }))
        val repo = LocalDispenserRepository(store, Clock.fixed(seed.device.now, DEFAULT_ZONE), clockTick = null)
        val vm = editVm(repo)
        editing(vm)
        vm.setAmountText("11")
        vm.save()

        val saving = editing(vm)
        assertTrue(saving.isSaving)
        assertFalse(saving.canSave)
        vm.setAmountText("99")       // ignored: would otherwise be lost when the editor closes
        vm.setName("Během ukládání")  // ignored
        vm.save()                     // ignored: no duplicate write
        vm.delete()                   // ignored
        assertEquals("11", editing(vm).form.amountText)
        assertEquals(null, vm.closeResult.value) // no navigation before the write completes

        store.gate.complete(Unit)
        assertEquals(EditorResult.SAVED, vm.closeResult.value)
        assertEquals(1, store.writes)
        val saved = repo.observeSchedules().first().single { it.id == "demo-morning" }
        assertEquals(11.0, saved.amountMl, 0.0)
        assertEquals("Ranní dávka", saved.name)
        assertEquals(2, saved.revision)
    }
}
