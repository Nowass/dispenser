package cz.nowass.dispenser.ui.schedule

import androidx.lifecycle.SavedStateHandle
import cz.nowass.dispenser.data.MockDispenserRepository
import cz.nowass.dispenser.domain.Recurrence
import cz.nowass.dispenser.domain.RecurrenceKind
import cz.nowass.dispenser.domain.SyncState
import cz.nowass.dispenser.domain.toDraft
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
import java.time.DayOfWeek
import java.time.LocalDate
import java.time.LocalTime
import kotlin.time.Duration

@OptIn(ExperimentalCoroutinesApi::class)
class ScheduleEditorViewModelTest {

    @Before
    fun setUp() = Dispatchers.setMain(UnconfinedTestDispatcher())

    @After
    fun tearDown() = Dispatchers.resetMain()

    private fun repo() = MockDispenserRepository(simulatedAckDelay = Duration.ZERO, idGenerator = { "new-1" })

    private fun newEditor(repo: MockDispenserRepository, date: String? = "2026-11-20", handle: SavedStateHandle? = null) =
        ScheduleEditorViewModel(repo, handle ?: SavedStateHandle(buildMap { if (date != null) put(ScheduleEditorViewModel.ARG_DATE, date) }))

    private fun editEditor(repo: MockDispenserRepository, id: String) =
        ScheduleEditorViewModel(repo, SavedStateHandle(mapOf(ScheduleEditorViewModel.ARG_SCHEDULE_ID to id)))

    private suspend fun TestScope.editing(vm: ScheduleEditorViewModel): ScheduleEditorUiState.Editing {
        backgroundScope.launch { vm.uiState.collect {} }
        return vm.uiState.filterIsInstance<ScheduleEditorUiState.Editing>().first()
    }

    @Test
    fun new_isPrefilledWithSelectedDate() = runTest {
        val s = editing(newEditor(repo()))
        assertTrue(s.isNew)
        assertEquals(LocalDate.of(2026, 11, 20), s.form.startDate)
        assertEquals(setOf(DayOfWeek.FRIDAY), s.form.weekdays)
        assertEquals(RecurrenceKind.DAILY, s.form.kind)
        assertTrue(s.form.enabled)
        assertFalse(s.isDirty)
        assertEquals(PreviewUi.Invalid, s.preview) // amount still empty
        assertTrue(s.visibleErrors.isEmpty())      // "missing amount" waits for a save attempt
        assertEquals("Europe/Prague", s.zoneId)
    }

    @Test
    fun new_withoutDateArg_usesDeviceToday() = runTest {
        assertEquals(LocalDate.of(2026, 11, 14), editing(newEditor(repo(), date = null)).form.startDate)
    }

    @Test
    fun saveWithEmptyAmount_showsError_andCreatesNothing() = runTest {
        val repo = repo()
        val vm = newEditor(repo)
        editing(vm)
        vm.save()
        assertEquals(setOf(EditorFieldError.AMOUNT_MISSING), editing(vm).visibleErrors)
        assertFalse(vm.closeRequested.value)
        assertTrue(repo.observeSchedules().first().none { it.id == "new-1" })
    }

    @Test
    fun amountParsing_andLiveErrors() = runTest {
        val vm = newEditor(repo())
        editing(vm)
        vm.setAmountText("abc"); assertEquals(setOf(EditorFieldError.AMOUNT_INVALID), editing(vm).visibleErrors)
        vm.setAmountText("NaN"); assertEquals(setOf(EditorFieldError.AMOUNT_INVALID), editing(vm).visibleErrors)
        vm.setAmountText("Infinity"); assertEquals(setOf(EditorFieldError.AMOUNT_INVALID), editing(vm).visibleErrors)
        vm.setAmountText("0"); assertEquals(setOf(EditorFieldError.AMOUNT_NOT_POSITIVE), editing(vm).visibleErrors)
        vm.setAmountText("12,5"); assertTrue(editing(vm).visibleErrors.isEmpty())
        assertEquals(12.5, ScheduleEditorViewModel.parseAmount("12,5")!!, 0.0)
        assertEquals(12.5, ScheduleEditorViewModel.parseAmount(" 12.5 ")!!, 0.0)
    }

