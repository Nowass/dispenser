package cz.nowass.dispenser.domain

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import java.time.DayOfWeek
import java.time.Instant
import java.time.LocalDate
import java.time.LocalDateTime
import java.time.LocalTime
import java.time.ZoneOffset
import java.time.ZonedDateTime

/**
 * Runs the shared recurrence test vectors (`test-vectors/` in the repository root, format v1)
 * against the Kotlin reference implementation. The firmware C engine runs the same files
 * (`firmware/test/host/test_recurrence_vectors.c`), which keeps both engines in parity.
 *
 * Records marked `scope=c` cover firmware-only canonical-form or range rules and are skipped.
 * Disabled schedules have no occurrences, as in every app consumer of [RecurrenceEngine]
 * ([NextDoseCalculator], [OccurrenceCalendar], simulation).
 */
class RecurrenceVectorTest {

    private val zone = DEFAULT_ZONE
    private val failures = mutableListOf<String>()
    private val counts = sortedMapOf<String, Int>()
    private var skipped = 0
    private val raw = mutableMapOf<String, Map<String, String>>()

    @Test
    fun recurrenceVectors_matchKotlinEngine() {
        run("recurrence_v1.txt")
        report(minimumTypes = 8)
    }

    @Test
    fun pragueDstReference_matchesJavaTimeRules() {
        run("europe_prague_dst_v1.txt")
        assertEquals(100, counts["tz_transition"])
        report(minimumTypes = 1)
    }

    private fun report(minimumTypes: Int) {
        println("vectors: $counts skipped(scope=c)=$skipped")
        assertTrue("record types exercised: $counts", counts.size >= minimumTypes)
        assertTrue(failures.joinToString("\n"), failures.isEmpty())
    }

    private fun vectorFile(name: String): File {
        var dir: File? = File(System.getProperty("user.dir")!!).absoluteFile
        while (dir != null) {
            val f = File(dir, "test-vectors/$name")
            if (f.isFile) return f
            dir = dir.parentFile
        }
        error("test-vectors/$name not found above ${System.getProperty("user.dir")}")
    }

    private fun run(name: String) {
        vectorFile(name).readLines().forEachIndexed { i, fullLine ->
            val line = fullLine.substringBefore('#').trim()
            if (line.isEmpty()) return@forEachIndexed
            val tokens = line.split(Regex("\\s+"))
            val kv = tokens.drop(1).associate { it.substringBefore('=') to it.substringAfter('=') }
            val where = "$name:${i + 1}"
            if (tokens[0] == "schedule") {
                raw[kv.getValue("name")] = kv
                return@forEachIndexed
            }
            if (kv["scope"] == "c") {
                skipped++
                return@forEachIndexed
            }
            try {
                when (tokens[0]) {
                    "validate" -> validate(kv, where)
                    "local_to_utc" -> localToUtc(kv, where)
                    "next" -> next(kv, where)
                    "seq" -> seq(kv, where)
                    "between" -> between(kv, where)
                    "occurrence_on" -> occurrenceOn(kv, where)
                    "occurrence_id" -> occurrenceId(kv, where)
                    "next_set" -> nextSet(kv, where)
                    "tz_transition" -> tzTransition(kv, where)
                    else -> error("unknown record type ${tokens[0]}")
                }
                counts.merge(tokens[0], 1, Int::plus)
            } catch (e: Exception) {
                failures += "$where: ${e::class.simpleName}: ${e.message}"
            }
        }
    }

    // ---- parsing ----

    private fun weekdays(s: String): Set<DayOfWeek> =
        if (s == "-") emptySet()
        else s.split(',').map { code -> DayOfWeek.entries.single { it.name.startsWith(code) } }.toSet()

    private fun draft(kv: Map<String, String>) = ScheduleDraft(
        name = kv.getValue("id"),
        amountMl = kv.getValue("amount_ul").toLong() / 1000.0,
        startDate = LocalDate.parse(kv.getValue("start")),
        localTime = LocalTime.parse(kv.getValue("time")),
        kind = RecurrenceKind.valueOf(kv.getValue("kind")),
        interval = kv.getValue("interval").toInt(),
        weekdays = weekdays(kv.getValue("weekdays")),
        endDate = kv.getValue("end").takeIf { it != "-" }?.let(LocalDate::parse),
        enabled = kv.getValue("enabled") == "true",
    )

    private fun schedule(label: String): Schedule {
        val kv = raw[label] ?: error("unknown schedule $label")
        return draft(kv).toSchedule(kv.getValue("id"), kv.getValue("rev").toInt())
    }

    private fun instant(s: String): Instant = Instant.parse(s)

    /** "date@instant,date@instant" or "-". */
    private fun occList(s: String): List<Pair<LocalDate, Instant>> =
        if (s == "-") emptyList()
        else s.split(',').map { LocalDate.parse(it.substringBefore('@')) to instant(it.substringAfter('@')) }

