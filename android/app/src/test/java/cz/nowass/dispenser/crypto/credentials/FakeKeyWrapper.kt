package cz.nowass.dispenser.crypto.credentials

import java.security.SecureRandom
import javax.crypto.AEADBadTagException
import javax.crypto.Cipher
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

/** JVM stand-in for the Android Keystore (AES-GCM keys in memory); can simulate key loss. */
class FakeKeyWrapper : KeyWrapper {
    val keys = mutableMapOf<String, ByteArray>()
    private val rnd = SecureRandom()

    override fun createKey(alias: String) {
        check(alias !in keys)
        keys[alias] = ByteArray(32).also { rnd.nextBytes(it) }
    }

    override fun hasKey(alias: String) = alias in keys

    override fun encrypt(alias: String, plaintext: ByteArray, aad: ByteArray): ByteArray {
        val k = keys[alias] ?: throw KeyLostException()
        val iv = ByteArray(12).also { rnd.nextBytes(it) }
        val c = Cipher.getInstance("AES/GCM/NoPadding")
        c.init(Cipher.ENCRYPT_MODE, SecretKeySpec(k, "AES"), GCMParameterSpec(128, iv))
        c.updateAAD(aad)
        return iv + c.doFinal(plaintext)
    }

    override fun decrypt(alias: String, blob: ByteArray, aad: ByteArray): ByteArray {
        val k = keys[alias] ?: throw KeyLostException()
        val c = Cipher.getInstance("AES/GCM/NoPadding")
        c.init(Cipher.DECRYPT_MODE, SecretKeySpec(k, "AES"), GCMParameterSpec(128, blob, 0, 12))
        c.updateAAD(aad)
        try {
            return c.doFinal(blob, 12, blob.size - 12)
        } catch (e: AEADBadTagException) {
            throw TamperedException(e)
        }
    }

    override fun deleteKey(alias: String) {
        keys.remove(alias)
    }

    override fun securityLevel(alias: String) = KeyWrapper.SecurityLevel.SOFTWARE
}
