package cz.nowass.dispenser.domain

import java.math.BigDecimal

/**
 * Validated conversion between the domain/UI unit (millilitres, [Double]) and the
 * storage unit (integer microlitres, [Long]).
 *
 * - Precision: 1 µl, i.e. at most 3 decimal places in ml. Finer values are rejected,
 *   never silently rounded.
 * - Range: (0, [MAX_MICROLITRES]]. The upper bound is purely technical: it is the
 *   largest value whose µl -> ml -> µl round trip through a [Double] is exact
 *   (2^53 - 1 µl ≈ 9 × 10^12 ml). It is NOT a dosing or pump limit (FR-016 is TBD).
 */
object Volume {
    const val MICROLITRES_PER_ML = 1_000L
    const val MAX_MICROLITRES: Long = (1L shl 53) - 1

    sealed interface Conversion {
        data class Ok(val microlitres: Long) : Conversion
        data object NotFinite : Conversion
        data object NotPositive : Conversion
        /** More than 3 decimal places in ml (finer than 1 µl). */
        data object TooPrecise : Conversion
        data object OutOfRange : Conversion
    }

    fun mlToMicrolitres(ml: Double): Conversion {
        if (!ml.isFinite()) return Conversion.NotFinite
        if (ml <= 0.0) return Conversion.NotPositive
        // valueOf uses the shortest decimal representation of the double (12.5 -> "12.5").
        val micro = BigDecimal.valueOf(ml).movePointRight(3)
        if (micro.stripTrailingZeros().scale() > 0) return Conversion.TooPrecise
        val value = try {
            micro.longValueExact()
        } catch (_: ArithmeticException) {
            return Conversion.OutOfRange
        }
        return if (value > MAX_MICROLITRES) Conversion.OutOfRange else Conversion.Ok(value)
    }

    /** Requires a value in (0, [MAX_MICROLITRES]]; see [isValidMicrolitres]. */
    fun microlitresToMl(microlitres: Long): Double {
        require(isValidMicrolitres(microlitres)) { "microlitres out of range: $microlitres" }
        return BigDecimal.valueOf(microlitres).movePointLeft(3).toDouble()
    }

    fun isValidMicrolitres(microlitres: Long): Boolean = microlitres in 1..MAX_MICROLITRES
}
