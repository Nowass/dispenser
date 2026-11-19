package cz.nowass.dispenser.ui.schedule

import cz.nowass.dispenser.domain.ConnectionState

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

/**
 * How the editor finished; null = closed without writing. [UNCERTAIN] (DEVICE mode): the change may
 * have been applied and is verified at the next connection — the editor closes and no retry is offered.
 */
enum class EditorResult { SAVED, DELETED, UNCERTAIN }

enum class EditorFieldError {
    AMOUNT_MISSING,
    AMOUNT_INVALID,
    AMOUNT_NOT_POSITIVE,
    AMOUNT_TOO_PRECISE,
    AMOUNT_OUT_OF_RANGE,
    /** ONCE whose local date/time does not exist (DST spring-forward gap). */
    TIME_NONEXISTENT,
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
        /** DEVICE mode while not connected: saving and deleting need a live session (E12). */
        val needsConnection: Boolean = false,
        /** DEVICE-mode outcome of the last write that kept the editor open (nothing changed). */
        val notice: ScheduleWriteNotice? = null,
    ) : ScheduleEditorUiState
}

private data class EditorMeta(
    val loaded: Boolean = false,
    val notFound: Boolean = false,
    /** Form as loaded/initialized; "modified" means a semantic difference from it (see [semanticKey]). */
    val original: EditorForm? = null,
    val showErrors: Boolean = false,
    /** Amount field was edited at least once; enables the "missing amount" field error. */
    val amountTouched: Boolean = false,
    val revision: Int = NO_REVISION,
    val isSaving: Boolean = false,
    val conflict: Boolean = false,
    val deletedMeanwhile: Boolean = false,
    val notice: ScheduleWriteNotice? = null,
)

private const val NO_REVISION = -1

