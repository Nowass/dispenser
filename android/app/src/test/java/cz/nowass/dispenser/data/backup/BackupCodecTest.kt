package cz.nowass.dispenser.data.backup

import cz.nowass.dispenser.domain.DEFAULT_ZONE
import cz.nowass.dispenser.domain.Recurrence
import cz.nowass.dispenser.domain.Schedule
import cz.nowass.dispenser.domain.simulation.SimulationEngine
import cz.nowass.dispenser.domain.simulation.SimulationResult
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import java.time.DayOfWeek
import java.time.Instant
import java.time.LocalDate
import java.time.LocalTime
import java.time.ZoneId

class BackupCodecTest {

    private val exportedAt = Instant.parse("2026-11-15T10:00:00Z")

    private val daily = Schedule("a1", 3, "Ranní", true, 12.5, LocalDate.of(2026, 11, 1), LocalTime.of(8, 0), Recurrence.Daily(2), LocalDate.of(2027, 1, 31))
    private val weekly = Schedule(
        "b2", 1, "Víkend", false, 0.001, LocalDate.of(2026, 11, 2), LocalTime.of(20, 30),
        Recurrence.Weekly(setOf(DayOfWeek.SATURDAY, DayOfWeek.SUNDAY, DayOfWeek.MONDAY), 3),
    )
    private val once = Schedule("c3", 7, "  Jednorázová ", true, 123.456, LocalDate.of(2026, 12, 24), LocalTime.of(18, 0), Recurrence.Once, zone = ZoneId.of("Europe/London"))

    private fun content(schedules: List<Schedule> = listOf(daily, weekly, once), name: String? = "Obývák", sim: SimulationBackup? = null) =
        BackupContent(exportedAt, "0.4.0 (4)", name, schedules, sim)

    private fun simulation(): SimulationBackup {
        val engine = SimulationEngine()
        val start = (engine.start("run-1", Instant.parse("2026-11-20T05:00:00Z"), 500_000, 120_000) as SimulationResult.Ok)
        val adv = engine.advanceDays(start.state, 3, listOf(daily.copy(startDate = LocalDate.of(2026, 11, 20)))) as SimulationResult.Ok
        assertTrue(adv.state.processedOccurrences.isNotEmpty())
        return SimulationBackup(adv.state, start.events + adv.events)
    }

    private fun roundTrip(c: BackupContent): BackupContent {
        val text = BackupCodec.encode(c)
        val d = BackupCodec.decode(text.toByteArray())
        assertTrue("$d", d is BackupCodec.Decoded.Valid)
        return (d as BackupCodec.Decoded.Valid).content
    }

    private fun error(text: String): BackupError {
        val d = BackupCodec.decode(text.toByteArray())
        if (d !is BackupCodec.Decoded.Invalid) fail("expected rejection, got $d")
        return (d as BackupCodec.Decoded.Invalid).error
    }

    /** Encodes [c], applies [edit] to the JSON tree and returns the resulting error. */
    private fun errorAfter(c: BackupContent = content(), edit: (MutableMap<String, JsonElement>) -> Unit): BackupError {
        val root = Json.parseToJsonElement(BackupCodec.encode(c)).jsonObject.toMutableMap()
        edit(root)
        return error(JsonObject(root).toString())
    }

    private fun MutableMap<String, JsonElement>.editSchedule(index: Int, edit: (MutableMap<String, JsonElement>) -> Unit) {
        val list = getValue("schedules").jsonArray.toMutableList()
        list[index] = JsonObject(list[index].jsonObject.toMutableMap().also(edit))
        this["schedules"] = JsonArray(list)
    }

    private fun MutableMap<String, JsonElement>.editSimulation(edit: (MutableMap<String, JsonElement>) -> Unit) {
        this["simulation"] = JsonObject(getValue("simulation").jsonObject.toMutableMap().also(edit))
    }

    // ---------------------------------------------------------------- round trips

    @Test
    fun roundTrip_multipleSchedules_allRecurrenceKinds_disabled_decimalVolumes() {
        val c = content()
        assertEquals(c, roundTrip(c))
        val back = roundTrip(c).schedules
        assertEquals(12.5, back[0].amountMl, 0.0)
        assertEquals(0.001, back[1].amountMl, 0.0)
        assertEquals(false, back[1].enabled)
        assertEquals("  Jednorázová ", back[2].name) // byte-identical name
        assertEquals(ZoneId.of("Europe/London"), back[2].zone)
    }