    @Test
    fun weeklyWithoutWeekdays_andEndBeforeStart_areReported() = runTest {
        val vm = newEditor(repo())
        editing(vm)
        vm.setAmountText("5")
        vm.setKind(RecurrenceKind.WEEKLY)
        vm.toggleWeekday(DayOfWeek.FRIDAY) // deselect the preselected start weekday
        vm.setHasEndDate(true)
        vm.setEndDate(LocalDate.of(2026, 11, 1))
        val s = editing(vm)
        assertEquals(setOf(EditorFieldError.NO_WEEKDAYS, EditorFieldError.END_BEFORE_START), s.visibleErrors)
        assertEquals(PreviewUi.Invalid, s.preview)
    }

    @Test
    fun preview_showsNextFiveFromEngine_andCreateSaves() = runTest {
        val repo = repo()
        val vm = newEditor(repo)
        editing(vm)
        vm.setName("  Nová  ")
        vm.setAmountText("12,5")
        vm.changeInterval(+1) // every 2 days from 20 Nov
        vm.setTime(LocalTime.of(21, 0))
        val preview = editing(vm).preview as PreviewUi.Items
        assertEquals(listOf(20, 22, 24, 26, 28).map { LocalDate.of(2026, 11, it) }, preview.items.map { it.date })
        assertTrue(preview.items.all { it.time == LocalTime.of(21, 0) })
        assertTrue(editing(vm).isDirty)

        vm.save()
        assertTrue(vm.closeRequested.value)
        val saved = repo.observeSchedules().first().single { it.id == "new-1" }
        assertEquals("Nová", saved.name)
        assertEquals(12.5, saved.amountMl, 0.0)
        assertEquals(Recurrence.Daily(2), saved.recurrence)
        assertEquals(1, saved.revision)
    }

    @Test
    fun oncePast_previewIsEmpty() = runTest {
        val vm = newEditor(repo(), date = "2026-11-01")
        editing(vm)
        vm.setAmountText("5")
        vm.setKind(RecurrenceKind.ONCE)
        assertEquals(PreviewUi.Empty, editing(vm).preview)
    }

    @Test
    fun edit_loadsDefinition_savesNewRevision() = runTest {
        val repo = repo()
        val vm = editEditor(repo, "demo-evening")
        val s = editing(vm)
        assertFalse(s.isNew)
        assertEquals("Večerní doplnění", s.form.name)
        assertEquals("5", s.form.amountText)
        assertEquals(RecurrenceKind.WEEKLY, s.form.kind)
        assertEquals(setOf(DayOfWeek.MONDAY, DayOfWeek.THURSDAY), s.form.weekdays)
        assertEquals(1, s.baseRevision)
        assertEquals(SyncState.SIMULATED_ACK, s.storedSync)
        assertFalse(s.isDirty)

        vm.toggleWeekday(DayOfWeek.SATURDAY)
        vm.save()
        assertTrue(vm.closeRequested.value)
        val saved = repo.observeSchedules().first().single { it.id == "demo-evening" }
        assertEquals(2, saved.revision)
        assertEquals(setOf(DayOfWeek.MONDAY, DayOfWeek.THURSDAY, DayOfWeek.SATURDAY), (saved.recurrence as Recurrence.Weekly).weekdays)
    }

    @Test
    fun concurrentChange_isConflict_thenReloadShowsStoredVersion() = runTest {
        val repo = repo()
        val vm = editEditor(repo, "demo-morning")
        editing(vm)
        vm.setAmountText("11")

        val stored = repo.observeSchedules().first().single { it.id == "demo-morning" }
        repo.updateSchedule(stored.id, stored.revision, stored.toDraft().copy(amountMl = 20.0))

        vm.save()
        val s = editing(vm)
        assertTrue(s.conflict)
        assertFalse(vm.closeRequested.value)
        assertEquals(20.0, repo.observeSchedules().first().single { it.id == "demo-morning" }.amountMl, 0.0)

        vm.reloadStored()
        val reloaded = editing(vm)
        assertFalse(reloaded.conflict)
        assertEquals("20", reloaded.form.amountText)
        assertEquals(2, reloaded.baseRevision)
    }