    private fun check(where: String, expected: Any?, actual: Any?) {
        if (expected != actual) failures += "$where: expected $expected, got $actual"
    }

    private fun DoseOccurrence.pair() = localDate to scheduledAt

    private fun occurrencesOf(s: Schedule, after: Instant, n: Int) =
        if (s.enabled) RecurrenceEngine.nextOccurrences(s, after, n) else emptyList()

    // ---- records ----

    private fun validate(kv: Map<String, String>, where: String) {
        val expected = kv.getValue("expect").let { if (it == "OK") emptySet() else it.split(',').toSet() }
        val actual = ScheduleValidator.validate(draft(raw.getValue(kv.getValue("schedule")))).map { it.name }.toSet()
        check(where, expected, actual)
    }

    private fun localToUtc(kv: Map<String, String>, where: String) {
        val local = LocalDateTime.of(LocalDate.parse(kv.getValue("date")), LocalTime.parse(kv.getValue("time")))
        val offsets = zone.rules.getValidOffsets(local)
        val kind = when (offsets.size) { 0 -> "gap"; 1 -> "unique"; else -> "overlap" }
        check(where, kv.getValue("kind"), kind)
        if (offsets.isNotEmpty()) {
            check(where, instant(kv.getValue("expect")), ZonedDateTime.ofLocal(local, zone, null).toInstant())
        }
    }

    private fun next(kv: Map<String, String>, where: String) {
        val s = schedule(kv.getValue("schedule"))
        val actual = NextDoseCalculator.nextDose(listOf(s), instant(kv.getValue("after")))?.pair()
        val expected = kv.getValue("expect").takeIf { it != "none" }?.let { occList(it).single() }
        check(where, expected, actual)
    }

    private fun seq(kv: Map<String, String>, where: String) {
        val s = schedule(kv.getValue("schedule"))
        val actual = occurrencesOf(s, instant(kv.getValue("after")), kv.getValue("n").toInt()).map { it.pair() }
        check(where, occList(kv.getValue("expect")), actual)
    }

    private fun between(kv: Map<String, String>, where: String) {
        val s = schedule(kv.getValue("schedule"))
        val result = if (!s.enabled) emptyList() else RecurrenceEngine.occurrencesBetween(
            s, instant(kv.getValue("after")), instant(kv.getValue("until")), kv.getValue("limit").toInt(),
        )
        val expect = kv.getValue("expect")
        if (expect == "overflow") check(where, null, result?.map { it.pair() })
        else check(where, occList(expect), result?.map { it.pair() })
    }

    private fun occurrenceOn(kv: Map<String, String>, where: String) {
        val s = schedule(kv.getValue("schedule"))
        val date = LocalDate.parse(kv.getValue("date"))
        val actual = OccurrenceCalendar.occurrencesOn(listOf(s), date).singleOrNull()?.scheduledAt
        check(where, kv.getValue("expect").takeIf { it != "none" }?.let(::instant), actual)
    }

    private fun occurrenceId(kv: Map<String, String>, where: String) {
        val s = schedule(kv.getValue("schedule"))
        val date = LocalDate.parse(kv.getValue("date"))
        check(where, kv.getValue("expect"), DoseOccurrence.occurrenceId(s.id, date))
        // The id of an actual occurrence (when the rule matches) is the same string.
        RecurrenceEngine.occurrenceOn(s, date)?.let { check(where, kv.getValue("expect"), it.id) }
    }

    private fun nextSet(kv: Map<String, String>, where: String) {
        val names = kv.getValue("schedules")
        val list = if (names == "-") emptyList() else names.split(',').map(::schedule)
        val actual = NextDoseCalculator.nextDose(list, instant(kv.getValue("after")))
        val expect = kv.getValue("expect")
        val expected = if (expect == "none") null else expect.substringBefore('@') to instant(expect.substringAfter('@'))
        check(where, expected, actual?.let { it.scheduleId to it.scheduledAt })
    }

    private fun tzTransition(kv: Map<String, String>, where: String) {
        val year = kv.getValue("year").toInt()
        val rules = zone.rules
        val first = rules.nextTransition(LocalDate.of(year, 1, 1).atStartOfDay().toInstant(ZoneOffset.UTC))
        val second = rules.nextTransition(first.instant)
        check(where, instant(kv.getValue("dst_start")), first.instant)
        check(where, ZoneOffset.ofHours(2), first.offsetAfter)
        check(where, instant(kv.getValue("dst_end")), second.instant)
        check(where, ZoneOffset.ofHours(1), second.offsetAfter)
        check(where, year, second.instant.atZone(ZoneOffset.UTC).year)
    }
}
