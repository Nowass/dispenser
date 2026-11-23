package cz.nowass.dispenser.data.update

import cz.nowass.dispenser.domain.update.FirmwareManifest
import cz.nowass.dispenser.domain.update.ReleaseAsset
import cz.nowass.dispenser.domain.update.ReleaseChannel
import cz.nowass.dispenser.domain.update.ReleaseInfo
import cz.nowass.dispenser.domain.update.SemanticVersion
import cz.nowass.dispenser.domain.update.UpdateCheckResult
import cz.nowass.dispenser.domain.update.UpdatePolicy
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.longOrNull
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URI
import java.net.URL
import java.time.Instant

/** Checks for a newer published release. Runs only when the user asks (no background polling). */
fun interface UpdateChecker {
    suspend fun check(installed: SemanticVersion, channel: ReleaseChannel): UpdateCheckResult
}

/**
 * GitHub Releases implementation (public REST API, unauthenticated).
 *
 * Privacy/security: sends only a GET for public release metadata with a generic User-Agent;
 * no Authorization header, no tokens, no app data. Nothing is downloaded or installed; the
 * user is sent to the official release page in the browser.
 */
class GitHubReleasesUpdateChecker(
    private val owner: String,
    private val repo: String,
    private val userAgent: String,
    private val apiBaseUrl: String = "https://api.github.com",
    private val timeoutMs: Int = 10_000,
    private val io: CoroutineDispatcher = Dispatchers.IO,
) : UpdateChecker {

    override suspend fun check(installed: SemanticVersion, channel: ReleaseChannel): UpdateCheckResult =
        when (val r = releases()) {
            is ReleaseList.Ok -> UpdatePolicy.evaluate(installed, channel, r.releases)
            is ReleaseList.Failed -> r.result
        }

    /** The public release list (also used by the firmware update check); never with credentials. */
    suspend fun releases(): ReleaseList = withContext(io) {
        val url = URL("$apiBaseUrl/repos/$owner/$repo/releases?per_page=100")
        val connection = try {
            url.openConnection() as HttpURLConnection
        } catch (_: IOException) {
            return@withContext ReleaseList.Failed(UpdateCheckResult.Offline)
        }
        try {
            connection.requestMethod = "GET"
            connection.connectTimeout = timeoutMs
            connection.readTimeout = timeoutMs
            connection.instanceFollowRedirects = true
            connection.setRequestProperty("Accept", "application/vnd.github+json")
            connection.setRequestProperty("X-GitHub-Api-Version", "2022-11-28")
            connection.setRequestProperty("User-Agent", userAgent)

            val code = connection.responseCode
            when {
                code == 200 -> {
                    val body = connection.inputStream.bufferedReader().use { it.readText() }
                    GitHubReleaseParser.parse(body)?.let { ReleaseList.Ok(it) } ?: ReleaseList.Failed(UpdateCheckResult.InvalidMetadata)
                }
                // Private or missing repository look identical without credentials.
                code == 404 -> ReleaseList.Failed(UpdateCheckResult.RepositoryUnavailable)
                else -> ReleaseList.Failed(rateLimitOr(connection, code))
            }
        } catch (_: IOException) {
            ReleaseList.Failed(UpdateCheckResult.Offline)
        } finally {
            connection.disconnect()
        }
    }
}

/** The release list or why it could not be read (Offline, RateLimited, RepositoryUnavailable, InvalidMetadata, HttpError). */
sealed interface ReleaseList {
    data class Ok(val releases: List<ReleaseInfo>) : ReleaseList
    data class Failed(val result: UpdateCheckResult) : ReleaseList
}

internal fun rateLimitOr(connection: HttpURLConnection, code: Int): UpdateCheckResult = when {
    code == 429 || (code == 403 && connection.getHeaderField("X-RateLimit-Remaining") == "0") ->
        UpdateCheckResult.RateLimited(connection.getHeaderField("X-RateLimit-Reset")?.toLongOrNull()?.let(Instant::ofEpochSecond))
    else -> UpdateCheckResult.HttpError(code)
}

/** Parses the GitHub "list releases" JSON; returns null for malformed metadata. */
object GitHubReleaseParser {
    private val json = Json { ignoreUnknownKeys = true }

    fun parse(body: String): List<ReleaseInfo>? {
        val root = runCatching { json.parseToJsonElement(body) }.getOrNull() as? JsonArray ?: return null
        return root.map { element ->
            val o = element as? JsonObject ?: return null
            val tag = (o["tag_name"] as? JsonPrimitive)?.contentOrNull ?: return null
            ReleaseInfo(
                tagName = tag,
                version = SemanticVersion.parse(tag),
                isPreRelease = (o["prerelease"] as? JsonPrimitive)?.booleanOrNull ?: false,
                isDraft = (o["draft"] as? JsonPrimitive)?.booleanOrNull ?: false,
                releasePageUrl = (o["html_url"] as? JsonPrimitive)?.contentOrNull?.takeIf(::isOfficialGitHubPage),
                apkAssetNames = (o["assets"] as? JsonArray).orEmpty().mapNotNull { asset ->
                    val a = asset as? JsonObject ?: return@mapNotNull null
                    val name = (a["name"] as? JsonPrimitive)?.contentOrNull ?: return@mapNotNull null
                    val type = (a["content_type"] as? JsonPrimitive)?.contentOrNull
                    name.takeIf { it.endsWith(".apk", ignoreCase = true) || type == APK_MIME }
                },
                assets = (o["assets"] as? JsonArray).orEmpty().mapNotNull { asset ->
                    val a = asset as? JsonObject ?: return@mapNotNull null
                    ReleaseAsset(
                        name = a.string("name") ?: return@mapNotNull null,
                        downloadUrl = a.string("browser_download_url"),
                        size = (a["size"] as? JsonPrimitive)?.takeIf { !it.isString }?.longOrNull,
                        digest = a.string("digest"),
                    )
                },
                body = o.string("body")?.takeIf { it.isNotBlank() },
            )
        }
    }

    private fun JsonObject.string(k: String): String? = (this[k] as? JsonPrimitive)?.takeIf { it.isString }?.contentOrNull

    /** Parses a firmware manifest (docs/FIRMWARE_UPDATE.md 2); null unless a JSON object. Wrong-typed fields are null. */
    fun parseManifest(text: String): FirmwareManifest? {
        val o = runCatching { json.parseToJsonElement(text) }.getOrNull() as? JsonObject ?: return null
        fun num(k: String) = (o[k] as? JsonPrimitive)?.takeIf { !it.isString }?.longOrNull
        return FirmwareManifest(
            schema = num("schema"), project = o.string("project"), hardware = o.string("hardware"),
            version = o.string("version"), asset = o.string("asset"), size = num("size"), sha256 = o.string("sha256"),
            minInstalled = o.string("min_installed"), notes = o.string("notes"),
        )
    }

    /** Only https://github.com/... pages are ever opened. */
    fun isOfficialGitHubPage(url: String): Boolean = runCatching {
        val uri = URI(url)
        uri.scheme == "https" && uri.host == "github.com"
    }.getOrDefault(false)

    private const val APK_MIME = "application/vnd.android.package-archive"
}
