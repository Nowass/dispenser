package cz.nowass.dispenser.crypto

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest

/**
 * Owner invitation `DSP2I1:` (docs/ANDROID_BLE_DESIGN.md 7.1 B). 79-byte payload in strict
 * canonical Crockford base32. The check digest is UNKEYED: it detects accidental corruption only,
 * never modification or authenticity. Authentication comes only from the device: HELLO
 * `device_id_hash`, the device static key in XX message 2 equal to [deviceStaticPub], and the
 * XXpsk3 handshake with PSK(setup code, token). [inviteToken] is secret; the device alone decides
 * validity (expiry, single use).
 */
class Invitation(
    val deviceId: ByteArray,
    deviceStaticPub: ByteArray,
    val inviteToken: SecretBytes,
    val expiresUtc: Long,
    val createdUtc: Long,
) : AutoCloseable {
    private val pub = deviceStaticPub.copyOf()
    val deviceStaticPub: ByteArray get() = pub.copyOf()

    /** Expected HELLO `device_id_hash` of the inviting device. */
    val expectedDeviceIdHash: ByteArray get() = SessionDerivation.claimedDeviceIdHash(pub)

    /** True if the device static key received in XX message 2 is the invitation's (constant time). */
    fun matchesDeviceKey(received: ByteArray): Boolean = MessageDigest.isEqual(received, pub)

    enum class Timing { OK, EXPIRED, CLOCK_WARNING }

    /** Convenience checks on this phone's clock (untrusted; the device decides). */
    fun timing(nowUtc: Long): Timing = when {
        nowUtc > expiresUtc + EXPIRY_GRACE_S -> Timing.EXPIRED
        createdUtc > nowUtc + CLOCK_SKEW_S -> Timing.CLOCK_WARNING
        else -> Timing.OK
    }

    override fun close() = inviteToken.close()
    override fun toString(): String = "Invitation(device=${deviceId.joinToString("") { "%02x".format(it) }}, token=**)"

    enum class Problem { PREFIX, LENGTH, CHARSET, PAD, CHECK, UNSUPPORTED, ZERO_KEY, SPAN }

    sealed interface Result {
        class Ok(val invitation: Invitation) : Result
        data class Invalid(val problem: Problem) : Result
    }

    companion object {
        const val PREFIX = "DSP2I1:"
        const val PAYLOAD_LEN = 79
        const val TEXT_CHARS = 127
        const val MAX_LIFETIME_S = 600L
        const val EXPIRY_GRACE_S = 120L
        const val CLOCK_SKEW_S = 300L
        private val CHECK_LABEL = "DSP2 invite v1".toByteArray(Charsets.US_ASCII)

        /** Strict parse of the scanned QR text (canonical form only; no manual entry). */
        fun parse(text: String): Result {
            if (!text.startsWith(PREFIX)) return Result.Invalid(Problem.PREFIX)
            val body = text.substring(PREFIX.length)
            if (body.length != TEXT_CHARS) return Result.Invalid(Problem.LENGTH)
            val n = Crockford.decodeStrict(body) ?: return Result.Invalid(Problem.CHARSET)
            if (n.testBit(0) || n.testBit(1) || n.testBit(2)) return Result.Invalid(Problem.PAD)
            val p = Crockford.toBytes(n.shiftRight(3), PAYLOAD_LEN)
            try {
                if (!MessageDigest.isEqual(digest(p.copyOf(63)), p.copyOfRange(63, 79))) return Result.Invalid(Problem.CHECK)
                if (p[0].toInt() != 1) return Result.Invalid(Problem.UNSUPPORTED)
                val pub = p.copyOfRange(7, 39)
                if (pub.all { it.toInt() == 0 }) return Result.Invalid(Problem.ZERO_KEY)
                val bb = ByteBuffer.wrap(p, 55, 8).order(ByteOrder.LITTLE_ENDIAN)
                val expires = bb.int.toLong() and 0xFFFF_FFFFL
                val created = bb.int.toLong() and 0xFFFF_FFFFL
                if (created > expires || expires - created > MAX_LIFETIME_S) return Result.Invalid(Problem.SPAN)
                return Result.Ok(Invitation(p.copyOfRange(1, 7), pub, SecretBytes.wrap(p.copyOfRange(39, 55)), expires, created))
            } finally {
                p.fill(0)
            }
        }

        /** Builds the QR text on the inviting owner phone from the encrypted INVITE_CREATE response. */
        fun encode(deviceId: ByteArray, deviceStaticPub: ByteArray, inviteToken: SecretBytes, createdUtc: Long, expiresInS: Int): String {
            require(deviceId.size == 6 && deviceStaticPub.size == 32)
            require(expiresInS in 0..MAX_LIFETIME_S.toInt() && createdUtc in 0..0xFFFF_FFFFL - MAX_LIFETIME_S)
            val p = ByteArray(PAYLOAD_LEN)
            try {
                p[0] = 1
                deviceId.copyInto(p, 1)
                deviceStaticPub.copyInto(p, 7)
                inviteToken.read { require(it.size == 16); it.copyInto(p, 39) }
                ByteBuffer.wrap(p, 55, 8).order(ByteOrder.LITTLE_ENDIAN).putInt((createdUtc + expiresInS).toInt()).putInt(createdUtc.toInt())
                digest(p.copyOf(63)).copyInto(p, 63)
                return PREFIX + Crockford.encode(p, TEXT_CHARS)
            } finally {
                p.fill(0)
            }
        }

        private fun digest(body63: ByteArray): ByteArray {
            val md = MessageDigest.getInstance("SHA-256")
            md.update(CHECK_LABEL)
            md.update(body63)
            body63.fill(0)
            return md.digest().copyOf(16)
        }
    }
}
