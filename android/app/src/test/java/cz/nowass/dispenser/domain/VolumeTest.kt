package cz.nowass.dispenser.domain

import cz.nowass.dispenser.domain.Volume.Conversion
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class VolumeTest {

    private fun ok(ml: Double) = (Volume.mlToMicrolitres(ml) as Conversion.Ok).microlitres

    @Test
    fun exactConversions() {
        assertEquals(10_000L, ok(10.0))
        assertEquals(12_500L, ok(12.5))
        assertEquals(12_345L, ok(12.345))
        assertEquals(1L, ok(0.001))
        assertEquals(100L, ok(0.1)) // binary 0.1 is not exact, decimal conversion must be
    }

    @Test
    fun precisionFinerThanOneMicrolitre_isRejectedNotRounded() {
        assertEquals(Conversion.TooPrecise, Volume.mlToMicrolitres(0.0005))
        assertEquals(Conversion.TooPrecise, Volume.mlToMicrolitres(12.3456))
    }

    @Test
    fun nonPositiveAndNonFinite_areRejected() {
        assertEquals(Conversion.NotPositive, Volume.mlToMicrolitres(0.0))
        assertEquals(Conversion.NotPositive, Volume.mlToMicrolitres(-1.0))
        assertEquals(Conversion.NotFinite, Volume.mlToMicrolitres(Double.NaN))
        assertEquals(Conversion.NotFinite, Volume.mlToMicrolitres(Double.POSITIVE_INFINITY))
    }

    @Test
    fun technicalRange_isEnforcedBothWays() {
        assertEquals(Conversion.OutOfRange, Volume.mlToMicrolitres(1e13))
        assertEquals(Conversion.OutOfRange, Volume.mlToMicrolitres(1e300))
        assertTrue(Volume.isValidMicrolitres(Volume.MAX_MICROLITRES))
        assertFalse(Volume.isValidMicrolitres(Volume.MAX_MICROLITRES + 1))
        assertFalse(Volume.isValidMicrolitres(0))
        assertFalse(Volume.isValidMicrolitres(-5))
    }

    @Test
    fun roundTrip_isLossless() {
        for (ml in listOf(0.001, 0.1, 1.0, 7.5, 12.345, 999.999, 1250.0)) {
            assertEquals(ml, Volume.microlitresToMl(ok(ml)), 0.0)
        }
    }

    @Test(expected = IllegalArgumentException::class)
    fun invalidMicrolitres_cannotBecomeMl() {
        Volume.microlitresToMl(0)
    }

    @Test
    fun validator_reportsPrecisionAndRange() {
        val base = ScheduleDraft("x", 10.0, java.time.LocalDate.of(2026, 11, 16), java.time.LocalTime.of(8, 0), RecurrenceKind.DAILY)
        assertEquals(setOf(ValidationError.AMOUNT_TOO_PRECISE), ScheduleValidator.validate(base.copy(amountMl = 1.2345)))
        assertEquals(setOf(ValidationError.AMOUNT_OUT_OF_RANGE), ScheduleValidator.validate(base.copy(amountMl = 1e14)))
    }
}
