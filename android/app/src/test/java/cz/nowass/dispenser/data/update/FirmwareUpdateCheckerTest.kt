package cz.nowass.dispenser.data.update

import cz.nowass.dispenser.domain.update.FirmwareCheckResult
import cz.nowass.dispenser.domain.update.ManifestFetch
import cz.nowass.dispenser.domain.update.ReleaseAsset
import cz.nowass.dispenser.domain.update.UpdateCheckResult
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.runBlocking
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

/** Firmware release discovery: parser extension, manifest download (local MockWebServer only), checker wiring. */
class FirmwareUpdateCheckerTest {
    private lateinit var server: MockWebServer
    private val sha = "ab".repeat(32)
    private val dl = "https://github.com/Nowass/dispenser/releases/download/firmware-v0.3.1/"

    @Before fun setUp() { server = MockWebServer(); server.start() }
    @After fun tearDown() { runCatching { server.shutdown() } }

    private val releasesJson = """[{"tag_name":"firmware-v0.3.1","draft":false,"prerelease":false,
        "html_url":"https://github.com/Nowass/dispenser/releases/tag/firmware-v0.3.1","body":"  Opravy.\n",
        "assets":[
          {"name":"dispenser-firmware-0.3.1.bin","browser_download_url":"${dl}dispenser-firmware-0.3.1.bin","size":1430528,"digest":"sha256:$sha","content_type":"application/octet-stream"},
          {"name":"dispenser-firmware-0.3.1.json","browser_download_url":"${dl}dispenser-firmware-0.3.1.json","size":321,"digest":null}
        ]}]"""

    private val manifestJson = """{"schema":1,"project":"dispenser","hardware":"xiao-esp32c3","version":"0.3.1",
        "asset":"dispenser-firmware-0.3.1.bin","size":1430528,"sha256":"$sha","min_installed":"0.3.0"}"""

    @Test
    fun parser_keeps_assets_body_and_digest_and_the_apk_list() {
        val r = GitHubReleaseParser.parse(releasesJson)!!.single()
        assertNull(r.version)
        assertTrue(r.apkAssetNames.isEmpty())
        assertEquals("  Opravy.\n", r.body)
        assertEquals(
            listOf(
                ReleaseAsset("dispenser-firmware-0.3.1.bin", "${dl}dispenser-firmware-0.3.1.bin", 1_430_528, "sha256:$sha"),
                ReleaseAsset("dispenser-firmware-0.3.1.json", "${dl}dispenser-firmware-0.3.1.json", 321, null),
            ),
            r.assets,
        )
        assertTrue(r.isFirmwareOnly)
        // a size given as a string is not a size
        val odd = GitHubReleaseParser.parse("""[{"tag_name":"v1.0.0","assets":[{"name":"a","size":"12"}],"body":""}]""")!!.single()
        assertNull(odd.assets.single().size)
        assertNull(odd.body)
    }

    @Test
    fun manifest_parser_types() {
        val m = GitHubReleaseParser.parseManifest(manifestJson)!!
        assertEquals(1L, m.schema)
        assertEquals("0.3.1", m.version)
        assertEquals(1_430_528L, m.size)
        assertEquals("0.3.0", m.minInstalled)
        assertNull(m.notes)
        val wrong = GitHubReleaseParser.parseManifest("""{"schema":"1","size":"5","version":3}""")!!
        assertNull(wrong.schema)
        assertNull(wrong.size)
        assertNull(wrong.version)
        assertNull(GitHubReleaseParser.parseManifest("[1,2]"))
        assertNull(GitHubReleaseParser.parseManifest("not json"))
    }

    private fun downloader(maxBytes: Int = 16 * 1024) = HttpsManifestDownloader(
        userAgent = "Dispenser-Android/0.5.0", allowedPrefix = server.url("/Nowass/dispenser/releases/download/").toString(),
        timeoutMs = 2_000, maxBytes = maxBytes, io = Dispatchers.IO,
    )

    private fun url(path: String) = server.url("/Nowass/dispenser/releases/download/$path").toString()

