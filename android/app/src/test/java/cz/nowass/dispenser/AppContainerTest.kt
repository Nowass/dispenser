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
    fun wipe() {
        context.deleteDatabase(DispenserDatabase.FILE_NAME)
        context.deleteDatabase(cz.nowass.dispenser.data.local.simulation.SimulationDatabase.FILE_NAME)
        java.io.File(context.filesDir, cz.nowass.dispenser.data.backup.DirectoryRestoreFiles.DIRECTORY).deleteRecursively()
    }

    /** R1 gate: with a pending restore journal no store may be used until recovery finished. */
    @Test
    fun pendingRestoreJournal_blocksStoreAccessUntilRecovered() = runBlocking<Unit> {
        val dir = java.io.File(context.filesDir, cz.nowass.dispenser.data.backup.DirectoryRestoreFiles.DIRECTORY).apply { mkdirs() }
        java.io.File(dir, cz.nowass.dispenser.data.backup.DirectoryRestoreFiles.JOURNAL_FILE)
            .writeBytes(cz.nowass.dispenser.data.backup.SafetySnapshotCodec.encodeJournal("missing"))
        val container = AppContainer(context)
        org.junit.Assert.assertEquals(cz.nowass.dispenser.data.backup.StorageState.Recovering, container.backup.state.value)
        org.junit.Assert.assertThrows(IllegalStateException::class.java) { container.repository }
        org.junit.Assert.assertThrows(IllegalStateException::class.java) { container.simulation }
        org.junit.Assert.assertThrows(IllegalStateException::class.java) { container.appSettings }
        // Snapshot missing: recovery fails safely and access stays blocked.
        val blocked = container.backup.recoverAtStartup()
        assertTrue(blocked is cz.nowass.dispenser.data.backup.StorageState.Blocked)
        org.junit.Assert.assertThrows(IllegalStateException::class.java) { container.repository }
    }

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

    @Test
    fun settings_areSeparateFromSchedulesAndSimulation() = runBlocking {
        val container = AppContainer(context)
        val repo = container.repository
        repo.createSchedule(cz.nowass.dispenser.domain.ScheduleDraft("Ranní", 10.0, java.time.LocalDate.of(2026, 11, 1), java.time.LocalTime.of(8, 0), cz.nowass.dispenser.domain.RecurrenceKind.DAILY))
        container.simulation.start(java.time.Instant.parse("2026-11-20T06:00:00Z"), 500_000, 400_000)
        container.simulation.advanceDays(3)
        val schedules = repo.observeSchedules().first()
        val history = repo.observeHistory().first()

        container.appSettings.setDisplayName("Obývák")
        assertTrue(container.appSettings.displayName.first() == "Obývák")
        assertTrue(repo.observeSchedules().first() == schedules)
        assertTrue(repo.observeHistory().first() == history)
        assertTrue(container.appVersion.name == "0.4.0" && container.appVersion.code == 4L)
        container.appSettings.setDisplayName(null)
    }

    @Test
    fun aboutConfiguration_sourceLinkDisabledWhilePrivate_versionFromPackage() {
        val container = AppContainer(context)
        assertTrue(container.sourceCodeUrl == null) // dispenser.github.sourceLinkEnabled=false
        assertTrue(container.appVersion.name == "0.4.0")
        assertTrue(container.assets.read("CHANGELOG.md")?.contains("Nevydáno") == true)
        assertTrue(container.assets.read("licenses/third_party_licenses.json") != null)
    }
}
