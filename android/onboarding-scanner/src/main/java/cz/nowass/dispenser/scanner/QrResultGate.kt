package cz.nowass.dispenser.scanner

/**
 * Delivers at most one scanned QR text per scanner session: the first non-blank value with a known
 * prefix (when [acceptedPrefixes] is given). Later frames are ignored, so one physical scan never
 * produces two submissions. Pure logic (unit tested); thread-safe.
 */
class QrResultGate(private val acceptedPrefixes: List<String> = emptyList()) {
    private var delivered = false

    /** Returns the value to deliver, or null to ignore this frame. */
    @Synchronized
    fun offer(raw: String?): String? {
        if (delivered || raw.isNullOrBlank()) return null
        if (acceptedPrefixes.isNotEmpty() && acceptedPrefixes.none { raw.startsWith(it, ignoreCase = true) }) return null
        delivered = true
        return raw
    }
}
