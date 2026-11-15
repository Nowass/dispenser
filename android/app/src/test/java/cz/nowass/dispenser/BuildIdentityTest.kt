package cz.nowass.dispenser

import android.content.Context
import androidx.test.core.app.ApplicationProvider
import cz.nowass.dispenser.domain.update.ReleaseChannel
import cz.nowass.dispenser.domain.update.SemanticVersion
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

/**
 * A1.5 build identity: debug = cz.nowass.dispenser.debug / "Dispenser DEBUG" / +debug,
 * release = cz.nowass.dispenser / "Dispenser" / plain SemVer. Variant-aware, so it also holds
 * if release unit tests are enabled (AGP runs only testDebugUnitTest by default).
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35])
class BuildIdentityTest {

    private val context: Context = ApplicationProvider.getApplicationContext()

    @Test
    fun applicationId_label_andVersion_matchTheVariant() {
        val container = AppContainer(context)
        val versionName = container.appVersion.name
        val isDebug = versionName.endsWith("+debug")
        assertEquals(if (isDebug) "cz.nowass.dispenser.debug" else "cz.nowass.dispenser", context.packageName)
        assertEquals(if (isDebug) "Dispenser DEBUG" else "Dispenser", context.getString(R.string.app_name))
        assertEquals("0.5.0-beta.1", versionName.substringBefore('+'))
        assertEquals(5L, container.appVersion.code)
    }

    @Test
    fun debugSuffix_isIgnoredByTheUpdateChecker() {
        val debug = SemanticVersion.parse("0.5.0-beta.1+debug")
        assertNotNull(debug)
        assertEquals(0, debug!!.compareTo(SemanticVersion.parse("0.5.0-beta.1")!!))
        assertEquals(ReleaseChannel.BETA, ReleaseChannel.forInstalledVersion(debug))
    }
}
