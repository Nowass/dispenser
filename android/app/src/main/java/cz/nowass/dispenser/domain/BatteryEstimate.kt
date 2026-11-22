package cz.nowass.dispenser.domain

import kotlin.math.roundToInt

/**
 * Approximate state of charge of a single-cell (1S) Li-ion / Li-polymer battery from its measured
 * voltage. This is an ESTIMATE, not a measured state of charge: the voltage-to-charge relation is
 * flat in the middle, depends on temperature, cell chemistry, ageing and load, and the dispenser
 * measures with a light load (awake, radio still off). The UI therefore always shows the measured
 * voltage next to the percentage and marks the percentage as approximate.
 *
 * Model: piecewise-linear interpolation over a typical resting open-circuit-voltage curve of a
 * LiCoO2/NMC cell (4.20 V full). 0 % is placed at 3.30 V, where the board's 3.3 V regulator runs
 * out of headroom (the device stops working well before a protected cell cuts off at ~2.5 V), not
 * at the chemical empty point. No unjustified linear 3.0-4.2 V mapping. The table is the single
 * place to refine once real discharge data of the device exists.
 */
object BatteryEstimate {
    /** (millivolts, percent), strictly increasing in both. */
    val CURVE: List<Pair<Int, Int>> = listOf(
        3300 to 0,
        3610 to 5,
        3690 to 10,
        3710 to 15,
        3730 to 20,
        3750 to 25,
        3770 to 30,
        3790 to 35,
        3800 to 40,
        3820 to 45,
        3840 to 50,
        3850 to 55,
        3870 to 60,
        3910 to 65,
        3950 to 70,
        3980 to 75,
        4020 to 80,
        4080 to 85,
        4110 to 90,
        4150 to 95,
        4200 to 100,
    )

    /** Estimated percentage 0..100 (clamped below 3.30 V and above 4.20 V). */
    fun percent(millivolts: Int): Int {
        val first = CURVE.first()
        val last = CURVE.last()
        if (millivolts <= first.first) return first.second
        if (millivolts >= last.first) return last.second
        val i = CURVE.indexOfFirst { it.first >= millivolts }
        val (v1, p1) = CURVE[i]
        val (v0, p0) = CURVE[i - 1]
        val p = p0 + (p1 - p0) * (millivolts - v0).toDouble() / (v1 - v0)
        return p.roundToInt().coerceIn(0, 100)
    }
}
