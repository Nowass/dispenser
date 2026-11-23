package cz.nowass.dispenser.domain.update

import cz.nowass.dispenser.domain.update.ReleaseChannel.BETA
import cz.nowass.dispenser.domain.update.ReleaseChannel.STABLE
import org.junit.Assert.assertEquals
import org.junit.Test

class UpdatePolicyTest {
    private fun v(s: String) = SemanticVersion.parse(s)!!
    private fun rel(tag: String, pre: Boolean = false, draft: Boolean = false, apk: Boolean = true, url: String? = "https://github.com/Nowass/dispenser/releases/tag/$tag") =
        ReleaseInfo(tag, SemanticVersion.parse(tag), pre, draft, url, if (apk) listOf("dispenser-$tag.apk") else emptyList())

    private val installed = v("0.4.0")

    @Test
    fun noReleases_andDraftsOnly() {
        assertEquals(UpdateCheckResult.NoReleases, UpdatePolicy.evaluate(installed, STABLE, emptyList()))
        assertEquals(UpdateCheckResult.NoReleases, UpdatePolicy.evaluate(installed, STABLE, listOf(rel("v0.9.0", draft = true))))
    }

    @Test
    fun upToDate_whenNothingNewer() {
        assertEquals(UpdateCheckResult.UpToDate(v("0.4.0")), UpdatePolicy.evaluate(installed, STABLE, listOf(rel("v0.3.0"), rel("v0.4.0"))))
    }

    @Test
    fun stable_offersNewestStableWithApk_neverBeta() {
        val releases = listOf(rel("v0.5.0"), rel("v0.6.0-beta.1", pre = true), rel("v0.4.1"))
        val r = UpdatePolicy.evaluate(installed, STABLE, releases) as UpdateCheckResult.UpdateAvailable
        assertEquals(v("0.5.0"), r.version)
        assertEquals("dispenser-v0.5.0.apk", r.apkAssetName)
        assertEquals(false, r.isPreRelease)
        // Only a beta is newer -> stable installation stays up to date (no silent channel switch).
        assertEquals(UpdateCheckResult.UpToDate(v("0.4.0")), UpdatePolicy.evaluate(installed, STABLE, listOf(rel("v0.4.0"), rel("v0.5.0-beta.1", pre = true))))
    }

    @Test
    fun prereleaseFlag_orSemverPrerelease_bothCountAsBeta() {
        // GitHub marks pre-release but tag is plain, or tag is a pre-release without the flag.
        assertEquals(UpdateCheckResult.UpToDate(null), UpdatePolicy.evaluate(installed, STABLE, listOf(rel("v0.5.0", pre = true))))
        assertEquals(UpdateCheckResult.UpToDate(null), UpdatePolicy.evaluate(installed, STABLE, listOf(rel("v0.5.0-rc.1"))))
    }

    @Test
    fun beta_considersBetaAndStable_offersNewest() {
        val beta = v("0.5.0-beta.1")
        val r1 = UpdatePolicy.evaluate(beta, BETA, listOf(rel("v0.5.0-beta.2", pre = true), rel("v0.4.0"))) as UpdateCheckResult.UpdateAvailable
        assertEquals(v("0.5.0-beta.2"), r1.version)
        assertEquals(true, r1.isPreRelease)
        // Stable 0.5.0 is newer than 0.5.0-beta.x and is offered to beta users.
        val r2 = UpdatePolicy.evaluate(beta, BETA, listOf(rel("v0.5.0-beta.2", pre = true), rel("v0.5.0"))) as UpdateCheckResult.UpdateAvailable
        assertEquals(v("0.5.0"), r2.version)
        assertEquals(false, r2.isPreRelease)
    }

    @Test
    fun missingApk_fallsBackToOlderNewerRelease_orReportsIt() {
        val r = UpdatePolicy.evaluate(installed, STABLE, listOf(rel("v0.6.0", apk = false), rel("v0.5.0"))) as UpdateCheckResult.UpdateAvailable
        assertEquals(v("0.5.0"), r.version)
        val none = UpdatePolicy.evaluate(installed, STABLE, listOf(rel("v0.6.0", apk = false)))
        assertEquals(UpdateCheckResult.NewerReleaseWithoutApk(v("0.6.0"), "https://github.com/Nowass/dispenser/releases/tag/v0.6.0"), none)
    }

    @Test
    fun invalidMetadata_whenNoUsableRelease() {
        assertEquals(UpdateCheckResult.InvalidMetadata, UpdatePolicy.evaluate(installed, STABLE, listOf(rel("latest"), rel("nightly"))))
        assertEquals(UpdateCheckResult.InvalidMetadata, UpdatePolicy.evaluate(installed, STABLE, listOf(rel("v0.5.0", url = null))))
        // Invalid entries are ignored when valid ones exist.
        assertEquals(v("0.5.0"), (UpdatePolicy.evaluate(installed, STABLE, listOf(rel("nightly"), rel("v0.5.0"))) as UpdateCheckResult.UpdateAvailable).version)
    }

    @Test
    fun firmwareOnlyRelease_neverAffectsTheAppCheck() {
        val fwOnly = ReleaseInfo(
            "firmware-v0.3.1", SemanticVersion.parse("firmware-v0.3.1"), false, false,
            "https://github.com/Nowass/dispenser/releases/tag/firmware-v0.3.1", emptyList(),
            assets = listOf(
                ReleaseAsset("dispenser-firmware-0.3.1.bin", "https://github.com/Nowass/dispenser/releases/download/firmware-v0.3.1/dispenser-firmware-0.3.1.bin", 100),
                ReleaseAsset("dispenser-firmware-0.3.1.json", "https://github.com/Nowass/dispenser/releases/download/firmware-v0.3.1/dispenser-firmware-0.3.1.json", 10),
            ),
        )
        assertEquals(null, fwOnly.version)
        assertEquals(true, fwOnly.isFirmwareOnly)
        // alone: no app release at all (not InvalidMetadata)
        assertEquals(UpdateCheckResult.NoReleases, UpdatePolicy.evaluate(installed, STABLE, listOf(fwOnly)))
        // next to app releases: ignored, the app result is unchanged
        assertEquals(UpdateCheckResult.UpToDate(v("0.4.0")), UpdatePolicy.evaluate(installed, STABLE, listOf(fwOnly, rel("v0.4.0"))))
        assertEquals(v("0.5.0"), (UpdatePolicy.evaluate(installed, BETA, listOf(fwOnly, rel("v0.5.0"))) as UpdateCheckResult.UpdateAvailable).version)
        // a release carrying an APK and firmware under a semantic tag stays an app release
        val both = rel("v0.6.0").copy(assets = fwOnly.assets)
        assertEquals(false, both.isFirmwareOnly)
        assertEquals(v("0.6.0"), (UpdatePolicy.evaluate(installed, STABLE, listOf(both)) as UpdateCheckResult.UpdateAvailable).version)
    }
}
