package cz.nowass.dispenser.ui

/** Strict parsing of user-entered millilitre amounts (shared by editor, refill and simulation forms). */
object AmountInput {
    /** Plain decimal only: digits with an optional single comma or dot ("12", "12,5", "12.5", ",5"). */
    private val PATTERN = Regex("""^(\d+([.,]\d*)?|[.,]\d+)$""")

    /**
     * Accepts "12", "12.5" and Czech "12,5". Returns null for anything else, including
     * scientific ("1e3"), hex, signed or grouped input, which toDouble() would accept.
     */
    fun parseMl(text: String): Double? =
        text.trim().takeIf { PATTERN.matches(it) }?.replace(',', '.')?.toDoubleOrNull()
}
