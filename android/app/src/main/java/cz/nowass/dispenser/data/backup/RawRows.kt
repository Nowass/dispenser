package cz.nowass.dispenser.data.backup

import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.longOrNull

/**
 * Exact copy of the stored schedule rows (every column, including rows that fail domain
 * validation) used only for the internal pre-restore safety snapshot and rollback. Never part
 * of the user-facing backup format.
 */
data class RawScheduleRows(val rows: List<JsonObject>)

/** Exact copy of the simulation tables (state, processed occurrences, history events). */
data class RawSimulationRows(
    val state: List<JsonObject>,
    val processed: List<JsonObject>,
    val events: List<JsonObject>,
) {
    companion object {
        val EMPTY = RawSimulationRows(emptyList(), emptyList(), emptyList())
    }
}

/** Strict column readers for raw rows; any mismatch throws [IllegalArgumentException]. */
internal object RawColumns {
    fun JsonObject.col(name: String): JsonElement = requireNotNull(this[name]) { "missing column $name" }

    fun JsonObject.str(name: String): String =
        (col(name) as? JsonPrimitive)?.takeIf { it.isString }?.content ?: throw IllegalArgumentException("column $name")

    fun JsonObject.strOrNull(name: String): String? = if (col(name) is JsonNull) null else str(name)

    fun JsonObject.long(name: String): Long =
        (col(name) as? JsonPrimitive)?.takeIf { !it.isString }?.longOrNull ?: throw IllegalArgumentException("column $name")

    fun JsonObject.longOrNull(name: String): Long? = if (col(name) is JsonNull) null else long(name)

    fun JsonObject.int(name: String): Int = Math.toIntExact(long(name))

    fun JsonObject.bool(name: String): Boolean =
        (col(name) as? JsonPrimitive)?.takeIf { !it.isString }?.booleanOrNull ?: throw IllegalArgumentException("column $name")
}
