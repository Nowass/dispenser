package cz.nowass.dispenser.data.about

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

class AboutContentTest {

    /** Unit tests run with the app module as working directory. */
    private val changelogFile = File("../../CHANGELOG.md")
    private val assetsDir = File("src/main/assets")

    @Test
    fun parser_handlesSectionsGroupsAndLinks() {
        val md = """
            # Changelog
            Úvod s [odkazem](https://example.com).

            ## [Nevydáno] – vývojové milníky
            ### Přidáno
            - Položka **A**
            ### Opraveno
            - Oprava B

            ## [0.5.0] – 2027-01-10
            ### Přidáno
            - Vydaná položka
        """.trimIndent()
        val c = ChangelogParser.parse(md)
        assertEquals(listOf("Úvod s odkazem."), c.intro)
        assertEquals(listOf("Nevydáno – vývojové milníky", "0.5.0 – 2027-01-10"), c.sections.map { it.title })
        assertTrue(c.sections[0].isUnreleased)
        assertEquals(listOf("Položka A"), c.sections[0].groups[0].items)
        assertEquals("0.5.0", c.relevantFor("0.5.0")!!.label)
        assertTrue(c.relevantFor("0.4.0")!!.isUnreleased)
    }

    @Test
    fun realChangelog_isCzech_andContainsNoInventedReleases() {
        val c = ChangelogParser.parse(changelogFile.readText())
        assertEquals(1, c.sections.size)
        assertTrue(c.sections.single().isUnreleased)
        assertEquals(listOf("Přidáno", "Změněno", "Opraveno"), c.sections.single().groups.map { it.title })
        assertTrue(c.intro.any { "nebyla zveřejněna žádná verze" in it })
        assertTrue(c.relevantFor("0.4.0")!!.isUnreleased)
    }

    @Test
    fun generatedLicenseAsset_isComplete_andEveryTextExists() {
        val libs = LicenseCatalog.parse(File(assetsDir, LicenseCatalog.DATA_PATH).readText())
        assertNotNull(libs)
        assertTrue(libs!!.size > 50)
        for (lib in libs) {
            assertTrue("${lib.artifact} has no license", lib.licenses.isNotEmpty())
            val spdx = lib.licenses.mapNotNull { it.spdx }
            assertTrue("${lib.artifact}: unidentified license ${lib.licenses}", spdx.size == lib.licenses.size || lib.noticePath != null)
            for (id in spdx) assertTrue("missing text for $id", File(assetsDir, LicenseCatalog.textPath(id)).exists() || lib.noticePath != null)
            lib.noticePath?.let { assertTrue("missing notice $it", File(assetsDir, it).exists() && LicenseCatalog.isAllowedTextPath(it)) }
        }
        // BSD-3-Clause requires the copyright notice: provided via the per-library notice.
        val protobuf = libs.single { it.artifact == "datastore-preferences-external-protobuf" }
        assertTrue(File(assetsDir, protobuf.noticePath!!).readText().contains("Copyright 2008 Google Inc."))
        assertTrue(libs.any { it.artifact == "listenablefuture" }) // license inherited from parent POM
    }

    @Test
    fun licenseTextPaths_areRestricted() {
        assertTrue(LicenseCatalog.isAllowedTextPath("licenses/texts/Apache-2.0.txt"))
        assertFalse(LicenseCatalog.isAllowedTextPath("licenses/texts/../../CHANGELOG.md"))
        assertFalse(LicenseCatalog.isAllowedTextPath("CHANGELOG.md"))
    }
}
