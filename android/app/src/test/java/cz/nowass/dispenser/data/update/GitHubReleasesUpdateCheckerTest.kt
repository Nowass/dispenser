package cz.nowass.dispenser.data.update

import cz.nowass.dispenser.domain.update.ReleaseChannel
import cz.nowass.dispenser.domain.update.SemanticVersion
import cz.nowass.dispenser.domain.update.UpdateCheckResult
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.runBlocking
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Before
import org.junit.Test
import java.time.Instant

class GitHubReleasesUpdateCheckerTest {
    private lateinit var server: MockWebServer
    private val installed = SemanticVersion.parse("0.4.0")!!

    @Before fun setUp() { server = MockWebServer(); server.start() }
    @After fun tearDown() { runCatching { server.shutdown() } }

    private fun checker() = GitHubReleasesUpdateChecker(
        owner = "Nowass", repo = "dispenser", userAgent = "Dispenser-Android/0.4.0",
        apiBaseUrl = server.url("").toString().trimEnd('/'), timeoutMs = 2_000, io = Dispatchers.IO,
    )

    private fun release(tag: String, prerelease: Boolean = false, assets: String = """[{"name":"dispenser-$tag.apk","content_type":"application/vnd.android.package-archive"}]""") =
        """{"tag_name":"$tag","name":"Dispenser $tag","draft":false,"prerelease":$prerelease,"html_url":"https://github.com/Nowass/dispenser/releases/tag/$tag","assets":$assets}"""

    private fun check() = runBlocking { checker().check(installed, ReleaseChannel.STABLE) }

    @Test
    fun requestsPublicMetadataOnly_withoutCredentials() {
        server.enqueue(MockResponse().setBody("[]"))
        check()
        val req = server.takeRequest()
        assertEquals("GET", req.method)
        assertEquals("/repos/Nowass/dispenser/releases?per_page=100", req.path)
        assertEquals("Dispenser-Android/0.4.0", req.getHeader("User-Agent"))
        assertEquals("application/vnd.github+json", req.getHeader("Accept"))
        assertNull(req.getHeader("Authorization"))
        assertEquals(0L, req.bodySize)
    }

    @Test
    fun noPublishedReleases() {
        server.enqueue(MockResponse().setBody("[]"))
        assertEquals(UpdateCheckResult.NoReleases, check())
    }

    @Test
    fun upToDate_andUpdateAvailable() {
        server.enqueue(MockResponse().setBody("[${release("v0.4.0")}]"))
        assertEquals(UpdateCheckResult.UpToDate(installed), check())

        server.enqueue(MockResponse().setBody("[${release("v0.5.0")},${release("v0.6.0-beta.1", prerelease = true)}]"))
        val r = check() as UpdateCheckResult.UpdateAvailable
        assertEquals(SemanticVersion.parse("0.5.0"), r.version)
        assertEquals("https://github.com/Nowass/dispenser/releases/tag/v0.5.0", r.releasePageUrl)
        assertEquals("dispenser-v0.5.0.apk", r.apkAssetName)
    }

    @Test
    fun missingApkAsset() {
        server.enqueue(MockResponse().setBody("[${release("v0.5.0", assets = """[{"name":"notes.zip","content_type":"application/zip"}]""")}]"))
        assertEquals(
            UpdateCheckResult.NewerReleaseWithoutApk(SemanticVersion.parse("0.5.0")!!, "https://github.com/Nowass/dispenser/releases/tag/v0.5.0"),
            check(),
        )
    }

    @Test
    fun privateOrMissingRepository_404_isUnavailable() {
        server.enqueue(MockResponse().setResponseCode(404).setBody("""{"message":"Not Found"}"""))
        assertEquals(UpdateCheckResult.RepositoryUnavailable, check())
    }

    @Test
    fun rateLimited_403and429() {
        server.enqueue(MockResponse().setResponseCode(403).addHeader("X-RateLimit-Remaining", "0").addHeader("X-RateLimit-Reset", "1800000000"))
        assertEquals(UpdateCheckResult.RateLimited(Instant.ofEpochSecond(1_800_000_000)), check())
        server.enqueue(MockResponse().setResponseCode(429))
        assertEquals(UpdateCheckResult.RateLimited(null), check())
        server.enqueue(MockResponse().setResponseCode(403).addHeader("X-RateLimit-Remaining", "12"))
        assertEquals(UpdateCheckResult.HttpError(403), check())
    }

    @Test
    fun invalidResponses() {
        server.enqueue(MockResponse().setBody("<html>not json</html>"))
        assertEquals(UpdateCheckResult.InvalidMetadata, check())
        server.enqueue(MockResponse().setBody("""{"message":"unexpected object"}"""))
        assertEquals(UpdateCheckResult.InvalidMetadata, check())
        server.enqueue(MockResponse().setBody("""[{"name":"no tag"}]"""))
        assertEquals(UpdateCheckResult.InvalidMetadata, check())
        // Release page outside github.com is never offered.
        server.enqueue(MockResponse().setBody("""[{"tag_name":"v0.9.0","prerelease":false,"html_url":"https://evil.example/r","assets":[{"name":"x.apk"}]}]"""))
        assertEquals(UpdateCheckResult.InvalidMetadata, check())
    }

    @Test
    fun serverError_isHttpError() {
        server.enqueue(MockResponse().setResponseCode(503))
        assertEquals(UpdateCheckResult.HttpError(503), check())
    }

    @Test
    fun networkFailure_isOffline() {
        val c = checker()
        server.shutdown()
        assertEquals(UpdateCheckResult.Offline, runBlocking { c.check(installed, ReleaseChannel.STABLE) })
    }
}
