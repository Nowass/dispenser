package cz.nowass.dispenser.crypto.credentials

import android.security.keystore.StrongBoxUnavailableException
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import java.security.ProviderException

/** StrongBox preference and fallback of [AndroidKeystoreKeyWrapper] (decision E6, 11.1). */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35])
class KeystoreFallbackTest {
    private val calls = mutableListOf<Boolean>()

    private fun wrapper(sdk: Int, strongBoxPresent: Boolean, otherFailure: Boolean = false) =
        AndroidKeystoreKeyWrapper(sdk) { _, strongBox ->
            calls += strongBox
            if (strongBox && otherFailure) throw ProviderException("keystore failure")
            if (strongBox && !strongBoxPresent) throw StrongBoxUnavailableException("no StrongBox")
        }

    @Test
    fun strongbox_used_when_present() {
        assertTrue(wrapper(35, strongBoxPresent = true).createWithFallback("a"))
        assertEquals(listOf(true), calls)
    }

    @Test
    fun falls_back_to_tee_when_strongbox_unavailable() {
        assertFalse(wrapper(35, strongBoxPresent = false).createWithFallback("a"))
        assertEquals(listOf(true, false), calls) // one StrongBox attempt, then the TEE key
    }

    @Test
    fun below_api_28_no_strongbox_attempt() {
        assertFalse(wrapper(27, strongBoxPresent = true).createWithFallback("a"))
        assertEquals(listOf(false), calls)
    }

    @Test
    fun other_keystore_failures_are_not_hidden_by_the_fallback() {
        try {
            wrapper(35, strongBoxPresent = true, otherFailure = true).createWithFallback("a")
            fail("failure swallowed")
        } catch (_: ProviderException) {
        }
        assertEquals(listOf(true), calls) // no silent weaker key after an unexpected error
    }
}
