package cz.nowass.dispenser.ui

import org.junit.Assert.assertEquals
import org.junit.Test
import java.time.LocalDate
import java.time.YearMonth

class FormatTest {

    @Test
    fun monthYear_isStandaloneNominative() {
        assertEquals("Listopad 2026", Format.monthYear(YearMonth.of(2026, 11)))
        assertEquals("Prosinec 2026", Format.monthYear(YearMonth.of(2026, 12)))
    }

    @Test
    fun weekRange_variants() {
        val same: (Int, Int, Int, Int) -> String = { a, b, m, y -> "$a.–$b. $m. $y" }
        val range: (String, String) -> String = { a, b -> "$a – $b" }
        assertEquals("16.–22. 11. 2026", Format.weekRange(LocalDate.of(2026, 11, 16), LocalDate.of(2026, 11, 22), same, range))
        assertEquals("30. 11. – 6. 12. 2026", Format.weekRange(LocalDate.of(2026, 11, 30), LocalDate.of(2026, 12, 6), same, range))
        assertEquals("28. 12. 2026 – 3. 1. 2027", Format.weekRange(LocalDate.of(2026, 12, 28), LocalDate.of(2027, 1, 3), same, range))
    }

    @Test
    fun fullDateTitle_isCapitalized() {
        assertEquals("Pondělí 16. listopadu", Format.fullDateTitle(LocalDate.of(2026, 11, 16)))
    }

    @Test
    fun ml_usesCzechDecimalComma() {
        assertEquals("10", Format.ml(10.0))
        assertEquals("12,5", Format.ml(12.5))
    }

    @Test
    fun weekdayAndDayLabels_areCzech() {
        val monday = LocalDate.of(2026, 11, 16)
        assertEquals("po", Format.shortWeekday(monday))
        assertEquals("16", Format.dayOfMonth(monday))
    }
}
