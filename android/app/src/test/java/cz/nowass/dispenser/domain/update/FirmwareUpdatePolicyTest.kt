package cz.nowass.dispenser.domain.update

import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/** docs/FIRMWARE_UPDATE.md section 3 on scripted release lists and manifests (no network). */
class FirmwareUpdatePolicyTest {
    private val prefix = FirmwareUpdatePolicy.MANIFEST_URL_PREFIX
    private val sha = "0123456789abcdef".repeat(4)

    private fun asset(tag: String, name: String, size: Long? = 1_430_528, digest: String? = null, url: String? = "$prefix$tag/$name") =
        ReleaseAsset(name, url, size, digest)

    private fun fw(
        tag: String, version: String, pre: Boolean = false, draft: Boolean = false, size: Long = 1_430_528,
        digest: String? = null, body: String? = "Release body", extra: List<ReleaseAsset> = emptyList(),
        bin: Boolean = true, json: Boolean = true, jsonUrl: String? = "$prefix$tag/dispenser-firmware-$version.json",
    ) = ReleaseInfo(
        tagName = tag, version = SemanticVersion.parse(tag), isPreRelease = pre, isDraft = draft,
        releasePageUrl = "https://github.com/Nowass/dispenser/releases/tag/$tag", apkAssetNames = emptyList(),
        assets = listOfNotNull(
            asset(tag, "dispenser-firmware-$version.bin", size, digest).takeIf { bin },
            asset(tag, "dispenser-firmware-$version.json", 400, url = jsonUrl).takeIf { json },
        ) + extra,
        body = body,
    )

    private fun manifest(version: String, size: Long = 1_430_528, minInstalled: String? = "0.3.0", notes: String? = null) = FirmwareManifest(
        schema = 1, project = "dispenser", hardware = "xiao-esp32c3", version = version, asset = "dispenser-firmware-$version.bin",
        size = size, sha256 = sha, minInstalled = minInstalled, notes = notes,
    )

    /** Manifests by version; a missing entry is "unavailable". Records which were fetched. */
    private class Fetcher(val manifests: Map<String, FirmwareManifest?>) {
        val fetched = mutableListOf<String>()
        suspend fun fetch(c: FirmwareCandidate): ManifestFetch {
            fetched += c.version
            return if (manifests.containsKey(c.version)) ManifestFetch.Fetched(manifests[c.version]) else ManifestFetch.Unavailable
        }
    }

    private fun select(running: String, releases: List<ReleaseInfo>, beta: Boolean = false, f: Fetcher) =
        runBlocking { FirmwareUpdatePolicy.select(running, releases, beta, f::fetch) }

    private fun offered(r: FirmwareCheckResult): String? = (r as? FirmwareCheckResult.Available)?.offer?.version

    @Test
    fun offers_the_highest_newer_compatible_release() {
        val releases = listOf(fw("firmware-v0.3.1", "0.3.1"), fw("firmware-v0.3.2", "0.3.2"), fw("firmware-v0.2.9", "0.2.9"))
        val f = Fetcher(mapOf("0.3.1" to manifest("0.3.1"), "0.3.2" to manifest("0.3.2", notes = "  Oprava pumpy.  ")))
        val r = select("0.3.0", releases, f = f) as FirmwareCheckResult.Available
        assertEquals("0.3.2", r.offer.version)
        assertEquals("firmware-v0.3.2", r.offer.tag)
        assertEquals(1_430_528L, r.offer.size)
        assertEquals(sha, r.offer.sha256Hex)
        assertEquals("Oprava pumpy.", r.offer.notes) // manifest notes win, trimmed
        assertEquals(listOf("0.3.2"), f.fetched) // only what was needed, newest first
    }

    @Test
    fun nothing_newer_is_up_to_date_and_downloads_nothing() {
        val f = Fetcher(emptyMap())
        assertEquals(FirmwareCheckResult.UpToDate(), select("0.3.1", listOf(fw("firmware-v0.3.1", "0.3.1")), f = f))
        assertEquals(FirmwareCheckResult.UpToDate(), select("0.3.0", emptyList(), f = f))
        assertTrue(f.fetched.isEmpty())
    }

    @Test
    fun drafts_are_ignored_and_prereleases_need_the_beta_channel() {
        val f = Fetcher(mapOf("0.4.0" to manifest("0.4.0"), "0.3.5" to manifest("0.3.5")))
        val releases = listOf(fw("firmware-v0.4.0", "0.4.0", pre = true), fw("firmware-v0.5.0", "0.5.0", draft = true), fw("v0.3.5", "0.3.5"))
        assertEquals("0.3.5", offered(select("0.3.0", releases, beta = false, f = f)))
        val beta = select("0.3.0", releases, beta = true, f = f) as FirmwareCheckResult.Available
        assertEquals("0.4.0", beta.offer.version)
        assertTrue(beta.offer.isPreRelease)
        assertTrue(f.fetched.none { it == "0.5.0" })
    }

