package cz.nowass.dispenser.data

import androidx.datastore.preferences.core.PreferenceDataStoreFactory
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.job
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

class AppSettingsRepositoryTest {

    @get:Rule val tmp = TemporaryFolder()

    @Test
    fun displayNameRules_normalizeAndValidate() {
        assertNull(DisplayNameRules.normalize(null))
        assertNull(DisplayNameRules.normalize("   "))
        assertEquals("Obývák 120 l", DisplayNameRules.normalize("  Obývák \t 120   l  "))
        assertEquals("AB", DisplayNameRules.normalize("A\u0000B\n"))
        assertNull(DisplayNameRules.validate("x".repeat(DisplayNameRules.MAX_LENGTH)))
        assertNull(DisplayNameRules.validate("  " + "x".repeat(DisplayNameRules.MAX_LENGTH) + "  ")) // trimmed first
        assertEquals(DisplayNameRules.Error.TOO_LONG, DisplayNameRules.validate("x".repeat(DisplayNameRules.MAX_LENGTH + 1)))
    }

    private fun open(file: File, scope: CoroutineScope) =
        DataStoreAppSettingsRepository(PreferenceDataStoreFactory.create(scope = scope) { file })

    @Test
    fun displayName_persistsAcrossReopen_defaultWhenAbsent_resetRemovesIt() = runBlocking {
        val file = File(tmp.root, "app_settings.preferences_pb")
        val scope1 = CoroutineScope(Dispatchers.IO + SupervisorJob())
        val first = open(file, scope1)
        assertNull(first.displayName.first())
        first.setDisplayName("  Obývák  ")
        assertEquals("Obývák", first.displayName.first())
        // Only one active DataStore per file: DataStore releases the file when its scope has COMPLETED
        // (cancel() only requests it; reopening before completion throws "multiple DataStores active").
        scope1.coroutineContext.job.cancelAndJoin()

        val scope2 = CoroutineScope(Dispatchers.IO + SupervisorJob())
        val reopened = open(file, scope2)
        assertEquals("Obývák", reopened.displayName.first())
        reopened.setDisplayName("   ")
        assertNull(reopened.displayName.first())
        assertThrows(IllegalArgumentException::class.java) { runBlocking { reopened.setDisplayName("x".repeat(31)) } }
        assertNull(reopened.displayName.first())
        scope2.coroutineContext.job.cancelAndJoin()
    }

    @Test
    fun betaFirmwareChannel_offByDefault_persists_andLeavesTheNameAlone() = runBlocking {
        val file = File(tmp.root, "app_settings.preferences_pb")
        val scope1 = CoroutineScope(Dispatchers.IO + SupervisorJob())
        val first = open(file, scope1)
        assertEquals(false, first.betaFirmwareChannel.first())
        first.setDisplayName("Obývák")
        first.setBetaFirmwareChannel(true)
        assertEquals(true, first.betaFirmwareChannel.first())
        scope1.coroutineContext.job.cancelAndJoin()

        val scope2 = CoroutineScope(Dispatchers.IO + SupervisorJob())
        val reopened = open(file, scope2)
        assertEquals(true, reopened.betaFirmwareChannel.first())
        assertEquals("Obývák", reopened.captureRaw()) // the backup snapshot is the name only
        reopened.setBetaFirmwareChannel(false)
        assertEquals(false, reopened.betaFirmwareChannel.first())
        scope2.coroutineContext.job.cancelAndJoin()
    }
}
