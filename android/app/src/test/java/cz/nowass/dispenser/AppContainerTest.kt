package cz.nowass.dispenser

import android.content.Context
import androidx.test.core.app.ApplicationProvider
import cz.nowass.dispenser.data.local.DispenserDatabase
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import org.junit.After
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import java.time.Duration
import java.time.Instant

/** The production wiring: fresh install is empty, "now" is the phone clock, DEMO stays on. */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35])
class AppContainerTest {

    private val context: Context = ApplicationProvider.getApplicationContext()

    @Before @After
    fun wipe() { context.deleteDatabase(DispenserDatabase.FILE_NAME) }

    @Test
    fun freshInstall_hasNoSchedules_noDemoSeed() = runBlocking {
        val container = AppContainer(context)
        assertTrue(container.repository.observeSchedules().first().isEmpty())
        assertTrue(container.isDemo)
    }

    @Test
    fun deviceTime_isThePhoneClock_notTheFixtureDate() = runBlocking {
        val now = AppContainer(context).repository.observeDevice().first().now
        assertTrue(Duration.between(now, Instant.now()).abs() < Duration.ofSeconds(5))
    }
}
