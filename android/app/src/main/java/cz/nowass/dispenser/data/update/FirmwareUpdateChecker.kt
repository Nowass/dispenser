package cz.nowass.dispenser.data.update

import cz.nowass.dispenser.domain.update.FirmwareCheckResult
import cz.nowass.dispenser.domain.update.FirmwareUpdatePolicy
import cz.nowass.dispenser.domain.update.ManifestFetch
import cz.nowass.dispenser.domain.update.UpdateCheckResult
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL

/**
 * Looks for a newer firmware for the connected device (docs/FIRMWARE_UPDATE.md 3). Runs only when the
 * owner asks (no background polling). [runningVersion]: GET_UPDATE_STATUS `running_version`.
 */
fun interface FirmwareUpdateChecker {
    suspend fun check(runningVersion: String, beta: Boolean): FirmwareCheckResult
}

/**
 * The existing GitHub release list ([GitHubReleasesUpdateChecker.releases]) plus one small manifest
 * download per newer candidate, newest first, until one qualifies.
 */
class GitHubFirmwareUpdateChecker(
    private val releases: suspend () -> ReleaseList,
    private val manifests: ManifestDownloader,
) : FirmwareUpdateChecker {
    override suspend fun check(runningVersion: String, beta: Boolean): FirmwareCheckResult = when (val r = releases()) {
        is ReleaseList.Failed -> FirmwareCheckResult.Failed(r.result)
        is ReleaseList.Ok -> FirmwareUpdatePolicy.select(runningVersion, r.releases, beta) { c ->
            c.manifest.downloadUrl?.let { manifests.fetch(it) } ?: ManifestFetch.Unavailable
        }
    }
}

fun interface ManifestDownloader {
    suspend fun fetch(url: String): ManifestFetch
}

/**
 * HTTPS GET of a manifest from the official release download path only ([allowedPrefix]); redirects
 * to GitHub's asset host are followed (HttpURLConnection never follows a redirect to another scheme).
 * Generic User-Agent, no credentials, at most [maxBytes].
 */
class HttpsManifestDownloader(
    private val userAgent: String,
    private val allowedPrefix: String = FirmwareUpdatePolicy.MANIFEST_URL_PREFIX,
    private val timeoutMs: Int = 10_000,
    private val maxBytes: Int = 16 * 1024,
    private val io: CoroutineDispatcher = Dispatchers.IO,
) : ManifestDownloader {
    override suspend fun fetch(url: String): ManifestFetch = withContext(io) {
        if (!url.startsWith(allowedPrefix)) return@withContext ManifestFetch.Unavailable
        val scheme = URL(allowedPrefix).protocol
        val connection = try {
            URL(url).openConnection() as HttpURLConnection
        } catch (_: IOException) {
            return@withContext ManifestFetch.Failed(UpdateCheckResult.Offline)
        }
        try {
            connection.requestMethod = "GET"
            connection.connectTimeout = timeoutMs
            connection.readTimeout = timeoutMs
            connection.instanceFollowRedirects = true
            connection.setRequestProperty("User-Agent", userAgent)
            val code = connection.responseCode
            when {
                connection.url.protocol != scheme -> ManifestFetch.Unavailable
                code == 200 -> {
                    val bytes = connection.inputStream.use { it.readNBytesCompat(maxBytes + 1) }
                    if (bytes.size > maxBytes) ManifestFetch.Unavailable
                    else ManifestFetch.Fetched(GitHubReleaseParser.parseManifest(bytes.toString(Charsets.UTF_8)))
                }
                code == 429 || code == 403 && connection.getHeaderField("X-RateLimit-Remaining") == "0" ->
                    ManifestFetch.Failed(rateLimitOr(connection, code))
                else -> ManifestFetch.Unavailable
            }
        } catch (_: IOException) {
            ManifestFetch.Failed(UpdateCheckResult.Offline)
        } finally {
            connection.disconnect()
        }
    }

    private fun java.io.InputStream.readNBytesCompat(n: Int): ByteArray {
        val out = java.io.ByteArrayOutputStream()
        val buf = ByteArray(4096)
        while (out.size() < n) {
            val k = read(buf, 0, minOf(buf.size, n - out.size()))
            if (k < 0) break
            out.write(buf, 0, k)
        }
        return out.toByteArray()
    }
}
