package cz.nowass.dispenser.data.local

import cz.nowass.dispenser.data.backup.RawColumns.bool
import cz.nowass.dispenser.data.backup.RawColumns.int
import cz.nowass.dispenser.data.backup.RawColumns.long
import cz.nowass.dispenser.data.backup.RawColumns.str
import cz.nowass.dispenser.data.backup.RawColumns.strOrNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put

/**
 * Lossless [ScheduleEntity] <-> JSON column mapping for the internal safety snapshot. Copies
 * every column verbatim and performs no domain validation, so unreadable rows survive a
 * rollback with all original values.
 */
object ScheduleRawRows {
    fun toJson(e: ScheduleEntity): JsonObject = buildJsonObject {
        put("id", e.id)
        put("revision", e.revision)
        put("name", e.name)
        put("enabled", e.enabled)
        put("amount_ul", e.amountMicrolitres)
        put("start_date", e.startDate)
        put("local_time", e.localTime)
        put("zone_id", e.zoneId)
        put("recurrence_kind", e.recurrenceKind)
        put("repeat_interval", e.repeatInterval)
        put("weekdays_mask", e.weekdaysMask)
        put("end_date", e.endDate)
        put("sync_state", e.syncState)
        put("updated_at", e.updatedAtEpochMs)
    }

    fun fromJson(o: JsonObject): ScheduleEntity = ScheduleEntity(
        id = o.str("id"),
        revision = o.int("revision"),
        name = o.str("name"),
        enabled = o.bool("enabled"),
        amountMicrolitres = o.long("amount_ul"),
        startDate = o.str("start_date"),
        localTime = o.str("local_time"),
        zoneId = o.str("zone_id"),
        recurrenceKind = o.str("recurrence_kind"),
        repeatInterval = o.int("repeat_interval"),
        weekdaysMask = o.int("weekdays_mask"),
        endDate = o.strOrNull("end_date"),
        syncState = o.str("sync_state"),
        updatedAtEpochMs = o.long("updated_at"),
    )
}
