package cz.nowass.dispenser

import android.content.Context
import android.content.pm.ApplicationInfo
import androidx.test.core.app.ApplicationProvider
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
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
}
