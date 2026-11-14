package cz.nowass.dispenser.ui.schedule

import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.createSavedStateHandle
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import cz.nowass.dispenser.data.DispenserRepository
import cz.nowass.dispenser.data.ScheduleWriteResult
import cz.nowass.dispenser.domain.DEFAULT_ZONE
import cz.nowass.dispenser.domain.DeviceStatus
import cz.nowass.dispenser.domain.RecurrenceEngine
import cz.nowass.dispenser.domain.RecurrenceKind
import cz.nowass.dispenser.domain.Schedule
import cz.nowass.dispenser.domain.ScheduleDraft
import cz.nowass.dispenser.domain.ScheduleValidator
import cz.nowass.dispenser.domain.SyncState
import cz.nowass.dispenser.domain.ValidationError
import cz.nowass.dispenser.domain.toDraft
import cz.nowass.dispenser.domain.toSchedule
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import java.time.DayOfWeek
import java.time.LocalDate
import java.time.LocalTime
import java.time.format.DateTimeParseException

/** Raw, user-editable form values (amount kept as text so typing is lossless). */
data class EditorForm(
    val name: String = "",
    val amountText: String = "",
    val startDate: LocalDate,
    val time: LocalTime = DEFAULT_TIME,
    val kind: RecurrenceKind = RecurrenceKind.DAILY,
    val interval: Int = 1,
    val weekdays: Set<DayOfWeek> = emptySet(),
    val hasEndDate: Boolean = false,
    val endDate: LocalDate? = null,
    val enabled: Boolean = true,
) {
    companion object {
        /** UI default for a new schedule only; not a product rule. */
        val DEFAULT_TIME: LocalTime = LocalTime.of(8, 0)
    }
}

enum class EditorFieldError {
    AMOUNT_MISSING,
    AMOUNT_INVALID,
    AMOUNT_NOT_POSITIVE,
    INTERVAL_TOO_SMALL,
    NO_WEEKDAYS,
    END_BEFORE_START,
}

sealed interface PreviewUi {
    /** Form has errors; no preview. */
    data object Invalid : PreviewUi
    /** Valid but no future occurrence (e.g. ONCE in the past, end date passed). */
    data object Empty : PreviewUi
    data class Items(val items: List<PreviewItem>) : PreviewUi
}

data class PreviewItem(val id: String, val date: LocalDate, val time: LocalTime)

sealed interface ScheduleEditorUiState {
    data object Loading : ScheduleEditorUiState
    data object NotFound : ScheduleEditorUiState

    data class Editing(
        val isNew: Boolean,
        val form: EditorForm,
        /** Errors to display now (some only appear after a save attempt). */
        val visibleErrors: Set<EditorFieldError>,
        /** All current errors, shown as the reason next to the disabled Save button. */
        val blockingErrors: Set<EditorFieldError>,
        /** Valid, not saving, and (for an existing schedule) actually changed. */
        val canSave: Boolean,
        val preview: PreviewUi,
        val zoneId: String,
        /** Revision the edit is based on; null for a new schedule. */
        val baseRevision: Int?,
        /** Confirmation state of the stored schedule; null for a new one. */
        val storedSync: SyncState?,
        val isDirty: Boolean,
        val isSaving: Boolean,
        /** Stored schedule changed since editing started (revision mismatch). */
        val conflict: Boolean,
        /** Stored schedule disappeared while editing. */
        val deletedMeanwhile: Boolean,
    ) : ScheduleEditorUiState
}

private data class EditorMeta(
    val loaded: Boolean = false,
    val notFound: Boolean = false,
    val dirty: Boolean = false,
    val showErrors: Boolean = false,
    /** Amount field was edited at least once; enables the "missing amount" field error. */
    val amountTouched: Boolean = false,
    val revision: Int = NO_REVISION,
    val isSaving: Boolean = false,
    val conflict: Boolean = false,
    val deletedMeanwhile: Boolean = false,
)

private const val NO_REVISION = -1