    @Test
    fun delete_removesSchedule_andCloses() = runTest {
        val repo = repo()
        val vm = editEditor(repo, "demo-weekend")
        editing(vm)
        vm.delete()
        assertTrue(vm.closeRequested.value)
        assertTrue(repo.observeSchedules().first().none { it.id == "demo-weekend" })
    }

    @Test
    fun unknownSchedule_isNotFound() = runTest {
        val vm = editEditor(repo(), "missing")
        backgroundScope.launch { vm.uiState.collect {} }
        assertEquals(ScheduleEditorUiState.NotFound, vm.uiState.first { it is ScheduleEditorUiState.NotFound })
    }

    @Test
    fun formSurvivesRecreation_viaSavedState() = runTest {
        val repo = repo()
        val handle = SavedStateHandle(mapOf(ScheduleEditorViewModel.ARG_DATE to "2026-11-20"))
        val first = newEditor(repo, handle = handle)
        editing(first)
        first.setName("Rozepsané")
        first.setKind(RecurrenceKind.WEEKLY)
        first.toggleWeekday(DayOfWeek.SUNDAY)

        val recreated = newEditor(repo, handle = handle)
        val s = editing(recreated)
        assertEquals("Rozepsané", s.form.name)
        assertEquals(RecurrenceKind.WEEKLY, s.form.kind)
        assertEquals(setOf(DayOfWeek.FRIDAY, DayOfWeek.SUNDAY), s.form.weekdays)
        assertTrue(s.isDirty)
    }

    @Test
    fun cancelWithoutSave_writesNothing() = runTest {
        val repo = repo()
        val before = repo.observeSchedules().first()
        val vm = editEditor(repo, "demo-morning")
        editing(vm)
        vm.setAmountText("99")
        // Screen closes without calling save(): repository must be unchanged.
        assertEquals(before, repo.observeSchedules().first())
    }

    @Test
    fun editorPrefill_isLossless() {
        assertEquals("12,25", cz.nowass.dispenser.ui.Format.mlInput(12.25))
        assertEquals("10", cz.nowass.dispenser.ui.Format.mlInput(10.0))
        assertEquals("1250", cz.nowass.dispenser.ui.Format.mlInput(1250.0))
    }

    // ---- Save enablement (pinned Save button) ----

    @Test
    fun newForm_saveDisabledUntilValid_withNeutralReason() = runTest {
        val vm = newEditor(repo())
        val fresh = editing(vm)
        assertFalse(fresh.canSave)
        assertEquals(setOf(EditorFieldError.AMOUNT_MISSING), fresh.blockingErrors)
        assertTrue(fresh.visibleErrors.isEmpty()) // not shouted at before touching the field

        vm.setAmountText("5")
        assertTrue(editing(vm).canSave)

        vm.setAmountText("") // touched and cleared -> real field error
        val cleared = editing(vm)
        assertFalse(cleared.canSave)
        assertEquals(setOf(EditorFieldError.AMOUNT_MISSING), cleared.visibleErrors)
    }

    @Test
    fun invalidRecurrence_disablesSave() = runTest {
        val vm = newEditor(repo())
        editing(vm)
        vm.setAmountText("5")
        vm.setKind(RecurrenceKind.WEEKLY)
        vm.toggleWeekday(DayOfWeek.FRIDAY)
        val s = editing(vm)
        assertFalse(s.canSave)
        assertEquals(setOf(EditorFieldError.NO_WEEKDAYS), s.blockingErrors)
    }

    @Test
    fun existingSchedule_saveEnabledOnlyAfterChange() = runTest {
        val vm = editEditor(repo(), "demo-morning")
        assertFalse(editing(vm).canSave) // unchanged: avoid a pointless new revision
        vm.setName("Ranní dávka – upraveno")
        assertTrue(editing(vm).canSave)
        vm.setAmountText("0")
        assertFalse(editing(vm).canSave)
    }
}
