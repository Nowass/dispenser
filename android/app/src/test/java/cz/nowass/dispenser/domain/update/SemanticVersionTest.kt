package cz.nowass.dispenser.domain.update

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class SemanticVersionTest {
    private fun v(s: String) = SemanticVersion.parse(s)!!

    @Test
    fun parse_validAndInvalid() {
        assertEquals(SemanticVersion(0, 4, 0), v("0.4.0"))
        assertEquals(SemanticVersion(1, 2, 3, listOf("beta", "1")), v("v1.2.3-beta.1"))
        assertEquals("build.5", v("1.0.0+build.5").buildMetadata)
        for (bad in listOf("1.0", "1.0.0.0", "01.0.0", "1.0.0-", "1.0.0-01", "abc", "", "1.0.0 beta", "1.0")) assertNull(bad, SemanticVersion.parse(bad))
    }

    @Test
    fun ordering_isNumeric_notLexicographic() {
        assertTrue(v("0.10.0") > v("0.9.0"))
        assertTrue(v("1.0.10") > v("1.0.9"))
        assertTrue(v("2.0.0") > v("1.99.99"))
    }

    @Test
    fun ordering_followsSemverPrecedence() {
        val ordered = listOf("1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta", "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0").map(::v)
        assertEquals(ordered, ordered.shuffled(java.util.Random(7)).sorted())
        assertTrue(v("1.0.0+a").samePrecedence(v("1.0.0+b")))
        assertTrue(v("0.5.0-beta.1").isPreRelease)
        assertEquals("1.2.3-beta.1+x", v("1.2.3-beta.1+x").toString())
    }

    @Test
    fun channel_isDerivedFromInstalledVersion() {
        assertEquals(ReleaseChannel.STABLE, ReleaseChannel.forInstalledVersion(v("0.4.0")))
        assertEquals(ReleaseChannel.BETA, ReleaseChannel.forInstalledVersion(v("0.5.0-beta.1")))
    }
}