class ScheduleEditorViewModel(
    private val repository: DispenserRepository,
    private val savedState: SavedStateHandle,
) : ViewModel() {

    private val scheduleId: String? = savedState.get<String>(ARG_SCHEDULE_ID)?.takeIf { it.isNotBlank() }
    private val form = MutableStateFlow<EditorForm?>(null)
    private val meta = MutableStateFlow(EditorMeta())
    private val _closeRequested = MutableStateFlow(false)

    /** True once a save/delete succeeded; the screen should navigate back. */
    val closeRequested: StateFlow<Boolean> = _closeRequested.asStateFlow()

    val uiState: StateFlow<ScheduleEditorUiState> = combine(
        form,
        meta,
        repository.observeDevice(),
        repository.observeScheduleSync(),
    ) { f, m, device, sync ->
        when {
            m.notFound -> ScheduleEditorUiState.NotFound
            f == null || !m.loaded -> ScheduleEditorUiState.Loading
            else -> buildEditing(f, m, device, sync[scheduleId])
        }
    }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), ScheduleEditorUiState.Loading)

    init {
        val restored = restore()
        if (restored != null) {
            form.value = restored.first
            meta.value = restored.second
        } else {
            viewModelScope.launch { loadInitial() }
        }
    }

    private suspend fun loadInitial() {
        if (scheduleId != null) {
            val schedule = repository.observeSchedules().first().firstOrNull { it.id == scheduleId }
            if (schedule == null) {
                meta.update { it.copy(notFound = true) }
            } else {
                commit(formFrom(schedule), EditorMeta(loaded = true, revision = schedule.revision))
            }
        } else {
            val device = repository.observeDevice().first()
            val today = device.now.atZone(device.zone).toLocalDate()
            val start = parseDate(savedState.get<String>(ARG_DATE)) ?: today
            commit(EditorForm(startDate = start, weekdays = setOf(start.dayOfWeek)), EditorMeta(loaded = true))
        }
    }

    // ---- Field updates (each marks the form dirty and persists it) ----

    fun setName(value: String) = edit { it.copy(name = value) }
    fun setAmountText(value: String) {
        val current = form.value ?: return
        commit(current.copy(amountText = value), meta.value.copy(dirty = true, amountTouched = true))
    }
    fun setStartDate(value: LocalDate) = edit { it.copy(startDate = value) }
    fun setTime(value: LocalTime) = edit { it.copy(time = value) }
    fun setEnabled(value: Boolean) = edit { it.copy(enabled = value) }
    fun setEndDate(value: LocalDate) = edit { it.copy(endDate = value) }

    fun setKind(value: RecurrenceKind) = edit {
        // Switching to WEEKLY with nothing selected preselects the start weekday (convenience).
        val weekdays = if (value == RecurrenceKind.WEEKLY && it.weekdays.isEmpty()) setOf(it.startDate.dayOfWeek) else it.weekdays
        it.copy(kind = value, weekdays = weekdays)
    }

    fun changeInterval(delta: Int) = edit { it.copy(interval = (it.interval + delta).coerceAtLeast(1)) }

    fun toggleWeekday(day: DayOfWeek) = edit {
        it.copy(weekdays = if (day in it.weekdays) it.weekdays - day else it.weekdays + day)
    }

    fun setHasEndDate(value: Boolean) = edit { it.copy(hasEndDate = value, endDate = it.endDate ?: it.startDate) }

    private fun edit(transform: (EditorForm) -> EditorForm) {
        val current = form.value ?: return
        commit(transform(current), meta.value.copy(dirty = true))
    }

    // ---- Commands ----

    fun save() {
        val f = form.value ?: return
        if (meta.value.isSaving) return
        val (draft, errors) = f.toDraft()
        if (draft == null || errors.isNotEmpty()) {
            commit(f, meta.value.copy(showErrors = true))
            return
        }
        meta.update { it.copy(isSaving = true, conflict = false) }
        viewModelScope.launch {
            val result = if (scheduleId == null) {
                repository.createSchedule(draft)
            } else {
                repository.updateSchedule(scheduleId, meta.value.revision, draft)
            }
            handleWrite(result)
        }
    }

    fun delete() {
        val id = scheduleId ?: return
        if (meta.value.isSaving) return
        meta.update { it.copy(isSaving = true, conflict = false) }
        viewModelScope.launch { handleWrite(repository.deleteSchedule(id, meta.value.revision)) }
    }

    /** After a conflict: discard local edits and load the currently stored version. */
    fun reloadStored() {
        val id = scheduleId ?: return
        viewModelScope.launch {
            val schedule = repository.observeSchedules().first().firstOrNull { it.id == id }
            if (schedule == null) {
                meta.update { it.copy(deletedMeanwhile = true, conflict = false) }
            } else {
                commit(formFrom(schedule), EditorMeta(loaded = true, revision = schedule.revision))
            }
        }
    }

    private fun handleWrite(result: ScheduleWriteResult) {
        when (result) {
            is ScheduleWriteResult.Success -> {
                commit(form.value ?: return, meta.value.copy(isSaving = false, dirty = false))
                _closeRequested.value = true
            }
            ScheduleWriteResult.Conflict -> meta.update { it.copy(isSaving = false, conflict = true) }
            ScheduleWriteResult.NotFound -> meta.update { it.copy(isSaving = false, deletedMeanwhile = true) }
            is ScheduleWriteResult.Invalid -> meta.update { it.copy(isSaving = false, showErrors = true) }
        }
    }

    // ---- State building ----

    private fun buildEditing(f: EditorForm, m: EditorMeta, device: DeviceStatus, sync: SyncState?): ScheduleEditorUiState.Editing {
        val (draft, errors) = f.toDraft()
        val preview = if (draft == null || errors.isNotEmpty()) {
            PreviewUi.Invalid
        } else {
            val schedule = draft.toSchedule(id = scheduleId ?: PREVIEW_ID, revision = 0)
            val next = RecurrenceEngine.nextOccurrences(schedule, device.now, PREVIEW_COUNT)
            if (next.isEmpty()) PreviewUi.Empty else PreviewUi.Items(
                next.map {
                    val local = it.scheduledAt.atZone(schedule.zone)
                    PreviewItem(it.id, local.toLocalDate(), local.toLocalTime())
                },
            )
        }
        return ScheduleEditorUiState.Editing(
            isNew = scheduleId == null,
            form = f,
            visibleErrors = if (m.showErrors) errors else errors.filterTo(mutableSetOf()) {
                it in LIVE_ERRORS || (it == EditorFieldError.AMOUNT_MISSING && m.amountTouched)
            },
            blockingErrors = errors,
            canSave = !m.isSaving && !m.deletedMeanwhile && errors.isEmpty() && (scheduleId == null || m.dirty),
            preview = preview,
            zoneId = DEFAULT_ZONE.id,
            baseRevision = m.revision.takeIf { it != NO_REVISION },
            storedSync = if (scheduleId == null) null else sync,
            isDirty = m.dirty,
            isSaving = m.isSaving,
            conflict = m.conflict,
            deletedMeanwhile = m.deletedMeanwhile,
        )
    }

    // ---- Persistence (rotation / process death) ----

    private fun commit(f: EditorForm, m: EditorMeta) {
        form.value = f
        meta.value = m
        savedState[K_NAME] = f.name
        savedState[K_AMOUNT] = f.amountText
        savedState[K_START] = f.startDate.toString()
        savedState[K_TIME] = f.time.toString()
        savedState[K_KIND] = f.kind.name
        savedState[K_INTERVAL] = f.interval
        savedState[K_WEEKDAYS] = f.weekdays.map { it.value }.sorted().toIntArray()
        savedState[K_HAS_END] = f.hasEndDate
        savedState[K_END] = f.endDate?.toString()
        savedState[K_ENABLED] = f.enabled
        savedState[K_DIRTY] = m.dirty
        savedState[K_SHOW_ERRORS] = m.showErrors
        savedState[K_AMOUNT_TOUCHED] = m.amountTouched
        savedState[K_REVISION] = m.revision
        savedState[K_LOADED] = m.loaded
    }

    private fun restore(): Pair<EditorForm, EditorMeta>? {
        if (savedState.get<Boolean>(K_LOADED) != true) return null
        val start = parseDate(savedState[K_START]) ?: return null
        val f = EditorForm(
            name = savedState[K_NAME] ?: "",
            amountText = savedState[K_AMOUNT] ?: "",
            startDate = start,
            time = savedState.get<String>(K_TIME)?.let { runCatching { LocalTime.parse(it) }.getOrNull() } ?: EditorForm.DEFAULT_TIME,
            kind = RecurrenceKind.entries.firstOrNull { it.name == savedState.get<String>(K_KIND) } ?: RecurrenceKind.DAILY,
            interval = savedState[K_INTERVAL] ?: 1,
            weekdays = savedState.get<IntArray>(K_WEEKDAYS)?.map(DayOfWeek::of)?.toSet() ?: emptySet(),
            hasEndDate = savedState[K_HAS_END] ?: false,
            endDate = parseDate(savedState[K_END]),
            enabled = savedState[K_ENABLED] ?: true,
        )
        val m = EditorMeta(
            loaded = true,
            dirty = savedState[K_DIRTY] ?: false,
            showErrors = savedState[K_SHOW_ERRORS] ?: false,
            amountTouched = savedState[K_AMOUNT_TOUCHED] ?: false,
            revision = savedState[K_REVISION] ?: NO_REVISION,
        )
        return f to m
    }

    companion object {
        const val ARG_SCHEDULE_ID = "scheduleId"
        const val ARG_DATE = "date"
        const val PREVIEW_COUNT = 5
        private const val PREVIEW_ID = "preview"

        /** Errors shown immediately; AMOUNT_MISSING waits until the amount field is touched (or a save attempt). */
        private val LIVE_ERRORS = setOf(
            EditorFieldError.AMOUNT_INVALID,
            EditorFieldError.AMOUNT_NOT_POSITIVE,
            EditorFieldError.NO_WEEKDAYS,
            EditorFieldError.END_BEFORE_START,
            EditorFieldError.INTERVAL_TOO_SMALL,
        )

        private const val K_NAME = "editor_name"
        private const val K_AMOUNT = "editor_amount"
        private const val K_START = "editor_start"
        private const val K_TIME = "editor_time"
        private const val K_KIND = "editor_kind"
        private const val K_INTERVAL = "editor_interval"
        private const val K_WEEKDAYS = "editor_weekdays"
        private const val K_HAS_END = "editor_has_end"
        private const val K_END = "editor_end"
        private const val K_ENABLED = "editor_enabled"
        private const val K_DIRTY = "editor_dirty"
        private const val K_SHOW_ERRORS = "editor_show_errors"
        private const val K_AMOUNT_TOUCHED = "editor_amount_touched"
        private const val K_REVISION = "editor_revision"
        private const val K_LOADED = "editor_loaded"

        fun factory(repository: DispenserRepository): ViewModelProvider.Factory = viewModelFactory {
            initializer { ScheduleEditorViewModel(repository, createSavedStateHandle()) }
        }

        internal fun parseDate(value: String?): LocalDate? =
            try { value?.let(LocalDate::parse) } catch (_: DateTimeParseException) { null }

        /** Accepts "12", "12.5" and Czech "12,5". Returns null for unparsable text. */
        internal fun parseAmount(text: String): Double? =
            text.trim().replace(',', '.').takeIf { it.isNotEmpty() }?.toDoubleOrNull()

        internal fun formFrom(s: Schedule): EditorForm {
            val d = s.toDraft()
            return EditorForm(
                name = d.name,
                amountText = cz.nowass.dispenser.ui.Format.mlInput(d.amountMl),
                startDate = d.startDate,
                time = d.localTime,
                kind = d.kind,
                interval = d.interval,
                weekdays = d.weekdays,
                hasEndDate = d.endDate != null,
                endDate = d.endDate,
                enabled = d.enabled,
            )
        }

        /** Converts the form to a draft; returns all field errors (parse + domain validation). */
        internal fun EditorForm.toDraft(): Pair<ScheduleDraft?, Set<EditorFieldError>> {
            val errors = mutableSetOf<EditorFieldError>()
            val amount = when {
                amountText.isBlank() -> { errors += EditorFieldError.AMOUNT_MISSING; null }
                else -> parseAmount(amountText).also { if (it == null) errors += EditorFieldError.AMOUNT_INVALID }
            }
            val draft = ScheduleDraft(
                name = name,
                amountMl = amount ?: Double.NaN,
                startDate = startDate,
                localTime = time,
                kind = kind,
                interval = interval,
                weekdays = if (kind == RecurrenceKind.WEEKLY) weekdays else emptySet(),
                endDate = if (hasEndDate && kind != RecurrenceKind.ONCE) endDate else null,
                enabled = enabled,
            )
            ScheduleValidator.validate(draft).forEach { e ->
                when (e) {
                    ValidationError.AMOUNT_NOT_FINITE -> if (amount != null) errors += EditorFieldError.AMOUNT_INVALID
                    ValidationError.AMOUNT_NOT_POSITIVE -> errors += EditorFieldError.AMOUNT_NOT_POSITIVE
                    ValidationError.INTERVAL_TOO_SMALL -> errors += EditorFieldError.INTERVAL_TOO_SMALL
                    ValidationError.NO_WEEKDAYS -> errors += EditorFieldError.NO_WEEKDAYS
                    ValidationError.END_BEFORE_START -> errors += EditorFieldError.END_BEFORE_START
                }
            }
            return (if (amount == null) null else draft) to errors
        }
    }
}
