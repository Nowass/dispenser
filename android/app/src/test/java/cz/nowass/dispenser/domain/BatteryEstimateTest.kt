package cz.nowass.dispenser.domain

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/** The approximate 1S Li-ion voltage-to-percentage model (an estimate, never a measurement). */
class BatteryEstimateTest {
    @Test
    fun owners_reference_voltage_is_about_73_percent() {
        // 3.97 V lies between 3.95 V (70 %) and 3.98 V (75 %): 70 + 5 * 20/30 = 73.3
        assertEquals(73, BatteryEstimate.percent(3970))
    }

    @Test
    fun curve_points_map_exactly() {
        for ((mv, p) in BatteryEstimate.CURVE) assertEquals("$mv", p, BatteryEstimate.percent(mv))
    }

    @Test
    fun clamped_to_0_and_100() {
        assertEquals(0, BatteryEstimate.percent(3300))
        assertEquals(0, BatteryEstimate.percent(3000))
        assertEquals(0, BatteryEstimate.percent(0))
        assertEquals(0, BatteryEstimate.percent(-5))
        assertEquals(100, BatteryEstimate.percent(4200))
        assertEquals(100, BatteryEstimate.percent(4350))
        assertEquals(100, BatteryEstimate.percent(Int.MAX_VALUE))
    }

    @Test
    fun monotonic_and_within_bounds_over_the_whole_range() {
        var last = -1
        for (mv in 2500..4600) {
            val p = BatteryEstimate.percent(mv)
            assertTrue("$mv -> $p", p in 0..100)
            assertTrue("$mv not monotonic", p >= last)
            last = p
        }
    }

    @Test
    fun not_a_linear_3_0_to_4_2_mapping() {
        // a linear 3.0-4.2 V scale would say 81 % at 3.97 V and 50 % at 3.60 V
        assertTrue(BatteryEstimate.percent(3970) != 81)
        assertEquals(5, BatteryEstimate.percent(3610))
        assertEquals(50, BatteryEstimate.percent(3840)) // the flat middle of the curve
    }

    @Test
    fun table_is_strictly_increasing() {
        BatteryEstimate.CURVE.zipWithNext().forEach { (a, b) ->
            assertTrue("$a $b", b.first > a.first && b.second > a.second)
        }
        assertEquals(0, BatteryEstimate.CURVE.first().second)
        assertEquals(100, BatteryEstimate.CURVE.last().second)
    }
}
