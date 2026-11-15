package cz.nowass.dispenser

import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertIsEnabled
import androidx.compose.ui.test.assertIsNotEnabled
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onAllNodesWithText
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import cz.nowass.dispenser.domain.RecurrenceKind
import cz.nowass.dispenser.domain.SyncState
import androidx.test.espresso.Espresso
import androidx.compose.ui.test.assertCountEquals
import org.junit.Assert.assertFalse
import cz.nowass.dispenser.ui.schedule.EDITOR_SAVE_TEST_TAG
import cz.nowass.dispenser.ui.schedule.EditorFieldError
import cz.nowass.dispenser.ui.schedule.EditorForm
import cz.nowass.dispenser.ui.schedule.PreviewItem
import cz.nowass.dispenser.ui.schedule.PreviewUi
import cz.nowass.dispenser.ui.schedule.ScheduleEditorActions
import cz.nowass.dispenser.ui.schedule.ScheduleEditorScreen
import cz.nowass.dispenser.ui.schedule.ScheduleEditorUiState
import cz.nowass.dispenser.ui.theme.DispenserTheme
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import java.time.DayOfWeek
import java.time.LocalDate
import java.time.LocalTime

/** State-driven tests of the editor screen (no repository, no navigation). */
@RunWith(AndroidJUnit4::class)
class ScheduleEditorScreenTest {

    @get:Rule
    val rule = createComposeRule()

    private val ctx = InstrumentationRegistry.getInstrumentation().targetContext
    private fun str(id: Int) = ctx.getString(id)

    private fun state(
        amount: String = "10",
        errors: Set<EditorFieldError> = emptySet(),
        dirty: Boolean = true,
        isNew: Boolean = false,
        saving: Boolean = false,
        sync: SyncState? = null,
        time: LocalTime = LocalTime.of(8, 0),
        kind: RecurrenceKind = RecurrenceKind.WEEKLY,
    ) = ScheduleEditorUiState.Editing(
        isNew = isNew,
        form = EditorForm(
            name = "Večerní doplnění",
            amountText = amount,
            startDate = LocalDate.of(2026, 11, 16),
            time = time,
            kind = kind,
            weekdays = setOf(DayOfWeek.MONDAY, DayOfWeek.THURSDAY),
        ),
        visibleErrors = errors,
        blockingErrors = errors,
        canSave = !saving && errors.isEmpty() && (isNew || dirty),
        preview = if (errors.isEmpty()) {
            PreviewUi.Items((0 until 5).map { PreviewItem("p$it", LocalDate.of(2026, 11, 16).plusDays(it * 3L), LocalTime.of(8, 0)) })
        } else {
            PreviewUi.Invalid
        },
        zoneId = "Europe/Prague",
        baseRevision = 1,
        storedSync = sync,
        isDirty = dirty,
        isSaving = saving,
        conflict = false,
        deletedMeanwhile = false,
    )

    private fun show(s: ScheduleEditorUiState.Editing, onClose: () -> Unit = {}, onSave: () -> Unit = {}) {
        rule.setContent {
            DispenserTheme {
                ScheduleEditorScreen(state = s, isDemo = true, onClose = onClose, actions = ScheduleEditorActions(onSave = onSave))
            }
        }
    }

    @Test
    fun saveButton_isVisibleWithoutScrolling_andTopSaveIsGone() {
        show(state())
        rule.onNodeWithTag(EDITOR_SAVE_TEST_TAG).assertIsDisplayed().assertIsEnabled()
        // Only one Save action exists (the pinned bottom button).
        assertEquals(1, rule.onAllNodesWithText(str(R.string.editor_save)).fetchSemanticsNodes().size)
    }

    @Test
    fun invalidForm_disablesSave_andShowsReasonAndFieldError() {
        show(state(amount = "abc", errors = setOf(EditorFieldError.AMOUNT_INVALID)))
        rule.onNodeWithTag(EDITOR_SAVE_TEST_TAG).assertIsNotEnabled()
        // Message appears both under the field and as the reason above Save.
        assertEquals(2, rule.onAllNodesWithText(str(R.string.editor_err_amount_invalid)).fetchSemanticsNodes().size)
    }

    @Test
    fun validForm_saveInvokesCallback() {
        var saved = 0
        show(state(), onSave = { saved++ })
        rule.onNodeWithTag(EDITOR_SAVE_TEST_TAG).performClick()
        assertEquals(1, saved)
    }

    @Test
    fun unchangedExistingSchedule_saveDisabledWithHint() {
        show(state(dirty = false))
        rule.onNodeWithTag(EDITOR_SAVE_TEST_TAG).assertIsNotEnabled()
        rule.onNodeWithText(str(R.string.editor_save_disabled_unchanged)).assertIsDisplayed()
    }

    @Test
    fun closeWithUnsavedChanges_asksBeforeDiscarding() {
        var closed = false
        show(state(dirty = true), onClose = { closed = true })
        rule.onNodeWithContentDescription(str(R.string.editor_close)).performClick()
        rule.onNodeWithText(str(R.string.editor_discard_title)).assertIsDisplayed()
        rule.onNodeWithText(str(R.string.editor_discard_keep)).performClick()
        assertTrue(!closed)
        rule.onNodeWithContentDescription(str(R.string.editor_close)).performClick()
        rule.onNodeWithText(str(R.string.editor_discard_confirm)).performClick()
        assertTrue(closed)
    }

    @Test
    fun closeWithoutChanges_closesImmediately() {
        var closed = false
        show(state(dirty = false), onClose = { closed = true })
        rule.onNodeWithContentDescription(str(R.string.editor_close)).performClick()
        assertTrue(closed)
    }

    @Test
    fun whileSaving_closeIsDisabled_andBackIsIgnored() {
        var closed = false
        show(state(saving = true), onClose = { closed = true })
        rule.onNodeWithContentDescription(str(R.string.editor_close)).assertIsNotEnabled()
        rule.onNodeWithTag(EDITOR_SAVE_TEST_TAG).assertIsNotEnabled()
        Espresso.pressBack()
        assertFalse(closed)
        assertEquals(0, rule.onAllNodesWithText(str(R.string.editor_discard_title)).fetchSemanticsNodes().size)
    }

    @Test
    fun statusTags_neverContradict() {
        show(state(dirty = true, sync = SyncState.PENDING))
        rule.onNodeWithText(str(R.string.editor_unsaved)).assertIsDisplayed()
        rule.onNodeWithText(ctx.getString(R.string.editor_saved_version_phone, 1)).assertIsDisplayed()
        rule.onAllNodesWithText(str(R.string.sync_pending)).assertCountEquals(0)
    }

    @Test
    fun unchangedSchedule_showsSavedOnPhone() {
        show(state(dirty = false, sync = SyncState.PENDING))
        rule.onNodeWithText(str(R.string.sync_pending)).assertIsDisplayed()
        rule.onAllNodesWithText(str(R.string.editor_unsaved)).assertCountEquals(0)
    }

    @Test
    fun onceInDstGap_showsTimeError_andDisablesSave() {
        show(state(errors = setOf(EditorFieldError.TIME_NONEXISTENT), kind = RecurrenceKind.ONCE, time = LocalTime.of(2, 15)))
        rule.onNodeWithTag(EDITOR_SAVE_TEST_TAG).assertIsNotEnabled()
        assertTrue(rule.onAllNodesWithText(str(R.string.editor_err_time_nonexistent)).fetchSemanticsNodes().isNotEmpty())
    }
}
