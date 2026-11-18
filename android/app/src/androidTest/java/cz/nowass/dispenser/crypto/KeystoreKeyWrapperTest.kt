package cz.nowass.dispenser.crypto

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import cz.nowass.dispenser.crypto.credentials.AndroidKeystoreKeyWrapper
import cz.nowass.dispenser.crypto.credentials.CredentialState
import cz.nowass.dispenser.crypto.credentials.CredentialStore
import cz.nowass.dispenser.crypto.credentials.KeyLostException
import cz.nowass.dispenser.crypto.credentials.KeyWrapper
import cz.nowass.dispenser.crypto.credentials.TamperedException
import org.junit.After
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

/** Android Keystore wrapping (decision E6) and the credential store on a real ART/Keystore (emulator or test phone). */
@RunWith(AndroidJUnit4::class)
class KeystoreKeyWrapperTest {
    private val ctx = InstrumentationRegistry.getInstrumentation().targetContext
    private val wrapper = AndroidKeystoreKeyWrapper()
    private val alias = "dsp_cred_v1_test_${System.nanoTime()}"
    private val dir = File(ctx.noBackupFilesDir, "credentials-test-${System.nanoTime()}")

    @After
    fun cleanup() {
        wrapper.deleteKey(alias)
        dir.deleteRecursively()
    }

    @Test
    fun wrap_unwrap_aad_tamper_and_loss() {
        wrapper.createKey(alias)
        assertTrue(wrapper.hasKey(alias))
        val plain = ByteArray(145) { it.toByte() }
        val a = wrapper.encrypt(alias, plain, byteArrayOf(1, 2))
        val b = wrapper.encrypt(alias, plain, byteArrayOf(1, 2))
        assertNotEquals(a.toList(), b.toList()) // randomized IV
        assertArrayEquals(plain, wrapper.decrypt(alias, a, byteArrayOf(1, 2)))
        try {
            wrapper.decrypt(alias, a, byteArrayOf(1, 3))
            fail("wrong AAD accepted")
        } catch (_: TamperedException) {
        }
        a[a.size - 1] = (a[a.size - 1].toInt() xor 1).toByte()
        try {
            wrapper.decrypt(alias, a, byteArrayOf(1, 2))
            fail("tampered blob accepted")
        } catch (_: TamperedException) {
        }
        assertNotEquals(KeyWrapper.SecurityLevel.UNKNOWN, wrapper.securityLevel(alias))
        wrapper.deleteKey(alias)
        assertFalse(wrapper.hasKey(alias))
        try {
            wrapper.decrypt(alias, b, byteArrayOf(1, 2))
            fail("decrypt without key")
        } catch (_: KeyLostException) {
        }
    }

    /** The real StrongBox path: StrongBox where the device has it, else the fallback (TEE/software). */
    @Test
    fun strongbox_or_fallback_matches_the_device() {
        wrapper.createKey(alias)
        val level = wrapper.securityLevel(alias)
        val hasStrongBox = android.os.Build.VERSION.SDK_INT >= 28 &&
            ctx.packageManager.hasSystemFeature(android.content.pm.PackageManager.FEATURE_STRONGBOX_KEYSTORE)
        if (hasStrongBox) {
            assertEquals(KeyWrapper.SecurityLevel.STRONGBOX, level)
        } else {
            assertNotEquals(KeyWrapper.SecurityLevel.STRONGBOX, level)
            assertNotEquals(KeyWrapper.SecurityLevel.UNKNOWN, level)
        }
        // the key works whichever path created it
        val blob = wrapper.encrypt(alias, byteArrayOf(1, 2, 3), byteArrayOf(9))
        assertArrayEquals(byteArrayOf(1, 2, 3), wrapper.decrypt(alias, blob, byteArrayOf(9)))
        android.util.Log.i("KeystoreTest", "strongbox feature=$hasStrongBox level=$level")
    }

    @Test
    fun credential_store_with_the_keystore_and_native_keys() {
        val store = CredentialStore(dir, wrapper, NativeNoiseProvider())
        val p = store.beginEnrollment("a0b1c2d3e4f5")
        store.commit(p, 1, ByteArray(32) { 3 }, "Test", 1795000000L).close()
        val c = (store.load("a0b1c2d3e4f5") as CredentialState.Present).credential
        assertEquals(1, c.slot)
        c.close()
        assertEquals(1L, store.nextManualSeq("a0b1c2d3e4f5", 1))
        store.delete("a0b1c2d3e4f5")
        assertEquals(CredentialState.Absent, store.load("a0b1c2d3e4f5"))
    }
}