    @Test
    fun roundTrip_emptyConfiguration_defaultName() {
        val c = content(schedules = emptyList(), name = null)
        assertEquals(c, roundTrip(c))
    }

    @Test
    fun roundTrip_withSimulation_keepsProcessedOccurrencesAndHistory() {
        val c = content(sim = simulation())
        assertEquals(c, roundTrip(c))
        val inactive = content(sim = SimulationBackup(null, simulation().history))
        assertEquals(inactive, roundTrip(inactive))
    }

    @Test
    fun encodedFormat_isDocumentedShape() {
        val o = Json.parseToJsonElement(BackupCodec.encode(content(sim = simulation()))).jsonObject
        assertEquals(setOf("format", "schemaVersion", "exportedAt", "applicationVersion", "settings", "schedules", "simulation"), o.keys)
        assertEquals(JsonPrimitive("dispenser-backup"), o["format"])
        assertEquals(JsonPrimitive(1), o["schemaVersion"])
        val s = o.getValue("schedules").jsonArray[0].jsonObject
        assertEquals(JsonPrimitive(12_500L), s["amountMicrolitres"])
        assertEquals(JsonPrimitive("08:00"), s["localTime"])
        assertEquals(JsonPrimitive("SIMULATION"), o.getValue("simulation").jsonObject["dataOrigin"])
        assertTrue("no sync state in a backup", "sync" !in BackupCodec.encode(content()).lowercase())
    }

    // ---------------------------------------------------------------- rejected files

    @Test
    fun corruptedOrForeignFiles_areRejected() {
        assertEquals(BackupError.Kind.EMPTY, BackupCodec.decode(ByteArray(0)).let { (it as BackupCodec.Decoded.Invalid).error.kind })
        assertEquals(BackupError.Kind.EMPTY, error("   ").kind)
        assertEquals(BackupError.Kind.NOT_JSON, error("{\"format\": \"dispenser-backup\", ").kind)
        assertEquals(BackupError.Kind.NOT_JSON, BackupCodec.decode(byteArrayOf(0xC3.toByte(), 0x28)).let { (it as BackupCodec.Decoded.Invalid).error.kind })
        assertEquals(BackupError.Kind.NOT_A_BACKUP, error("[]").kind)
        assertEquals(BackupError.Kind.NOT_A_BACKUP, error("{\"format\":\"something-else\",\"schemaVersion\":1}").kind)
        val tooLarge = ByteArray(BackupCodec.MAX_BYTES + 1) { ' '.code.toByte() }
        assertEquals(BackupError.Kind.TOO_LARGE, (BackupCodec.decode(tooLarge) as BackupCodec.Decoded.Invalid).error.kind)
    }

    @Test
    fun unsupportedSchemaVersions_areRejected() {
        val v2 = errorAfter { it["schemaVersion"] = JsonPrimitive(2) }
        assertEquals(BackupError.Kind.UNSUPPORTED_VERSION, v2.kind)
        assertEquals("2", v2.detail)
        assertEquals(BackupError.Kind.UNSUPPORTED_VERSION, errorAfter { it["schemaVersion"] = JsonPrimitive(0) }.kind)
        assertEquals(BackupError.Kind.UNSUPPORTED_VERSION, errorAfter { it["schemaVersion"] = JsonPrimitive("1") }.kind)
        assertEquals(BackupError.Kind.UNSUPPORTED_VERSION, errorAfter { it.remove("schemaVersion") }.kind)
    }

    @Test
    fun missingUnknownAndMistypedFields_areRejected_withPath() {
        assertEquals(BackupError(BackupError.Kind.MISSING_FIELD, "settings"), errorAfter { it.remove("settings") })
        assertEquals(BackupError(BackupError.Kind.UNKNOWN_FIELD, "extra"), errorAfter { it["extra"] = JsonPrimitive(1) })
        assertEquals(BackupError(BackupError.Kind.MISSING_FIELD, "schedules[1].enabled"), errorAfter { r -> r.editSchedule(1) { it.remove("enabled") } })
        assertEquals(BackupError(BackupError.Kind.UNKNOWN_FIELD, "schedules[0].syncState"), errorAfter { r -> r.editSchedule(0) { it["syncState"] = JsonPrimitive("DEVICE_APPLIED") } })
        assertEquals(BackupError(BackupError.Kind.WRONG_TYPE, "schedules[0].amountMicrolitres"), errorAfter { r -> r.editSchedule(0) { it["amountMicrolitres"] = JsonPrimitive(12.5) } })
        assertEquals(BackupError(BackupError.Kind.WRONG_TYPE, "schedules[0].enabled"), errorAfter { r -> r.editSchedule(0) { it["enabled"] = JsonPrimitive("true") } })
        assertEquals(BackupError(BackupError.Kind.WRONG_TYPE, "schedules"), errorAfter { it["schedules"] = JsonObject(emptyMap()) })
    }

