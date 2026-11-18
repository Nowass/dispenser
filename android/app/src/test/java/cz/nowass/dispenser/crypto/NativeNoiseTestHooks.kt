package cz.nowass.dispenser.crypto

/**
 * TEST-ONLY native entry points (src/test/native/dspnoise_test_hooks.c), present only in the host
 * test library libdspnoise_hosttest and never in the APK: responder role and fixed ephemerals for
 * the shared transcripts and the official Noise vectors.
 */
object NativeNoiseTestHooks {
    @JvmStatic external fun hsNewTest(pattern: Int, initiator: Boolean, prologue: ByteArray, priv: ByteArray,
                                      remotePub: ByteArray?, psk: ByteArray?, fixedEphemeral: ByteArray?): Long
    @JvmStatic external fun isTestBuild(): Boolean

    /** A handshake (either role) with optional fixed ephemeral, wrapped like production handshakes. */
    fun handshake(xx: Boolean, initiator: Boolean, prologue: ByteArray, priv: ByteArray, remotePub: ByteArray?,
                  psk: ByteArray?, eph: ByteArray?): HandshakeInitiator =
        NativeHandshake(hsNewTest(if (xx) NativeNoise.PATTERN_XXPSK3 else NativeNoise.PATTERN_IK, initiator, prologue, priv, remotePub, psk, eph))
}
