package cz.nowass.dispenser.crypto

import java.security.MessageDigest

/**
 * Public derivations of docs/BLE_PROTOCOL.md 11.1 (no secrets): the Noise prologue and
 * `device_id_hash`. Checked against test-vectors/ble_session_v1.txt.
 */
object SessionDerivation {
    private val PROLOGUE_LABEL = "DSP2 BLE v1".toByteArray(Charsets.US_ASCII)
    const val PROLOGUE_LEN = 26

    fun prologue(major: Int, minor: Int, purpose: Purpose, deviceIdHash: ByteArray, sessionId: Long): ByteArray {
        require(deviceIdHash.size == 8) { "device_id_hash is 8 bytes" }
        require(sessionId in 0..0xFFFF_FFFFL) { "session_id is a u32" }
        val out = ByteArray(PROLOGUE_LEN)
        PROLOGUE_LABEL.copyInto(out, 0)
        out[11] = major.toByte()
        out[12] = minor.toByte()
        out[13] = purpose.wire.toByte()
        deviceIdHash.copyInto(out, 14)
        for (i in 0 until 4) out[22 + i] = (sessionId ushr (8 * i)).toByte()
        return out
    }

    /** CLAIMED: SHA-256("DSP2 id" || identity public key)[0..8). */
    fun claimedDeviceIdHash(identityPublicKey: ByteArray): ByteArray {
        require(identityPublicKey.size == 32)
        return sha256Prefix("DSP2 id", identityPublicKey)
    }

    /** UNCLAIMED / credentials damaged: SHA-256("DSP2 dev" || device ID (6))[0..8). */
    fun unclaimedDeviceIdHash(deviceId: ByteArray): ByteArray {
        require(deviceId.size == 6)
        return sha256Prefix("DSP2 dev", deviceId)
    }

    private fun sha256Prefix(label: String, data: ByteArray): ByteArray {
        val md = MessageDigest.getInstance("SHA-256")
        md.update(label.toByteArray(Charsets.US_ASCII))
        md.update(data)
        return md.digest().copyOf(8)
    }
}