    @Test
    fun both_assets_of_the_same_version_are_required() {
        val f = Fetcher(mapOf("0.3.1" to manifest("0.3.1")))
        assertEquals(FirmwareCheckResult.UpToDate(), select("0.3.0", listOf(fw("t1", "0.3.1", json = false)), f = f))
        assertEquals(FirmwareCheckResult.UpToDate(), select("0.3.0", listOf(fw("t1", "0.3.1", bin = false)), f = f))
        // .bin of one version and .json of another
        val mixed = ReleaseInfo("t2", null, false, false, null, emptyList(),
            listOf(asset("t2", "dispenser-firmware-0.3.1.bin"), asset("t2", "dispenser-firmware-0.3.2.json")))
        assertEquals(FirmwareCheckResult.UpToDate(), select("0.3.0", listOf(mixed), f = f))
        assertTrue(f.fetched.isEmpty())
    }

    @Test
    fun names_tags_and_urls_outside_the_rules_are_never_candidates() {
        val cands = FirmwareUpdatePolicy.candidates(
            listOf(
                fw("firmware/v0.3.1", "0.3.1"), // tag charset
                fw("x".repeat(41), "0.3.1"),
                fw("t1", "0.3.01"), // leading zero
                fw("t2", "0.3.1-beta"), // not plain X.Y.Z
                fw("t3", "0.3.70000"),
                fw("t4", "0.3.1", jsonUrl = "https://example.com/dispenser-firmware-0.3.1.json"),
                fw("t5", "0.3.1", jsonUrl = "http://github.com/Nowass/dispenser/releases/download/t5/dispenser-firmware-0.3.1.json"),
                fw("t6", "0.3.1", jsonUrl = null),
                // the .bin is not where the device will download it from
                ReleaseInfo("t7", null, false, false, null, emptyList(), listOf(
                    asset("t7", "dispenser-firmware-0.3.1.bin", url = "${prefix}other/dispenser-firmware-0.3.1.bin"),
                    asset("t7", "dispenser-firmware-0.3.1.json"))),
            ),
            beta = true,
        )
        assertTrue("$cands", cands.isEmpty())
        assertEquals(1, FirmwareUpdatePolicy.candidates(listOf(fw("firmware-v0.3.1_rc.1", "0.3.1")), beta = false).size)
    }

    @Test
    fun manifest_fields_are_checked() {
        val c = FirmwareUpdatePolicy.candidates(listOf(fw("t", "0.3.1")), false).single()
        val running = SemanticVersion.parse("0.3.0")!!
        val good = manifest("0.3.1")
        assertTrue(FirmwareUpdatePolicy.check(c, good, running) is ManifestCheck.Ok)
        val bad = mapOf(
            "json" to null,
            "schema" to good.copy(schema = 2), "schema missing" to good.copy(schema = null),
            "project" to good.copy(project = "other"), "hardware" to good.copy(hardware = "esp32s3"),
            "version" to good.copy(version = "0.3.2"), "asset" to good.copy(asset = "dispenser-firmware-0.3.2.bin"),
            "size" to good.copy(size = 1_430_527), "size missing" to good.copy(size = null),
            "sha upper" to good.copy(sha256 = sha.uppercase()), "sha short" to good.copy(sha256 = sha.drop(1)),
            "sha missing" to good.copy(sha256 = null),
            "min missing" to good.copy(minInstalled = null), "min not plain" to good.copy(minInstalled = "0.3"),
        )
        for ((what, m) in bad) assertTrue(what, FirmwareUpdatePolicy.check(c, m, running) is ManifestCheck.Invalid)
    }

    @Test
    fun size_must_equal_the_asset_and_fit_the_slot() {
        val running = SemanticVersion.parse("0.3.0")!!
        val big = FirmwareUpdatePolicy.candidates(listOf(fw("t", "0.3.1", size = 1_835_009)), false).single()
        assertTrue(FirmwareUpdatePolicy.check(big, manifest("0.3.1", size = 1_835_009), running) is ManifestCheck.Invalid)
        val unknownSize = FirmwareUpdatePolicy.candidates(listOf(ReleaseInfo("t", null, false, false, null, emptyList(),
            listOf(asset("t", "dispenser-firmware-0.3.1.bin", size = null), asset("t", "dispenser-firmware-0.3.1.json")))), false).single()
        assertTrue(FirmwareUpdatePolicy.check(unknownSize, manifest("0.3.1"), running) is ManifestCheck.Invalid)
        // a size mismatch skips the release in the selection
        val f = Fetcher(mapOf("0.3.1" to manifest("0.3.1", size = 1_000_000)))
        assertEquals(FirmwareCheckResult.UpToDate(rejectedNewer = 1), select("0.3.0", listOf(fw("t", "0.3.1")), f = f))
    }

