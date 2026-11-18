package cz.nowass.dispenser.crypto.credentials

import android.annotation.SuppressLint
import android.os.Build
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyInfo
import android.security.keystore.KeyPermanentlyInvalidatedException
import android.security.keystore.KeyProperties
import android.security.keystore.StrongBoxUnavailableException
import androidx.annotation.RequiresApi
import java.security.KeyStore
import java.security.UnrecoverableKeyException
import javax.crypto.AEADBadTagException
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.SecretKeyFactory
import javax.crypto.spec.GCMParameterSpec

/**
 * [KeyWrapper] in the Android Keystore (decision E6): non-exportable AES-256-GCM key per device
 * association, StrongBox preferred (API 28+) with TEE fallback, randomized encryption required,
 * no per-use user authentication, `setUnlockedDeviceRequired(true)` on API 28+. Keystore keys are
 * never part of any backup. Keystore behavior: instrumented tests; the StrongBox fallback decision:
 * KeystoreFallbackTest (Robolectric, injected generator).
 */
class AndroidKeystoreKeyWrapper internal constructor(
    private val sdkInt: Int,
    private val generator: (alias: String, strongBox: Boolean) -> Unit,
) : KeyWrapper {
    constructor() : this(Build.VERSION.SDK_INT, ::generateKeystoreKey)

    private val ks: KeyStore by lazy { KeyStore.getInstance(PROVIDER).apply { load(null) } }

    override fun createKey(alias: String) {
        check(!ks.containsAlias(alias)) { "key alias already exists" }
        createWithFallback(alias)
    }

    /**
     * StrongBox first on API 28+; on StrongBoxUnavailableException (no StrongBox, or StrongBox
     * cannot hold this key) the key is created in the TEE (or the platform's software keystore on
     * devices without one). Below API 28: TEE directly. Returns whether StrongBox was used.
     */
    // The guard is `sdkInt` (Build.VERSION.SDK_INT in production, injected only by tests), which
    // lint cannot follow; the API 28 call is reached only when sdkInt >= 28.
    @SuppressLint("NewApi")
    internal fun createWithFallback(alias: String): Boolean {
        if (sdkInt >= Build.VERSION_CODES.P && generateInStrongBox(alias)) return true
        generator(alias, false)
        return false
    }

    @RequiresApi(Build.VERSION_CODES.P)
    private fun generateInStrongBox(alias: String): Boolean = try {
        generator(alias, true)
        true
    } catch (e: StrongBoxUnavailableException) {
        false
    }

    private companion object Keys {
        const val PROVIDER = "AndroidKeyStore"
        const val TRANSFORMATION = "AES/GCM/NoPadding"
        const val IV_LEN = 12
        const val TAG_BITS = 128

        fun generateKeystoreKey(alias: String, strongBox: Boolean) {
        val b = KeyGenParameterSpec.Builder(alias, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
            .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
            .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
            .setKeySize(256)
            .setRandomizedEncryptionRequired(true)
            .setUserAuthenticationRequired(false)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            b.setUnlockedDeviceRequired(true)
            if (strongBox) b.setIsStrongBoxBacked(true)
        }
        KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, PROVIDER).apply { init(b.build()) }.generateKey()
        }
    }

    override fun hasKey(alias: String): Boolean = ks.containsAlias(alias)

    private fun key(alias: String): SecretKey = try {
        (ks.getKey(alias, null) as? SecretKey) ?: throw KeyLostException()
    } catch (e: UnrecoverableKeyException) {
        throw KeyLostException(e)
    }

    override fun encrypt(alias: String, plaintext: ByteArray, aad: ByteArray): ByteArray {
        val c = Cipher.getInstance(TRANSFORMATION)
        try {
            c.init(Cipher.ENCRYPT_MODE, key(alias)) // the Keystore generates the IV
        } catch (e: KeyPermanentlyInvalidatedException) {
            throw KeyLostException(e)
        }
        c.updateAAD(aad)
        val ct = c.doFinal(plaintext)
        val iv = c.iv
        check(iv.size == IV_LEN)
        return iv + ct
    }

    override fun decrypt(alias: String, blob: ByteArray, aad: ByteArray): ByteArray {
        if (blob.size < IV_LEN + TAG_BITS / 8) throw TamperedException()
        val c = Cipher.getInstance(TRANSFORMATION)
        try {
            c.init(Cipher.DECRYPT_MODE, key(alias), GCMParameterSpec(TAG_BITS, blob, 0, IV_LEN))
        } catch (e: KeyPermanentlyInvalidatedException) {
            throw KeyLostException(e)
        }
        c.updateAAD(aad)
        try {
            return c.doFinal(blob, IV_LEN, blob.size - IV_LEN)
        } catch (e: AEADBadTagException) {
            throw TamperedException(e)
        }
    }

    override fun deleteKey(alias: String) {
        if (ks.containsAlias(alias)) ks.deleteEntry(alias)
    }

    override fun securityLevel(alias: String): KeyWrapper.SecurityLevel = try {
        val k = key(alias)
        val info = SecretKeyFactory.getInstance(k.algorithm, PROVIDER).getKeySpec(k, KeyInfo::class.java) as KeyInfo
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            when (info.securityLevel) {
                KeyProperties.SECURITY_LEVEL_STRONGBOX -> KeyWrapper.SecurityLevel.STRONGBOX
                KeyProperties.SECURITY_LEVEL_TRUSTED_ENVIRONMENT -> KeyWrapper.SecurityLevel.TRUSTED_ENVIRONMENT
                KeyProperties.SECURITY_LEVEL_SOFTWARE -> KeyWrapper.SecurityLevel.SOFTWARE
                else -> KeyWrapper.SecurityLevel.UNKNOWN
            }
        } else {
            @Suppress("DEPRECATION")
            if (info.isInsideSecureHardware) KeyWrapper.SecurityLevel.TRUSTED_ENVIRONMENT else KeyWrapper.SecurityLevel.SOFTWARE
        }
    } catch (e: Exception) {
        KeyWrapper.SecurityLevel.UNKNOWN
    }
}
