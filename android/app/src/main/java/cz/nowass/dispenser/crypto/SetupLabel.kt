package cz.nowass.dispenser.crypto

import java.math.BigInteger
import java.security.MessageDigest

/**
 * The device's setup label `DSP2:<device-id>:<code>` (docs/ANDROID_BLE_DESIGN.md 7.1 A; format of
 * firmware/tools/provision_dev_secrets.py). [setupCode] is secret: used for one PSK derivation,
 * then closed; never stored or logged.
 */
class SetupLabel(val deviceIdHex: String, val setupCode: SecretBytes) : AutoCloseable {
    val deviceId: ByteArray get() = ByteArray(6) { deviceIdHex.substring(2 * it, 2 * it + 2).toInt(16).toByte() }
    override fun close() = setupCode.close()
    override fun toString(): String = "SetupLabel(device=$deviceIdHex, code=**)"

    /** Why a label was refused (first failing rule; names match test-vectors/qr_formats_v1.txt). */
    enum class Problem { PREFIX, FIELDS, DEVICE_ID, LENGTH, CHARSET, PAD, CHECK }

    sealed interface Result {
        class Ok(val label: SetupLabel) : Result
        data class Invalid(val problem: Problem) : Result
    }

    companion object {
        private const val CODE_CHARS = 28

        /**
         * Parses a scanned or typed label. Manual-entry tolerance: case-insensitive, `I`/`L` -> 1,
         * `O` -> 0, hyphens and spaces in the code part ignored; the device ID in either case.
         */
        fun parse(input: String): Result {
            val text = input.trim()
            if (!text.regionMatches(0, "DSP2:", 0, 5, ignoreCase = true)) return Result.Invalid(Problem.PREFIX)
            val fields = text.split(':')
            if (fields.size != 3) return Result.Invalid(Problem.FIELDS)
            val id = fields[1].lowercase()
            if (id.length != 12 || id.any { it !in '0'..'9' && it !in 'a'..'f' }) return Result.Invalid(Problem.DEVICE_ID)
            val norm = StringBuilder()
            for (c in fields[2]) {
                when (val u = c.uppercaseChar()) {
                    '-', ' ' -> Unit
                    'I', 'L' -> norm.append('1')
                    'O' -> norm.append('0')
                    else -> norm.append(u)
                }
            }
            val code = norm.toString()
            if (code.any { Crockford.ALPHABET.indexOf(it) < 0 }) return Result.Invalid(Problem.CHARSET)
            if (code.length != CODE_CHARS) return Result.Invalid(Problem.LENGTH)
            val body = Crockford.decodeStrict(code.substring(0, 26))!!
            if (body.testBit(0) || body.testBit(1)) return Result.Invalid(Problem.PAD)
            val bytes = Crockford.toBytes(body.shiftRight(2), 16)
            val check = checkChars(bytes)
            if (check != code.substring(26)) {
                bytes.fill(0)
                return Result.Invalid(Problem.CHECK)
            }
            return Result.Ok(SetupLabel(id, SecretBytes.wrap(bytes)))
        }

        /** Canonical label text (tests, and showing the device ID; the code is never displayed back). */
        internal fun format(deviceIdHex: String, code: ByteArray): String =
            "DSP2:" + deviceIdHex.lowercase() + ":" + Crockford.encode(code, 26) + checkChars(code)

        private fun checkChars(code: ByteArray): String {
            val md = MessageDigest.getInstance("SHA-256")
            md.update("DSP2 setup".toByteArray(Charsets.US_ASCII))
            md.update(code)
            val d = md.digest()
            val v = BigInteger(1, d.copyOf(2)).toInt() ushr 6 // first 10 bits
            return "" + Crockford.ALPHABET[v ushr 5] + Crockford.ALPHABET[v and 31]
        }
    }
}