    @Test
    fun github_digest_must_match_when_reported() {
        val running = SemanticVersion.parse("0.3.0")!!
        val match = FirmwareUpdatePolicy.candidates(listOf(fw("t", "0.3.1", digest = "sha256:$sha")), false).single()
        assertTrue(FirmwareUpdatePolicy.check(match, manifest("0.3.1"), running) is ManifestCheck.Ok)
        val other = FirmwareUpdatePolicy.candidates(listOf(fw("t", "0.3.1", digest = "sha256:" + "f".repeat(64))), false).single()
        assertEquals(ManifestCheck.Invalid("digest"), FirmwareUpdatePolicy.check(other, manifest("0.3.1"), running))
    }

    @Test
    fun an_invalid_newest_release_falls_back_to_the_next_one() {
        val releases = listOf(fw("a", "0.3.3"), fw("b", "0.3.2"), fw("c", "0.3.1"))
        val f = Fetcher(mapOf("0.3.3" to manifest("0.3.3").copy(project = "x"), "0.3.1" to manifest("0.3.1"))) // 0.3.2 unavailable
        assertEquals("0.3.1", offered(select("0.3.0", releases, f = f)))
        assertEquals(listOf("0.3.3", "0.3.2", "0.3.1"), f.fetched)
    }

    @Test
    fun min_installed_above_the_running_version_is_not_offered() {
        val releases = listOf(fw("a", "0.5.0"), fw("b", "0.4.0"))
        val f = Fetcher(mapOf("0.5.0" to manifest("0.5.0", minInstalled = "0.4.0"), "0.4.0" to manifest("0.4.0")))
        assertEquals("0.4.0", offered(select("0.3.0", releases, f = f)))
        assertEquals("0.5.0", offered(select("0.4.0", releases, f = f)))
        val none = Fetcher(mapOf("0.5.0" to manifest("0.5.0", minInstalled = "0.4.0")))
        assertEquals(FirmwareCheckResult.UpToDate(incompatibleNewer = "0.5.0"), select("0.3.0", listOf(fw("a", "0.5.0")), f = none))
    }

    @Test
    fun a_dev_build_is_older_than_its_release() {
        val f = Fetcher(mapOf("0.3.0" to manifest("0.3.0", minInstalled = "0.2.0"), "0.3.1" to manifest("0.3.1")))
        val releases = listOf(fw("a", "0.3.0"), fw("b", "0.3.1"))
        // 0.3.0-dev < 0.3.0: 0.3.1 needs 0.3.0 installed, so 0.3.0 itself is offered
        assertEquals("0.3.0", offered(select("0.3.0-dev", releases, f = f)))
        assertEquals(FirmwareCheckResult.UnknownRunningVersion, select("dev-build", releases, f = f))
    }

    @Test
    fun release_body_is_the_fallback_for_notes_and_is_capped() {
        val running = SemanticVersion.parse("0.3.0")!!
        val c = FirmwareUpdatePolicy.candidates(listOf(fw("t", "0.3.1", body = "  Poznámky k vydání\n")), false).single()
        assertEquals("Poznámky k vydání", (FirmwareUpdatePolicy.check(c, manifest("0.3.1", notes = " "), running) as ManifestCheck.Ok).offer.notes)
        val long = FirmwareUpdatePolicy.candidates(listOf(fw("t", "0.3.1", body = "x".repeat(5000))), false).single()
        val notes = (FirmwareUpdatePolicy.check(long, manifest("0.3.1"), running) as ManifestCheck.Ok).offer.notes!!
        assertEquals(FirmwareUpdatePolicy.NOTES_MAX + 1, notes.length)
        val none = FirmwareUpdatePolicy.candidates(listOf(fw("t", "0.3.1", body = null)), false).single()
        assertEquals(null, (FirmwareUpdatePolicy.check(none, manifest("0.3.1"), running) as ManifestCheck.Ok).offer.notes)
    }

    @Test
    fun a_network_failure_while_fetching_stops_the_check() {
        val r = runBlocking {
            FirmwareUpdatePolicy.select("0.3.0", listOf(fw("a", "0.3.1")), false) { ManifestFetch.Failed(UpdateCheckResult.Offline) }
        }
        assertEquals(FirmwareCheckResult.Failed(UpdateCheckResult.Offline), r)
    }
}