    @Test
    fun manifest_download_follows_redirects_without_credentials() {
        server.enqueue(MockResponse().setResponseCode(302).addHeader("Location", server.url("/asset-host/manifest.json")))
        server.enqueue(MockResponse().setBody(manifestJson))
        val r = runBlocking { downloader().fetch(url("firmware-v0.3.1/dispenser-firmware-0.3.1.json")) }
        assertEquals("0.3.1", (r as ManifestFetch.Fetched).manifest?.version)
        val first = server.takeRequest()
        assertEquals("GET", first.method)
        assertEquals("Dispenser-Android/0.5.0", first.getHeader("User-Agent"))
        assertNull(first.getHeader("Authorization"))
        assertEquals("/asset-host/manifest.json", server.takeRequest().path)
    }

    @Test
    fun manifest_outside_the_release_path_is_never_requested() {
        val r = runBlocking { downloader().fetch(server.url("/other/dispenser-firmware-0.3.1.json").toString()) }
        assertEquals(ManifestFetch.Unavailable, r)
        val official = runBlocking { HttpsManifestDownloader("UA").fetch("https://example.com/x.json") }
        assertEquals(ManifestFetch.Unavailable, official)
        assertEquals(0, server.requestCount)
    }

    @Test
    fun manifest_errors() {
        server.enqueue(MockResponse().setResponseCode(404))
        assertEquals(ManifestFetch.Unavailable, runBlocking { downloader().fetch(url("t/m.json")) })
        server.enqueue(MockResponse().setBody("x".repeat(200)))
        assertEquals(ManifestFetch.Unavailable, runBlocking { downloader(maxBytes = 100).fetch(url("t/m.json")) })
        server.enqueue(MockResponse().setBody("<html/>"))
        assertEquals(ManifestFetch.Fetched(null), runBlocking { downloader().fetch(url("t/m.json")) })
        server.enqueue(MockResponse().setResponseCode(429))
        assertEquals(ManifestFetch.Failed(UpdateCheckResult.RateLimited(null)), runBlocking { downloader().fetch(url("t/m.json")) })
        val d = downloader()
        server.shutdown()
        assertEquals(ManifestFetch.Failed(UpdateCheckResult.Offline), runBlocking { d.fetch(url("t/m.json")) })
    }

    @Test
    fun checker_uses_the_release_list_and_the_manifest() {
        val releases = GitHubReleaseParser.parse(releasesJson)!!
        val fetched = mutableListOf<String>()
        val checker = GitHubFirmwareUpdateChecker(
            releases = { ReleaseList.Ok(releases) },
            manifests = { u -> fetched += u; ManifestFetch.Fetched(GitHubReleaseParser.parseManifest(manifestJson)) },
        )
        val r = runBlocking { checker.check("0.3.0", beta = false) } as FirmwareCheckResult.Available
        assertEquals("0.3.1", r.offer.version)
        assertEquals("firmware-v0.3.1", r.offer.tag)
        assertEquals("Opravy.", r.offer.notes) // release body (no manifest notes)
        assertEquals(listOf("${dl}dispenser-firmware-0.3.1.json"), fetched)

        val failing = GitHubFirmwareUpdateChecker({ ReleaseList.Failed(UpdateCheckResult.RepositoryUnavailable) }, { error("not fetched") })
        assertEquals(FirmwareCheckResult.Failed(UpdateCheckResult.RepositoryUnavailable), runBlocking { failing.check("0.3.0", false) })
    }

    @Test
    fun release_list_is_shared_with_the_app_check() {
        server.enqueue(MockResponse().setBody(releasesJson))
        val c = GitHubReleasesUpdateChecker("Nowass", "dispenser", "UA", apiBaseUrl = server.url("").toString().trimEnd('/'), timeoutMs = 2_000, io = Dispatchers.IO)
        val list = runBlocking { c.releases() } as ReleaseList.Ok
        assertEquals("firmware-v0.3.1", list.releases.single().tagName)
        assertEquals("/repos/Nowass/dispenser/releases?per_page=100", server.takeRequest().path)
        // the same list through the app check: a firmware-only release is not an app release
        server.enqueue(MockResponse().setBody(releasesJson))
        assertEquals(UpdateCheckResult.NoReleases, runBlocking { c.check(cz.nowass.dispenser.domain.update.SemanticVersion.parse("0.4.0")!!, cz.nowass.dispenser.domain.update.ReleaseChannel.STABLE) })
    }
}
