package cz.nowass.dispenser.crypto

/**
 * JNI facade of `libdspnoise` (src/main/cpp: pinned noise-c + libsodium, glue only). Internal to
 * the crypto package: callers use [NativeNoiseProvider]. Handles are native pointers owned by the
 * wrappers in this package, freed exactly once. Every method throws [NoiseException] on failure.
 *
 * The APK library offers only the initiator role with CSPRNG ephemerals; test-only entry points
 * (responder, fixed ephemerals) live in the host test library and its test-source-set class.
 */
internal object NativeNoise {
    /** Replaced only by JVM tests (host build of the same sources); production loads the APK library. */
    @Volatile
    internal var libraryLoader: () -> Unit = { System.loadLibrary("dspnoise") }

    @Volatile
    private var loaded = false

    fun ensureLoaded() {
        if (loaded) return
        synchronized(this) {
            if (loaded) return
            libraryLoader()
            val rc = nativeInit()
            if (rc != 0) throw NoiseException(rc)
            loaded = true
        }
    }

    @JvmStatic external fun nativeInit(): Int
    @JvmStatic external fun rngName(): String
    @JvmStatic external fun psk(purpose: Int, setup: ByteArray, token: ByteArray?): ByteArray
    @JvmStatic external fun keypair(): ByteArray
    @JvmStatic external fun publicKey(priv: ByteArray): ByteArray
    @JvmStatic external fun hsNew(pattern: Int, prologue: ByteArray, priv: ByteArray, remotePub: ByteArray?, psk: ByteArray?): Long
    @JvmStatic external fun hsWrite(handle: Long, payload: ByteArray?): ByteArray
    @JvmStatic external fun hsRead(handle: Long, message: ByteArray): ByteArray
    @JvmStatic external fun hsFinished(handle: Long): Boolean
    @JvmStatic external fun hsRemoteStatic(handle: Long): ByteArray
    @JvmStatic external fun hsHash(handle: Long): ByteArray
    @JvmStatic external fun hsSplit(handle: Long): Long
    @JvmStatic external fun hsFree(handle: Long)
    @JvmStatic external fun csEncrypt(handle: Long, ad: ByteArray, plain: ByteArray?): ByteArray
    @JvmStatic external fun csDecrypt(handle: Long, ad: ByteArray, ciphertext: ByteArray): ByteArray
    @JvmStatic external fun csFree(handle: Long)

    const val PATTERN_XXPSK3 = 0
    const val PATTERN_IK = 1
}