class ScheduleEditorViewModel(
    private val repository: DispenserRepository,
    private val savedState: SavedStateHandle,
) : ViewModel() {

    private val scheduleId: String? = savedState.get<String>(ARG_SCHEDULE_ID)?.takeIf { it.isNotBlank() }
    private val form = MutableStateFlow<EditorForm?>(null)
    private val meta = MutableStateFlow(EditorMeta())
    private val _closeResult = MutableStateFlow<EditorResult?>(null)

    /** Set once a save/delete succeeded; the screen should navigate back and confirm it. */
    val closeResult: StateFlow<EditorResult?> = _closeResult.asStateFlow()

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
                val loaded = formFrom(schedule)
                commit(loaded, EditorMeta(loaded = true, revision = schedule.revision, original = loaded))
            }
        } else {
            val device = repository.observeDevice().first()
            val today = device.now.atZone(device.zone).toLocalDate()
            val start = parseDate(savedState.get<String>(ARG_DATE)) ?: today
            val initial = EditorForm(startDate = start, weekdays = setOf(start.dayOfWeek))
            commit(initial, EditorMeta(loaded = true, original = initial))
        }
    }

    // ---- Field updates (each persists the form; "modified" is derived semantically) ----

    fun setName(value: String) = edit { it.copy(name = value) }
    fun setAmountText(value: String) {
        val current = form.value ?: return
        if (meta.value.isSaving) return
        commit(current.copy(amountText = value), meta.value.copy(amountTouched = true))
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
        // While a save is in flight the form is frozen: an edit now would be silently lost.
        if (meta.value.isSaving) return
        commit(transform(current), meta.value)
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
        meta.update { it.copy(isSaving = true, conflict = false, notice = null) }
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
        meta.update { it.copy(isSaving = true, conflict = false, notice = null) }
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
                val loaded = formFrom(schedule)
                commit(loaded, EditorMeta(loaded = true, revision = schedule.revision, original = loaded))
            }
        }
    }

    private fun handleWrite(result: ScheduleWriteResult) {
        when (result) {
            is ScheduleWriteResult.Success -> {
                val saved = form.value ?: return
                commit(saved, meta.value.copy(isSaving = false, original = saved))
                _closeResult.value = if (result.schedule == null) EditorResult.DELETED else EditorResult.SAVED
            }
            ScheduleWriteResult.Conflict -> meta.update { it.copy(isSaving = false, conflict = true) }
            ScheduleWriteResult.NotFound -> meta.update { it.copy(isSaving = false, deletedMeanwhile = true) }
            is ScheduleWriteResult.Invalid -> meta.update { it.copy(isSaving = false, showErrors = true) }
            // DEVICE mode: may have been applied; verified by read-back at the next connection. Not a
            // failure, and the user is not invited to repeat it: the editor closes with that message.
            ScheduleWriteResult.Uncertain -> {
                meta.update { it.copy(isSaving = false) }
                _closeResult.value = EditorResult.UNCERTAIN
            }
            // DEVICE mode, nothing changed on the device: the edit stays open with the reason.
            ScheduleWriteResult.NotSent, ScheduleWriteResult.AwaitingVerification,
            is ScheduleWriteResult.DeviceRejected, is ScheduleWriteResult.DeviceLimit ->
                meta.update { it.copy(isSaving = false, notice = ScheduleWriteNotice.of(result)) }
        }
    }

    // ---- State building ----

    private fun buildEditing(f: EditorForm, m: EditorMeta, device: DeviceStatus, sync: SyncState?): ScheduleEditorUiState.Editing {
        val (draft, errors) = f.toDraft()
        val dirty = m.original != null && f.semanticKey() != m.original.semanticKey()
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
            canSave = !m.isSaving && !m.deletedMeanwhile && errors.isEmpty() && (scheduleId == null || dirty) && !needsConnection(device),
            preview = preview,
            zoneId = DEFAULT_ZONE.id,
            baseRevision = m.revision.takeIf { it != NO_REVISION },
            storedSync = if (scheduleId == null) null else sync,
            isDirty = dirty,
            isSaving = m.isSaving,
            conflict = m.conflict,
            deletedMeanwhile = m.deletedMeanwhile,
            needsConnection = needsConnection(device),
            notice = m.notice,
        )
    }

    private fun needsConnection(device: DeviceStatus): Boolean =
        repository.writesRequireConnection && device.connection != ConnectionState.CONNECTED

    // ---- Persistence (rotation / process death) ----

    private fun commit(f: EditorForm, m: EditorMeta) {
        form.value = f
        meta.value = m
        writeForm(PREFIX_FORM, f)
        m.original?.let { writeForm(PREFIX_ORIGINAL, it) }
        savedState[K_SHOW_ERRORS] = m.showErrors
        savedState[K_AMOUNT_TOUCHED] = m.amountTouched
        savedState[K_REVISION] = m.revision
        savedState[K_LOADED] = m.loaded
    }

    private fun writeForm(prefix: String, f: EditorForm) {
        savedState[prefix + "name"] = f.name
        savedState[prefix + "amount"] = f.amountText
        savedState[prefix + "start"] = f.startDate.toString()
        savedState[prefix + "time"] = f.time.toString()
        savedState[prefix + "kind"] = f.kind.name
        savedState[prefix + "interval"] = f.interval
        savedState[prefix + "weekdays"] = f.weekdays.map { it.value }.sorted().toIntArray()
        savedState[prefix + "has_end"] = f.hasEndDate
        savedState[prefix + "end"] = f.endDate?.toString()
        savedState[prefix + "enabled"] = f.enabled
    }

    private fun readForm(prefix: String): EditorForm? {
        val start = parseDate(savedState[prefix + "start"]) ?: return null
        return EditorForm(
            name = savedState[prefix + "name"] ?: "",
            amountText = savedState[prefix + "amount"] ?: "",
            startDate = start,
            time = savedState.get<String>(prefix + "time")?.let { runCatching { LocalTime.parse(it) }.getOrNull() } ?: EditorForm.DEFAULT_TIME,
            kind = RecurrenceKind.entries.firstOrNull { it.name == savedState.get<String>(prefix + "kind") } ?: RecurrenceKind.DAILY,
            interval = savedState[prefix + "interval"] ?: 1,
            weekdays = savedState.get<IntArray>(prefix + "weekdays")?.map(DayOfWeek::of)?.toSet() ?: emptySet(),
            hasEndDate = savedState[prefix + "has_end"] ?: false,
            endDate = parseDate(savedState[prefix + "end"]),
            enabled = savedState[prefix + "enabled"] ?: true,
        )
    }

    private fun restore(): Pair<EditorForm, EditorMeta>? {
        if (savedState.get<Boolean>(K_LOADED) != true) return null
        val f = readForm(PREFIX_FORM) ?: return null
        val m = EditorMeta(
            loaded = true,
            original = readForm(PREFIX_ORIGINAL) ?: f,
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
            EditorFieldError.AMOUNT_TOO_PRECISE,
            EditorFieldError.AMOUNT_OUT_OF_RANGE,
            EditorFieldError.TIME_NONEXISTENT,
            EditorFieldError.NO_WEEKDAYS,
            EditorFieldError.END_BEFORE_START,
            EditorFieldError.INTERVAL_TOO_SMALL,
        )

        private const val PREFIX_FORM = "editor_form_"
        private const val PREFIX_ORIGINAL = "editor_original_"
        private const val K_SHOW_ERRORS = "editor_show_errors"
        private const val K_AMOUNT_TOUCHED = "editor_amount_touched"
        private const val K_REVISION = "editor_revision"
        private const val K_LOADED = "editor_loaded"

        fun factory(repository: DispenserRepository): ViewModelProvider.Factory = viewModelFactory {
            initializer { ScheduleEditorViewModel(repository, createSavedStateHandle()) }
        }

        internal fun parseDate(value: String?): LocalDate? =
            try { value?.let(LocalDate::parse) } catch (_: DateTimeParseException) { null }

        /** See [cz.nowass.dispenser.ui.AmountInput.parseMl]. */
        internal fun parseAmount(text: String): Double? = cz.nowass.dispenser.ui.AmountInput.parseMl(text)

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

        /**
         * Values that define the schedule, normalized so that equivalent input compares equal
         * ("10" vs "10,0", trimmed name, fields irrelevant for the chosen recurrence ignored).
         */
        internal fun EditorForm.semanticKey(): List<Any?> = listOf(
            name.trim(),
            parseAmount(amountText) ?: amountText.trim(),
            startDate,
            time,
            kind,
            interval.takeIf { kind != RecurrenceKind.ONCE },
            weekdays.takeIf { kind == RecurrenceKind.WEEKLY },
            endDate.takeIf { hasEndDate && kind != RecurrenceKind.ONCE },
            enabled,
        )

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
                    ValidationError.AMOUNT_TOO_PRECISE -> errors += EditorFieldError.AMOUNT_TOO_PRECISE
                    ValidationError.AMOUNT_OUT_OF_RANGE -> errors += EditorFieldError.AMOUNT_OUT_OF_RANGE
                    ValidationError.TIME_NONEXISTENT -> errors += EditorFieldError.TIME_NONEXISTENT
                    ValidationError.INTERVAL_TOO_SMALL -> errors += EditorFieldError.INTERVAL_TOO_SMALL
                    ValidationError.NO_WEEKDAYS -> errors += EditorFieldError.NO_WEEKDAYS
                    ValidationError.END_BEFORE_START -> errors += EditorFieldError.END_BEFORE_START
                }
            }
            return (if (amount == null) null else draft) to errors
        }
    }
}
