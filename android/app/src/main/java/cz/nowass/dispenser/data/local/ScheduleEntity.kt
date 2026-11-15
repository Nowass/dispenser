package cz.nowass.dispenser.data.local

import androidx.room.ColumnInfo
import androidx.room.Entity
import androidx.room.PrimaryKey

/**
 * Room row for one schedule. Plain columns only (no TypeConverters); conversion to the
 * domain [cz.nowass.dispenser.domain.Schedule] lives in [ScheduleEntityMapper].
 *
 * - Dates: ISO `yyyy-MM-dd`; time: `HH:mm`; zone: IANA id (e.g. `Europe/Prague`).
 * - Amount: integer microlitres (1 µl precision), see [cz.nowass.dispenser.domain.Volume].
 * - Weekdays: bit mask, bit 0 = Monday … bit 6 = Sunday (matches docs/ARCHITECTURE.md).
 */
@Entity(tableName = "schedules")
data class ScheduleEntity(
    @PrimaryKey val id: String,
    val revision: Int,
    val name: String,
    val enabled: Boolean,
    @ColumnInfo(name = "amount_ul") val amountMicrolitres: Long,
    @ColumnInfo(name = "start_date") val startDate: String,
    @ColumnInfo(name = "local_time") val localTime: String,
    @ColumnInfo(name = "zone_id") val zoneId: String,
    @ColumnInfo(name = "recurrence_kind") val recurrenceKind: String,
    @ColumnInfo(name = "repeat_interval") val repeatInterval: Int,
    @ColumnInfo(name = "weekdays_mask") val weekdaysMask: Int,
    @ColumnInfo(name = "end_date") val endDate: String?,
    @ColumnInfo(name = "sync_state") val syncState: String,
    /** Phone wall-clock time of the last local write (epoch ms); diagnostics only. */
    @ColumnInfo(name = "updated_at") val updatedAtEpochMs: Long,
)
