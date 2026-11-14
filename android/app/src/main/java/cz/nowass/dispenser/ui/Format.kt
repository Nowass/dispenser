package cz.nowass.dispenser.ui

import java.text.NumberFormat
import java.time.LocalDate
import java.time.ZonedDateTime
import java.time.format.DateTimeFormatter
import java.util.Locale

/** UI formatting. The default UI language is Czech, so formats use cs-CZ. */
object Format {
    val CZECH: Locale = Locale.forLanguageTag("cs-CZ")

    private val time = DateTimeFormatter.ofPattern("HH:mm", CZECH)
    private val dayAndDate = DateTimeFormatter.ofPattern("EEEE d. M.", CZECH)
    private val dateTime = DateTimeFormatter.ofPattern("d. M. yyyy HH:mm", CZECH)
    private val shortWeekday = DateTimeFormatter.ofPattern("EEE", CZECH)
    private val dayOfMonth = DateTimeFormatter.ofPattern("d", CZECH)
    private val fullDate = DateTimeFormatter.ofPattern("EEEE d. MMMM", CZECH)

    /** e.g. "10" or "12,5" (Czech decimal comma, at most one decimal). */
    fun ml(value: Double): String =
        NumberFormat.getNumberInstance(CZECH).apply {
            maximumFractionDigits = 1
            minimumFractionDigits = 0
        }.format(value)

    fun time(at: ZonedDateTime): String = time.format(at)

    fun dateTime(at: ZonedDateTime): String = dateTime.format(at)

    enum class RelativeDay { TODAY, TOMORROW, OTHER }

    fun relativeDay(at: ZonedDateTime, now: ZonedDateTime): RelativeDay {
        val day: LocalDate = at.toLocalDate()
        val today = now.withZoneSameInstant(at.zone).toLocalDate()
        return when (day) {
            today -> RelativeDay.TODAY
            today.plusDays(1) -> RelativeDay.TOMORROW
            else -> RelativeDay.OTHER
        }
    }

    /** e.g. "Pondělí 16. 11." (capitalized for sentence start). */
    fun dayAndDate(at: ZonedDateTime): String = dayAndDate.format(at).capitalizeCzech()

    /** e.g. "po", "út". */
    fun shortWeekday(date: LocalDate): String = shortWeekday.format(date)

    fun dayOfMonth(date: LocalDate): String = dayOfMonth.format(date)

    /** e.g. "pondělí 16. listopadu" — for accessibility labels. */
    fun fullDate(date: LocalDate): String = fullDate.format(date)

    enum class CzechPlural { ONE, FEW, OTHER }

    /** Czech plural category for integers: 1 / 2–4 / everything else (incl. 0). */
    fun czechPlural(n: Int): CzechPlural = when (n) {
        1 -> CzechPlural.ONE
        in 2..4 -> CzechPlural.FEW
        else -> CzechPlural.OTHER
    }

    private fun String.capitalizeCzech(): String =
        replaceFirstChar { if (it.isLowerCase()) it.titlecase(CZECH) else it.toString() }
}
