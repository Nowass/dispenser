package cz.nowass.dispenser

import android.content.Context
import android.content.pm.ApplicationInfo
import androidx.test.core.app.ApplicationProvider
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import androidx.datastore.preferences.preferencesDataStoreFile
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.xmlpull.v1.XmlPullParser

/**
 * A1.5: app data never goes to Android system backup (cloud) or device-to-device transfer;
 * only the explicit JSON export moves data.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35])
class SystemBackupPolicyTest {

    private val context: Context = ApplicationProvider.getApplicationContext()

    @Test
    fun allowBackup_isDisabled() {
        assertEquals(0, context.applicationInfo.flags and ApplicationInfo.FLAG_ALLOW_BACKUP)
    }

    /** Section name -> excluded domains (all with path "."). */
    private fun rules(xmlRes: Int): Map<String, Set<String>> {
        val parser = context.resources.getXml(xmlRes)
        val result = mutableMapOf<String, MutableSet<String>>()
        var section = "root"
        while (parser.next() != XmlPullParser.END_DOCUMENT) {
            if (parser.eventType != XmlPullParser.START_TAG) continue
            when (parser.name) {
                "cloud-backup", "device-transfer" -> section = parser.name
                "full-backup-content", "data-extraction-rules" -> section = parser.name
                "include" -> error("no include rules expected")
                "exclude" -> {
                    assertEquals(".", parser.getAttributeValue(null, "path"))
                    result.getOrPut(section) { mutableSetOf() } += parser.getAttributeValue(null, "domain")
                }
            }
        }
        return result
    }

    @Test
    fun dataExtractionRules_excludeDatabasesSettingsAndFiles_forCloudAndDeviceTransfer() {
        val rules = rules(R.xml.data_extraction_rules)
        val required = setOf("root", "file", "database", "sharedpref", "external", "device_root", "device_file", "device_database", "device_sharedpref")
        assertEquals(required, rules["cloud-backup"])
        assertEquals(required, rules["device-transfer"])
    }

    @Test
    fun legacyBackupRules_excludeEverything() {
        val rules = rules(R.xml.backup_rules)
        assertTrue(rules.getValue("full-backup-content").containsAll(setOf("root", "file", "database", "sharedpref", "external")))
    }

    /**
     * M1.10e.4a: every location of device-mode data is in a domain excluded from BOTH cloud Auto Backup
     * and device-to-device transfer (and from the legacy rules): the mode/device-selection DataStore
     * (files), the device cache and pending-operations journal (`device.db`, databases) and the
     * credential records (no-backup directory, never backed up by the platform, and under the
     * excluded root). None of them is part of the JSON export either (BackupService knows none of them).
     */
    @Test
    fun deviceModeData_isInExcludedDomains() {
        val extraction = rules(R.xml.data_extraction_rules)
        val legacy = rules(R.xml.backup_rules).getValue("full-backup-content")
        fun excludedEverywhere(domain: String) {
            assertTrue("cloud: $domain", domain in extraction.getValue("cloud-backup"))
            assertTrue("transfer: $domain", domain in extraction.getValue("device-transfer"))
            assertTrue("legacy: $domain", domain in legacy)
        }
        val mode = context.preferencesDataStoreFile(cz.nowass.dispenser.data.mode.DataStoreAppModeStore.FILE_NAME).canonicalFile
        assertTrue("$mode under files/", mode.path.startsWith(context.filesDir.canonicalPath + java.io.File.separator))
        excludedEverywhere("file")
        val deviceDb = context.getDatabasePath(cz.nowass.dispenser.data.device.local.DeviceDatabase.FILE_NAME).canonicalFile
        assertTrue("$deviceDb under databases/", deviceDb.parentFile!!.name == "databases")
        excludedEverywhere("database")
        val credentials = credentialDirectory(context).canonicalFile
        assertTrue("$credentials under no_backup/", credentials.path.startsWith(context.noBackupFilesDir.canonicalPath + java.io.File.separator))
        excludedEverywhere("root")
        // The JSON export/restore service references none of these stores.
        val backupSources = java.io.File(System.getProperty("dsp.source.dir")!!, "cz/nowass/dispenser/data/backup").walk().filter { it.isFile }.joinToString("\n") { it.readText() }
        for (forbidden in listOf("DataStoreAppModeStore", "app_mode", "DeviceDatabase", "device.db", "CredentialStore", "credentials")) {
            assertTrue("backup code must not touch $forbidden", forbidden !in backupSources)
        }
    }
}
