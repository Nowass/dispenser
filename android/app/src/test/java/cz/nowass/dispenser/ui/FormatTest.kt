package cz.nowass.dispenser.ui

import cz.nowass.dispenser.ui.Format.CzechPlural
import org.junit.Assert.assertEquals
import org.junit.Test
import java.time.LocalDate

class FormatTest {

    @Test
    fun czechPlural_categories() {
        assertEquals(CzechPlural.OTHER, Format.czechPlural(0))
        assertEquals(CzechPlural.ONE, Format.czechPlural(1))
        assertEquals(CzechPlural.FEW, Format.czechPlural(2))
        assertEquals(CzechPlural.FEW, Format.czechPlural(4))
        assertEquals(CzechPlural.OTHER, Format.czechPlural(5))
        assertEquals(CzechPlural.OTHER, Format.czechPlural(22))
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