    @Test
    fun duplicateScheduleIds_areRejected() {
        val e = errorAfter(content(listOf(daily, weekly.copy(id = daily.id))) ) { }
        assertEquals(BackupError(BackupError.Kind.DUPLICATE_ID, "schedules[1].id"), e)
    }

    @Test
    fun invalidIdsVolumesDatesTimesZones_areRejected() {
        fun sched(field: String, value: JsonElement) = errorAfter { r -> r.editSchedule(0) { it[field] = value } }
        assertEquals(BackupError.Kind.INVALID_VALUE, sched("id", JsonPrimitive("")).kind)
        assertEquals(BackupError.Kind.INVALID_VALUE, sched("id", JsonPrimitive("a@2026-11-01")).kind)
        assertEquals(BackupError.Kind.INVALID_VALUE, sched("revision", JsonPrimitive(0)).kind)
        assertEquals(BackupError.Kind.INVALID_VALUE, sched("amountMicrolitres", JsonPrimitive(0)).kind)
        assertEquals(BackupError.Kind.INVALID_VALUE, sched("amountMicrolitres", JsonPrimitive(-5)).kind)
        assertEquals(BackupError.Kind.INVALID_VALUE, sched("amountMicrolitres", JsonPrimitive(1L shl 53)).kind)
        assertEquals(BackupError(BackupError.Kind.INVALID_VALUE, "schedules[0].startDate"), sched("startDate", JsonPrimitive("2026-02-30")))
        assertEquals(BackupError.Kind.INVALID_VALUE, sched("localTime", JsonPrimitive("25:00")).kind)
        assertEquals(BackupError.Kind.INVALID_VALUE, sched("localTime", JsonPrimitive("8:00")).kind)
        assertEquals(BackupError.Kind.INVALID_VALUE, sched("localTime", JsonPrimitive("08:00:30")).kind)
        assertEquals(BackupError.Kind.INVALID_VALUE, sched("zoneId", JsonPrimitive("Mars/Olympus")).kind)
        assertEquals(BackupError.Kind.INVALID_VALUE, sched("zoneId", JsonPrimitive("+01:00")).kind)
        assertEquals(BackupError(BackupError.Kind.INVALID_VALUE, "settings.displayName"), errorAfter { it["settings"] = JsonObject(mapOf("displayName" to JsonPrimitive("x".repeat(31)))) })
    }

    @Test
    fun invalidRecurrenceRules_areRejected_byDomainValidator() {
        // Weekly without weekdays -> NO_WEEKDAYS from ScheduleValidator.
        val noDays = errorAfter { r -> r.editSchedule(1) { it["recurrence"] = JsonObject(mapOf("kind" to JsonPrimitive("WEEKLY"), "interval" to JsonPrimitive(1), "weekdays" to JsonArray(emptyList()))) } }
        assertEquals(BackupError(BackupError.Kind.INVALID_SCHEDULE, "schedules[1]", "NO_WEEKDAYS"), noDays)
        val zeroInterval = errorAfter { r -> r.editSchedule(0) { it["recurrence"] = JsonObject(mapOf("kind" to JsonPrimitive("DAILY"), "interval" to JsonPrimitive(0), "weekdays" to JsonArray(emptyList()))) } }
        assertEquals("INTERVAL_TOO_SMALL", zeroInterval.detail)
        val endBeforeStart = errorAfter { r -> r.editSchedule(0) { it["endDate"] = JsonPrimitive("2026-10-01") } }
        assertEquals("END_BEFORE_START", endBeforeStart.detail)
        // ONCE in the spring-forward gap does not exist (TIME_NONEXISTENT).
        val gap = errorAfter(content(listOf(once.copy(zone = DEFAULT_ZONE, startDate = LocalDate.of(2027, 3, 28), localTime = LocalTime.of(2, 30))))) {}
        assertEquals("TIME_NONEXISTENT", gap.detail)
        assertEquals(BackupError.Kind.INVALID_VALUE, errorAfter { r -> r.editSchedule(1) { it["recurrence"] = JsonObject(mapOf("kind" to JsonPrimitive("MONTHLY"), "interval" to JsonPrimitive(1), "weekdays" to JsonArray(emptyList()))) } }.kind)
        assertEquals(BackupError.Kind.INVALID_VALUE, errorAfter { r -> r.editSchedule(1) { it["recurrence"] = JsonObject(mapOf("kind" to JsonPrimitive("WEEKLY"), "interval" to JsonPrimitive(1), "weekdays" to JsonArray(listOf(JsonPrimitive("MONDAY"), JsonPrimitive("MONDAY"))))) } }.kind)
        assertEquals(BackupError(BackupError.Kind.INVALID_VALUE, "schedules[2].endDate"), errorAfter { r -> r.editSchedule(2) { it["endDate"] = JsonPrimitive("2027-01-01") } })
        assertEquals(BackupError.Kind.INVALID_VALUE, errorAfter { r -> r.editSchedule(0) { it["recurrence"] = JsonObject(mapOf("kind" to JsonPrimitive("DAILY"), "interval" to JsonPrimitive(1), "weekdays" to JsonArray(listOf(JsonPrimitive("MONDAY"))))) } }.kind)
    }

