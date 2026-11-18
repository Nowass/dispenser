package cz.nowass.dispenser.crypto

/**
 * A failure of the native Noise provider. [code] is a `DSPN_E_*` value of dspnoise_core.h; the
 * message never contains key material or protocol payloads.
 */
class NoiseException(val code: Int) : Exception("noise error $code (${describe(code)})") {
    val isAuthenticationFailure: Boolean get() = code == E_NOISE

    companion object {
        const val E_ARG = -1
        const val E_NOISE = -2
        const val E_STATE = -3
        const val E_NOMEM = -4
        const val E_INIT = -5

        fun describe(code: Int): String = when (code) {
            E_ARG -> "invalid argument"
            E_NOISE -> "rejected"
            E_STATE -> "wrong state"
            E_NOMEM -> "out of memory"
            E_INIT -> "not initialized"
            else -> "unknown"
        }
    }
}
