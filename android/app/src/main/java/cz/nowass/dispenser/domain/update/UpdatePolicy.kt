package cz.nowass.dispenser.domain.update

import java.time.Instant

/**
 * Release channel of the INSTALLED build (D6): a pre-release versionName (e.g. 0.5.0-beta.1)
 * means BETA, otherwise STABLE. Derived, never switched automatically.
 *
 * - STABLE considers stable releases only.
 * - BETA considers beta and stable releases and offers the newest one.
 */
enum class ReleaseChannel {
    STABLE, BETA;

    companion object {
        fun forInstalledVersion(version: SemanticVersion): ReleaseChannel = if (version.isPreRelease) BETA else STABLE
    }
}

/** Public metadata of one GitHub release (no credentials, no user data involved). */
data class ReleaseInfo(
    val tagName: String,
    /** Parsed from [tagName]; null when the tag is not a semantic version. */
    val version: SemanticVersion?,
    val isPreRelease: Boolean,
    val isDraft: Boolean,
    /** Official release page (https://github.com/...); null if missing or not on github.com. */
    val releasePageUrl: String?,
    val apkAssetNames: List<String>,
    /** Every asset (firmware selection, docs/FIRMWARE_UPDATE.md 3). */
    val assets: List<ReleaseAsset> = emptyList(),
    /** Release notes (Markdown as published); null when empty. */
    val body: String? = null,
) {
    /**
     * A firmware-only release: its tag is not a semantic version (`firmware-v0.3.1`) and it carries
     * firmware assets. Never an app release (docs/FIRMWARE_UPDATE.md 2).
     */
    val isFirmwareOnly: Boolean get() = version == null && assets.any { FirmwareUpdatePolicy.isFirmwareAsset(it.name) }
}

/** One release asset as GitHub lists it; [digest] e.g. "sha256:<hex>" when GitHub reports one. */
data class ReleaseAsset(
    val name: String,
    val downloadUrl: String?,
    val size: Long?,
    val digest: String? = null,
)

sealed interface UpdateCheckResult {
    /** No newer release for this channel. [latest] is the newest known release of the channel, if any. */
    data class UpToDate(val latest: SemanticVersion?) : UpdateCheckResult

    /** A newer release with an Android APK asset exists. Open [releasePageUrl] to download. */
    data class UpdateAvailable(
        val version: SemanticVersion,
        val releasePageUrl: String,
        val apkAssetName: String,
        val isPreRelease: Boolean,
    ) : UpdateCheckResult

    /** Newer release(s) exist, but none provides an APK asset. */
    data class NewerReleaseWithoutApk(val version: SemanticVersion, val releasePageUrl: String) : UpdateCheckResult

    /** The repository has no published releases. */
    data object NoReleases : UpdateCheckResult

    /** Release metadata not accessible (repository private or not found); no credentials are used. */
    data object RepositoryUnavailable : UpdateCheckResult

    data object Offline : UpdateCheckResult
    data class RateLimited(val resetAt: Instant?) : UpdateCheckResult
    data object InvalidMetadata : UpdateCheckResult
    data class HttpError(val code: Int) : UpdateCheckResult
}

/** Pure decision logic: installed version + channel + release list -> result. */
object UpdatePolicy {

    fun evaluate(installed: SemanticVersion, channel: ReleaseChannel, releases: List<ReleaseInfo>): UpdateCheckResult {
        // firmware-only releases are not app releases at all (no InvalidMetadata because of them)
        val published = releases.filterNot { it.isDraft || it.isFirmwareOnly }
        if (published.isEmpty()) return UpdateCheckResult.NoReleases

        // Only releases with a valid semantic version and an official release page are usable.
        val valid = published.filter { it.version != null && it.releasePageUrl != null }
        if (valid.isEmpty()) return UpdateCheckResult.InvalidMetadata

        // A release counts as beta if GitHub marks it pre-release OR its version is a pre-release.
        val inChannel = when (channel) {
            ReleaseChannel.STABLE -> valid.filter { !it.isPreRelease && !it.version!!.isPreRelease }
            ReleaseChannel.BETA -> valid
        }
        val latest = inChannel.maxByOrNull { it.version!! }?.version
        val newer = inChannel.filter { it.version!! > installed }.sortedByDescending { it.version!! }
        if (newer.isEmpty()) return UpdateCheckResult.UpToDate(latest)

        val withApk = newer.firstOrNull { it.apkAssetNames.isNotEmpty() }
            ?: return UpdateCheckResult.NewerReleaseWithoutApk(newer.first().version!!, newer.first().releasePageUrl!!)
        return UpdateCheckResult.UpdateAvailable(
            version = withApk.version!!,
            releasePageUrl = withApk.releasePageUrl!!,
            apkAssetName = withApk.apkAssetNames.first(),
            isPreRelease = withApk.isPreRelease || withApk.version.isPreRelease,
        )
    }
}