    @Test
    fun inconsistentSimulation_isRejected() {
        val c = content(sim = simulation())
        assertEquals(BackupError.Kind.INVALID_VALUE, errorAfter(c) { r -> r.editSimulation { it["dataOrigin"] = JsonPrimitive("DEVICE") } }.kind)
        // A history event claiming real device origin is never accepted.
        val deviceEvent = errorAfter(c) { r ->
            r.editSimulation { sim ->
                val list = sim.getValue("history").jsonArray.toMutableList()
                list[0] = JsonObject(list[0].jsonObject.toMutableMap().also { it["origin"] = JsonPrimitive("DEVICE") })
                sim["history"] = JsonArray(list)
            }
        }
        assertEquals(BackupError(BackupError.Kind.INVALID_VALUE, "simulation.history[0].origin"), deviceEvent)
        val dupSeq = errorAfter(c) { r ->
            r.editSimulation { sim ->
                val list = sim.getValue("history").jsonArray.toMutableList()
                list[1] = JsonObject(list[1].jsonObject.toMutableMap().also { it["seq"] = list[0].jsonObject.getValue("seq") })
                sim["history"] = JsonArray(list)
            }
        }
        assertEquals(BackupError.Kind.INCONSISTENT_SIMULATION, dupSeq.kind)
        val seqBeyondState = errorAfter(c) { r -> r.editSimulation { sim -> sim["state"] = JsonObject(sim.getValue("state").jsonObject.toMutableMap().also { it["nextSeq"] = JsonPrimitive(1) }) } }
        assertEquals(BackupError.Kind.INCONSISTENT_SIMULATION, seqBeyondState.kind)
        val clockBeforeStart = errorAfter(c) { r -> r.editSimulation { sim -> sim["state"] = JsonObject(sim.getValue("state").jsonObject.toMutableMap().also { it["clock"] = JsonPrimitive("2020-01-01T00:00:00Z") }) } }
        assertEquals(BackupError(BackupError.Kind.INCONSISTENT_SIMULATION, "simulation.state.clock"), clockBeforeStart)
        val badProcessed = errorAfter(c) { r ->
            r.editSimulation { sim ->
                val state = sim.getValue("state").jsonObject.toMutableMap()
                state["processedOccurrences"] = JsonArray(listOf(JsonObject(mapOf("occurrenceId" to JsonPrimitive("a1@2026-11-21"), "localDate" to JsonPrimitive("2026-11-22")))))
                sim["state"] = JsonObject(state)
            }
        }
        assertEquals(BackupError.Kind.INCONSISTENT_SIMULATION, badProcessed.kind)
        val overfull = errorAfter(c) { r ->
            r.editSimulation { sim ->
                val state = sim.getValue("state").jsonObject.toMutableMap()
                val res = state.getValue("reservoir").jsonObject.toMutableMap().also { it["remainingMicrolitres"] = JsonPrimitive(600_000) }
                state["reservoir"] = JsonObject(res)
                sim["state"] = JsonObject(state)
            }
        }
        assertEquals(BackupError(BackupError.Kind.INVALID_VALUE, "simulation.state.reservoir"), overfull)
    }
}
