package cz.nowass.dispenser.data.local.simulation

import androidx.room.ColumnInfo
import androidx.room.Entity
import androidx.room.Index
import androidx.room.PrimaryKey

/** Single-row table (id = 1) holding the active simulation run; absent when inactive. */
@Entity(tableName = "sim_state")
data class SimStateEntity(
    @PrimaryKey val id: Int = SINGLE_ROW_ID,
    @ColumnInfo(name = "run_id") val runId: String,
    @ColumnInfo(name = "started_at_ms") val startedAtMs: Long,
    @ColumnInfo(name = "clock_ms") val clockMs: Long,
    @ColumnInfo(name = "capacity_ul") val capacityUl: Long,
    @ColumnInfo(name = "remaining_ul") val remainingUl: Long,
    @ColumnInfo(name = "reservoir_updated_ms") val reservoirUpdatedMs: Long,
    val blocked: Boolean,
    @ColumnInfo(name = "low_warning_latched") val lowWarningLatched: Boolean,
    val readiness: String,
    @ColumnInfo(name = "next_seq") val nextSeq: Long,
) {
    companion object { const val SINGLE_ROW_ID = 1 }
}

/** Occurrences already processed in the current run (duplicate guard). */
@Entity(tableName = "sim_processed")
data class SimProcessedEntity(
    @PrimaryKey @ColumnInfo(name = "occurrence_id") val occurrenceId: String,
    @ColumnInfo(name = "local_date") val localDate: String,
)

/** Immutable simulated history event (origin is always SIMULATION in this database). */
@Entity(tableName = "sim_events", indices = [Index("seq")])
data class SimEventEntity(
    @PrimaryKey val id: String,
    val seq: Long,
    @ColumnInfo(name = "time_ms") val timeMs: Long,
    val type: String,
    val origin: String,
    val reason: String,
    @ColumnInfo(name = "schedule_id") val scheduleId: String?,
    @ColumnInfo(name = "schedule_name") val scheduleName: String?,
    @ColumnInfo(name = "occurrence_id") val occurrenceId: String?,
    @ColumnInfo(name = "requested_ul") val requestedUl: Long?,
    @ColumnInfo(name = "dispensed_ul") val dispensedUl: Long,
    @ColumnInfo(name = "reservoir_before_ul") val reservoirBeforeUl: Long,
    @ColumnInfo(name = "reservoir_after_ul") val reservoirAfterUl: Long,
    @ColumnInfo(name = "capacity_ul") val capacityUl: Long,
)
