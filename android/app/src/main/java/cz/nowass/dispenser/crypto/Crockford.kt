package cz.nowass.dispenser.crypto

import java.math.BigInteger

/** Crockford base32 (alphabet without I, L, O, U), MSB first, zero pad bits at the end. */
internal object Crockford {
    const val ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"

    fun encode(data: ByteArray, chars: Int): String {
        val pad = chars * 5 - data.size * 8
        require(pad in 0..4)
        val n = BigInteger(1, data).shiftLeft(pad)
        val sb = StringBuilder(chars)
        for (i in chars - 1 downTo 0) sb.append(ALPHABET[n.shiftRight(5 * i).toInt() and 31])
        return sb.toString()
    }

    /** Canonical characters only (uppercase alphabet); returns the value or null. */
    fun decodeStrict(text: String): BigInteger? {
        var n = BigInteger.ZERO
        for (c in text) {
            val v = ALPHABET.indexOf(c)
            if (v < 0) return null
            n = n.shiftLeft(5).or(BigInteger.valueOf(v.toLong()))
        }
        return n
    }

    /** Big-endian bytes of [n], exactly [len] bytes. */
    fun toBytes(n: BigInteger, len: Int): ByteArray {
        val raw = n.toByteArray()
        val out = ByteArray(len)
        val src = if (raw.size > len) raw.copyOfRange(raw.size - len, raw.size) else raw
        src.copyInto(out, len - src.size)
        raw.fill(0)
        return out
    }
}
