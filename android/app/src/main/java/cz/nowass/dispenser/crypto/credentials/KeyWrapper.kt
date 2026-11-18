package cz.nowass.dispenser.crypto.credentials

/**
 * Hardware-backed wrapping of credential records (docs/ANDROID_BLE_DESIGN.md 3.3, 11.1).
 * Implemented by [AndroidKeystoreKeyWrapper] (AES-256-GCM in the Android Keystore) and by fakes in
 * JVM tests. One key per device association (alias derived from the device ID).
 */
interface KeyWrapper {
    /** Creates a new key under [alias], replacing nothing: fails if the alias exists. */
    fun createKey(alias: String)
    fun hasKey(alias: String): Boolean
    /** Returns iv || ciphertext || tag. */
    fun encrypt(alias: String, plaintext: ByteArray, aad: ByteArray): ByteArray
    /** Throws [KeyLostException] if the key is gone/invalidated, [TamperedException] if authentication fails. */
    fun decrypt(alias: String, blob: ByteArray, aad: ByteArray): ByteArray
    fun deleteKey(alias: String)
    /** Where the key lives (diagnostics only; no secret). */
    fun securityLevel(alias: String): SecurityLevel

    enum class SecurityLevel { STRONGBOX, TRUSTED_ENVIRONMENT, SOFTWARE, UNKNOWN }
}

/** The wrapping key no longer exists or was permanently invalidated: the credential is LOST. */
class KeyLostException(cause: Throwable? = null) : Exception("credential key lost", cause)

/** The wrapped record failed authentication (modified, truncated or wrong AAD). */
class TamperedException(cause: Throwable? = null) : Exception("credential record failed authentication", cause)
